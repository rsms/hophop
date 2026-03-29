#include "libsl-impl.h"
#include "codegen.h"
#include <stdbool.h>

SL_API_BEGIN

typedef struct {
    const SLCodegenUnit*    unit;
    const SLCodegenOptions* options;
    SLDiag* _Nullable diag;
    const char* _Nullable limitDetail;
    uint8_t* _Nullable data;
    uint32_t len;
    uint32_t cap;
    uint32_t maxLen;
    uint32_t limitStart;
    uint32_t limitEnd;
} SLWasmBuf;

typedef struct {
    uint32_t typeIndex;
    uint8_t  logicalParamCount;
    uint8_t  wasmParamCount;
    uint8_t  logicalResultKind;
    uint8_t  wasmResultCount;
    uint8_t  usesSRet;
    uint8_t  _reserved[3];
    uint32_t logicalResultTypeRef;
    uint8_t  logicalParamKinds[32];
    uint8_t  wasmParamTypes[64];
    uint8_t  wasmResultTypes[2];
} SLWasmFnSig;

typedef struct {
    uint16_t wasmValueIndex[256];
    uint16_t wasmParamValueCount;
    uint16_t wasmLocalValueCount;
    uint16_t hiddenLocalStart;
    uint16_t frameBaseLocal;
    uint16_t scratch0Local;
    uint16_t scratch1Local;
    uint16_t scratch2Local;
    uint16_t scratch3Local;
    uint16_t scratch4Local;
    uint16_t scratch5Local;
    uint16_t scratch6Local;
    uint32_t frameOffsets[256];
    uint32_t auxOffsets[256];
    uint32_t allocCallTempOffset;
    uint32_t arrayCounts[256];
    uint32_t localTypeRefs[256];
    uint32_t stackTypeRefs[256];
    uint32_t frameSize;
    uint32_t tempFrameStart;
    uint32_t stackLen;
    uint8_t  localKinds[256];
    uint8_t  localStorage[256];
    uint8_t  localIntKinds[256];
    uint8_t  stackKinds[256];
    uint8_t  usesFrame;
    uint8_t  _reserved[1];
} SLWasmEmitState;

typedef struct {
    uint32_t objectOffset;
    uint32_t dataOffset;
    uint32_t len;
} SLWasmStringRef;

typedef struct {
    SLWasmStringRef* _Nullable constRefs;
    SLWasmBuf       data;
    SLWasmStringRef assertPanic;
    SLWasmStringRef allocNullPanic;
    uint32_t        rootAllocatorOffset;
    uint8_t         hasAssertPanicString;
    uint8_t         hasAllocNullPanicString;
    uint8_t         hasRootAllocator;
    uint8_t         _reserved[1];
} SLWasmStringLayout;

typedef struct {
    int      hasContinue;
    uint32_t continueTargetPc;
    uint32_t continueDepth;
    int      hasBreak;
    uint32_t breakTargetPc;
    uint32_t breakDepth;
} SLWasmBranchTargets;

typedef struct {
    uint32_t headerPc;
    uint32_t condBranchPc;
    uint32_t bodyStartPc;
    uint32_t tailStartPc;
    uint32_t backedgePc;
    uint32_t exitPc;
} SLWasmLoopRegion;

typedef struct {
    uint32_t importFuncCount;
    uint32_t frameGlobalIndex;
    uint32_t heapGlobalIndex;
    uint32_t allocatorIndirectTypeIndex;
    uint32_t rootAllocFuncIndex;
    uint32_t tableFuncCount;
    uint32_t rootAllocTableIndex;
    uint32_t wasmMinExitTypeIndex;
    uint32_t wasmMinExitFuncIndex;
    uint32_t wasmMinConsoleLogTypeIndex;
    uint32_t wasmMinConsoleLogFuncIndex;
    uint32_t wasmMinPanicTypeIndex;
    uint32_t wasmMinPanicFuncIndex;
    uint8_t  hasFunctionTable;
    uint8_t  hasRootAllocThunk;
    uint8_t  hasWasmMinExit;
    uint8_t  hasWasmMinConsoleLog;
    uint8_t  hasWasmMinPanic;
    uint8_t  needsFrameGlobal;
    uint8_t  needsHeapGlobal;
    uint8_t  _reserved[1];
} SLWasmImportLayout;

typedef struct {
    uint32_t mainFuncIndex;
    uint32_t wrapperTypeIndex;
    uint32_t wrapperFuncIndex;
    uint32_t resultOffset;
    uint32_t resultSize;
    uint8_t  hasWrapper;
    uint8_t  resultKind;
    uint8_t  usesSRet;
    uint8_t  _reserved[1];
} SLWasmEntryLayout;

enum {
    SLWasmType_VOID = 0,
    SLWasmType_I32 = 1,
    SLWasmType_STR_REF = 2,
    SLWasmType_STR_PTR = 3,
    SLWasmType_U8_PTR = 4,
    SLWasmType_I32_PTR = 5,
    SLWasmType_I8_PTR = 6,
    SLWasmType_U16_PTR = 7,
    SLWasmType_I16_PTR = 8,
    SLWasmType_U32_PTR = 9,
    SLWasmType_OPAQUE_PTR = 10,
    SLWasmType_ARRAY_VIEW_U8 = 11,
    SLWasmType_ARRAY_VIEW_I8 = 12,
    SLWasmType_ARRAY_VIEW_U16 = 13,
    SLWasmType_ARRAY_VIEW_I16 = 14,
    SLWasmType_ARRAY_VIEW_U32 = 15,
    SLWasmType_ARRAY_VIEW_I32 = 16,
    SLWasmType_SLICE_U8 = 17,
    SLWasmType_SLICE_I8 = 18,
    SLWasmType_SLICE_U16 = 19,
    SLWasmType_SLICE_I16 = 20,
    SLWasmType_SLICE_U32 = 21,
    SLWasmType_SLICE_I32 = 22,
    SLWasmType_SLICE_AGG = 23,
    SLWasmType_AGG_REF = 24,
    SLWasmType_FUNC_REF = 25,
    SLWasmType_FUNC_REF_PTR = 26,
};

enum {
    SLWasmLocalStorage_PLAIN = 0,
    SLWasmLocalStorage_ARRAY = 1,
    SLWasmLocalStorage_AGG = 2,
};

static void WasmSetDiag(SLDiag* _Nullable diag, SLDiagCode code, uint32_t start, uint32_t end) {
    if (diag == NULL) {
        return;
    }
    diag->code = code;
    diag->type = SLDiagTypeOfCode(code);
    diag->start = start;
    diag->end = end;
    diag->argStart = 0;
    diag->argEnd = 0;
    diag->detail = NULL;
    diag->hintOverride = NULL;
}

static bool WasmSliceEqLiteral(const char* src, uint32_t start, uint32_t end, const char* lit) {
    uint32_t i = 0;
    if (src == NULL || lit == NULL || end < start) {
        return false;
    }
    while (start + i < end) {
        if (lit[i] == '\0' || src[start + i] != lit[i]) {
            return false;
        }
        i++;
    }
    return lit[i] == '\0';
}

static bool WasmFunctionIsNamedMain(const SLMirProgram* program, const SLMirFunction* fn) {
    return program != NULL && fn != NULL && fn->sourceRef < program->sourceLen
        && WasmSliceEqLiteral(
               program->sources[fn->sourceRef].src.ptr, fn->nameStart, fn->nameEnd, "main");
}

static int WasmReserve(SLWasmBuf* b, uint32_t extra) {
    uint32_t need;
    uint32_t allocSize = 0;
    uint32_t targetCap;
    uint8_t* newData;
    if (b == NULL || b->options == NULL || b->options->arenaGrow == NULL) {
        return -1;
    }
    if (UINT32_MAX - b->len < extra) {
        return -1;
    }
    need = b->len + extra;
    if (need <= b->cap) {
        return 0;
    }
    if (b->maxLen != 0u && need > b->maxLen) {
        WasmSetDiag(b->diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, b->limitStart, b->limitEnd);
        if (b->diag != NULL && b->limitDetail != NULL) {
            b->diag->detail = b->limitDetail;
        }
        return -1;
    }
    // Grow geometrically so append-heavy emission does not degenerate into quadratic memcpy churn.
    targetCap = b->cap >= 256u ? b->cap : 256u;
    while (targetCap < need) {
        if (targetCap > UINT32_MAX / 2u) {
            targetCap = need;
            break;
        }
        targetCap *= 2u;
    }
    if (targetCap < need) {
        targetCap = need;
    }
    newData = (uint8_t*)b->options->arenaGrow(b->options->allocatorCtx, targetCap, &allocSize);
    if (newData == NULL || allocSize < need) {
        if (newData != NULL && b->options->arenaFree != NULL) {
            b->options->arenaFree(b->options->allocatorCtx, newData, allocSize);
        }
        return -1;
    }
    if (b->data != NULL && b->len > 0) {
        memcpy(newData, b->data, b->len);
        if (b->options->arenaFree != NULL) {
            b->options->arenaFree(b->options->allocatorCtx, b->data, b->cap);
        }
    }
    b->data = newData;
    b->cap = allocSize;
    return 0;
}

static int WasmAppendByte(SLWasmBuf* b, uint8_t v) {
    if (WasmReserve(b, 1u) != 0) {
        return -1;
    }
    b->data[b->len++] = v;
    return 0;
}

static int WasmAppendBytes(SLWasmBuf* b, const void* p, uint32_t len) {
    if (len == 0) {
        return 0;
    }
    if (WasmReserve(b, len) != 0) {
        return -1;
    }
    memcpy(b->data + b->len, p, len);
    b->len += len;
    return 0;
}

static int WasmAppendULEB(SLWasmBuf* b, uint32_t v) {
    do {
        uint8_t byte = (uint8_t)(v & 0x7fu);
        v >>= 7u;
        if (v != 0) {
            byte |= 0x80u;
        }
        if (WasmAppendByte(b, byte) != 0) {
            return -1;
        }
    } while (v != 0);
    return 0;
}

static int WasmAppendSLEB32(SLWasmBuf* b, int32_t v) {
    int more = 1;
    while (more) {
        uint8_t byte = (uint8_t)(v & 0x7f);
        int32_t sign = byte & 0x40u;
        v >>= 7;
        more = !((v == 0 && sign == 0) || (v == -1 && sign != 0));
        if (more) {
            byte |= 0x80u;
        }
        if (WasmAppendByte(b, byte) != 0) {
            return -1;
        }
    }
    return 0;
}

static int WasmAppendU32LE(SLWasmBuf* b, uint32_t v) {
    uint8_t bytes[4];
    bytes[0] = (uint8_t)(v & 0xffu);
    bytes[1] = (uint8_t)((v >> 8u) & 0xffu);
    bytes[2] = (uint8_t)((v >> 16u) & 0xffu);
    bytes[3] = (uint8_t)((v >> 24u) & 0xffu);
    return WasmAppendBytes(b, bytes, 4u);
}

static int WasmAppendSection(SLWasmBuf* out, uint8_t id, const SLWasmBuf* section) {
    if (WasmAppendByte(out, id) != 0 || WasmAppendULEB(out, section->len) != 0
        || WasmAppendBytes(out, section->data, section->len) != 0)
    {
        return -1;
    }
    return 0;
}

static const char* WasmMirOpName(SLMirOp op) {
    switch (op) {
        case SLMirOp_PUSH_CONST:    return "PUSH_CONST";
        case SLMirOp_UNARY:         return "UNARY";
        case SLMirOp_BINARY:        return "BINARY";
        case SLMirOp_CAST:          return "CAST";
        case SLMirOp_COERCE:        return "COERCE";
        case SLMirOp_AGG_MAKE:      return "AGG_MAKE";
        case SLMirOp_AGG_ZERO:      return "AGG_ZERO";
        case SLMirOp_AGG_SET:       return "AGG_SET";
        case SLMirOp_LOCAL_ZERO:    return "LOCAL_ZERO";
        case SLMirOp_LOCAL_LOAD:    return "LOCAL_LOAD";
        case SLMirOp_LOCAL_STORE:   return "LOCAL_STORE";
        case SLMirOp_ARRAY_ADDR:    return "ARRAY_ADDR";
        case SLMirOp_DROP:          return "DROP";
        case SLMirOp_CALL_FN:       return "CALL_FN";
        case SLMirOp_RETURN:        return "RETURN";
        case SLMirOp_RETURN_VOID:   return "RETURN_VOID";
        case SLMirOp_LOAD_IDENT:    return "LOAD_IDENT";
        case SLMirOp_CALL:          return "CALL";
        case SLMirOp_CALL_HOST:     return "CALL_HOST";
        case SLMirOp_CALL_INDIRECT: return "CALL_INDIRECT";
        case SLMirOp_JUMP:          return "JUMP";
        case SLMirOp_JUMP_IF_FALSE: return "JUMP_IF_FALSE";
        case SLMirOp_ASSERT:        return "ASSERT";
        default:                    return "unsupported";
    }
}

static bool WasmStrEq(const char* a, const char* b) {
    if (a == NULL || b == NULL) {
        return a == b;
    }
    for (; *a != 0 && *b != 0; a++, b++) {
        if (*a != *b) {
            return false;
        }
    }
    return *a == *b;
}

static bool WasmIsWasmMinPlatform(const SLCodegenUnit* unit) {
    return unit != NULL && unit->platformTarget != NULL
        && WasmStrEq(unit->platformTarget, "wasm-min");
}

static bool WasmProgramHasAssert(const SLMirProgram* program) {
    uint32_t i;
    if (program == NULL) {
        return false;
    }
    for (i = 0; i < program->instLen; i++) {
        if (program->insts[i].op == SLMirOp_ASSERT) {
            return true;
        }
    }
    return false;
}

static bool WasmProgramNeedsRootAllocator(const SLMirProgram* program) {
    uint32_t i;
    if (program == NULL) {
        return false;
    }
    for (i = 0; i < program->instLen; i++) {
        if (program->insts[i].op == SLMirOp_CTX_GET
            && (program->insts[i].aux == SLMirContextField_MEM
                || program->insts[i].aux == SLMirContextField_TEMP_MEM))
        {
            return true;
        }
    }
    return false;
}

static bool WasmProgramHasAllocNullPanic(const SLMirProgram* program) {
    uint32_t funcIndex;
    if (program == NULL) {
        return false;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const SLMirFunction* fn = &program->funcs[funcIndex];
        uint32_t             pc;
        for (pc = 0; pc + 1u < fn->instLen; pc++) {
            const SLMirInst* allocInst = &program->insts[fn->instStart + pc];
            const SLMirInst* nextInst = &program->insts[fn->instStart + pc + 1u];
            uint32_t         typeRef;
            if (allocInst->op != SLMirOp_ALLOC_NEW
                || (allocInst->tok & SLAstFlag_NEW_HAS_ALLOC) == 0u
                || nextInst->op != SLMirOp_LOCAL_STORE || nextInst->aux >= fn->localCount)
            {
                continue;
            }
            typeRef = program->locals[fn->localStart + nextInst->aux].typeRef;
            if (typeRef < program->typeLen && !SLMirTypeRefIsOptional(&program->types[typeRef])) {
                return true;
            }
        }
    }
    return false;
}

static bool WasmProgramNeedsFunctionTable(const SLMirProgram* program) {
    uint32_t i;
    if (program == NULL) {
        return false;
    }
    for (i = 0; i < program->constLen; i++) {
        if (program->consts[i].kind == SLMirConst_FUNCTION) {
            return true;
        }
    }
    for (i = 0; i < program->instLen; i++) {
        if (program->insts[i].op == SLMirOp_CALL_INDIRECT
            || (program->insts[i].op == SLMirOp_ALLOC_NEW
                && (program->insts[i].tok & SLAstFlag_NEW_HAS_ALLOC) != 0u))
        {
            return true;
        }
    }
    return false;
}

static bool WasmProgramNeedsFrameMemory(const SLMirProgram* program);
static bool WasmProgramNeedsHeapMemory(const SLMirProgram* program);
static bool WasmFunctionNeedsIndirectScratch(const SLMirProgram* program, const SLMirFunction* fn);
static int  WasmEmitAddrFromFrame(
    SLWasmBuf* body, const SLWasmEmitState* state, uint32_t offset, uint16_t addend);
static int WasmTypeByteSize(const SLMirProgram* program, uint32_t typeRefIndex);
static int WasmTypeByteAlign(const SLMirProgram* program, uint32_t typeRefIndex);
static int WasmFindAggregateField(
    const SLMirProgram* program,
    uint32_t            ownerTypeRef,
    const SLMirField*   fieldRef,
    uint32_t*           outFieldIndex,
    uint32_t*           outOffset);
static bool WasmAggregateHasDynamicLayout(const SLMirProgram* program, uint32_t ownerTypeRef);
static int  WasmTempTypeRefForInst(const SLMirProgram* program, const SLMirInst* inst);
static int  WasmTempOffsetForPc(
    const SLMirProgram*    program,
    const SLMirFunction*   fn,
    const SLWasmEmitState* state,
    uint32_t               pc,
    uint32_t* _Nonnull outOffset);

static uint8_t WasmTypeKindSlotCount(uint8_t typeKind) {
    switch (typeKind) {
        case SLWasmType_I32:
        case SLWasmType_STR_PTR:
        case SLWasmType_U8_PTR:
        case SLWasmType_I32_PTR:
        case SLWasmType_I8_PTR:
        case SLWasmType_U16_PTR:
        case SLWasmType_I16_PTR:
        case SLWasmType_U32_PTR:
        case SLWasmType_OPAQUE_PTR:
        case SLWasmType_ARRAY_VIEW_U8:
        case SLWasmType_ARRAY_VIEW_I8:
        case SLWasmType_ARRAY_VIEW_U16:
        case SLWasmType_ARRAY_VIEW_I16:
        case SLWasmType_ARRAY_VIEW_U32:
        case SLWasmType_ARRAY_VIEW_I32:
        case SLWasmType_AGG_REF:
        case SLWasmType_FUNC_REF:
        case SLWasmType_FUNC_REF_PTR:   return 1u;
        case SLWasmType_STR_REF:
        case SLWasmType_SLICE_U8:
        case SLWasmType_SLICE_I8:
        case SLWasmType_SLICE_U16:
        case SLWasmType_SLICE_I16:
        case SLWasmType_SLICE_U32:
        case SLWasmType_SLICE_I32:
        case SLWasmType_SLICE_AGG:      return 2u;
        default:                        return 0u;
    }
}

static bool WasmTypeKindIsSupported(uint8_t typeKind) {
    return WasmTypeKindSlotCount(typeKind) != 0u;
}

static bool WasmTypeKindFromMirType(
    const SLMirProgram* program, uint32_t typeRefIndex, uint8_t* outTypeKind) {
    const SLMirTypeRef* typeRef;
    if (outTypeKind == NULL) {
        return false;
    }
    *outTypeKind = SLWasmType_VOID;
    if (typeRefIndex == UINT32_MAX) {
        return true;
    }
    if (program == NULL || typeRefIndex >= program->typeLen) {
        return false;
    }
    typeRef = &program->types[typeRefIndex];
    if (SLMirTypeRefIsStrRef(typeRef)) {
        *outTypeKind = SLWasmType_STR_REF;
        return true;
    }
    if (SLMirTypeRefIsStrObj(typeRef)) {
        *outTypeKind = SLWasmType_STR_PTR;
        return true;
    }
    if (SLMirTypeRefIsStrPtr(typeRef)) {
        *outTypeKind = SLWasmType_STR_PTR;
        return true;
    }
    if (SLMirTypeRefIsU8Ptr(typeRef)) {
        *outTypeKind = SLWasmType_U8_PTR;
        return true;
    }
    if (SLMirTypeRefIsI8Ptr(typeRef)) {
        *outTypeKind = SLWasmType_I8_PTR;
        return true;
    }
    if (SLMirTypeRefIsU16Ptr(typeRef)) {
        *outTypeKind = SLWasmType_U16_PTR;
        return true;
    }
    if (SLMirTypeRefIsI16Ptr(typeRef)) {
        *outTypeKind = SLWasmType_I16_PTR;
        return true;
    }
    if (SLMirTypeRefIsU32Ptr(typeRef)) {
        *outTypeKind = SLWasmType_U32_PTR;
        return true;
    }
    if (SLMirTypeRefIsFixedArray(typeRef)) {
        switch (SLMirTypeRefIntKind(typeRef)) {
            case SLMirIntKind_U8:   *outTypeKind = SLWasmType_ARRAY_VIEW_U8; return true;
            case SLMirIntKind_I8:   *outTypeKind = SLWasmType_ARRAY_VIEW_I8; return true;
            case SLMirIntKind_U16:  *outTypeKind = SLWasmType_ARRAY_VIEW_U16; return true;
            case SLMirIntKind_I16:  *outTypeKind = SLWasmType_ARRAY_VIEW_I16; return true;
            case SLMirIntKind_U32:  *outTypeKind = SLWasmType_ARRAY_VIEW_U32; return true;
            case SLMirIntKind_BOOL:
            case SLMirIntKind_I32:  *outTypeKind = SLWasmType_ARRAY_VIEW_I32; return true;
            default:                return false;
        }
    }
    if (SLMirTypeRefIsFixedArrayView(typeRef)) {
        switch (SLMirTypeRefIntKind(typeRef)) {
            case SLMirIntKind_U8:   *outTypeKind = SLWasmType_ARRAY_VIEW_U8; return true;
            case SLMirIntKind_I8:   *outTypeKind = SLWasmType_ARRAY_VIEW_I8; return true;
            case SLMirIntKind_U16:  *outTypeKind = SLWasmType_ARRAY_VIEW_U16; return true;
            case SLMirIntKind_I16:  *outTypeKind = SLWasmType_ARRAY_VIEW_I16; return true;
            case SLMirIntKind_U32:  *outTypeKind = SLWasmType_ARRAY_VIEW_U32; return true;
            case SLMirIntKind_BOOL:
            case SLMirIntKind_I32:  *outTypeKind = SLWasmType_ARRAY_VIEW_I32; return true;
            default:                return false;
        }
    }
    if (SLMirTypeRefIsSliceView(typeRef)) {
        switch (SLMirTypeRefIntKind(typeRef)) {
            case SLMirIntKind_U8:   *outTypeKind = SLWasmType_SLICE_U8; return true;
            case SLMirIntKind_I8:   *outTypeKind = SLWasmType_SLICE_I8; return true;
            case SLMirIntKind_U16:  *outTypeKind = SLWasmType_SLICE_U16; return true;
            case SLMirIntKind_I16:  *outTypeKind = SLWasmType_SLICE_I16; return true;
            case SLMirIntKind_U32:  *outTypeKind = SLWasmType_SLICE_U32; return true;
            case SLMirIntKind_BOOL:
            case SLMirIntKind_I32:  *outTypeKind = SLWasmType_SLICE_I32; return true;
            default:                return false;
        }
    }
    if (SLMirTypeRefIsVArrayView(typeRef)) {
        switch (SLMirTypeRefIntKind(typeRef)) {
            case SLMirIntKind_U8:   *outTypeKind = SLWasmType_SLICE_U8; return true;
            case SLMirIntKind_I8:   *outTypeKind = SLWasmType_SLICE_I8; return true;
            case SLMirIntKind_U16:  *outTypeKind = SLWasmType_SLICE_U16; return true;
            case SLMirIntKind_I16:  *outTypeKind = SLWasmType_SLICE_I16; return true;
            case SLMirIntKind_U32:  *outTypeKind = SLWasmType_SLICE_U32; return true;
            case SLMirIntKind_BOOL:
            case SLMirIntKind_I32:  *outTypeKind = SLWasmType_SLICE_I32; return true;
            default:                return false;
        }
    }
    if (SLMirTypeRefIsAggSliceView(typeRef)) {
        *outTypeKind = SLWasmType_SLICE_AGG;
        return true;
    }
    if (SLMirTypeRefIsAggregate(typeRef)) {
        *outTypeKind = SLWasmType_AGG_REF;
        return true;
    }
    if (SLMirTypeRefIsFuncRef(typeRef)) {
        *outTypeKind = SLWasmType_FUNC_REF;
        return true;
    }
    if (SLMirTypeRefIsOpaquePtr(typeRef)) {
        *outTypeKind = SLWasmType_OPAQUE_PTR;
        return true;
    }
    if (SLMirTypeRefIsI32Ptr(typeRef)) {
        *outTypeKind = SLWasmType_I32_PTR;
        return true;
    }
    if (SLMirTypeRefScalarKind(typeRef) == SLMirTypeScalar_I32) {
        *outTypeKind = SLWasmType_I32;
        return true;
    }
    return false;
}

static bool WasmTypeKindIsPointer(uint8_t typeKind) {
    return typeKind == SLWasmType_STR_PTR || typeKind == SLWasmType_U8_PTR
        || typeKind == SLWasmType_I32_PTR || typeKind == SLWasmType_I8_PTR
        || typeKind == SLWasmType_U16_PTR || typeKind == SLWasmType_I16_PTR
        || typeKind == SLWasmType_U32_PTR || typeKind == SLWasmType_OPAQUE_PTR
        || typeKind == SLWasmType_FUNC_REF_PTR;
}

static bool WasmTypeKindIsArrayView(uint8_t typeKind) {
    return typeKind == SLWasmType_ARRAY_VIEW_U8 || typeKind == SLWasmType_ARRAY_VIEW_I8
        || typeKind == SLWasmType_ARRAY_VIEW_U16 || typeKind == SLWasmType_ARRAY_VIEW_I16
        || typeKind == SLWasmType_ARRAY_VIEW_U32 || typeKind == SLWasmType_ARRAY_VIEW_I32;
}

static bool WasmTypeKindUsesSRet(uint8_t typeKind) {
    return typeKind == SLWasmType_AGG_REF || WasmTypeKindIsArrayView(typeKind);
}

static bool WasmTypeKindIsSlice(uint8_t typeKind) {
    return typeKind == SLWasmType_SLICE_U8 || typeKind == SLWasmType_SLICE_I8
        || typeKind == SLWasmType_SLICE_U16 || typeKind == SLWasmType_SLICE_I16
        || typeKind == SLWasmType_SLICE_U32 || typeKind == SLWasmType_SLICE_I32
        || typeKind == SLWasmType_SLICE_AGG;
}

static bool WasmTypeKindIsRawSingleSlot(uint8_t typeKind) {
    return typeKind == SLWasmType_I32 || WasmTypeKindIsPointer(typeKind)
        || WasmTypeKindIsArrayView(typeKind) || typeKind == SLWasmType_FUNC_REF;
}

static uint32_t WasmTypeKindElementSize(uint8_t typeKind) {
    switch (typeKind) {
        case SLWasmType_U8_PTR:
        case SLWasmType_I8_PTR:
        case SLWasmType_ARRAY_VIEW_U8:
        case SLWasmType_ARRAY_VIEW_I8:
        case SLWasmType_SLICE_U8:
        case SLWasmType_SLICE_I8:       return 1u;
        case SLWasmType_U16_PTR:
        case SLWasmType_I16_PTR:
        case SLWasmType_ARRAY_VIEW_U16:
        case SLWasmType_ARRAY_VIEW_I16:
        case SLWasmType_SLICE_U16:
        case SLWasmType_SLICE_I16:      return 2u;
        case SLWasmType_U32_PTR:
        case SLWasmType_I32_PTR:
        case SLWasmType_ARRAY_VIEW_U32:
        case SLWasmType_ARRAY_VIEW_I32:
        case SLWasmType_SLICE_U32:
        case SLWasmType_SLICE_I32:
        case SLWasmType_STR_PTR:        return 4u;
        default:                        return 0u;
    }
}

static uint32_t WasmScalarByteWidth(uint8_t intKind) {
    switch ((SLMirIntKind)intKind) {
        case SLMirIntKind_U8:
        case SLMirIntKind_I8:  return 1u;
        case SLMirIntKind_U16:
        case SLMirIntKind_I16: return 2u;
        default:               return 4u;
    }
}

static uint32_t WasmIntKindByteWidth(SLMirIntKind intKind) {
    switch (intKind) {
        case SLMirIntKind_U8:
        case SLMirIntKind_I8:  return 1u;
        case SLMirIntKind_U16:
        case SLMirIntKind_I16: return 2u;
        default:               return 4u;
    }
}

static uint8_t WasmLocalAddressTypeKind(const SLWasmEmitState* state, uint32_t localIndex) {
    uint8_t intKind;
    if (state == NULL || localIndex >= 256u) {
        return SLWasmType_VOID;
    }
    if (state->localKinds[localIndex] == SLWasmType_STR_REF) {
        return SLWasmType_STR_PTR;
    }
    if (state->localKinds[localIndex] != SLWasmType_I32) {
        return SLWasmType_VOID;
    }
    intKind = state->localIntKinds[localIndex];
    switch ((SLMirIntKind)intKind) {
        case SLMirIntKind_U8:  return SLWasmType_U8_PTR;
        case SLMirIntKind_I8:  return SLWasmType_I8_PTR;
        case SLMirIntKind_U16: return SLWasmType_U16_PTR;
        case SLMirIntKind_I16: return SLWasmType_I16_PTR;
        case SLMirIntKind_U32: return SLWasmType_U32_PTR;
        default:               return SLWasmType_I32_PTR;
    }
}

static uint32_t WasmAlign4(uint32_t n) {
    return (n + 3u) & ~3u;
}

static uint32_t WasmOpaquePtrPointeeTypeRef(const SLMirProgram* program, uint32_t typeRefIndex) {
    if (program == NULL || typeRefIndex >= program->typeLen) {
        return UINT32_MAX;
    }
    if (SLMirTypeRefIsAggregate(&program->types[typeRefIndex])) {
        return typeRefIndex;
    }
    return SLMirTypeRefOpaquePointeeTypeRef(&program->types[typeRefIndex]);
}

static uint32_t WasmAggSliceElemTypeRef(const SLMirProgram* program, uint32_t typeRefIndex) {
    if (program == NULL || typeRefIndex >= program->typeLen) {
        return UINT32_MAX;
    }
    return SLMirTypeRefAggSliceElemTypeRef(&program->types[typeRefIndex]);
}

static uint32_t WasmTypeElementSize(
    const SLMirProgram* program, uint8_t typeKind, uint32_t typeRefIndex) {
    uint32_t elemTypeRef;
    uint32_t elemSize;
    elemSize = WasmTypeKindElementSize(typeKind);
    if (elemSize != 0u || typeKind != SLWasmType_SLICE_AGG) {
        return elemSize;
    }
    elemTypeRef = WasmAggSliceElemTypeRef(program, typeRefIndex);
    if (elemTypeRef == UINT32_MAX || elemTypeRef >= program->typeLen
        || WasmAggregateHasDynamicLayout(program, elemTypeRef))
    {
        return 0u;
    }
    elemSize = (uint32_t)WasmTypeByteSize(program, elemTypeRef);
    return elemSize;
}

static uint32_t WasmAllocatedByteSizeForType(
    const SLMirProgram* program, uint8_t typeKind, uint32_t typeRefIndex) {
    uint32_t pointeeTypeRef;
    if (program == NULL || typeRefIndex == UINT32_MAX || typeRefIndex >= program->typeLen) {
        return 0u;
    }
    if (typeKind == SLWasmType_STR_PTR) {
        return 8u;
    }
    if (typeKind == SLWasmType_OPAQUE_PTR) {
        pointeeTypeRef = WasmOpaquePtrPointeeTypeRef(program, typeRefIndex);
        return pointeeTypeRef == UINT32_MAX
                 ? 0u
                 : (uint32_t)WasmTypeByteSize(program, pointeeTypeRef);
    }
    if (SLMirTypeRefIsFixedArray(&program->types[typeRefIndex])
        || SLMirTypeRefIsFixedArrayView(&program->types[typeRefIndex]))
    {
        return WasmIntKindByteWidth(SLMirTypeRefIntKind(&program->types[typeRefIndex]))
             * SLMirTypeRefFixedArrayCount(&program->types[typeRefIndex]);
    }
    switch (typeKind) {
        case SLWasmType_U8_PTR:
        case SLWasmType_I8_PTR:  return 1u;
        case SLWasmType_U16_PTR:
        case SLWasmType_I16_PTR: return 2u;
        case SLWasmType_U32_PTR:
        case SLWasmType_I32_PTR: return 4u;
        default:                 return 0u;
    }
}

static uint32_t WasmAllocatedByteAlignForType(
    const SLMirProgram* program, uint8_t typeKind, uint32_t typeRefIndex) {
    uint32_t pointeeTypeRef;
    if (program == NULL || typeRefIndex == UINT32_MAX || typeRefIndex >= program->typeLen) {
        return 0u;
    }
    if (typeKind == SLWasmType_STR_PTR) {
        return 4u;
    }
    if (typeKind == SLWasmType_OPAQUE_PTR) {
        pointeeTypeRef = WasmOpaquePtrPointeeTypeRef(program, typeRefIndex);
        return pointeeTypeRef == UINT32_MAX
                 ? 0u
                 : (uint32_t)WasmTypeByteAlign(program, pointeeTypeRef);
    }
    if (SLMirTypeRefIsFixedArray(&program->types[typeRefIndex])
        || SLMirTypeRefIsFixedArrayView(&program->types[typeRefIndex]))
    {
        return WasmIntKindByteWidth(SLMirTypeRefIntKind(&program->types[typeRefIndex]));
    }
    switch (typeKind) {
        case SLWasmType_U8_PTR:
        case SLWasmType_I8_PTR:  return 1u;
        case SLWasmType_U16_PTR:
        case SLWasmType_I16_PTR: return 2u;
        case SLWasmType_U32_PTR:
        case SLWasmType_I32_PTR: return 4u;
        default:                 return 0u;
    }
}

static uint32_t WasmFrameSlotSize(uint8_t typeKind) {
    switch (typeKind) {
        case SLWasmType_STR_REF:
        case SLWasmType_SLICE_U8:
        case SLWasmType_SLICE_I8:
        case SLWasmType_SLICE_U16:
        case SLWasmType_SLICE_I16:
        case SLWasmType_SLICE_U32:
        case SLWasmType_SLICE_I32:
        case SLWasmType_SLICE_AGG:      return 8u;
        case SLWasmType_I32:
        case SLWasmType_STR_PTR:
        case SLWasmType_U8_PTR:
        case SLWasmType_I32_PTR:
        case SLWasmType_I8_PTR:
        case SLWasmType_U16_PTR:
        case SLWasmType_I16_PTR:
        case SLWasmType_U32_PTR:
        case SLWasmType_OPAQUE_PTR:
        case SLWasmType_ARRAY_VIEW_U8:
        case SLWasmType_ARRAY_VIEW_I8:
        case SLWasmType_ARRAY_VIEW_U16:
        case SLWasmType_ARRAY_VIEW_I16:
        case SLWasmType_ARRAY_VIEW_U32:
        case SLWasmType_ARRAY_VIEW_I32:
        case SLWasmType_AGG_REF:
        case SLWasmType_FUNC_REF:
        case SLWasmType_FUNC_REF_PTR:   return 4u;
        default:                        return 0u;
    }
}

static int WasmAnalyzeImports(
    const SLCodegenUnit* unit, SLWasmImportLayout* imports, SLDiag* _Nullable diag) {
    uint32_t i;
    bool     allowWasmMin = WasmIsWasmMinPlatform(unit);
    if (imports == NULL) {
        return -1;
    }
    *imports = (SLWasmImportLayout){ 0 };
    if (unit == NULL || unit->mirProgram == NULL) {
        return -1;
    }
    imports->needsFrameGlobal = WasmProgramNeedsFrameMemory(unit->mirProgram) ? 1u : 0u;
    imports->needsHeapGlobal = WasmProgramNeedsHeapMemory(unit->mirProgram) ? 1u : 0u;
    imports->hasRootAllocThunk = WasmProgramNeedsRootAllocator(unit->mirProgram) ? 1u : 0u;
    imports->hasFunctionTable =
        (imports->hasRootAllocThunk || WasmProgramNeedsFunctionTable(unit->mirProgram)) ? 1u : 0u;
    imports->frameGlobalIndex = 0u;
    imports->heapGlobalIndex = imports->needsFrameGlobal ? 1u : 0u;
    for (i = 0; i < unit->mirProgram->hostLen; i++) {
        const SLMirHostRef* host = &unit->mirProgram->hosts[i];
        if (host->kind == SLMirHost_GENERIC && host->target == SLMirHostTarget_PLATFORM_EXIT
            && allowWasmMin)
        {
            imports->hasWasmMinExit = 1u;
            continue;
        }
        if (host->kind == SLMirHost_GENERIC && host->target == SLMirHostTarget_PLATFORM_CONSOLE_LOG
            && allowWasmMin)
        {
            imports->hasWasmMinConsoleLog = 1u;
            continue;
        }
        WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_HOSTCALL, host->nameStart, host->nameEnd);
        if (diag != NULL) {
            diag->detail = "host calls are not supported in this Wasm mode";
        }
        return -1;
    }
    if (imports->hasWasmMinExit) {
        imports->wasmMinExitFuncIndex = imports->importFuncCount++;
        imports->wasmMinExitTypeIndex = unit->mirProgram->funcLen + imports->wasmMinExitFuncIndex;
    }
    if (imports->hasWasmMinConsoleLog) {
        imports->wasmMinConsoleLogFuncIndex = imports->importFuncCount++;
        imports->wasmMinConsoleLogTypeIndex =
            unit->mirProgram->funcLen + imports->wasmMinConsoleLogFuncIndex;
    }
    if (allowWasmMin
        && (WasmProgramHasAssert(unit->mirProgram)
            || WasmProgramHasAllocNullPanic(unit->mirProgram)))
    {
        imports->hasWasmMinPanic = 1u;
        imports->wasmMinPanicFuncIndex = imports->importFuncCount++;
        imports->wasmMinPanicTypeIndex = unit->mirProgram->funcLen + imports->wasmMinPanicFuncIndex;
    }
    if (imports->hasFunctionTable) {
        imports->allocatorIndirectTypeIndex = unit->mirProgram->funcLen + imports->importFuncCount;
        imports->tableFuncCount =
            unit->mirProgram->funcLen + (imports->hasRootAllocThunk ? 1u : 0u);
        imports->rootAllocTableIndex = unit->mirProgram->funcLen;
        if (imports->hasRootAllocThunk) {
            imports->rootAllocFuncIndex = imports->importFuncCount + unit->mirProgram->funcLen;
        }
    }
    return 0;
}

static int WasmBuildStringLayout(
    const SLCodegenUnit*      unit,
    const SLWasmImportLayout* imports,
    const SLCodegenOptions*   options,
    SLWasmStringLayout* _Nonnull strings,
    SLDiag* _Nullable diag) {
    static const char panicMessage[] = "assertion failed";
    static const char allocPanicMessage[] = "unwrap: null value";
    uint32_t          i;
    uint32_t          allocSize = 0;
    if (unit == NULL || unit->mirProgram == NULL || options == NULL || strings == NULL) {
        return -1;
    }
    memset(strings, 0, sizeof(*strings));
    strings->data.options = options;
    if (unit->mirProgram->constLen != 0u) {
        strings->constRefs = (SLWasmStringRef*)options->arenaGrow(
            options->allocatorCtx,
            unit->mirProgram->constLen * (uint32_t)sizeof(SLWasmStringRef),
            &allocSize);
        if (strings->constRefs == NULL
            || allocSize < unit->mirProgram->constLen * sizeof(SLWasmStringRef))
        {
            if (strings->constRefs != NULL && options->arenaFree != NULL) {
                options->arenaFree(options->allocatorCtx, strings->constRefs, allocSize);
            }
            WasmSetDiag(diag, SLDiag_ARENA_OOM, 0, 0);
            return -1;
        }
        for (i = 0; i < unit->mirProgram->constLen; i++) {
            strings->constRefs[i] = (SLWasmStringRef){
                .objectOffset = UINT32_MAX, .dataOffset = UINT32_MAX, .len = 0u
            };
        }
    }
    for (i = 0; i < unit->mirProgram->constLen; i++) {
        const SLMirConst* c = &unit->mirProgram->consts[i];
        if (c->kind != SLMirConst_STRING) {
            continue;
        }
        strings->constRefs[i].objectOffset = strings->data.len;
        strings->constRefs[i].dataOffset = strings->data.len + 8u;
        strings->constRefs[i].len = c->bytes.len;
        if (WasmAppendU32LE(&strings->data, strings->constRefs[i].dataOffset) != 0
            || WasmAppendU32LE(&strings->data, c->bytes.len) != 0
            || WasmAppendBytes(&strings->data, c->bytes.ptr, c->bytes.len) != 0
            || WasmAppendByte(&strings->data, 0u) != 0)
        {
            WasmSetDiag(diag, SLDiag_ARENA_OOM, 0, 0);
            return -1;
        }
    }
    if (WasmProgramNeedsRootAllocator(unit->mirProgram)) {
        while (strings->data.len < 4u) {
            if (WasmAppendByte(&strings->data, 0u) != 0) {
                WasmSetDiag(diag, SLDiag_ARENA_OOM, 0, 0);
                return -1;
            }
        }
        strings->hasRootAllocator = 1u;
        strings->rootAllocatorOffset = strings->data.len;
        if (WasmAppendU32LE(&strings->data, imports != NULL ? imports->rootAllocTableIndex : 0u)
            != 0)
        {
            WasmSetDiag(diag, SLDiag_ARENA_OOM, 0, 0);
            return -1;
        }
    } else {
        strings->rootAllocatorOffset = UINT32_MAX;
    }
    if (imports != NULL && imports->hasWasmMinPanic) {
        strings->hasAssertPanicString = 1u;
        strings->assertPanic.objectOffset = UINT32_MAX;
        strings->assertPanic.dataOffset = strings->data.len;
        strings->assertPanic.len = (uint32_t)(sizeof(panicMessage) - 1u);
        if (WasmAppendBytes(&strings->data, panicMessage, strings->assertPanic.len) != 0
            || WasmAppendByte(&strings->data, 0u) != 0)
        {
            WasmSetDiag(diag, SLDiag_ARENA_OOM, 0, 0);
            return -1;
        }
        if (WasmProgramHasAllocNullPanic(unit->mirProgram)) {
            strings->hasAllocNullPanicString = 1u;
            strings->allocNullPanic.objectOffset = UINT32_MAX;
            strings->allocNullPanic.dataOffset = strings->data.len;
            strings->allocNullPanic.len = (uint32_t)(sizeof(allocPanicMessage) - 1u);
            if (WasmAppendBytes(&strings->data, allocPanicMessage, strings->allocNullPanic.len) != 0
                || WasmAppendByte(&strings->data, 0u) != 0)
            {
                WasmSetDiag(diag, SLDiag_ARENA_OOM, 0, 0);
                return -1;
            }
        }
    }
    return 0;
}

static uint32_t WasmStaticDataEnd(
    const SLWasmStringLayout* strings, const SLWasmEntryLayout* entry) {
    uint32_t end = 0u;
    if (strings != NULL) {
        end = WasmAlign4(strings->data.len);
    }
    if (entry != NULL && entry->hasWrapper) {
        uint32_t resultEnd = WasmAlign4(entry->resultOffset + entry->resultSize);
        if (resultEnd > end) {
            end = resultEnd;
        }
    }
    return end;
}

static int WasmPlanEntryLayout(
    const SLMirProgram*       program,
    const SLWasmFnSig*        sigs,
    const SLWasmImportLayout* imports,
    const SLWasmStringLayout* strings,
    SLWasmEntryLayout* _Nonnull entry,
    SLDiag* _Nullable diag) {
    uint32_t i;
    if (program == NULL || sigs == NULL || imports == NULL || strings == NULL || entry == NULL) {
        return -1;
    }
    *entry = (SLWasmEntryLayout){
        .mainFuncIndex = UINT32_MAX,
        .wrapperTypeIndex = UINT32_MAX,
        .wrapperFuncIndex = UINT32_MAX,
        .resultOffset = UINT32_MAX,
    };
    for (i = 0; i < program->funcLen; i++) {
        const SLMirFunction* fn = &program->funcs[i];
        const SLWasmFnSig*   sig = &sigs[i];
        if (!WasmFunctionIsNamedMain(program, fn)) {
            continue;
        }
        entry->mainFuncIndex = i;
        if (sig->logicalResultKind == SLWasmType_VOID || sig->logicalResultKind == SLWasmType_I32) {
            return 0;
        }
        entry->hasWrapper = 1u;
        entry->resultKind = sig->logicalResultKind;
        entry->usesSRet = sig->usesSRet;
        if (sig->usesSRet) {
            entry->resultSize = (uint32_t)WasmTypeByteSize(program, fn->typeRef);
        } else if (WasmTypeKindSlotCount(sig->logicalResultKind) == 2u) {
            entry->resultSize = 8u;
        } else if (WasmTypeKindSlotCount(sig->logicalResultKind) == 1u) {
            entry->resultSize = 4u;
        } else {
            WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, fn->nameStart, fn->nameEnd);
            if (diag != NULL) {
                diag->detail = "unsupported exported main return kind";
            }
            return -1;
        }
        entry->resultOffset = WasmAlign4(strings->data.len);
        entry->wrapperTypeIndex =
            program->funcLen + imports->importFuncCount + (imports->hasFunctionTable ? 1u : 0u);
        entry->wrapperFuncIndex =
            imports->importFuncCount + program->funcLen + (imports->hasRootAllocThunk ? 1u : 0u);
        return 0;
    }
    return 0;
}

static bool WasmFunctionNeedsFrameMemory(const SLMirProgram* program, const SLMirFunction* fn) {
    uint32_t i;
    if (program == NULL || fn == NULL) {
        return false;
    }
    for (i = 0; i < fn->localCount; i++) {
        uint8_t typeKind = 0;
        if (WasmTypeKindFromMirType(program, program->locals[fn->localStart + i].typeRef, &typeKind)
            && (typeKind == SLWasmType_STR_PTR || typeKind == SLWasmType_AGG_REF
                || WasmTypeKindIsPointer(typeKind) || WasmTypeKindIsArrayView(typeKind)
                || WasmTypeKindIsSlice(typeKind)))
        {
            return true;
        }
    }
    for (i = 0; i < fn->instLen; i++) {
        switch (program->insts[fn->instStart + i].op) {
            case SLMirOp_AGG_ZERO:
            case SLMirOp_AGG_SET:
            case SLMirOp_LOCAL_ADDR:
            case SLMirOp_ARRAY_ADDR:
            case SLMirOp_DEREF_LOAD:
            case SLMirOp_DEREF_STORE:
            case SLMirOp_SEQ_LEN:
            case SLMirOp_STR_CSTR:
            case SLMirOp_INDEX:
            case SLMirOp_AGG_GET:
            case SLMirOp_AGG_ADDR:
            case SLMirOp_ALLOC_NEW:
            case SLMirOp_SLICE_MAKE:  return true;
            default:                  break;
        }
    }
    return false;
}

static bool WasmProgramNeedsFrameMemory(const SLMirProgram* program) {
    uint32_t i;
    if (program == NULL) {
        return false;
    }
    for (i = 0; i < program->funcLen; i++) {
        if (WasmFunctionNeedsFrameMemory(program, &program->funcs[i])) {
            return true;
        }
    }
    return false;
}

static bool WasmProgramNeedsHeapMemory(const SLMirProgram* program) {
    uint32_t i;
    if (program == NULL) {
        return false;
    }
    for (i = 0; i < program->instLen; i++) {
        if (program->insts[i].op == SLMirOp_ALLOC_NEW) {
            return true;
        }
    }
    return false;
}

static bool WasmFunctionNeedsIndirectScratch(const SLMirProgram* program, const SLMirFunction* fn) {
    uint32_t pc;
    if (program == NULL || fn == NULL) {
        return false;
    }
    for (pc = 0; pc < fn->instLen; pc++) {
        if (program->insts[fn->instStart + pc].op == SLMirOp_CALL_INDIRECT) {
            return true;
        }
    }
    return false;
}

static bool WasmSigMatchesAllocatorIndirect(const SLWasmFnSig* sig) {
    if (sig == NULL || sig->wasmParamCount != 6u || sig->wasmResultCount != 1u
        || sig->wasmResultTypes[0] != 0x7fu)
    {
        return false;
    }
    return sig->wasmParamTypes[0] == 0x7fu && sig->wasmParamTypes[1] == 0x7fu
        && sig->wasmParamTypes[2] == 0x7fu && sig->wasmParamTypes[3] == 0x7fu
        && sig->wasmParamTypes[4] == 0x7fu && sig->wasmParamTypes[5] == 0x7fu;
}

static bool WasmSigSameShape(const SLWasmFnSig* a, const SLWasmFnSig* b) {
    if (a == NULL || b == NULL || a->logicalParamCount != b->logicalParamCount
        || a->wasmParamCount != b->wasmParamCount || a->logicalResultKind != b->logicalResultKind
        || a->wasmResultCount != b->wasmResultCount || a->usesSRet != b->usesSRet)
    {
        return false;
    }
    if (a->logicalParamCount != 0u
        && memcmp(a->logicalParamKinds, b->logicalParamKinds, a->logicalParamCount) != 0)
    {
        return false;
    }
    if (a->wasmParamCount != 0u
        && memcmp(a->wasmParamTypes, b->wasmParamTypes, a->wasmParamCount) != 0)
    {
        return false;
    }
    if (a->wasmResultCount != 0u
        && memcmp(a->wasmResultTypes, b->wasmResultTypes, a->wasmResultCount) != 0)
    {
        return false;
    }
    return true;
}

static uint32_t WasmFindFunctionValueTypeRef(const SLMirProgram* program, uint32_t functionIndex) {
    uint32_t i;
    if (program == NULL || functionIndex >= program->funcLen) {
        return UINT32_MAX;
    }
    for (i = 0; i < program->typeLen; i++) {
        if (SLMirTypeRefFuncRefFunctionIndex(&program->types[i]) == functionIndex) {
            return i;
        }
    }
    return UINT32_MAX;
}

static int WasmBuildFunctionSignatures(
    const SLMirProgram* program, SLWasmFnSig* sigs, SLDiag* _Nullable diag) {
    uint32_t i;
    if (program == NULL || sigs == NULL) {
        return -1;
    }
    for (i = 0; i < program->funcLen; i++) {
        const SLMirFunction* fn = &program->funcs[i];
        SLWasmFnSig*         sig = &sigs[i];
        uint32_t             j;
        memset(sig, 0, sizeof(*sig));
        sig->typeIndex = i;
        sig->logicalResultTypeRef = fn->typeRef;
        if (fn->paramCount > sizeof(sig->logicalParamKinds)) {
            WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, fn->nameStart, fn->nameEnd);
            if (diag != NULL) {
                diag->detail = "too many Wasm parameters";
            }
            return -1;
        }
        for (j = 0; j < fn->paramCount; j++) {
            const SLMirLocal* local;
            uint8_t           paramType = 0;
            if (fn->localStart + j >= program->localLen) {
                WasmSetDiag(diag, SLDiag_WASM_BACKEND_INTERNAL, fn->nameStart, fn->nameEnd);
                return -1;
            }
            local = &program->locals[fn->localStart + j];
            if (!WasmTypeKindFromMirType(program, local->typeRef, &paramType)
                || !WasmTypeKindIsSupported(paramType) || paramType == SLWasmType_AGG_REF)
            {
                WasmSetDiag(
                    diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, local->nameStart, local->nameEnd);
                if (diag != NULL) {
                    diag->detail = "only scalar, pointer-like, array-view, slice, and &str "
                                   "parameters are supported";
                }
                return -1;
            }
            if (sig->wasmParamCount + WasmTypeKindSlotCount(paramType)
                > sizeof(sig->wasmParamTypes))
            {
                WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, fn->nameStart, fn->nameEnd);
                if (diag != NULL) {
                    diag->detail = "too many Wasm parameters";
                }
                return -1;
            }
            sig->logicalParamKinds[j] = paramType;
            if (WasmTypeKindSlotCount(paramType) == 2u) {
                sig->wasmParamTypes[sig->wasmParamCount++] = 0x7fu;
                sig->wasmParamTypes[sig->wasmParamCount++] = 0x7fu;
            } else {
                sig->wasmParamTypes[sig->wasmParamCount++] = 0x7fu;
            }
        }
        sig->logicalParamCount = (uint8_t)fn->paramCount;
        if (!WasmTypeKindFromMirType(program, fn->typeRef, &sig->logicalResultKind)) {
            WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, fn->nameStart, fn->nameEnd);
            if (diag != NULL) {
                diag->detail =
                    "only void, scalar, pointer-like, slice, and &str returns are supported";
            }
            return -1;
        }
        sig->usesSRet = WasmTypeKindUsesSRet(sig->logicalResultKind) ? 1u : 0u;
        if (sig->usesSRet) {
            if (sig->wasmParamCount + 1u > sizeof(sig->wasmParamTypes)) {
                WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, fn->nameStart, fn->nameEnd);
                if (diag != NULL) {
                    diag->detail = "too many Wasm parameters";
                }
                return -1;
            }
            sig->wasmParamTypes[sig->wasmParamCount++] = 0x7fu;
            sig->wasmResultCount = 0u;
        } else {
            sig->wasmResultCount = WasmTypeKindSlotCount(sig->logicalResultKind);
        }
        if (sig->wasmResultCount == 2u) {
            sig->wasmResultTypes[0] = 0x7fu;
            sig->wasmResultTypes[1] = 0x7fu;
        } else if (sig->logicalResultKind != SLWasmType_VOID) {
            sig->wasmResultTypes[0] = 0x7fu;
        }
        for (j = 0; j < i; j++) {
            if (WasmSigSameShape(sig, &sigs[j])) {
                sig->typeIndex = sigs[j].typeIndex;
                break;
            }
        }
    }
    return 0;
}

static int WasmStackPushEx(SLWasmEmitState* state, uint8_t typeKind, uint32_t typeRef) {
    if (state == NULL || state->stackLen >= sizeof(state->stackKinds)) {
        return -1;
    }
    state->stackKinds[state->stackLen++] = typeKind;
    state->stackTypeRefs[state->stackLen - 1u] = typeRef;
    return 0;
}

static int WasmStackPopEx(
    SLWasmEmitState* state, uint8_t* _Nullable outTypeKind, uint32_t* _Nullable outTypeRef) {
    if (state == NULL || state->stackLen == 0) {
        return -1;
    }
    state->stackLen--;
    if (outTypeKind != NULL) {
        *outTypeKind = state->stackKinds[state->stackLen];
    }
    if (outTypeRef != NULL) {
        *outTypeRef = state->stackTypeRefs[state->stackLen];
    }
    return 0;
}

static int WasmStackPush(SLWasmEmitState* state, uint8_t typeKind) {
    return WasmStackPushEx(state, typeKind, UINT32_MAX);
}

static int WasmStackPop(SLWasmEmitState* state, uint8_t* outTypeKind) {
    return WasmStackPopEx(state, outTypeKind, NULL);
}

static int WasmPrepareFunctionState(
    const SLMirProgram*  program,
    const SLMirFunction* fn,
    SLWasmEmitState*     state,
    SLDiag* _Nullable diag) {
    uint32_t i;
    uint16_t nextValueIndex = 0;
    uint8_t  resultKind = SLWasmType_VOID;
    if (program == NULL || fn == NULL || state == NULL) {
        return -1;
    }
    memset(state, 0, sizeof(*state));
    state->allocCallTempOffset = UINT32_MAX;
    if (fn->localCount > sizeof(state->localKinds)) {
        WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, fn->nameStart, fn->nameEnd);
        if (diag != NULL) {
            diag->detail = "too many locals";
        }
        return -1;
    }
    state->usesFrame = WasmFunctionNeedsFrameMemory(program, fn) ? 1u : 0u;
    for (i = 0; i < fn->localCount; i++) {
        const SLMirLocal* local = &program->locals[fn->localStart + i];
        uint8_t           typeKind = 0;
        if (!WasmTypeKindFromMirType(program, local->typeRef, &typeKind)
            || !WasmTypeKindIsSupported(typeKind))
        {
            WasmSetDiag(
                diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, local->nameStart, local->nameEnd);
            if (diag != NULL) {
                diag->detail =
                    "only scalar, pointer-like, array-view, slice, aggregate, and &str locals "
                    "are supported";
            }
            return -1;
        }
        state->localKinds[i] = typeKind;
        state->localTypeRefs[i] = local->typeRef;
        state->localStorage[i] =
            (local->typeRef < program->typeLen
             && SLMirTypeRefIsFixedArray(&program->types[local->typeRef]))
                ? SLWasmLocalStorage_ARRAY
            : (local->typeRef < program->typeLen
               && SLMirTypeRefIsAggregate(&program->types[local->typeRef]))
                ? SLWasmLocalStorage_AGG
                : SLWasmLocalStorage_PLAIN;
        state->localIntKinds[i] =
            (uint8_t)((local->typeRef < program->typeLen)
                          ? SLMirTypeRefIntKind(&program->types[local->typeRef])
                          : SLMirIntKind_NONE);
        state->arrayCounts[i] =
            (local->typeRef < program->typeLen)
                ? SLMirTypeRefFixedArrayCount(&program->types[local->typeRef])
                : 0u;
        if (i < fn->paramCount) {
            state->wasmValueIndex[i] = nextValueIndex;
            nextValueIndex = (uint16_t)(nextValueIndex + WasmTypeKindSlotCount(typeKind));
        } else if (!state->usesFrame) {
            state->wasmValueIndex[i] = nextValueIndex;
            nextValueIndex = (uint16_t)(nextValueIndex + WasmTypeKindSlotCount(typeKind));
        }
        state->frameOffsets[i] = state->usesFrame ? WasmAlign4(state->frameSize) : UINT32_MAX;
        state->auxOffsets[i] = UINT32_MAX;
        if (state->usesFrame) {
            uint32_t slotSize = WasmFrameSlotSize(typeKind);
            if (state->localStorage[i] == SLWasmLocalStorage_ARRAY) {
                slotSize = WasmTypeKindElementSize(typeKind) * state->arrayCounts[i];
            } else if (state->localStorage[i] == SLWasmLocalStorage_AGG) {
                slotSize = (uint32_t)WasmTypeByteSize(program, local->typeRef);
            } else if (typeKind == SLWasmType_I32) {
                slotSize = WasmScalarByteWidth(state->localIntKinds[i]);
            }
            if (slotSize == 0u) {
                WasmSetDiag(
                    diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, local->nameStart, local->nameEnd);
                if (diag != NULL) {
                    diag->detail = "unsupported local layout";
                }
                return -1;
            }
            state->frameSize = state->frameOffsets[i] + slotSize;
        }
        if (state->usesFrame && typeKind == SLWasmType_STR_PTR) {
            state->auxOffsets[i] = WasmAlign4(state->frameSize);
            state->frameSize = state->auxOffsets[i] + 8u;
        }
        if (i + 1u == fn->paramCount) {
            state->wasmParamValueCount = nextValueIndex;
        }
    }
    if (fn->paramCount == 0u) {
        state->wasmParamValueCount = 0u;
    }
    if (state->usesFrame) {
        uint32_t tempPc;
        state->frameSize = WasmAlign4(state->frameSize);
        state->tempFrameStart = state->frameSize;
        for (tempPc = 0; tempPc < fn->instLen; tempPc++) {
            const SLMirInst* inst = &program->insts[fn->instStart + tempPc];
            int              typeRef = WasmTempTypeRefForInst(program, inst);
            if (typeRef < 0) {
                continue;
            }
            state->frameSize = WasmAlign4(state->frameSize);
            state->frameSize += (uint32_t)WasmTypeByteSize(program, (uint32_t)typeRef);
        }
        for (tempPc = 0; tempPc < fn->instLen; tempPc++) {
            const SLMirInst* inst = &program->insts[fn->instStart + tempPc];
            if (inst->op == SLMirOp_ALLOC_NEW && (inst->tok & SLAstFlag_NEW_HAS_ALLOC) != 0u) {
                state->frameSize = WasmAlign4(state->frameSize);
                state->allocCallTempOffset = state->frameSize;
                state->frameSize += 4u;
                break;
            }
        }
        state->frameSize = WasmAlign4(state->frameSize);
        if (!WasmTypeKindFromMirType(program, fn->typeRef, &resultKind)) {
            WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, fn->nameStart, fn->nameEnd);
            return -1;
        }
        state->hiddenLocalStart =
            (uint16_t)(state->wasmParamValueCount + (WasmTypeKindUsesSRet(resultKind) ? 1u : 0u));
        state->frameBaseLocal = state->hiddenLocalStart;
        state->scratch0Local = (uint16_t)(state->hiddenLocalStart + 1u);
        state->scratch1Local = (uint16_t)(state->hiddenLocalStart + 2u);
        state->scratch2Local = (uint16_t)(state->hiddenLocalStart + 3u);
        state->scratch3Local = (uint16_t)(state->hiddenLocalStart + 4u);
        state->scratch4Local = (uint16_t)(state->hiddenLocalStart + 5u);
        state->scratch5Local = (uint16_t)(state->hiddenLocalStart + 6u);
        state->scratch6Local = (uint16_t)(state->hiddenLocalStart + 7u);
        state->wasmLocalValueCount = (uint16_t)(state->wasmParamValueCount + 8u);
        (void)resultKind;
    } else if (WasmFunctionNeedsIndirectScratch(program, fn)) {
        state->hiddenLocalStart = nextValueIndex;
        state->frameBaseLocal = state->hiddenLocalStart;
        state->scratch0Local = state->hiddenLocalStart;
        state->scratch1Local = (uint16_t)(state->hiddenLocalStart + 1u);
        state->scratch2Local = (uint16_t)(state->hiddenLocalStart + 2u);
        state->scratch3Local = (uint16_t)(state->hiddenLocalStart + 3u);
        state->scratch4Local = (uint16_t)(state->hiddenLocalStart + 4u);
        state->scratch5Local = (uint16_t)(state->hiddenLocalStart + 5u);
        state->scratch6Local = (uint16_t)(state->hiddenLocalStart + 6u);
        state->wasmLocalValueCount = (uint16_t)(nextValueIndex + 7u);
    } else {
        state->wasmLocalValueCount = nextValueIndex;
    }
    return 0;
}

static int WasmRequireI32Value(
    uint8_t valueTypeKind,
    SLDiag* _Nullable diag,
    uint32_t start,
    uint32_t end,
    const char* _Nonnull detail) {
    if (valueTypeKind == SLWasmType_I32) {
        return 0;
    }
    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, start, end);
    if (diag != NULL) {
        diag->detail = detail;
    }
    return -1;
}

static int WasmRequireAllocatorValue(
    uint8_t valueTypeKind,
    SLDiag* _Nullable diag,
    uint32_t start,
    uint32_t end,
    const char* _Nonnull detail) {
    if (valueTypeKind == SLWasmType_I32 || WasmTypeKindIsPointer(valueTypeKind)) {
        return 0;
    }
    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, start, end);
    if (diag != NULL) {
        diag->detail = detail;
    }
    return -1;
}

static int WasmRequirePointerValue(
    uint8_t valueTypeKind,
    SLDiag* _Nullable diag,
    uint32_t start,
    uint32_t end,
    const char* _Nonnull detail) {
    if (WasmTypeKindIsPointer(valueTypeKind)) {
        return 0;
    }
    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, start, end);
    if (diag != NULL) {
        diag->detail = detail;
    }
    return -1;
}

static int WasmAppendMemArg(SLWasmBuf* body, uint32_t alignLog2, uint32_t offset) {
    return WasmAppendULEB(body, alignLog2) != 0 || WasmAppendULEB(body, offset) != 0 ? -1 : 0;
}

static int WasmEmitI32Load(SLWasmBuf* body) {
    return WasmAppendByte(body, 0x28u) != 0 || WasmAppendMemArg(body, 2u, 0u) != 0 ? -1 : 0;
}

static int WasmEmitI32Store(SLWasmBuf* body) {
    return WasmAppendByte(body, 0x36u) != 0 || WasmAppendMemArg(body, 2u, 0u) != 0 ? -1 : 0;
}

static int WasmEmitU8Load(SLWasmBuf* body) {
    return WasmAppendByte(body, 0x2du) != 0 || WasmAppendMemArg(body, 0u, 0u) != 0 ? -1 : 0;
}

static int WasmEmitI8Load(SLWasmBuf* body) {
    return WasmAppendByte(body, 0x2cu) != 0 || WasmAppendMemArg(body, 0u, 0u) != 0 ? -1 : 0;
}

static int WasmEmitU16Load(SLWasmBuf* body) {
    return WasmAppendByte(body, 0x2fu) != 0 || WasmAppendMemArg(body, 1u, 0u) != 0 ? -1 : 0;
}

static int WasmEmitI16Load(SLWasmBuf* body) {
    return WasmAppendByte(body, 0x2eu) != 0 || WasmAppendMemArg(body, 1u, 0u) != 0 ? -1 : 0;
}

static int WasmEmitU8Store(SLWasmBuf* body) {
    return WasmAppendByte(body, 0x3au) != 0 || WasmAppendMemArg(body, 0u, 0u) != 0 ? -1 : 0;
}

static int WasmEmitU16Store(SLWasmBuf* body) {
    return WasmAppendByte(body, 0x3bu) != 0 || WasmAppendMemArg(body, 1u, 0u) != 0 ? -1 : 0;
}

static int WasmEmitTypedLoad(SLWasmBuf* body, uint8_t typeKind) {
    switch (typeKind) {
        case SLWasmType_U8_PTR:
        case SLWasmType_ARRAY_VIEW_U8:
        case SLWasmType_SLICE_U8:       return WasmEmitU8Load(body);
        case SLWasmType_I8_PTR:
        case SLWasmType_ARRAY_VIEW_I8:
        case SLWasmType_SLICE_I8:       return WasmEmitI8Load(body);
        case SLWasmType_U16_PTR:
        case SLWasmType_ARRAY_VIEW_U16:
        case SLWasmType_SLICE_U16:      return WasmEmitU16Load(body);
        case SLWasmType_I16_PTR:
        case SLWasmType_ARRAY_VIEW_I16:
        case SLWasmType_SLICE_I16:      return WasmEmitI16Load(body);
        case SLWasmType_U32_PTR:
        case SLWasmType_I32_PTR:
        case SLWasmType_ARRAY_VIEW_U32:
        case SLWasmType_ARRAY_VIEW_I32:
        case SLWasmType_SLICE_U32:
        case SLWasmType_SLICE_I32:
        case SLWasmType_FUNC_REF_PTR:   return WasmEmitI32Load(body);
        default:                        return -1;
    }
}

static int WasmEmitTypedStore(SLWasmBuf* body, uint8_t typeKind) {
    switch (typeKind) {
        case SLWasmType_U8_PTR:
        case SLWasmType_I8_PTR:
        case SLWasmType_ARRAY_VIEW_U8:
        case SLWasmType_ARRAY_VIEW_I8:  return WasmEmitU8Store(body);
        case SLWasmType_U16_PTR:
        case SLWasmType_I16_PTR:
        case SLWasmType_ARRAY_VIEW_U16:
        case SLWasmType_ARRAY_VIEW_I16: return WasmEmitU16Store(body);
        case SLWasmType_U32_PTR:
        case SLWasmType_I32_PTR:
        case SLWasmType_ARRAY_VIEW_U32:
        case SLWasmType_ARRAY_VIEW_I32:
        case SLWasmType_FUNC_REF_PTR:   return WasmEmitI32Store(body);
        default:                        return -1;
    }
}

static int WasmEmitScaleIndex(SLWasmBuf* body, uint32_t elemSize) {
    if (body == NULL) {
        return -1;
    }
    if (elemSize <= 1u) {
        return 0;
    }
    if (WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, (int32_t)elemSize) != 0
        || WasmAppendByte(body, 0x6cu) != 0)
    {
        return -1;
    }
    return 0;
}

static int WasmEmitZeroFrameRange(
    SLWasmBuf* body, const SLWasmEmitState* state, uint32_t offset, uint32_t size) {
    uint32_t i = 0;
    if (body == NULL || state == NULL) {
        return -1;
    }
    while (i + 4u <= size) {
        if (WasmEmitAddrFromFrame(body, state, offset + i, 0u) != 0
            || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0
            || WasmEmitI32Store(body) != 0)
        {
            return -1;
        }
        i += 4u;
    }
    while (i < size) {
        if (WasmEmitAddrFromFrame(body, state, offset + i, 0u) != 0
            || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0
            || WasmEmitU8Store(body) != 0)
        {
            return -1;
        }
        i++;
    }
    return 0;
}

static int WasmEmitCopyLocalAddrToFrame(
    SLWasmBuf*             body,
    const SLWasmEmitState* state,
    uint16_t               srcLocal,
    uint32_t               dstOffset,
    uint32_t               size) {
    uint32_t i = 0;
    if (body == NULL || state == NULL) {
        return -1;
    }
    while (i + 4u <= size) {
        if (WasmEmitAddrFromFrame(body, state, dstOffset + i, 0u) != 0
            || WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, srcLocal) != 0
            || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, (int32_t)i) != 0
            || WasmAppendByte(body, 0x6au) != 0 || WasmEmitI32Load(body) != 0
            || WasmEmitI32Store(body) != 0)
        {
            return -1;
        }
        i += 4u;
    }
    if (i + 2u <= size) {
        if (WasmEmitAddrFromFrame(body, state, dstOffset + i, 0u) != 0
            || WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, srcLocal) != 0
            || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, (int32_t)i) != 0
            || WasmAppendByte(body, 0x6au) != 0 || WasmEmitU16Load(body) != 0
            || WasmEmitU16Store(body) != 0)
        {
            return -1;
        }
        i += 2u;
    }
    while (i < size) {
        if (WasmEmitAddrFromFrame(body, state, dstOffset + i, 0u) != 0
            || WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, srcLocal) != 0
            || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, (int32_t)i) != 0
            || WasmAppendByte(body, 0x6au) != 0 || WasmEmitU8Load(body) != 0
            || WasmEmitU8Store(body) != 0)
        {
            return -1;
        }
        i++;
    }
    return 0;
}

static int WasmEmitCopyLocalAddrToLocalAddr(
    SLWasmBuf* body, uint16_t srcLocal, uint16_t dstLocal, uint32_t dstAddend, uint32_t size) {
    uint32_t i = 0;
    if (body == NULL) {
        return -1;
    }
    while (i + 4u <= size) {
        if (WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, dstLocal) != 0
            || WasmAppendByte(body, 0x41u) != 0
            || WasmAppendSLEB32(body, (int32_t)(dstAddend + i)) != 0
            || WasmAppendByte(body, 0x6au) != 0 || WasmAppendByte(body, 0x20u) != 0
            || WasmAppendULEB(body, srcLocal) != 0 || WasmAppendByte(body, 0x41u) != 0
            || WasmAppendSLEB32(body, (int32_t)i) != 0 || WasmAppendByte(body, 0x6au) != 0
            || WasmEmitI32Load(body) != 0 || WasmEmitI32Store(body) != 0)
        {
            return -1;
        }
        i += 4u;
    }
    if (i + 2u <= size) {
        if (WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, dstLocal) != 0
            || WasmAppendByte(body, 0x41u) != 0
            || WasmAppendSLEB32(body, (int32_t)(dstAddend + i)) != 0
            || WasmAppendByte(body, 0x6au) != 0 || WasmAppendByte(body, 0x20u) != 0
            || WasmAppendULEB(body, srcLocal) != 0 || WasmAppendByte(body, 0x41u) != 0
            || WasmAppendSLEB32(body, (int32_t)i) != 0 || WasmAppendByte(body, 0x6au) != 0
            || WasmEmitU16Load(body) != 0 || WasmEmitU16Store(body) != 0)
        {
            return -1;
        }
        i += 2u;
    }
    while (i < size) {
        if (WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, dstLocal) != 0
            || WasmAppendByte(body, 0x41u) != 0
            || WasmAppendSLEB32(body, (int32_t)(dstAddend + i)) != 0
            || WasmAppendByte(body, 0x6au) != 0 || WasmAppendByte(body, 0x20u) != 0
            || WasmAppendULEB(body, srcLocal) != 0 || WasmAppendByte(body, 0x41u) != 0
            || WasmAppendSLEB32(body, (int32_t)i) != 0 || WasmAppendByte(body, 0x6au) != 0
            || WasmEmitU8Load(body) != 0 || WasmEmitU8Store(body) != 0)
        {
            return -1;
        }
        i++;
    }
    return 0;
}

static int WasmEmitZeroLocalAddrRange(SLWasmBuf* body, uint16_t dstLocal, uint32_t size) {
    uint32_t i = 0;
    if (body == NULL) {
        return -1;
    }
    while (i + 4u <= size) {
        if (WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, dstLocal) != 0
            || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, (int32_t)i) != 0
            || WasmAppendByte(body, 0x6au) != 0 || WasmAppendByte(body, 0x41u) != 0
            || WasmAppendSLEB32(body, 0) != 0 || WasmEmitI32Store(body) != 0)
        {
            return -1;
        }
        i += 4u;
    }
    if (i + 2u <= size) {
        if (WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, dstLocal) != 0
            || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, (int32_t)i) != 0
            || WasmAppendByte(body, 0x6au) != 0 || WasmAppendByte(body, 0x41u) != 0
            || WasmAppendSLEB32(body, 0) != 0 || WasmEmitU16Store(body) != 0)
        {
            return -1;
        }
        i += 2u;
    }
    while (i < size) {
        if (WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, dstLocal) != 0
            || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, (int32_t)i) != 0
            || WasmAppendByte(body, 0x6au) != 0 || WasmAppendByte(body, 0x41u) != 0
            || WasmAppendSLEB32(body, 0) != 0 || WasmEmitU8Store(body) != 0)
        {
            return -1;
        }
        i++;
    }
    return 0;
}

static int WasmEmitZeroDynamicLocalAddrRange(
    SLWasmBuf* body, uint16_t dstLocal, uint16_t sizeLocal, uint16_t cursorLocal) {
    if (body == NULL) {
        return -1;
    }
    if (WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, dstLocal) != 0
        || WasmAppendByte(body, 0x21u) != 0 || WasmAppendULEB(body, cursorLocal) != 0
        || WasmAppendByte(body, 0x02u) != 0 || WasmAppendByte(body, 0x40u) != 0
        || WasmAppendByte(body, 0x03u) != 0 || WasmAppendByte(body, 0x40u) != 0
        || WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, cursorLocal) != 0
        || WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, dstLocal) != 0
        || WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, sizeLocal) != 0
        || WasmAppendByte(body, 0x6au) != 0 || WasmAppendByte(body, 0x4fu) != 0
        || WasmAppendByte(body, 0x0du) != 0 || WasmAppendULEB(body, 1u) != 0
        || WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, cursorLocal) != 0
        || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0
        || WasmEmitU8Store(body) != 0 || WasmAppendByte(body, 0x20u) != 0
        || WasmAppendULEB(body, cursorLocal) != 0 || WasmAppendByte(body, 0x41u) != 0
        || WasmAppendSLEB32(body, 1) != 0 || WasmAppendByte(body, 0x6au) != 0
        || WasmAppendByte(body, 0x21u) != 0 || WasmAppendULEB(body, cursorLocal) != 0
        || WasmAppendByte(body, 0x0cu) != 0 || WasmAppendULEB(body, 0u) != 0
        || WasmAppendByte(body, 0x0bu) != 0 || WasmAppendByte(body, 0x0bu) != 0)
    {
        return -1;
    }
    return 0;
}

static uint32_t WasmAggregateOwnerTypeRef(
    const SLMirProgram* program, uint8_t baseType, uint32_t baseTypeRef) {
    if (program == NULL || baseTypeRef == UINT32_MAX) {
        return UINT32_MAX;
    }
    if (baseType == SLWasmType_AGG_REF) {
        return baseTypeRef;
    }
    if (baseType == SLWasmType_OPAQUE_PTR) {
        return WasmOpaquePtrPointeeTypeRef(program, baseTypeRef);
    }
    return UINT32_MAX;
}

static int WasmSourceSliceEq(
    const SLMirProgram* program,
    uint32_t            sourceRefA,
    uint32_t            startA,
    uint32_t            endA,
    uint32_t            sourceRefB,
    uint32_t            startB,
    uint32_t            endB) {
    uint32_t len;
    if (program == NULL || sourceRefA >= program->sourceLen || sourceRefB >= program->sourceLen
        || endA < startA || endB < startB)
    {
        return 0;
    }
    len = endA - startA;
    if (len != endB - startB || endA > program->sources[sourceRefA].src.len
        || endB > program->sources[sourceRefB].src.len)
    {
        return 0;
    }
    return memcmp(
               program->sources[sourceRefA].src.ptr + startA,
               program->sources[sourceRefB].src.ptr + startB,
               len)
        == 0;
}

static bool WasmFieldNameEq(
    const SLMirProgram* program, const SLMirField* fieldRef, const char* name) {
    size_t nameLen = 0u;
    if (program == NULL || fieldRef == NULL || name == NULL
        || fieldRef->sourceRef >= program->sourceLen || fieldRef->nameEnd < fieldRef->nameStart)
    {
        return false;
    }
    while (name[nameLen] != '\0') {
        nameLen++;
    }
    return (size_t)(fieldRef->nameEnd - fieldRef->nameStart) == nameLen
        && memcmp(
               program->sources[fieldRef->sourceRef].src.ptr + fieldRef->nameStart, name, nameLen)
               == 0;
}

static bool WasmFieldIsEmbeddedAllocator(const SLMirProgram* program, const SLMirField* fieldRef) {
    return WasmFieldNameEq(program, fieldRef, "Allocator");
}

static int WasmTypeByteAlign(const SLMirProgram* program, uint32_t typeRefIndex);

static int WasmTypeByteSize(const SLMirProgram* program, uint32_t typeRefIndex) {
    const SLMirTypeRef* typeRef;
    uint32_t            i;
    uint32_t            offset = 0u;
    uint32_t            maxAlign = 1u;
    if (program == NULL || typeRefIndex == UINT32_MAX || typeRefIndex >= program->typeLen) {
        return -1;
    }
    typeRef = &program->types[typeRefIndex];
    if (SLMirTypeRefIsAggregate(typeRef)) {
        for (i = 0; i < program->fieldLen; i++) {
            int fieldSize;
            int fieldAlign;
            if (program->fields[i].ownerTypeRef != typeRefIndex) {
                continue;
            }
            fieldSize = WasmTypeByteSize(program, program->fields[i].typeRef);
            fieldAlign = WasmTypeByteAlign(program, program->fields[i].typeRef);
            if ((fieldSize <= 0 || fieldAlign <= 0)
                && WasmFieldIsEmbeddedAllocator(program, &program->fields[i]))
            {
                fieldSize = 4;
                fieldAlign = 4;
            }
            if (fieldSize < 0 || fieldAlign <= 0) {
                return -1;
            }
            if ((uint32_t)fieldAlign > maxAlign) {
                maxAlign = (uint32_t)fieldAlign;
            }
            offset = (offset + ((uint32_t)fieldAlign - 1u)) & ~((uint32_t)fieldAlign - 1u);
            offset += (uint32_t)fieldSize;
        }
        return (int)(maxAlign > 1u ? ((offset + (maxAlign - 1u)) & ~(maxAlign - 1u)) : offset);
    }
    if (SLMirTypeRefIsStrObj(typeRef)) {
        return 8;
    }
    if (SLMirTypeRefIsFixedArray(typeRef)) {
        return (int)(WasmIntKindByteWidth(SLMirTypeRefIntKind(typeRef))
                     * SLMirTypeRefFixedArrayCount(typeRef));
    }
    if (SLMirTypeRefIsVArrayView(typeRef)) {
        return 0;
    }
    if (SLMirTypeRefIsStrRef(typeRef) || SLMirTypeRefIsSliceView(typeRef)
        || SLMirTypeRefIsAggSliceView(typeRef))
    {
        return 8;
    }
    if (SLMirTypeRefIsStrPtr(typeRef) || SLMirTypeRefIsOpaquePtr(typeRef)
        || SLMirTypeRefIsFixedArrayView(typeRef) || SLMirTypeRefIsU8Ptr(typeRef)
        || SLMirTypeRefIsI8Ptr(typeRef) || SLMirTypeRefIsU16Ptr(typeRef)
        || SLMirTypeRefIsI16Ptr(typeRef) || SLMirTypeRefIsU32Ptr(typeRef)
        || SLMirTypeRefIsI32Ptr(typeRef) || SLMirTypeRefIsFuncRef(typeRef))
    {
        return 4;
    }
    if (SLMirTypeRefScalarKind(typeRef) == SLMirTypeScalar_I32) {
        return (int)WasmScalarByteWidth((uint8_t)SLMirTypeRefIntKind(typeRef));
    }
    return -1;
}

static int WasmTypeByteAlign(const SLMirProgram* program, uint32_t typeRefIndex) {
    const SLMirTypeRef* typeRef;
    int                 size = WasmTypeByteSize(program, typeRefIndex);
    if (program == NULL || typeRefIndex == UINT32_MAX || typeRefIndex >= program->typeLen) {
        return -1;
    }
    typeRef = &program->types[typeRefIndex];
    if (SLMirTypeRefIsVArrayView(typeRef)) {
        uint32_t elemSize = WasmIntKindByteWidth(SLMirTypeRefIntKind(typeRef));
        return elemSize >= 4u ? 4 : (int)elemSize;
    }
    if (size <= 0) {
        return -1;
    }
    return size >= 4 ? 4 : size;
}

static bool WasmAggregateHasDynamicLayout(const SLMirProgram* program, uint32_t ownerTypeRef) {
    uint32_t i;
    if (program == NULL || ownerTypeRef == UINT32_MAX) {
        return false;
    }
    for (i = 0; i < program->fieldLen; i++) {
        if (program->fields[i].ownerTypeRef != ownerTypeRef
            || program->fields[i].typeRef >= program->typeLen)
        {
            continue;
        }
        if (SLMirTypeRefIsStrObj(&program->types[program->fields[i].typeRef])
            || SLMirTypeRefIsVArrayView(&program->types[program->fields[i].typeRef]))
        {
            return true;
        }
    }
    return false;
}

static int WasmEmitAggregateFieldAddress(
    SLWasmBuf*             body,
    const SLWasmEmitState* state,
    const SLMirProgram*    program,
    uint32_t               ownerTypeRef,
    uint32_t               fieldIndex,
    uint32_t               fieldOffset) {
    uint32_t i;
    if (body == NULL || state == NULL || program == NULL || fieldIndex >= program->fieldLen) {
        return -1;
    }
    if (!WasmAggregateHasDynamicLayout(program, ownerTypeRef)) {
        return WasmAppendByte(body, 0x20u) == 0 && WasmAppendULEB(body, state->scratch0Local) == 0
                    && WasmAppendByte(body, 0x41u) == 0
                    && WasmAppendSLEB32(body, (int32_t)fieldOffset) == 0
                    && WasmAppendByte(body, 0x6au) == 0
                 ? 0
                 : -1;
    }
    if (WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0
        || WasmAppendByte(body, 0x21u) != 0 || WasmAppendULEB(body, state->scratch1Local) != 0)
    {
        return -1;
    }
    for (i = 0; i < program->fieldLen; i++) {
        const SLMirField*   fieldRef;
        const SLMirTypeRef* fieldType;
        int                 fieldSize;
        int                 fieldAlign;
        if (program->fields[i].ownerTypeRef != ownerTypeRef
            || program->fields[i].typeRef >= program->typeLen)
        {
            continue;
        }
        fieldRef = &program->fields[i];
        fieldType = &program->types[fieldRef->typeRef];
        fieldAlign = WasmTypeByteAlign(program, fieldRef->typeRef);
        if (fieldAlign <= 0) {
            return -1;
        }
        if (fieldAlign > 1) {
            if (WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, state->scratch1Local) != 0
                || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, fieldAlign - 1) != 0
                || WasmAppendByte(body, 0x6au) != 0 || WasmAppendByte(body, 0x41u) != 0
                || WasmAppendSLEB32(body, -fieldAlign) != 0 || WasmAppendByte(body, 0x71u) != 0
                || WasmAppendByte(body, 0x21u) != 0
                || WasmAppendULEB(body, state->scratch1Local) != 0)
            {
                return -1;
            }
        }
        if (i == fieldIndex) {
            if (WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, state->scratch0Local) != 0
                || WasmAppendByte(body, 0x20u) != 0
                || WasmAppendULEB(body, state->scratch1Local) != 0
                || WasmAppendByte(body, 0x6au) != 0)
            {
                return -1;
            }
            return 0;
        }
        if (SLMirTypeRefIsStrObj(fieldType)) {
            if (WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, state->scratch0Local) != 0
                || WasmAppendByte(body, 0x20u) != 0
                || WasmAppendULEB(body, state->scratch1Local) != 0
                || WasmAppendByte(body, 0x6au) != 0 || WasmAppendByte(body, 0x21u) != 0
                || WasmAppendULEB(body, state->scratch2Local) != 0
                || WasmAppendByte(body, 0x20u) != 0
                || WasmAppendULEB(body, state->scratch1Local) != 0
                || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 8) != 0
                || WasmAppendByte(body, 0x6au) != 0 || WasmAppendByte(body, 0x21u) != 0
                || WasmAppendULEB(body, state->scratch1Local) != 0
                || WasmAppendByte(body, 0x20u) != 0
                || WasmAppendULEB(body, state->scratch2Local) != 0
                || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 4) != 0
                || WasmAppendByte(body, 0x6au) != 0 || WasmEmitI32Load(body) != 0
                || WasmAppendByte(body, 0x21u) != 0
                || WasmAppendULEB(body, state->scratch3Local) != 0
                || WasmAppendByte(body, 0x20u) != 0
                || WasmAppendULEB(body, state->scratch3Local) != 0
                || WasmAppendByte(body, 0x45u) != 0 || WasmAppendByte(body, 0x04u) != 0
                || WasmAppendByte(body, 0x40u) != 0 || WasmAppendByte(body, 0x05u) != 0
                || WasmAppendByte(body, 0x20u) != 0
                || WasmAppendULEB(body, state->scratch1Local) != 0
                || WasmAppendByte(body, 0x20u) != 0
                || WasmAppendULEB(body, state->scratch3Local) != 0
                || WasmAppendByte(body, 0x6au) != 0 || WasmAppendByte(body, 0x41u) != 0
                || WasmAppendSLEB32(body, 1) != 0 || WasmAppendByte(body, 0x6au) != 0
                || WasmAppendByte(body, 0x21u) != 0
                || WasmAppendULEB(body, state->scratch1Local) != 0
                || WasmAppendByte(body, 0x0bu) != 0)
            {
                return -1;
            }
            continue;
        }
        if (SLMirTypeRefIsVArrayView(fieldType)) {
            uint32_t countFieldRef = SLMirTypeRefVArrayCountField(fieldType);
            uint32_t countFieldIndex = UINT32_MAX;
            uint32_t countFieldOffset = UINT32_MAX;
            uint32_t elemSize = WasmIntKindByteWidth(SLMirTypeRefIntKind(fieldType));
            if (countFieldRef == UINT32_MAX || countFieldRef >= program->fieldLen || elemSize == 0u
                || !WasmFindAggregateField(
                    program,
                    ownerTypeRef,
                    &program->fields[countFieldRef],
                    &countFieldIndex,
                    &countFieldOffset))
            {
                return -1;
            }
            if (WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, state->scratch1Local) != 0
                || WasmAppendByte(body, 0x20u) != 0
                || WasmAppendULEB(body, state->scratch0Local) != 0
                || WasmAppendByte(body, 0x41u) != 0
                || WasmAppendSLEB32(body, (int32_t)countFieldOffset) != 0
                || WasmAppendByte(body, 0x6au) != 0 || WasmEmitI32Load(body) != 0)
            {
                return -1;
            }
            if (elemSize != 1u
                && (WasmAppendByte(body, 0x41u) != 0
                    || WasmAppendSLEB32(body, (int32_t)elemSize) != 0
                    || WasmAppendByte(body, 0x6cu) != 0))
            {
                return -1;
            }
            if (WasmAppendByte(body, 0x6au) != 0 || WasmAppendByte(body, 0x21u) != 0
                || WasmAppendULEB(body, state->scratch1Local) != 0)
            {
                return -1;
            }
            continue;
        }
        fieldSize = WasmTypeByteSize(program, fieldRef->typeRef);
        if (fieldSize < 0) {
            return -1;
        }
        if (fieldSize != 0
            && (WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, state->scratch1Local) != 0
                || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, fieldSize) != 0
                || WasmAppendByte(body, 0x6au) != 0 || WasmAppendByte(body, 0x21u) != 0
                || WasmAppendULEB(body, state->scratch1Local) != 0))
        {
            return -1;
        }
    }
    return -1;
}

static bool WasmTypeRefsCompatible(
    const SLMirProgram* program, uint32_t expectedTypeRef, uint32_t actualTypeRef) {
    uint32_t            expectedField = 0;
    uint32_t            actualField = 0;
    const SLMirTypeRef* expected;
    const SLMirTypeRef* actual;
    if (program == NULL || expectedTypeRef >= program->typeLen || actualTypeRef >= program->typeLen)
    {
        return false;
    }
    if (expectedTypeRef == actualTypeRef) {
        return true;
    }
    expected = &program->types[expectedTypeRef];
    actual = &program->types[actualTypeRef];
    if (!SLMirTypeRefIsAggregate(expected) || !SLMirTypeRefIsAggregate(actual)) {
        return expected->flags == actual->flags && expected->aux == actual->aux;
    }
    if (WasmTypeByteSize(program, expectedTypeRef) != WasmTypeByteSize(program, actualTypeRef)) {
        return false;
    }
    while (expectedField < program->fieldLen && actualField < program->fieldLen) {
        const SLMirField* expectedFieldRef;
        const SLMirField* actualFieldRef;
        if (program->fields[expectedField].ownerTypeRef != expectedTypeRef) {
            expectedField++;
            continue;
        }
        if (program->fields[actualField].ownerTypeRef != actualTypeRef) {
            actualField++;
            continue;
        }
        expectedFieldRef = &program->fields[expectedField];
        actualFieldRef = &program->fields[actualField];
        if (!WasmSourceSliceEq(
                program,
                expectedFieldRef->sourceRef,
                expectedFieldRef->nameStart,
                expectedFieldRef->nameEnd,
                actualFieldRef->sourceRef,
                actualFieldRef->nameStart,
                actualFieldRef->nameEnd)
            || !WasmTypeRefsCompatible(program, expectedFieldRef->typeRef, actualFieldRef->typeRef))
        {
            return false;
        }
        expectedField++;
        actualField++;
    }
    while (expectedField < program->fieldLen) {
        if (program->fields[expectedField].ownerTypeRef == expectedTypeRef) {
            return false;
        }
        expectedField++;
    }
    while (actualField < program->fieldLen) {
        if (program->fields[actualField].ownerTypeRef == actualTypeRef) {
            return false;
        }
        actualField++;
    }
    return true;
}

static int WasmTempTypeRefForInst(const SLMirProgram* program, const SLMirInst* inst) {
    uint8_t typeKind = SLWasmType_VOID;
    if (program == NULL || inst == NULL) {
        return -1;
    }
    if (inst->op == SLMirOp_AGG_ZERO) {
        return (inst->aux < program->typeLen && SLMirTypeRefIsAggregate(&program->types[inst->aux]))
                 ? (int)inst->aux
                 : -1;
    }
    if (inst->op == SLMirOp_CALL_FN && inst->aux < program->funcLen) {
        uint32_t typeRef = program->funcs[inst->aux].typeRef;
        if (typeRef < program->typeLen && WasmTypeKindFromMirType(program, typeRef, &typeKind)
            && WasmTypeKindUsesSRet(typeKind))
        {
            return (int)typeRef;
        }
    }
    return -1;
}

static int WasmTempOffsetForPc(
    const SLMirProgram*    program,
    const SLMirFunction*   fn,
    const SLWasmEmitState* state,
    uint32_t               pc,
    uint32_t* _Nonnull outOffset) {
    uint32_t scanPc;
    uint32_t offset;
    if (program == NULL || fn == NULL || state == NULL || outOffset == NULL || pc >= fn->instLen) {
        return -1;
    }
    offset = state->tempFrameStart;
    for (scanPc = 0; scanPc <= pc; scanPc++) {
        const SLMirInst* inst = &program->insts[fn->instStart + scanPc];
        int              typeRef = WasmTempTypeRefForInst(program, inst);
        if (typeRef < 0) {
            continue;
        }
        offset = WasmAlign4(offset);
        if (scanPc == pc) {
            *outOffset = offset;
            return 0;
        }
        offset += (uint32_t)WasmTypeByteSize(program, (uint32_t)typeRef);
    }
    return -1;
}

static int WasmFindAggregateField(
    const SLMirProgram* program,
    uint32_t            ownerTypeRef,
    const SLMirField*   fieldRef,
    uint32_t*           outFieldIndex,
    uint32_t*           outOffset) {
    uint32_t offset = 0u;
    uint32_t i;
    if (outFieldIndex != NULL) {
        *outFieldIndex = UINT32_MAX;
    }
    if (outOffset != NULL) {
        *outOffset = UINT32_MAX;
    }
    if (program == NULL || fieldRef == NULL) {
        return 0;
    }
    for (i = 0; i < program->fieldLen; i++) {
        int      fieldSize;
        int      fieldAlign;
        uint32_t fieldOffset;
        if (program->fields[i].ownerTypeRef != ownerTypeRef) {
            continue;
        }
        fieldSize = WasmTypeByteSize(program, program->fields[i].typeRef);
        fieldAlign = WasmTypeByteAlign(program, program->fields[i].typeRef);
        if ((fieldSize <= 0 || fieldAlign <= 0)
            && WasmFieldIsEmbeddedAllocator(program, &program->fields[i]))
        {
            fieldSize = 4;
            fieldAlign = 4;
        }
        if (fieldSize < 0 || fieldAlign <= 0) {
            return 0;
        }
        fieldOffset = (offset + ((uint32_t)fieldAlign - 1u)) & ~((uint32_t)fieldAlign - 1u);
        if (WasmSourceSliceEq(
                program,
                program->fields[i].sourceRef,
                program->fields[i].nameStart,
                program->fields[i].nameEnd,
                fieldRef->sourceRef,
                fieldRef->nameStart,
                fieldRef->nameEnd))
        {
            if (outFieldIndex != NULL) {
                *outFieldIndex = i;
            }
            if (outOffset != NULL) {
                *outOffset = fieldOffset;
            }
            return 1;
        }
        offset = fieldOffset + (uint32_t)fieldSize;
    }
    return 0;
}

static uint8_t WasmAddressTypeFromTypeRef(const SLMirProgram* program, uint32_t typeRefIndex) {
    const SLMirTypeRef* typeRef;
    if (program == NULL || typeRefIndex == UINT32_MAX || typeRefIndex >= program->typeLen) {
        return SLWasmType_VOID;
    }
    typeRef = &program->types[typeRefIndex];
    if (SLMirTypeRefIsStrRef(typeRef)) {
        return SLWasmType_STR_PTR;
    }
    if (SLMirTypeRefIsAggSliceView(typeRef)) {
        return SLWasmType_SLICE_AGG;
    }
    if (SLMirTypeRefIsStrObj(typeRef)) {
        return SLWasmType_STR_PTR;
    }
    if (SLMirTypeRefIsFuncRef(typeRef)) {
        return SLWasmType_FUNC_REF_PTR;
    }
    if (SLMirTypeRefIsOpaquePtr(typeRef)) {
        return SLWasmType_OPAQUE_PTR;
    }
    if (SLMirTypeRefIsAggregate(typeRef)) {
        return SLWasmType_AGG_REF;
    }
    if (SLMirTypeRefIsFixedArray(typeRef) || SLMirTypeRefIsFixedArrayView(typeRef)) {
        uint8_t typeKind = SLWasmType_VOID;
        if (WasmTypeKindFromMirType(program, typeRefIndex, &typeKind)) {
            return typeKind;
        }
        return SLWasmType_VOID;
    }
    if (SLMirTypeRefScalarKind(typeRef) == SLMirTypeScalar_I32) {
        switch (SLMirTypeRefIntKind(typeRef)) {
            case SLMirIntKind_U8:  return SLWasmType_U8_PTR;
            case SLMirIntKind_I8:  return SLWasmType_I8_PTR;
            case SLMirIntKind_U16: return SLWasmType_U16_PTR;
            case SLMirIntKind_I16: return SLWasmType_I16_PTR;
            case SLMirIntKind_U32: return SLWasmType_U32_PTR;
            default:               return SLWasmType_I32_PTR;
        }
    }
    if (SLMirTypeRefIsStrPtr(typeRef)) {
        return SLWasmType_STR_PTR;
    }
    if (SLMirTypeRefIsU8Ptr(typeRef)) {
        return SLWasmType_U8_PTR;
    }
    if (SLMirTypeRefIsI8Ptr(typeRef)) {
        return SLWasmType_I8_PTR;
    }
    if (SLMirTypeRefIsU16Ptr(typeRef)) {
        return SLWasmType_U16_PTR;
    }
    if (SLMirTypeRefIsI16Ptr(typeRef)) {
        return SLWasmType_I16_PTR;
    }
    if (SLMirTypeRefIsU32Ptr(typeRef)) {
        return SLWasmType_U32_PTR;
    }
    if (SLMirTypeRefIsI32Ptr(typeRef)) {
        return SLWasmType_I32_PTR;
    }
    return SLWasmType_VOID;
}

static int WasmEmitAddrFromFrame(
    SLWasmBuf* body, const SLWasmEmitState* state, uint32_t offset, uint16_t addend) {
    if (body == NULL || state == NULL) {
        return -1;
    }
    if (WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, state->frameBaseLocal) != 0
        || WasmAppendByte(body, 0x41u) != 0
        || WasmAppendSLEB32(body, (int32_t)(offset + (uint32_t)addend)) != 0
        || WasmAppendByte(body, 0x6au) != 0)
    {
        return -1;
    }
    return 0;
}

static int WasmEmitRestoreFrameAndReturn(
    SLWasmBuf*                body,
    const SLWasmImportLayout* imports,
    const SLWasmEmitState*    state,
    uint8_t                   resultKind) {
    if (body == NULL || imports == NULL || state == NULL) {
        return -1;
    }
    if (WasmTypeKindSlotCount(resultKind) == 1u) {
        if (WasmAppendByte(body, 0x21u) != 0 || WasmAppendULEB(body, state->scratch0Local) != 0) {
            return -1;
        }
    } else if (WasmTypeKindSlotCount(resultKind) == 2u) {
        if (WasmAppendByte(body, 0x21u) != 0 || WasmAppendULEB(body, state->scratch1Local) != 0
            || WasmAppendByte(body, 0x21u) != 0 || WasmAppendULEB(body, state->scratch0Local) != 0)
        {
            return -1;
        }
    }
    if (WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, state->frameBaseLocal) != 0
        || WasmAppendByte(body, 0x24u) != 0 || WasmAppendULEB(body, imports->frameGlobalIndex) != 0)
    {
        return -1;
    }
    if (WasmTypeKindSlotCount(resultKind) == 1u) {
        if (WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, state->scratch0Local) != 0) {
            return -1;
        }
    } else if (WasmTypeKindSlotCount(resultKind) == 2u) {
        if (WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, state->scratch0Local) != 0
            || WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, state->scratch1Local) != 0)
        {
            return -1;
        }
    }
    return WasmAppendByte(body, 0x0fu);
}

static bool WasmBranchDepthForJump(
    const SLWasmBranchTargets* targets, uint32_t targetPc, uint32_t* outDepth) {
    if (targets == NULL || outDepth == NULL) {
        return false;
    }
    if (targets->hasContinue && targets->continueTargetPc == targetPc) {
        *outDepth = targets->continueDepth;
        return true;
    }
    if (targets->hasBreak && targets->breakTargetPc == targetPc) {
        *outDepth = targets->breakDepth;
        return true;
    }
    return false;
}

static SLWasmBranchTargets WasmNestedBranchTargets(const SLWasmBranchTargets* targets) {
    SLWasmBranchTargets nested = { 0 };
    if (targets == NULL) {
        return nested;
    }
    nested = *targets;
    if (nested.hasContinue) {
        nested.continueDepth++;
    }
    if (nested.hasBreak) {
        nested.breakDepth++;
    }
    return nested;
}

static bool WasmFindLoopRegion(
    const SLMirProgram*  program,
    const SLMirFunction* fn,
    uint32_t             startPc,
    uint32_t             endPc,
    SLWasmLoopRegion*    out) {
    uint32_t branchPc;
    if (program == NULL || fn == NULL || out == NULL || startPc >= endPc) {
        return false;
    }
    for (branchPc = startPc; branchPc < endPc; branchPc++) {
        const SLMirInst* branchInst = &program->insts[fn->instStart + branchPc];
        uint32_t         falseTarget;
        const SLMirInst* backedgeInst;
        uint32_t         scanPc;
        uint32_t         tailStartPc = 0;
        if (branchInst->op != SLMirOp_JUMP_IF_FALSE) {
            continue;
        }
        falseTarget = branchInst->aux;
        if (falseTarget <= branchPc + 1u || falseTarget > endPc) {
            continue;
        }
        if (branchPc + 1u >= falseTarget - 1u) {
            continue;
        }
        backedgeInst = &program->insts[fn->instStart + falseTarget - 1u];
        if (backedgeInst->op != SLMirOp_JUMP || backedgeInst->aux != startPc) {
            continue;
        }
        for (scanPc = branchPc + 1u; scanPc + 1u < falseTarget; scanPc++) {
            const SLMirInst* inst = &program->insts[fn->instStart + scanPc];
            if (inst->op == SLMirOp_JUMP && inst->aux > scanPc && inst->aux < falseTarget
                && inst->aux > tailStartPc)
            {
                tailStartPc = inst->aux;
            }
        }
        out->headerPc = startPc;
        out->condBranchPc = branchPc;
        out->bodyStartPc = branchPc + 1u;
        out->backedgePc = falseTarget - 1u;
        out->tailStartPc = tailStartPc != 0u ? tailStartPc : out->backedgePc;
        out->exitPc = falseTarget;
        return true;
    }
    return false;
}

static bool WasmFindUnconditionalLoopRegion(
    const SLMirProgram*  program,
    const SLMirFunction* fn,
    uint32_t             startPc,
    uint32_t             endPc,
    SLWasmLoopRegion*    out) {
    uint32_t backedgePc;
    if (program == NULL || fn == NULL || out == NULL || startPc + 1u >= endPc) {
        return false;
    }
    for (backedgePc = endPc; backedgePc-- > startPc + 1u;) {
        const SLMirInst* backedgeInst = &program->insts[fn->instStart + backedgePc];
        uint32_t         scanPc;
        if (backedgeInst->op != SLMirOp_JUMP || backedgeInst->aux != startPc) {
            continue;
        }
        for (scanPc = startPc; scanPc < backedgePc; scanPc++) {
            const SLMirInst* inst = &program->insts[fn->instStart + scanPc];
            switch (inst->op) {
                case SLMirOp_JUMP:
                    if (inst->aux != startPc && inst->aux != backedgePc + 1u
                        && (inst->aux <= scanPc || inst->aux > backedgePc))
                    {
                        goto next_backedge;
                    }
                    break;
                case SLMirOp_JUMP_IF_FALSE:
                    if (inst->aux <= scanPc || inst->aux > backedgePc + 1u) {
                        goto next_backedge;
                    }
                    break;
                default: break;
            }
        }
        out->headerPc = startPc;
        out->condBranchPc = UINT32_MAX;
        out->bodyStartPc = startPc;
        out->tailStartPc = backedgePc;
        out->backedgePc = backedgePc;
        out->exitPc = backedgePc + 1u;
        return true;
    next_backedge:;
    }
    return false;
}

static int WasmEmitBinaryI32(
    SLWasmBuf* body, uint16_t tok, uint32_t start, uint32_t end, SLDiag* diag) {
    uint8_t opcode = 0;
    switch ((SLTokenKind)tok) {
        case SLTok_ADD: opcode = 0x6au; break;
        case SLTok_SUB: opcode = 0x6bu; break;
        case SLTok_MUL: opcode = 0x6cu; break;
        case SLTok_AND: opcode = 0x71u; break;
        case SLTok_EQ:  opcode = 0x46u; break;
        case SLTok_NEQ: opcode = 0x47u; break;
        case SLTok_LT:  opcode = 0x48u; break;
        case SLTok_GT:  opcode = 0x4au; break;
        case SLTok_LTE: opcode = 0x4cu; break;
        case SLTok_GTE: opcode = 0x4eu; break;
        default:
            WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, start, end);
            if (diag != NULL) {
                diag->detail = "unsupported i32 binary op";
            }
            return -1;
    }
    return WasmAppendByte(body, opcode);
}

static int WasmEmitUnaryI32(
    SLWasmBuf* body, uint16_t tok, uint32_t start, uint32_t end, SLDiag* diag) {
    switch ((SLTokenKind)tok) {
        case SLTok_ADD: return 0;
        case SLTok_NOT: return WasmAppendByte(body, 0x45u);
        case SLTok_SUB:
            if (WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0
                || WasmAppendByte(body, 0x6bu) != 0)
            {
                return -1;
            }
            return 0;
        default:
            WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, start, end);
            if (diag != NULL) {
                diag->detail = "unsupported i32 unary op";
            }
            return -1;
    }
}

static int WasmEmitFunctionRange(
    const SLMirProgram*        program,
    const SLWasmFnSig*         sigs,
    const SLWasmImportLayout*  imports,
    const SLWasmStringLayout*  strings,
    const SLMirFunction*       fn,
    SLWasmBuf*                 body,
    SLWasmEmitState*           state,
    const SLWasmBranchTargets* branchTargets,
    uint8_t                    resultKind,
    uint32_t                   startPc,
    uint32_t                   endPc,
    SLDiag* _Nullable diag) {
    uint32_t pc = startPc;
    uint32_t stepCount = 0u;
    uint32_t maxSteps = ((endPc > startPc ? (endPc - startPc) : 1u) * 8u) + 32u;
    while (pc < endPc) {
        int allowLoopRecognition = !(
            branchTargets != NULL && branchTargets->hasContinue
            && branchTargets->continueTargetPc == pc);
        if (++stepCount > maxSteps) {
            WasmSetDiag(diag, SLDiag_WASM_BACKEND_INTERNAL, fn->nameStart, fn->nameEnd);
            if (diag != NULL) {
                diag->detail = "Wasm emitter range exceeded progress limit";
            }
            return -1;
        }
        SLWasmLoopRegion loop;
        if (allowLoopRecognition && WasmFindLoopRegion(program, fn, pc, endPc, &loop)) {
            SLWasmBranchTargets condTargets = { 0 };
            SLWasmBranchTargets bodyTargets = { 0 };
            SLWasmBranchTargets tailTargets = { 0 };
            const SLMirInst*    branchInst = &program->insts[fn->instStart + loop.condBranchPc];
            uint8_t             condType = 0;
            uint32_t            stackDepthBefore = state->stackLen;
            if (WasmAppendByte(body, 0x02u) != 0 || WasmAppendByte(body, 0x40u) != 0
                || WasmAppendByte(body, 0x03u) != 0 || WasmAppendByte(body, 0x40u) != 0)
            {
                return -1;
            }
            if (WasmEmitFunctionRange(
                    program,
                    sigs,
                    imports,
                    strings,
                    fn,
                    body,
                    state,
                    &condTargets,
                    resultKind,
                    loop.headerPc,
                    loop.condBranchPc,
                    diag)
                != 0)
            {
                return -1;
            }
            if (WasmStackPop(state, &condType) != 0 || condType != SLWasmType_I32) {
                WasmSetDiag(
                    diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, branchInst->start, branchInst->end);
                if (diag != NULL) {
                    diag->detail = "unsupported branch condition type";
                }
                return -1;
            }
            if (state->stackLen != stackDepthBefore) {
                WasmSetDiag(
                    diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, branchInst->start, branchInst->end);
                if (diag != NULL) {
                    diag->detail = "loop condition stack mismatch";
                }
                return -1;
            }
            if (WasmAppendByte(body, 0x45u) != 0 || WasmAppendByte(body, 0x0du) != 0
                || WasmAppendULEB(body, 1u) != 0)
            {
                return -1;
            }

            bodyTargets.hasContinue = 1;
            bodyTargets.continueTargetPc =
                (loop.tailStartPc < loop.backedgePc) ? loop.tailStartPc : loop.headerPc;
            bodyTargets.continueDepth = (loop.tailStartPc < loop.backedgePc) ? 0u : 1u;
            bodyTargets.hasBreak = 1;
            bodyTargets.breakTargetPc = loop.exitPc;
            bodyTargets.breakDepth = 2u;
            if (WasmAppendByte(body, 0x02u) != 0 || WasmAppendByte(body, 0x40u) != 0) {
                return -1;
            }
            if (WasmEmitFunctionRange(
                    program,
                    sigs,
                    imports,
                    strings,
                    fn,
                    body,
                    state,
                    &bodyTargets,
                    resultKind,
                    loop.bodyStartPc,
                    loop.tailStartPc,
                    diag)
                != 0)
            {
                return -1;
            }
            if (state->stackLen != stackDepthBefore) {
                WasmSetDiag(
                    diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, branchInst->start, branchInst->end);
                if (diag != NULL) {
                    diag->detail = "loop body stack mismatch";
                }
                return -1;
            }
            if (WasmAppendByte(body, 0x0bu) != 0) {
                return -1;
            }

            tailTargets.hasContinue = 1;
            tailTargets.continueTargetPc = loop.headerPc;
            tailTargets.continueDepth = 0u;
            tailTargets.hasBreak = 1;
            tailTargets.breakTargetPc = loop.exitPc;
            tailTargets.breakDepth = 1u;
            if (WasmEmitFunctionRange(
                    program,
                    sigs,
                    imports,
                    strings,
                    fn,
                    body,
                    state,
                    &tailTargets,
                    resultKind,
                    loop.tailStartPc,
                    loop.backedgePc,
                    diag)
                != 0)
            {
                return -1;
            }
            if (state->stackLen != stackDepthBefore) {
                WasmSetDiag(
                    diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, branchInst->start, branchInst->end);
                if (diag != NULL) {
                    diag->detail = "loop tail stack mismatch";
                }
                return -1;
            }
            if (WasmAppendByte(body, 0x0cu) != 0 || WasmAppendULEB(body, 0u) != 0
                || WasmAppendByte(body, 0x0bu) != 0 || WasmAppendByte(body, 0x0bu) != 0)
            {
                return -1;
            }
            pc = loop.exitPc;
            continue;
        }
        if (allowLoopRecognition && WasmFindUnconditionalLoopRegion(program, fn, pc, endPc, &loop))
        {
            SLWasmBranchTargets loopTargets = { 0 };
            uint32_t            stackDepthBefore = state->stackLen;
            loopTargets.hasContinue = 1;
            loopTargets.continueTargetPc = loop.headerPc;
            loopTargets.continueDepth = 0u;
            loopTargets.hasBreak = 1;
            loopTargets.breakTargetPc = loop.exitPc;
            loopTargets.breakDepth = 1u;
            if (WasmAppendByte(body, 0x02u) != 0 || WasmAppendByte(body, 0x40u) != 0
                || WasmAppendByte(body, 0x03u) != 0 || WasmAppendByte(body, 0x40u) != 0)
            {
                return -1;
            }
            if (WasmEmitFunctionRange(
                    program,
                    sigs,
                    imports,
                    strings,
                    fn,
                    body,
                    state,
                    &loopTargets,
                    resultKind,
                    loop.bodyStartPc,
                    loop.backedgePc,
                    diag)
                != 0)
            {
                return -1;
            }
            if (state->stackLen != stackDepthBefore) {
                WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, fn->nameStart, fn->nameEnd);
                if (diag != NULL) {
                    diag->detail = "loop body stack mismatch";
                }
                return -1;
            }
            if (WasmAppendByte(body, 0x0cu) != 0 || WasmAppendULEB(body, 0u) != 0
                || WasmAppendByte(body, 0x0bu) != 0 || WasmAppendByte(body, 0x0bu) != 0)
            {
                return -1;
            }
            pc = loop.exitPc;
            continue;
        }
        const SLMirInst* inst = &program->insts[fn->instStart + pc];
        switch (inst->op) {
            case SLMirOp_PUSH_CONST:
                if (inst->aux >= program->constLen) {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_INTERNAL, inst->start, inst->end);
                    return -1;
                }
                switch (program->consts[inst->aux].kind) {
                    case SLMirConst_INT:
                        if (WasmAppendByte(body, 0x41u) != 0
                            || WasmAppendSLEB32(body, (int32_t)program->consts[inst->aux].bits) != 0
                            || WasmStackPushEx(state, SLWasmType_I32, UINT32_MAX) != 0)
                        {
                            return -1;
                        }
                        break;
                    case SLMirConst_BOOL:
                        if (WasmAppendByte(body, 0x41u) != 0
                            || WasmAppendSLEB32(body, program->consts[inst->aux].bits != 0 ? 1 : 0)
                                   != 0
                            || WasmStackPushEx(state, SLWasmType_I32, UINT32_MAX) != 0)
                        {
                            return -1;
                        }
                        break;
                    case SLMirConst_NULL:
                        if (WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0
                            || WasmStackPushEx(state, SLWasmType_I32, UINT32_MAX) != 0)
                        {
                            return -1;
                        }
                        break;
                    case SLMirConst_STRING:
                        if (strings == NULL || strings->constRefs == NULL
                            || inst->aux >= program->constLen
                            || strings->constRefs[inst->aux].dataOffset == UINT32_MAX
                            || WasmAppendByte(body, 0x41u) != 0
                            || WasmAppendSLEB32(
                                   body, (int32_t)strings->constRefs[inst->aux].dataOffset)
                                   != 0
                            || WasmAppendByte(body, 0x41u) != 0
                            || WasmAppendSLEB32(body, (int32_t)strings->constRefs[inst->aux].len)
                                   != 0
                            || WasmStackPushEx(state, SLWasmType_STR_REF, UINT32_MAX) != 0)
                        {
                            WasmSetDiag(
                                diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                            if (diag != NULL) {
                                diag->detail = "unsupported string constant use";
                            }
                            return -1;
                        }
                        break;
                    case SLMirConst_FUNCTION:
                        if (imports == NULL || !imports->hasFunctionTable
                            || program->consts[inst->aux].aux >= program->funcLen
                            || WasmAppendByte(body, 0x41u) != 0
                            || WasmAppendSLEB32(body, (int32_t)program->consts[inst->aux].aux) != 0
                            || WasmStackPushEx(
                                   state,
                                   SLWasmType_FUNC_REF,
                                   WasmFindFunctionValueTypeRef(
                                       program, program->consts[inst->aux].aux))
                                   != 0)
                        {
                            WasmSetDiag(
                                diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                            if (diag != NULL) {
                                diag->detail = "unsupported function constant use";
                            }
                            return -1;
                        }
                        break;
                    default:
                        WasmSetDiag(
                            diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                        if (diag != NULL) {
                            diag->detail = "unsupported constant kind";
                        }
                        return -1;
                }
                break;
            case SLMirOp_UNARY: {
                uint8_t  operandType = 0;
                uint32_t operandTypeRef = UINT32_MAX;
                if (WasmStackPopEx(state, &operandType, &operandTypeRef) != 0
                    || WasmRequireI32Value(
                           operandType, diag, inst->start, inst->end, "unsupported unary operand")
                           != 0
                    || WasmEmitUnaryI32(body, inst->tok, inst->start, inst->end, diag) != 0
                    || WasmStackPushEx(state, SLWasmType_I32, UINT32_MAX) != 0)
                {
                    return -1;
                }
                break;
            }
            case SLMirOp_BINARY: {
                uint8_t  rhsType = 0;
                uint8_t  lhsType = 0;
                uint32_t rhsTypeRef = UINT32_MAX;
                uint32_t lhsTypeRef = UINT32_MAX;
                if (WasmStackPopEx(state, &rhsType, &rhsTypeRef) != 0
                    || WasmStackPopEx(state, &lhsType, &lhsTypeRef) != 0)
                {
                    return -1;
                }
                if ((inst->tok == SLTok_EQ || inst->tok == SLTok_NEQ) && lhsType == rhsType
                    && WasmTypeKindIsRawSingleSlot(lhsType))
                {
                    if (WasmEmitBinaryI32(body, inst->tok, inst->start, inst->end, diag) != 0
                        || WasmStackPushEx(state, SLWasmType_I32, UINT32_MAX) != 0)
                    {
                        return -1;
                    }
                    break;
                }
                if ((inst->tok == SLTok_EQ || inst->tok == SLTok_NEQ)
                    && ((WasmTypeKindIsRawSingleSlot(lhsType) && rhsType == SLWasmType_I32)
                        || (lhsType == SLWasmType_I32 && WasmTypeKindIsRawSingleSlot(rhsType))))
                {
                    if (WasmEmitBinaryI32(body, inst->tok, inst->start, inst->end, diag) != 0
                        || WasmStackPushEx(state, SLWasmType_I32, UINT32_MAX) != 0)
                    {
                        return -1;
                    }
                    break;
                }
                if ((inst->tok == SLTok_EQ || inst->tok == SLTok_NEQ)
                    && WasmTypeKindIsSlice(lhsType) && rhsType == SLWasmType_I32)
                {
                    if (WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch2Local) != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch2Local) != 0
                        || WasmEmitBinaryI32(body, inst->tok, inst->start, inst->end, diag) != 0
                        || WasmStackPushEx(state, SLWasmType_I32, UINT32_MAX) != 0)
                    {
                        return -1;
                    }
                    break;
                }
                if ((inst->tok == SLTok_EQ || inst->tok == SLTok_NEQ) && lhsType == SLWasmType_I32
                    && WasmTypeKindIsSlice(rhsType))
                {
                    if (WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch2Local) != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0
                        || WasmEmitBinaryI32(body, inst->tok, inst->start, inst->end, diag) != 0
                        || WasmStackPushEx(state, SLWasmType_I32, UINT32_MAX) != 0)
                    {
                        return -1;
                    }
                    break;
                }
                if ((inst->tok == SLTok_EQ || inst->tok == SLTok_NEQ)
                    && lhsType == SLWasmType_STR_REF && rhsType == SLWasmType_STR_REF)
                {
                    if (WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch3Local) != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch2Local) != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch2Local) != 0
                        || WasmAppendByte(body, 0x46u) != 0 || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch3Local) != 0
                        || WasmAppendByte(body, 0x46u) != 0 || WasmAppendByte(body, 0x71u) != 0)
                    {
                        return -1;
                    }
                    if (inst->tok == SLTok_NEQ && WasmAppendByte(body, 0x45u) != 0) {
                        return -1;
                    }
                    if (WasmStackPushEx(state, SLWasmType_I32, UINT32_MAX) != 0) {
                        return -1;
                    }
                    break;
                }
                if (WasmRequireI32Value(
                        rhsType, diag, inst->start, inst->end, "unsupported binary rhs")
                        != 0
                    || WasmRequireI32Value(
                           lhsType, diag, inst->start, inst->end, "unsupported binary lhs")
                           != 0
                    || WasmEmitBinaryI32(body, inst->tok, inst->start, inst->end, diag) != 0
                    || WasmStackPushEx(state, SLWasmType_I32, UINT32_MAX) != 0)
                {
                    return -1;
                }
                break;
            }
            case SLMirOp_CAST:
            case SLMirOp_COERCE: {
                uint8_t  valueType = 0;
                uint8_t  targetType = 0;
                uint32_t valueTypeRef = UINT32_MAX;
                if (WasmStackPopEx(state, &valueType, &valueTypeRef) != 0
                    || !WasmTypeKindFromMirType(program, inst->aux, &targetType))
                {
                    return -1;
                }
                if ((valueType == SLWasmType_I32 && targetType == SLWasmType_I32)
                    || (valueType == SLWasmType_I32
                        && (WasmTypeKindIsPointer(targetType)
                            || WasmTypeKindIsArrayView(targetType)))
                    || (valueType == SLWasmType_STR_REF && targetType == SLWasmType_STR_PTR)
                    || (valueType == SLWasmType_AGG_REF && targetType == SLWasmType_AGG_REF))
                {
                    if (WasmStackPushEx(state, targetType, inst->aux) != 0) {
                        return -1;
                    }
                    break;
                }
                WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                if (diag != NULL) {
                    diag->detail = "unsupported cast/coerce";
                }
                return -1;
            }
            case SLMirOp_AGG_ZERO: {
                uint32_t tempOffset = UINT32_MAX;
                if (!state->usesFrame || inst->aux >= program->typeLen
                    || !SLMirTypeRefIsAggregate(&program->types[inst->aux])
                    || WasmTempOffsetForPc(program, fn, state, pc, &tempOffset) != 0
                    || WasmEmitZeroFrameRange(
                           body, state, tempOffset, (uint32_t)WasmTypeByteSize(program, inst->aux))
                           != 0
                    || WasmEmitAddrFromFrame(body, state, tempOffset, 0u) != 0
                    || WasmStackPushEx(state, SLWasmType_AGG_REF, inst->aux) != 0)
                {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL && diag->detail == NULL) {
                        diag->detail = "unsupported aggregate zero-init";
                    }
                    return -1;
                }
                break;
            }
            case SLMirOp_LOCAL_ZERO:
                if (inst->aux >= fn->localCount) {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported LOCAL_ZERO type";
                    }
                    return -1;
                }
                if (state->usesFrame) {
                    if (state->localStorage[inst->aux] == SLWasmLocalStorage_ARRAY) {
                        if (WasmEmitZeroFrameRange(
                                body,
                                state,
                                state->frameOffsets[inst->aux],
                                state->arrayCounts[inst->aux]
                                    * WasmTypeKindElementSize(state->localKinds[inst->aux]))
                            != 0)
                        {
                            return -1;
                        }
                    } else if (state->localStorage[inst->aux] == SLWasmLocalStorage_AGG) {
                        if (WasmEmitZeroFrameRange(
                                body,
                                state,
                                state->frameOffsets[inst->aux],
                                (uint32_t)WasmTypeByteSize(
                                    program, state->localTypeRefs[inst->aux]))
                            != 0)
                        {
                            return -1;
                        }
                    } else if (WasmTypeKindIsRawSingleSlot(state->localKinds[inst->aux])) {
                        if (WasmEmitAddrFromFrame(body, state, state->frameOffsets[inst->aux], 0u)
                                != 0
                            || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0
                            || (state->localKinds[inst->aux] == SLWasmType_I32
                                    ? WasmEmitTypedStore(
                                          body, WasmLocalAddressTypeKind(state, inst->aux))
                                    : WasmEmitI32Store(body))
                                   != 0)
                        {
                            return -1;
                        }
                    } else if (
                        state->localKinds[inst->aux] == SLWasmType_STR_REF
                        || WasmTypeKindIsSlice(state->localKinds[inst->aux]))
                    {
                        if (WasmEmitAddrFromFrame(body, state, state->frameOffsets[inst->aux], 0u)
                                != 0
                            || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0
                            || WasmEmitI32Store(body) != 0
                            || WasmEmitAddrFromFrame(
                                   body, state, state->frameOffsets[inst->aux], 4u)
                                   != 0
                            || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0
                            || WasmEmitI32Store(body) != 0)
                        {
                            return -1;
                        }
                    } else {
                        WasmSetDiag(
                            diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                        if (diag != NULL) {
                            diag->detail = "unsupported LOCAL_ZERO type";
                        }
                        return -1;
                    }
                    break;
                }
                if (WasmTypeKindIsRawSingleSlot(state->localKinds[inst->aux])) {
                    if (WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->wasmValueIndex[inst->aux]) != 0)
                    {
                        return -1;
                    }
                } else if (
                    state->localKinds[inst->aux] == SLWasmType_STR_REF
                    || WasmTypeKindIsSlice(state->localKinds[inst->aux]))
                {
                    if (WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->wasmValueIndex[inst->aux] + 1u) != 0
                        || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->wasmValueIndex[inst->aux]) != 0)
                    {
                        return -1;
                    }
                } else {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported LOCAL_ZERO type";
                    }
                    return -1;
                }
                break;
            case SLMirOp_LOCAL_LOAD:
                if (inst->aux >= fn->localCount) {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_INTERNAL, inst->start, inst->end);
                    return -1;
                }
                if (state->usesFrame) {
                    if (state->localStorage[inst->aux] == SLWasmLocalStorage_ARRAY) {
                        if (WasmEmitAddrFromFrame(body, state, state->frameOffsets[inst->aux], 0u)
                                != 0
                            || WasmStackPushEx(
                                   state,
                                   state->localKinds[inst->aux],
                                   state->localTypeRefs[inst->aux])
                                   != 0)
                        {
                            return -1;
                        }
                    } else if (state->localStorage[inst->aux] == SLWasmLocalStorage_AGG) {
                        if (WasmEmitAddrFromFrame(body, state, state->frameOffsets[inst->aux], 0u)
                                != 0
                            || WasmStackPushEx(
                                   state, SLWasmType_AGG_REF, state->localTypeRefs[inst->aux])
                                   != 0)
                        {
                            return -1;
                        }
                    } else if (WasmTypeKindIsRawSingleSlot(state->localKinds[inst->aux])) {
                        if (WasmEmitAddrFromFrame(body, state, state->frameOffsets[inst->aux], 0u)
                                != 0
                            || (state->localKinds[inst->aux] == SLWasmType_I32
                                    ? WasmEmitTypedLoad(
                                          body, WasmLocalAddressTypeKind(state, inst->aux))
                                    : WasmEmitI32Load(body))
                                   != 0
                            || WasmStackPushEx(
                                   state,
                                   state->localKinds[inst->aux],
                                   state->localTypeRefs[inst->aux])
                                   != 0)
                        {
                            return -1;
                        }
                    } else if (
                        state->localKinds[inst->aux] == SLWasmType_STR_REF
                        || WasmTypeKindIsSlice(state->localKinds[inst->aux]))
                    {
                        if (WasmEmitAddrFromFrame(body, state, state->frameOffsets[inst->aux], 0u)
                                != 0
                            || WasmEmitI32Load(body) != 0
                            || WasmEmitAddrFromFrame(
                                   body, state, state->frameOffsets[inst->aux], 4u)
                                   != 0
                            || WasmEmitI32Load(body) != 0
                            || WasmStackPushEx(
                                   state,
                                   state->localKinds[inst->aux],
                                   state->localTypeRefs[inst->aux])
                                   != 0)
                        {
                            return -1;
                        }
                    } else {
                        WasmSetDiag(
                            diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                        if (diag != NULL) {
                            diag->detail = "unsupported local load type";
                        }
                        return -1;
                    }
                    break;
                }
                if (WasmTypeKindIsRawSingleSlot(state->localKinds[inst->aux])) {
                    if (WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->wasmValueIndex[inst->aux]) != 0
                        || WasmStackPushEx(
                               state, state->localKinds[inst->aux], state->localTypeRefs[inst->aux])
                               != 0)
                    {
                        return -1;
                    }
                } else if (
                    state->localKinds[inst->aux] == SLWasmType_STR_REF
                    || WasmTypeKindIsSlice(state->localKinds[inst->aux]))
                {
                    if (WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->wasmValueIndex[inst->aux]) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->wasmValueIndex[inst->aux] + 1u) != 0
                        || WasmStackPushEx(
                               state, state->localKinds[inst->aux], state->localTypeRefs[inst->aux])
                               != 0)
                    {
                        return -1;
                    }
                } else {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported local load type";
                    }
                    return -1;
                }
                break;
            case SLMirOp_LOCAL_STORE: {
                uint8_t  valueType = 0;
                uint32_t valueTypeRef = UINT32_MAX;
                if (inst->aux >= fn->localCount
                    || WasmStackPopEx(state, &valueType, &valueTypeRef) != 0
                    || (valueType != state->localKinds[inst->aux]
                        && !(
                            valueType == SLWasmType_STR_REF
                            && state->localKinds[inst->aux] == SLWasmType_STR_PTR)
                        && !(
                            valueType == SLWasmType_I32
                            && WasmTypeKindIsPointer(state->localKinds[inst->aux]))))
                {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "local store type mismatch";
                    }
                    return -1;
                }
                if (state->usesFrame) {
                    if (state->localStorage[inst->aux] == SLWasmLocalStorage_ARRAY) {
                        uint32_t copySize;
                        if (!WasmTypeKindIsArrayView(valueType) || valueTypeRef == UINT32_MAX
                            || (state->localTypeRefs[inst->aux] != UINT32_MAX
                                && !WasmTypeRefsCompatible(
                                    program, state->localTypeRefs[inst->aux], valueTypeRef)))
                        {
                            WasmSetDiag(
                                diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                            if (diag != NULL) {
                                diag->detail = "array local store type mismatch";
                            }
                            return -1;
                        }
                        copySize = (uint32_t)WasmTypeByteSize(
                            program,
                            state->localTypeRefs[inst->aux] != UINT32_MAX
                                ? state->localTypeRefs[inst->aux]
                                : valueTypeRef);
                        if (copySize == 0u || WasmAppendByte(body, 0x21u) != 0
                            || WasmAppendULEB(body, state->scratch0Local) != 0
                            || WasmEmitCopyLocalAddrToFrame(
                                   body,
                                   state,
                                   state->scratch0Local,
                                   state->frameOffsets[inst->aux],
                                   copySize)
                                   != 0)
                        {
                            WasmSetDiag(
                                diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                            if (diag != NULL && diag->detail == NULL) {
                                diag->detail = "array local store is not supported";
                            }
                            return -1;
                        }
                        break;
                    }
                    if (state->localStorage[inst->aux] == SLWasmLocalStorage_AGG) {
                        uint32_t copySize;
                        if (valueType != SLWasmType_AGG_REF || valueTypeRef == UINT32_MAX) {
                            WasmSetDiag(
                                diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                            if (diag != NULL) {
                                diag->detail = "aggregate local store type mismatch";
                            }
                            return -1;
                        }
                        if (state->localTypeRefs[inst->aux] != UINT32_MAX
                            && !WasmTypeRefsCompatible(
                                program, state->localTypeRefs[inst->aux], valueTypeRef))
                        {
                            WasmSetDiag(
                                diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                            if (diag != NULL) {
                                diag->detail = "aggregate local store type mismatch";
                            }
                            return -1;
                        }
                        copySize = (uint32_t)WasmTypeByteSize(program, valueTypeRef);
                        if (WasmAppendByte(body, 0x21u) != 0
                            || WasmAppendULEB(body, state->scratch0Local) != 0
                            || WasmEmitCopyLocalAddrToFrame(
                                   body,
                                   state,
                                   state->scratch0Local,
                                   state->frameOffsets[inst->aux],
                                   copySize)
                                   != 0)
                        {
                            return -1;
                        }
                        break;
                    }
                    if (WasmTypeKindIsRawSingleSlot(state->localKinds[inst->aux])) {
                        if (state->localKinds[inst->aux] == SLWasmType_STR_PTR
                            && valueType == SLWasmType_STR_REF)
                        {
                            if (state->auxOffsets[inst->aux] == UINT32_MAX
                                || WasmAppendByte(body, 0x21u) != 0
                                || WasmAppendULEB(body, state->scratch1Local) != 0
                                || WasmAppendByte(body, 0x21u) != 0
                                || WasmAppendULEB(body, state->scratch0Local) != 0
                                || WasmEmitAddrFromFrame(
                                       body, state, state->auxOffsets[inst->aux], 0u)
                                       != 0
                                || WasmAppendByte(body, 0x20u) != 0
                                || WasmAppendULEB(body, state->scratch0Local) != 0
                                || WasmEmitI32Store(body) != 0
                                || WasmEmitAddrFromFrame(
                                       body, state, state->auxOffsets[inst->aux], 4u)
                                       != 0
                                || WasmAppendByte(body, 0x20u) != 0
                                || WasmAppendULEB(body, state->scratch1Local) != 0
                                || WasmEmitI32Store(body) != 0
                                || WasmEmitAddrFromFrame(
                                       body, state, state->frameOffsets[inst->aux], 0u)
                                       != 0
                                || WasmAppendByte(body, 0x41u) != 0
                                || WasmAppendSLEB32(body, (int32_t)state->auxOffsets[inst->aux])
                                       != 0
                                || WasmAppendByte(body, 0x20u) != 0
                                || WasmAppendULEB(body, state->frameBaseLocal) != 0
                                || WasmAppendByte(body, 0x6au) != 0 || WasmEmitI32Store(body) != 0)
                            {
                                return -1;
                            }
                            break;
                        }
                        if (WasmAppendByte(body, 0x21u) != 0
                            || WasmAppendULEB(body, state->scratch0Local) != 0
                            || WasmEmitAddrFromFrame(
                                   body, state, state->frameOffsets[inst->aux], 0u)
                                   != 0
                            || WasmAppendByte(body, 0x20u) != 0
                            || WasmAppendULEB(body, state->scratch0Local) != 0
                            || (state->localKinds[inst->aux] == SLWasmType_I32
                                    ? WasmEmitTypedStore(
                                          body, WasmLocalAddressTypeKind(state, inst->aux))
                                    : WasmEmitI32Store(body))
                                   != 0)
                        {
                            return -1;
                        }
                    } else if (
                        state->localKinds[inst->aux] == SLWasmType_STR_REF
                        || WasmTypeKindIsSlice(state->localKinds[inst->aux]))
                    {
                        if (WasmAppendByte(body, 0x21u) != 0
                            || WasmAppendULEB(body, state->scratch1Local) != 0
                            || WasmAppendByte(body, 0x21u) != 0
                            || WasmAppendULEB(body, state->scratch0Local) != 0
                            || WasmEmitAddrFromFrame(
                                   body, state, state->frameOffsets[inst->aux], 0u)
                                   != 0
                            || WasmAppendByte(body, 0x20u) != 0
                            || WasmAppendULEB(body, state->scratch0Local) != 0
                            || WasmEmitI32Store(body) != 0
                            || WasmEmitAddrFromFrame(
                                   body, state, state->frameOffsets[inst->aux], 4u)
                                   != 0
                            || WasmAppendByte(body, 0x20u) != 0
                            || WasmAppendULEB(body, state->scratch1Local) != 0
                            || WasmEmitI32Store(body) != 0)
                        {
                            return -1;
                        }
                    } else {
                        WasmSetDiag(
                            diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                        if (diag != NULL) {
                            diag->detail = "unsupported local store type";
                        }
                        return -1;
                    }
                    break;
                }
                if (WasmTypeKindIsRawSingleSlot(valueType)) {
                    if (WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->wasmValueIndex[inst->aux]) != 0)
                    {
                        return -1;
                    }
                } else if (valueType == SLWasmType_STR_REF || WasmTypeKindIsSlice(valueType)) {
                    if (WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->wasmValueIndex[inst->aux] + 1u) != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->wasmValueIndex[inst->aux]) != 0)
                    {
                        return -1;
                    }
                } else {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported local store type";
                    }
                    return -1;
                }
                break;
            }
            case SLMirOp_LOCAL_ADDR:
                if (!state->usesFrame || inst->aux >= fn->localCount) {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported local address";
                    }
                    return -1;
                }
                if (state->localKinds[inst->aux] == SLWasmType_I32) {
                    uint8_t addrType = WasmLocalAddressTypeKind(state, inst->aux);
                    if (addrType == SLWasmType_VOID) {
                        WasmSetDiag(
                            diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                        if (diag != NULL) {
                            diag->detail = "unsupported local address type";
                        }
                        return -1;
                    }
                    if (WasmEmitAddrFromFrame(body, state, state->frameOffsets[inst->aux], 0u) != 0
                        || WasmStackPushEx(state, addrType, UINT32_MAX) != 0)
                    {
                        return -1;
                    }
                } else if (state->localKinds[inst->aux] == SLWasmType_STR_REF) {
                    if (WasmEmitAddrFromFrame(body, state, state->frameOffsets[inst->aux], 0u) != 0
                        || WasmStackPushEx(state, SLWasmType_STR_PTR, UINT32_MAX) != 0)
                    {
                        return -1;
                    }
                } else if (state->localStorage[inst->aux] == SLWasmLocalStorage_ARRAY) {
                    if (WasmEmitAddrFromFrame(body, state, state->frameOffsets[inst->aux], 0u) != 0
                        || WasmStackPushEx(
                               state, state->localKinds[inst->aux], state->localTypeRefs[inst->aux])
                               != 0)
                    {
                        return -1;
                    }
                } else if (state->localStorage[inst->aux] == SLWasmLocalStorage_AGG) {
                    if (WasmEmitAddrFromFrame(body, state, state->frameOffsets[inst->aux], 0u) != 0
                        || WasmStackPushEx(state, SLWasmType_I32, UINT32_MAX) != 0)
                    {
                        return -1;
                    }
                } else {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported local address type";
                    }
                    return -1;
                }
                break;
            case SLMirOp_ARRAY_ADDR: {
                uint8_t  indexType = 0;
                uint8_t  baseType = 0;
                uint32_t baseTypeRef = UINT32_MAX;
                uint8_t  ptrType = SLWasmType_VOID;
                uint32_t elemSize;
                if (WasmStackPop(state, &indexType) != 0
                    || WasmStackPopEx(state, &baseType, &baseTypeRef) != 0
                    || WasmRequireI32Value(
                           indexType, diag, inst->start, inst->end, "unsupported array index")
                           != 0)
                {
                    return -1;
                }
                switch (baseType) {
                    case SLWasmType_ARRAY_VIEW_U8:  ptrType = SLWasmType_U8_PTR; break;
                    case SLWasmType_ARRAY_VIEW_I8:  ptrType = SLWasmType_I8_PTR; break;
                    case SLWasmType_ARRAY_VIEW_U16: ptrType = SLWasmType_U16_PTR; break;
                    case SLWasmType_ARRAY_VIEW_I16: ptrType = SLWasmType_I16_PTR; break;
                    case SLWasmType_ARRAY_VIEW_U32: ptrType = SLWasmType_U32_PTR; break;
                    case SLWasmType_ARRAY_VIEW_I32: ptrType = SLWasmType_I32_PTR; break;
                    default:                        break;
                }
                if (ptrType == SLWasmType_VOID) {
                    switch (baseType) {
                        case SLWasmType_SLICE_U8:  ptrType = SLWasmType_U8_PTR; break;
                        case SLWasmType_SLICE_I8:  ptrType = SLWasmType_I8_PTR; break;
                        case SLWasmType_SLICE_U16: ptrType = SLWasmType_U16_PTR; break;
                        case SLWasmType_SLICE_I16: ptrType = SLWasmType_I16_PTR; break;
                        case SLWasmType_SLICE_U32: ptrType = SLWasmType_U32_PTR; break;
                        case SLWasmType_SLICE_I32: ptrType = SLWasmType_I32_PTR; break;
                        case SLWasmType_SLICE_AGG: ptrType = SLWasmType_OPAQUE_PTR; break;
                        default:                   break;
                    }
                }
                if (ptrType == SLWasmType_VOID) {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported array address base";
                    }
                    return -1;
                }
                elemSize = WasmTypeElementSize(program, baseType, baseTypeRef);
                if (elemSize == 0u) {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported array address base";
                    }
                    return -1;
                }
                if (WasmTypeKindIsSlice(baseType)) {
                    if (WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch2Local) != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch2Local) != 0
                        || WasmEmitScaleIndex(body, elemSize) != 0
                        || WasmAppendByte(body, 0x6au) != 0
                        || WasmStackPushEx(
                               state,
                               ptrType,
                               ptrType == SLWasmType_OPAQUE_PTR
                                   ? WasmAggSliceElemTypeRef(program, baseTypeRef)
                                   : UINT32_MAX)
                               != 0)
                    {
                        return -1;
                    }
                } else if (
                    WasmEmitScaleIndex(body, elemSize) != 0 || WasmAppendByte(body, 0x6au) != 0
                    || WasmStackPush(state, ptrType) != 0)
                {
                    return -1;
                }
                break;
            }
            case SLMirOp_AGG_SET: {
                uint8_t           valueType = 0;
                uint8_t           baseType = 0;
                uint32_t          valueTypeRef = UINT32_MAX;
                uint32_t          baseTypeRef = UINT32_MAX;
                uint32_t          ownerTypeRef = UINT32_MAX;
                uint32_t          fieldIndex = UINT32_MAX;
                uint32_t          fieldOffset = UINT32_MAX;
                uint32_t          fieldTypeRef = UINT32_MAX;
                uint8_t           fieldType = SLWasmType_VOID;
                uint8_t           fieldAddrType = SLWasmType_VOID;
                const SLMirField* fieldRef;
                if (inst->aux >= program->fieldLen
                    || WasmStackPopEx(state, &valueType, &valueTypeRef) != 0
                    || WasmStackPopEx(state, &baseType, &baseTypeRef) != 0)
                {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported aggregate set base";
                    }
                    return -1;
                }
                fieldRef = &program->fields[inst->aux];
                if (baseType == SLWasmType_STR_PTR && WasmFieldNameEq(program, fieldRef, "len")) {
                    if (valueType != SLWasmType_I32 || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 4) != 0
                        || WasmAppendByte(body, 0x6au) != 0 || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0
                        || WasmEmitI32Store(body) != 0 || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0
                        || WasmAppendByte(body, 0x45u) != 0 || WasmAppendByte(body, 0x04u) != 0
                        || WasmAppendByte(body, 0x40u) != 0 || WasmAppendByte(body, 0x05u) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmEmitI32Load(body) != 0 || WasmAppendByte(body, 0x45u) != 0
                        || WasmAppendByte(body, 0x04u) != 0 || WasmAppendByte(body, 0x40u) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 8) != 0
                        || WasmAppendByte(body, 0x6au) != 0 || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch2Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch2Local) != 0
                        || WasmEmitI32Store(body) != 0 || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch2Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0
                        || WasmAppendByte(body, 0x6au) != 0 || WasmAppendByte(body, 0x41u) != 0
                        || WasmAppendSLEB32(body, 0) != 0 || WasmEmitU8Store(body) != 0
                        || WasmAppendByte(body, 0x0bu) != 0 || WasmAppendByte(body, 0x0bu) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmStackPushEx(state, baseType, baseTypeRef) != 0)
                    {
                        WasmSetDiag(
                            diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                        if (diag != NULL && diag->detail == NULL) {
                            diag->detail = "unsupported aggregate field set";
                        }
                        return -1;
                    }
                    break;
                }
                ownerTypeRef = WasmAggregateOwnerTypeRef(program, baseType, baseTypeRef);
                if (ownerTypeRef == UINT32_MAX) {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported aggregate set base";
                    }
                    return -1;
                }
                if (!WasmFindAggregateField(
                        program, ownerTypeRef, fieldRef, &fieldIndex, &fieldOffset))
                {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported aggregate field set";
                    }
                    return -1;
                }
                fieldTypeRef = program->fields[fieldIndex].typeRef;
                if (!WasmTypeKindFromMirType(program, fieldTypeRef, &fieldType)) {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported aggregate field set";
                    }
                    return -1;
                }
                fieldAddrType = WasmAddressTypeFromTypeRef(program, fieldTypeRef);
                if (fieldType == SLWasmType_STR_PTR
                    && SLMirTypeRefIsStrObj(&program->types[fieldTypeRef]))
                {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported aggregate field set";
                    }
                    return -1;
                }
                if (WasmTypeKindIsRawSingleSlot(fieldType)) {
                    uint16_t valueLocal =
                        state->usesFrame && WasmAggregateHasDynamicLayout(program, ownerTypeRef)
                            ? state->scratch4Local
                            : state->scratch1Local;
                    if (valueType != fieldType || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, valueLocal) != 0 || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmEmitAggregateFieldAddress(
                               body, state, program, ownerTypeRef, fieldIndex, fieldOffset)
                               != 0
                        || WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, valueLocal) != 0
                        || (fieldType == SLWasmType_I32
                                ? WasmEmitTypedStore(body, fieldAddrType)
                                : WasmEmitI32Store(body))
                               != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmStackPushEx(state, baseType, baseTypeRef) != 0)
                    {
                        return -1;
                    }
                    break;
                }
                if (fieldType == SLWasmType_STR_REF || WasmTypeKindIsSlice(fieldType)) {
                    bool isVArrayView = SLMirTypeRefIsVArrayView(&program->types[fieldTypeRef]);
                    if (isVArrayView) {
                        WasmSetDiag(
                            diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                        if (diag != NULL) {
                            diag->detail = "unsupported aggregate field set";
                        }
                        return -1;
                    }
                    if (valueType != fieldType || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch2Local) != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0)
                    {
                        return -1;
                    }
                    if (WasmEmitAggregateFieldAddress(
                            body, state, program, ownerTypeRef, fieldIndex, fieldOffset)
                            != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0
                        || WasmEmitI32Store(body) != 0
                        || WasmEmitAggregateFieldAddress(
                               body, state, program, ownerTypeRef, fieldIndex, fieldOffset)
                               != 0
                        || WasmAppendByte(body, 0x41u) != 0
                        || WasmAppendSLEB32(body, (int32_t)(isVArrayView ? 0u : 4u)) != 0
                        || WasmAppendByte(body, 0x6au) != 0 || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch2Local) != 0
                        || WasmEmitI32Store(body) != 0 || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmStackPushEx(state, baseType, baseTypeRef) != 0)
                    {
                        return -1;
                    }
                    break;
                }
                if (fieldType == SLWasmType_AGG_REF) {
                    uint32_t copySize = (uint32_t)WasmTypeByteSize(program, fieldTypeRef);
                    if (valueType != SLWasmType_AGG_REF || valueTypeRef == UINT32_MAX
                        || !WasmTypeRefsCompatible(program, fieldTypeRef, valueTypeRef)
                        || copySize == 0u || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmEmitCopyLocalAddrToLocalAddr(
                               body,
                               state->scratch1Local,
                               state->scratch0Local,
                               fieldOffset,
                               copySize)
                               != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmStackPushEx(state, baseType, baseTypeRef) != 0)
                    {
                        WasmSetDiag(
                            diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                        if (diag != NULL && diag->detail == NULL) {
                            diag->detail = "unsupported aggregate field set";
                        }
                        return -1;
                    }
                    break;
                }
                WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                if (diag != NULL) {
                    diag->detail = "unsupported aggregate field set";
                }
                return -1;
            }
            case SLMirOp_AGG_ADDR: {
                uint8_t           baseType = 0;
                uint32_t          baseTypeRef = UINT32_MAX;
                uint32_t          ownerTypeRef = UINT32_MAX;
                uint32_t          fieldIndex = UINT32_MAX;
                uint32_t          fieldOffset = UINT32_MAX;
                uint8_t           addrType = SLWasmType_VOID;
                const SLMirField* fieldRef;
                if (inst->aux >= program->fieldLen
                    || WasmStackPopEx(state, &baseType, &baseTypeRef) != 0)
                {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported aggregate address base";
                    }
                    return -1;
                }
                fieldRef = &program->fields[inst->aux];
                if (WasmFieldNameEq(program, fieldRef, "impl")) {
                    if (baseType != SLWasmType_I32 && !WasmTypeKindIsPointer(baseType)
                        && baseType != SLWasmType_AGG_REF)
                    {
                        WasmSetDiag(
                            diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                        if (diag != NULL) {
                            diag->detail = "unsupported aggregate field address type";
                        }
                        return -1;
                    }
                    if (WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0
                        || WasmAppendByte(body, 0x6au) != 0
                        || WasmStackPushEx(state, SLWasmType_FUNC_REF_PTR, UINT32_MAX) != 0)
                    {
                        return -1;
                    }
                    break;
                }
                if ((ownerTypeRef = WasmAggregateOwnerTypeRef(program, baseType, baseTypeRef))
                    == UINT32_MAX)
                {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported aggregate address base";
                    }
                    return -1;
                }
                if (!WasmFindAggregateField(
                        program, ownerTypeRef, fieldRef, &fieldIndex, &fieldOffset)
                    || fieldIndex >= program->fieldLen)
                {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unknown aggregate field";
                    }
                    return -1;
                }
                addrType = WasmAddressTypeFromTypeRef(program, program->fields[fieldIndex].typeRef);
                if (addrType == SLWasmType_VOID) {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported aggregate field address type";
                    }
                    return -1;
                }
                if (WasmAppendByte(body, 0x21u) != 0
                    || WasmAppendULEB(body, state->scratch0Local) != 0
                    || WasmEmitAggregateFieldAddress(
                           body, state, program, ownerTypeRef, fieldIndex, fieldOffset)
                           != 0
                    || WasmStackPushEx(state, addrType, program->fields[fieldIndex].typeRef) != 0)
                {
                    return -1;
                }
                break;
            }
            case SLMirOp_AGG_GET: {
                uint8_t           baseType = 0;
                uint32_t          baseTypeRef = UINT32_MAX;
                uint32_t          ownerTypeRef = UINT32_MAX;
                uint32_t          fieldIndex = UINT32_MAX;
                uint32_t          fieldOffset = UINT32_MAX;
                uint8_t           fieldType = SLWasmType_VOID;
                const SLMirField* fieldRef;
                if (inst->aux >= program->fieldLen
                    || WasmStackPopEx(state, &baseType, &baseTypeRef) != 0)
                {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported aggregate get base";
                    }
                    return -1;
                }
                fieldRef = &program->fields[inst->aux];
                if (WasmFieldNameEq(program, fieldRef, "impl")) {
                    if (WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmEmitI32Load(body) != 0
                        || WasmStackPushEx(state, SLWasmType_FUNC_REF, UINT32_MAX) != 0)
                    {
                        return -1;
                    }
                    break;
                }
                if (baseType == SLWasmType_STR_PTR && WasmFieldNameEq(program, fieldRef, "len")) {
                    if (WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x45u) != 0 || WasmAppendByte(body, 0x04u) != 0
                        || WasmAppendByte(body, 0x7fu) != 0 || WasmAppendByte(body, 0x41u) != 0
                        || WasmAppendSLEB32(body, 0) != 0 || WasmAppendByte(body, 0x05u) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 4) != 0
                        || WasmAppendByte(body, 0x6au) != 0 || WasmEmitI32Load(body) != 0
                        || WasmAppendByte(body, 0x0bu) != 0
                        || WasmStackPushEx(state, SLWasmType_I32, UINT32_MAX) != 0)
                    {
                        return -1;
                    }
                    break;
                }
                if ((ownerTypeRef = WasmAggregateOwnerTypeRef(program, baseType, baseTypeRef))
                    == UINT32_MAX)
                {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported aggregate get base";
                    }
                    return -1;
                }
                if (!WasmFindAggregateField(
                        program, ownerTypeRef, fieldRef, &fieldIndex, &fieldOffset)
                    || fieldIndex >= program->fieldLen
                    || !WasmTypeKindFromMirType(
                        program, program->fields[fieldIndex].typeRef, &fieldType))
                {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unknown aggregate field";
                    }
                    return -1;
                }
                if (WasmAppendByte(body, 0x21u) != 0
                    || WasmAppendULEB(body, state->scratch0Local) != 0)
                {
                    return -1;
                }
                if (WasmTypeKindIsArrayView(fieldType)) {
                    if (WasmEmitAggregateFieldAddress(
                            body, state, program, ownerTypeRef, fieldIndex, fieldOffset)
                            != 0
                        || WasmStackPushEx(state, fieldType, program->fields[fieldIndex].typeRef)
                               != 0)
                    {
                        return -1;
                    }
                    break;
                }
                if (fieldType == SLWasmType_STR_PTR
                    && SLMirTypeRefIsStrObj(&program->types[program->fields[fieldIndex].typeRef]))
                {
                    if (WasmEmitAggregateFieldAddress(
                            body, state, program, ownerTypeRef, fieldIndex, fieldOffset)
                            != 0
                        || WasmStackPushEx(state, fieldType, program->fields[fieldIndex].typeRef)
                               != 0)
                    {
                        return -1;
                    }
                    break;
                }
                if (fieldType == SLWasmType_STR_REF || WasmTypeKindIsSlice(fieldType)) {
                    bool isVArrayView = SLMirTypeRefIsVArrayView(
                        &program->types[program->fields[fieldIndex].typeRef]);
                    uint32_t countFieldRef = SLMirTypeRefVArrayCountField(
                        &program->types[program->fields[fieldIndex].typeRef]);
                    uint32_t countFieldIndex = UINT32_MAX;
                    uint32_t countFieldOffset = UINT32_MAX;
                    if (isVArrayView
                        && (countFieldRef == UINT32_MAX || countFieldRef >= program->fieldLen
                            || !WasmFindAggregateField(
                                program,
                                ownerTypeRef,
                                &program->fields[countFieldRef],
                                &countFieldIndex,
                                &countFieldOffset)))
                    {
                        WasmSetDiag(
                            diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                        if (diag != NULL) {
                            diag->detail = "unknown aggregate field";
                        }
                        return -1;
                    }
                    if (WasmEmitAggregateFieldAddress(
                            body, state, program, ownerTypeRef, fieldIndex, fieldOffset)
                        != 0)
                    {
                        return -1;
                    }
                    if (isVArrayView) {
                        if (WasmAppendByte(body, 0x20u) != 0
                            || WasmAppendULEB(body, state->scratch0Local) != 0
                            || WasmAppendByte(body, 0x41u) != 0
                            || WasmAppendSLEB32(body, (int32_t)countFieldOffset) != 0
                            || WasmAppendByte(body, 0x6au) != 0 || WasmEmitI32Load(body) != 0
                            || WasmStackPushEx(
                                   state, fieldType, program->fields[fieldIndex].typeRef)
                                   != 0)
                        {
                            return -1;
                        }
                    } else if (
                        WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0
                        || WasmEmitI32Load(body) != 0 || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0
                        || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 4) != 0
                        || WasmAppendByte(body, 0x6au) != 0 || WasmEmitI32Load(body) != 0
                        || WasmStackPushEx(state, fieldType, program->fields[fieldIndex].typeRef)
                               != 0)
                    {
                        return -1;
                    }
                    break;
                }
                if (fieldType == SLWasmType_AGG_REF) {
                    if (WasmEmitAggregateFieldAddress(
                            body, state, program, ownerTypeRef, fieldIndex, fieldOffset)
                            != 0
                        || WasmStackPushEx(
                               state, SLWasmType_AGG_REF, program->fields[fieldIndex].typeRef)
                               != 0)
                    {
                        return -1;
                    }
                    break;
                }
                if (WasmEmitAggregateFieldAddress(
                        body, state, program, ownerTypeRef, fieldIndex, fieldOffset)
                        != 0
                    || ((fieldType == SLWasmType_I32)
                            ? WasmEmitTypedLoad(
                                  body,
                                  WasmAddressTypeFromTypeRef(
                                      program, program->fields[fieldIndex].typeRef))
                            : WasmEmitI32Load(body))
                           != 0
                    || WasmStackPushEx(state, fieldType, program->fields[fieldIndex].typeRef) != 0)
                {
                    return -1;
                }
                break;
            }
            case SLMirOp_DEREF_LOAD: {
                uint8_t refType = 0;
                if (WasmStackPop(state, &refType) != 0) {
                    return -1;
                }
                if (refType == SLWasmType_I32_PTR || refType == SLWasmType_U8_PTR
                    || refType == SLWasmType_I8_PTR || refType == SLWasmType_U16_PTR
                    || refType == SLWasmType_I16_PTR || refType == SLWasmType_U32_PTR)
                {
                    if (WasmEmitTypedLoad(body, refType) != 0
                        || WasmStackPush(state, SLWasmType_I32) != 0)
                    {
                        return -1;
                    }
                } else if (refType == SLWasmType_FUNC_REF_PTR) {
                    if (WasmEmitTypedLoad(body, refType) != 0
                        || WasmStackPush(state, SLWasmType_FUNC_REF) != 0)
                    {
                        return -1;
                    }
                } else if (refType == SLWasmType_STR_PTR) {
                    if (WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmEmitI32Load(body) != 0 || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 4) != 0
                        || WasmAppendByte(body, 0x6au) != 0 || WasmEmitI32Load(body) != 0
                        || WasmStackPush(state, SLWasmType_STR_REF) != 0)
                    {
                        return -1;
                    }
                } else {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported dereference load";
                    }
                    return -1;
                }
                break;
            }
            case SLMirOp_DEREF_STORE: {
                uint8_t refType = 0;
                uint8_t valueType = 0;
                if (WasmStackPop(state, &refType) != 0 || WasmStackPop(state, &valueType) != 0) {
                    return -1;
                }
                if ((refType == SLWasmType_I32_PTR || refType == SLWasmType_U8_PTR
                     || refType == SLWasmType_I8_PTR || refType == SLWasmType_U16_PTR
                     || refType == SLWasmType_I16_PTR || refType == SLWasmType_U32_PTR)
                    && (valueType == SLWasmType_I32
                        || (refType == SLWasmType_I32_PTR && WasmTypeKindIsPointer(valueType))))
                {
                    if (WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmEmitTypedStore(body, refType) != 0)
                    {
                        return -1;
                    }
                } else if (refType == SLWasmType_FUNC_REF_PTR && valueType == SLWasmType_FUNC_REF) {
                    if (WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmEmitTypedStore(body, refType) != 0)
                    {
                        return -1;
                    }
                } else if (
                    refType == SLWasmType_OPAQUE_PTR && WasmTypeKindIsRawSingleSlot(valueType))
                {
                    if (WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmEmitI32Store(body) != 0)
                    {
                        return -1;
                    }
                } else if (refType == SLWasmType_STR_PTR && valueType == SLWasmType_STR_REF) {
                    if (WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch2Local) != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch2Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmEmitI32Store(body) != 0 || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch2Local) != 0
                        || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 4) != 0
                        || WasmAppendByte(body, 0x6au) != 0 || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0
                        || WasmEmitI32Store(body) != 0)
                    {
                        return -1;
                    }
                } else {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported dereference store";
                    }
                    return -1;
                }
                break;
            }
            case SLMirOp_SEQ_LEN: {
                uint8_t  valueType = 0;
                uint32_t valueTypeRef = UINT32_MAX;
                if (WasmStackPopEx(state, &valueType, &valueTypeRef) != 0) {
                    return -1;
                }
                if (valueType == SLWasmType_STR_REF) {
                    if (WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x1au) != 0 || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmStackPush(state, SLWasmType_I32) != 0)
                    {
                        return -1;
                    }
                } else if (WasmTypeKindIsArrayView(valueType) && valueTypeRef < program->typeLen) {
                    if (WasmAppendByte(body, 0x1au) != 0 || WasmAppendByte(body, 0x41u) != 0
                        || WasmAppendSLEB32(
                               body,
                               (int32_t)SLMirTypeRefFixedArrayCount(&program->types[valueTypeRef]))
                               != 0
                        || WasmStackPushEx(state, SLWasmType_I32, UINT32_MAX) != 0)
                    {
                        return -1;
                    }
                } else if (WasmTypeKindIsSlice(valueType)) {
                    if (WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x1au) != 0 || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmStackPushEx(state, SLWasmType_I32, UINT32_MAX) != 0)
                    {
                        return -1;
                    }
                } else if (valueType == SLWasmType_STR_PTR) {
                    if (WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x45u) != 0 || WasmAppendByte(body, 0x04u) != 0
                        || WasmAppendByte(body, 0x7fu) != 0 || WasmAppendByte(body, 0x41u) != 0
                        || WasmAppendSLEB32(body, 0) != 0 || WasmAppendByte(body, 0x05u) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 4) != 0
                        || WasmAppendByte(body, 0x6au) != 0 || WasmEmitI32Load(body) != 0
                        || WasmAppendByte(body, 0x0bu) != 0
                        || WasmStackPush(state, SLWasmType_I32) != 0)
                    {
                        return -1;
                    }
                } else {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported seq len operand";
                    }
                    return -1;
                }
                break;
            }
            case SLMirOp_STR_CSTR: {
                uint8_t valueType = 0;
                if (WasmStackPop(state, &valueType) != 0) {
                    return -1;
                }
                if (valueType == SLWasmType_STR_REF) {
                    if (WasmAppendByte(body, 0x1au) != 0
                        || WasmStackPush(state, SLWasmType_U8_PTR) != 0)
                    {
                        return -1;
                    }
                } else if (valueType == SLWasmType_STR_PTR) {
                    if (WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x45u) != 0 || WasmAppendByte(body, 0x04u) != 0
                        || WasmAppendByte(body, 0x7fu) != 0 || WasmAppendByte(body, 0x41u) != 0
                        || WasmAppendSLEB32(body, 0) != 0 || WasmAppendByte(body, 0x05u) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmEmitI32Load(body) != 0 || WasmAppendByte(body, 0x0bu) != 0
                        || WasmStackPush(state, SLWasmType_U8_PTR) != 0)
                    {
                        return -1;
                    }
                } else {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported cstr operand";
                    }
                    return -1;
                }
                break;
            }
            case SLMirOp_INDEX: {
                uint8_t  indexType = 0;
                uint8_t  baseType = 0;
                uint32_t indexTypeRef = UINT32_MAX;
                uint32_t baseTypeRef = UINT32_MAX;
                if (WasmStackPopEx(state, &indexType, &indexTypeRef) != 0
                    || WasmStackPopEx(state, &baseType, &baseTypeRef) != 0
                    || WasmRequireI32Value(
                           indexType, diag, inst->start, inst->end, "unsupported index operand")
                           != 0)
                {
                    return -1;
                }
                if (baseType == SLWasmType_U8_PTR) {
                    if (WasmAppendByte(body, 0x6au) != 0 || WasmEmitU8Load(body) != 0
                        || WasmStackPush(state, SLWasmType_I32) != 0)
                    {
                        return -1;
                    }
                } else if (baseType == SLWasmType_STR_REF) {
                    if (WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0
                        || WasmAppendByte(body, 0x1au) != 0 || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0
                        || WasmAppendByte(body, 0x6au) != 0 || WasmEmitU8Load(body) != 0
                        || WasmStackPush(state, SLWasmType_I32) != 0)
                    {
                        return -1;
                    }
                } else if (
                    baseType == SLWasmType_I8_PTR || baseType == SLWasmType_U16_PTR
                    || baseType == SLWasmType_I16_PTR || baseType == SLWasmType_U32_PTR
                    || baseType == SLWasmType_I32_PTR || WasmTypeKindIsArrayView(baseType))
                {
                    uint32_t elemSize = WasmTypeKindElementSize(baseType);
                    if (WasmEmitScaleIndex(body, elemSize) != 0 || WasmAppendByte(body, 0x6au) != 0
                        || WasmEmitTypedLoad(body, baseType) != 0
                        || WasmStackPush(state, SLWasmType_I32) != 0)
                    {
                        return -1;
                    }
                } else if (WasmTypeKindIsSlice(baseType)) {
                    uint32_t elemSize = WasmTypeElementSize(program, baseType, baseTypeRef);
                    if (elemSize == 0u) {
                        WasmSetDiag(
                            diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                        if (diag != NULL) {
                            diag->detail = "unsupported index base";
                        }
                        return -1;
                    }
                    if (WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch2Local) != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch2Local) != 0
                        || WasmEmitScaleIndex(body, elemSize) != 0
                        || WasmAppendByte(body, 0x6au) != 0)
                    {
                        return -1;
                    }
                    if (baseType == SLWasmType_SLICE_AGG) {
                        if (WasmStackPushEx(
                                state,
                                SLWasmType_OPAQUE_PTR,
                                WasmAggSliceElemTypeRef(program, baseTypeRef))
                            != 0)
                        {
                            return -1;
                        }
                    } else if (
                        WasmEmitTypedLoad(body, baseType) != 0
                        || WasmStackPushEx(state, SLWasmType_I32, UINT32_MAX) != 0)
                    {
                        return -1;
                    }
                } else if (baseType == SLWasmType_STR_PTR) {
                    if (WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x45u) != 0 || WasmAppendByte(body, 0x04u) != 0
                        || WasmAppendByte(body, 0x7fu) != 0 || WasmAppendByte(body, 0x41u) != 0
                        || WasmAppendSLEB32(body, 0) != 0 || WasmAppendByte(body, 0x05u) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmEmitI32Load(body) != 0 || WasmAppendByte(body, 0x0bu) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0
                        || WasmAppendByte(body, 0x6au) != 0 || WasmEmitU8Load(body) != 0
                        || WasmStackPush(state, SLWasmType_I32) != 0)
                    {
                        return -1;
                    }
                } else {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported index base";
                    }
                    return -1;
                }
                break;
            }
            case SLMirOp_SLICE_MAKE: {
                uint8_t  baseType = 0;
                uint32_t baseTypeRef = UINT32_MAX;
                uint32_t totalLen = 0u;
                uint8_t  resultType = SLWasmType_VOID;
                int      hasStart = (inst->tok & SLAstFlag_INDEX_HAS_START) != 0u;
                int      hasEnd = (inst->tok & SLAstFlag_INDEX_HAS_END) != 0u;
                if (hasEnd) {
                    uint8_t endType = 0;
                    if (WasmStackPop(state, &endType) != 0
                        || WasmRequireI32Value(
                               endType, diag, inst->start, inst->end, "unsupported slice end")
                               != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch3Local) != 0)
                    {
                        return -1;
                    }
                }
                if (hasStart) {
                    uint8_t startType = 0;
                    if (WasmStackPop(state, &startType) != 0
                        || WasmRequireI32Value(
                               startType, diag, inst->start, inst->end, "unsupported slice start")
                               != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch2Local) != 0)
                    {
                        return -1;
                    }
                }
                if (WasmStackPopEx(state, &baseType, &baseTypeRef) != 0) {
                    return -1;
                }
                if (WasmTypeKindIsArrayView(baseType)) {
                    totalLen = baseTypeRef < program->typeLen
                                 ? SLMirTypeRefFixedArrayCount(&program->types[baseTypeRef])
                                 : 0u;
                    if (totalLen == 0u) {
                        WasmSetDiag(
                            diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                        if (diag != NULL) {
                            diag->detail = "unsupported slice base";
                        }
                        return -1;
                    }
                    resultType =
                        (uint8_t)(baseType + (SLWasmType_SLICE_U8 - SLWasmType_ARRAY_VIEW_U8));
                    if (WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0)
                    {
                        return -1;
                    }
                    if (hasStart) {
                        if (WasmAppendByte(body, 0x20u) != 0
                            || WasmAppendULEB(body, state->scratch0Local) != 0
                            || WasmAppendByte(body, 0x20u) != 0
                            || WasmAppendULEB(body, state->scratch2Local) != 0
                            || WasmEmitScaleIndex(body, WasmTypeKindElementSize(baseType)) != 0
                            || WasmAppendByte(body, 0x6au) != 0)
                        {
                            return -1;
                        }
                    } else if (
                        WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0)
                    {
                        return -1;
                    }
                    if (hasStart && hasEnd) {
                        if (WasmAppendByte(body, 0x20u) != 0
                            || WasmAppendULEB(body, state->scratch3Local) != 0
                            || WasmAppendByte(body, 0x20u) != 0
                            || WasmAppendULEB(body, state->scratch2Local) != 0
                            || WasmAppendByte(body, 0x6bu) != 0)
                        {
                            return -1;
                        }
                    } else if (hasStart) {
                        if (WasmAppendByte(body, 0x41u) != 0
                            || WasmAppendSLEB32(body, (int32_t)totalLen) != 0
                            || WasmAppendByte(body, 0x20u) != 0
                            || WasmAppendULEB(body, state->scratch2Local) != 0
                            || WasmAppendByte(body, 0x6bu) != 0)
                        {
                            return -1;
                        }
                    } else if (hasEnd) {
                        if (WasmAppendByte(body, 0x20u) != 0
                            || WasmAppendULEB(body, state->scratch3Local) != 0)
                        {
                            return -1;
                        }
                    } else if (
                        WasmAppendByte(body, 0x41u) != 0
                        || WasmAppendSLEB32(body, (int32_t)totalLen) != 0)
                    {
                        return -1;
                    }
                    if (WasmStackPushEx(state, resultType, UINT32_MAX) != 0) {
                        return -1;
                    }
                    break;
                }
                if (WasmTypeKindIsSlice(baseType)) {
                    resultType = baseType;
                    if (WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0)
                    {
                        return -1;
                    }
                    if (hasStart) {
                        if (WasmAppendByte(body, 0x20u) != 0
                            || WasmAppendULEB(body, state->scratch0Local) != 0
                            || WasmAppendByte(body, 0x20u) != 0
                            || WasmAppendULEB(body, state->scratch2Local) != 0
                            || WasmEmitScaleIndex(body, WasmTypeKindElementSize(baseType)) != 0
                            || WasmAppendByte(body, 0x6au) != 0)
                        {
                            return -1;
                        }
                    } else if (
                        WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0)
                    {
                        return -1;
                    }
                    if (hasStart && hasEnd) {
                        if (WasmAppendByte(body, 0x20u) != 0
                            || WasmAppendULEB(body, state->scratch3Local) != 0
                            || WasmAppendByte(body, 0x20u) != 0
                            || WasmAppendULEB(body, state->scratch2Local) != 0
                            || WasmAppendByte(body, 0x6bu) != 0)
                        {
                            return -1;
                        }
                    } else if (hasStart) {
                        if (WasmAppendByte(body, 0x20u) != 0
                            || WasmAppendULEB(body, state->scratch1Local) != 0
                            || WasmAppendByte(body, 0x20u) != 0
                            || WasmAppendULEB(body, state->scratch2Local) != 0
                            || WasmAppendByte(body, 0x6bu) != 0)
                        {
                            return -1;
                        }
                    } else if (hasEnd) {
                        if (WasmAppendByte(body, 0x20u) != 0
                            || WasmAppendULEB(body, state->scratch3Local) != 0)
                        {
                            return -1;
                        }
                    } else if (
                        WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0)
                    {
                        return -1;
                    }
                    if (WasmStackPushEx(state, resultType, baseTypeRef) != 0) {
                        return -1;
                    }
                    break;
                }
                WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                if (diag != NULL) {
                    diag->detail = "unsupported slice base";
                }
                return -1;
            }
            case SLMirOp_CTX_GET:
                if ((inst->aux == SLMirContextField_MEM || inst->aux == SLMirContextField_TEMP_MEM)
                    && strings != NULL && strings->rootAllocatorOffset != UINT32_MAX)
                {
                    if (WasmAppendByte(body, 0x41u) != 0
                        || WasmAppendSLEB32(body, (int32_t)strings->rootAllocatorOffset) != 0
                        || WasmStackPushEx(state, SLWasmType_OPAQUE_PTR, UINT32_MAX) != 0)
                    {
                        return -1;
                    }
                    break;
                }
                WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                if (diag != NULL) {
                    diag->detail = "unsupported context access";
                }
                return -1;
            case SLMirOp_ALLOC_NEW: {
                const SLMirInst* nextInst;
                uint32_t         localIndex;
                uint8_t          allocType = SLWasmType_VOID;
                uint32_t         allocTypeRef = UINT32_MAX;
                uint32_t         allocSize = 0u;
                uint8_t          allocArgType = SLWasmType_VOID;
                bool             hasAllocArg = (inst->tok & SLAstFlag_NEW_HAS_ALLOC) != 0u;
                bool             useFixedCountAlloc = false;
                bool             isOptional = false;
                if (imports == NULL || !imports->needsHeapGlobal
                    || (inst->tok & SLAstFlag_NEW_HAS_INIT) != 0 || pc + 1u >= fn->instLen)
                {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported ALLOC_NEW shape";
                    }
                    return -1;
                }
                nextInst = &program->insts[fn->instStart + pc + 1u];
                if (nextInst->op != SLMirOp_LOCAL_STORE || nextInst->aux >= fn->localCount) {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported ALLOC_NEW shape";
                    }
                    return -1;
                }
                localIndex = nextInst->aux;
                allocType = state->localKinds[localIndex];
                allocTypeRef = state->localTypeRefs[localIndex];
                useFixedCountAlloc =
                    (inst->tok & SLAstFlag_NEW_HAS_COUNT) != 0u && allocTypeRef < program->typeLen
                    && (SLMirTypeRefIsFixedArray(&program->types[allocTypeRef])
                        || SLMirTypeRefIsFixedArrayView(&program->types[allocTypeRef]));
                isOptional = allocTypeRef < program->typeLen
                          && SLMirTypeRefIsOptional(&program->types[allocTypeRef]);
                if (hasAllocArg) {
                    if (WasmStackPop(state, &allocArgType) != 0
                        || WasmRequireAllocatorValue(
                               allocArgType,
                               diag,
                               inst->start,
                               inst->end,
                               "unsupported allocator value")
                               != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch2Local) != 0)
                    {
                        return -1;
                    }
                }
                if (WasmTypeKindIsSlice(allocType)) {
                    uint32_t elemSize = WasmTypeElementSize(program, allocType, allocTypeRef);
                    uint8_t  countType = 0;
                    if ((inst->tok & SLAstFlag_NEW_HAS_COUNT) == 0u || elemSize == 0u
                        || WasmStackPop(state, &countType) != 0
                        || WasmRequireI32Value(
                               countType,
                               diag,
                               inst->start,
                               inst->end,
                               "unsupported dynamic ALLOC_NEW count")
                               != 0)
                    {
                        if (diag != NULL && diag->code == SLDiag_NONE) {
                            WasmSetDiag(
                                diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                            diag->detail = "unsupported dynamic ALLOC_NEW type";
                        }
                        return -1;
                    }
                    if (WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0)
                    {
                        return -1;
                    }
                    if (hasAllocArg) {
                        if (state->allocCallTempOffset == UINT32_MAX
                            || WasmAppendByte(body, 0x20u) != 0
                            || WasmAppendULEB(body, state->scratch2Local) != 0
                            || WasmAppendByte(body, 0x45u) != 0 || WasmAppendByte(body, 0x04u) != 0
                            || WasmAppendByte(body, 0x40u) != 0)
                        {
                            return -1;
                        }
                        if (isOptional) {
                            if (WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0
                                || WasmAppendByte(body, 0x21u) != 0
                                || WasmAppendULEB(body, state->scratch0Local) != 0
                                || WasmAppendByte(body, 0x41u) != 0
                                || WasmAppendSLEB32(body, 0) != 0
                                || WasmAppendByte(body, 0x21u) != 0
                                || WasmAppendULEB(body, state->scratch1Local) != 0)
                            {
                                return -1;
                            }
                        } else {
                            if (imports->hasWasmMinPanic && strings != NULL
                                && strings->hasAllocNullPanicString)
                            {
                                if (WasmAppendByte(body, 0x41u) != 0
                                    || WasmAppendSLEB32(
                                           body, (int32_t)strings->allocNullPanic.dataOffset)
                                           != 0
                                    || WasmAppendByte(body, 0x41u) != 0
                                    || WasmAppendSLEB32(body, (int32_t)strings->allocNullPanic.len)
                                           != 0
                                    || WasmAppendByte(body, 0x10u) != 0
                                    || WasmAppendULEB(body, imports->wasmMinPanicFuncIndex) != 0)
                                {
                                    return -1;
                                }
                            }
                            if (WasmAppendByte(body, 0x00u) != 0) {
                                return -1;
                            }
                        }
                        if (WasmAppendByte(body, 0x05u) != 0) {
                            return -1;
                        }
                        if (WasmAppendByte(body, 0x20u) != 0
                            || WasmAppendULEB(body, state->scratch1Local) != 0)
                        {
                            return -1;
                        }
                        if (elemSize != 1u
                            && (WasmAppendByte(body, 0x41u) != 0
                                || WasmAppendSLEB32(body, (int32_t)elemSize) != 0
                                || WasmAppendByte(body, 0x6cu) != 0))
                        {
                            return -1;
                        }
                        if (WasmAppendByte(body, 0x21u) != 0
                            || WasmAppendULEB(body, state->scratch3Local) != 0
                            || WasmEmitAddrFromFrame(body, state, state->allocCallTempOffset, 0u)
                                   != 0
                            || WasmAppendByte(body, 0x20u) != 0
                            || WasmAppendULEB(body, state->scratch3Local) != 0
                            || WasmEmitI32Store(body) != 0 || WasmAppendByte(body, 0x20u) != 0
                            || WasmAppendULEB(body, state->scratch2Local) != 0
                            || WasmEmitI32Load(body) != 0 || WasmAppendByte(body, 0x21u) != 0
                            || WasmAppendULEB(body, state->scratch4Local) != 0
                            || WasmAppendByte(body, 0x20u) != 0
                            || WasmAppendULEB(body, state->scratch2Local) != 0
                            || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0
                            || WasmAppendByte(body, 0x41u) != 0
                            || WasmAppendSLEB32(body, (int32_t)elemSize) != 0
                            || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0
                            || WasmEmitAddrFromFrame(body, state, state->allocCallTempOffset, 0u)
                                   != 0
                            || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0
                            || WasmAppendByte(body, 0x20u) != 0
                            || WasmAppendULEB(body, state->scratch4Local) != 0
                            || WasmAppendByte(body, 0x11u) != 0
                            || WasmAppendULEB(body, imports->allocatorIndirectTypeIndex) != 0
                            || WasmAppendByte(body, 0x00u) != 0 || WasmAppendByte(body, 0x21u) != 0
                            || WasmAppendULEB(body, state->scratch0Local) != 0)
                        {
                            return -1;
                        }
                        if (WasmAppendByte(body, 0x20u) != 0
                            || WasmAppendULEB(body, state->scratch0Local) != 0
                            || WasmAppendByte(body, 0x45u) != 0 || WasmAppendByte(body, 0x04u) != 0
                            || WasmAppendByte(body, 0x40u) != 0)
                        {
                            return -1;
                        }
                        if (isOptional) {
                            if (WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0
                                || WasmAppendByte(body, 0x21u) != 0
                                || WasmAppendULEB(body, state->scratch1Local) != 0)
                            {
                                return -1;
                            }
                        } else {
                            if (imports->hasWasmMinPanic && strings != NULL
                                && strings->hasAllocNullPanicString)
                            {
                                if (WasmAppendByte(body, 0x41u) != 0
                                    || WasmAppendSLEB32(
                                           body, (int32_t)strings->allocNullPanic.dataOffset)
                                           != 0
                                    || WasmAppendByte(body, 0x41u) != 0
                                    || WasmAppendSLEB32(body, (int32_t)strings->allocNullPanic.len)
                                           != 0
                                    || WasmAppendByte(body, 0x10u) != 0
                                    || WasmAppendULEB(body, imports->wasmMinPanicFuncIndex) != 0)
                                {
                                    return -1;
                                }
                            }
                            if (WasmAppendByte(body, 0x00u) != 0) {
                                return -1;
                            }
                        }
                        if (WasmAppendByte(body, 0x05u) != 0
                            || WasmEmitZeroDynamicLocalAddrRange(
                                   body,
                                   state->scratch0Local,
                                   state->scratch3Local,
                                   state->scratch4Local)
                                   != 0
                            || WasmAppendByte(body, 0x0bu) != 0 || WasmAppendByte(body, 0x0bu) != 0)
                        {
                            return -1;
                        }
                    } else if (
                        WasmAppendByte(body, 0x23u) != 0
                        || WasmAppendULEB(body, imports->heapGlobalIndex) != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0)
                    {
                        return -1;
                    } else if (
                        elemSize != 1u
                        && (WasmAppendByte(body, 0x41u) != 0
                            || WasmAppendSLEB32(body, (int32_t)elemSize) != 0
                            || WasmAppendByte(body, 0x6cu) != 0))
                    {
                        return -1;
                    } else if (
                        WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 3) != 0
                        || WasmAppendByte(body, 0x6au) != 0 || WasmAppendByte(body, 0x41u) != 0
                        || WasmAppendSLEB32(body, -4) != 0 || WasmAppendByte(body, 0x71u) != 0
                        || WasmAppendByte(body, 0x6au) != 0 || WasmAppendByte(body, 0x24u) != 0
                        || WasmAppendULEB(body, imports->heapGlobalIndex) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch3Local) != 0)
                    {
                        return -1;
                    }
                    if (WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0
                        || WasmStackPushEx(state, allocType, allocTypeRef) != 0)
                    {
                        return -1;
                    }
                    break;
                }
                if ((inst->tok & SLAstFlag_NEW_HAS_COUNT) != 0u && !useFixedCountAlloc) {
                    uint8_t countType = 0;
                    if (WasmStackPop(state, &countType) != 0
                        || WasmRequireI32Value(
                               countType,
                               diag,
                               inst->start,
                               inst->end,
                               "unsupported dynamic ALLOC_NEW count")
                               != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0)
                    {
                        return -1;
                    }
                    if (hasAllocArg) {
                        if (state->allocCallTempOffset == UINT32_MAX
                            || WasmAppendByte(body, 0x20u) != 0
                            || WasmAppendULEB(body, state->scratch2Local) != 0
                            || WasmAppendByte(body, 0x45u) != 0 || WasmAppendByte(body, 0x04u) != 0
                            || WasmAppendByte(body, 0x40u) != 0)
                        {
                            return -1;
                        }
                        if (isOptional) {
                            if (WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0
                                || WasmAppendByte(body, 0x21u) != 0
                                || WasmAppendULEB(body, state->scratch0Local) != 0)
                            {
                                return -1;
                            }
                        } else {
                            if (imports->hasWasmMinPanic && strings != NULL
                                && strings->hasAllocNullPanicString)
                            {
                                if (WasmAppendByte(body, 0x41u) != 0
                                    || WasmAppendSLEB32(
                                           body, (int32_t)strings->allocNullPanic.dataOffset)
                                           != 0
                                    || WasmAppendByte(body, 0x41u) != 0
                                    || WasmAppendSLEB32(body, (int32_t)strings->allocNullPanic.len)
                                           != 0
                                    || WasmAppendByte(body, 0x10u) != 0
                                    || WasmAppendULEB(body, imports->wasmMinPanicFuncIndex) != 0)
                                {
                                    return -1;
                                }
                            }
                            if (WasmAppendByte(body, 0x00u) != 0) {
                                return -1;
                            }
                        }
                        if (WasmAppendByte(body, 0x05u) != 0
                            || WasmEmitAddrFromFrame(body, state, state->allocCallTempOffset, 0u)
                                   != 0
                            || WasmAppendByte(body, 0x20u) != 0
                            || WasmAppendULEB(body, state->scratch1Local) != 0
                            || WasmEmitI32Store(body) != 0 || WasmAppendByte(body, 0x20u) != 0
                            || WasmAppendULEB(body, state->scratch2Local) != 0
                            || WasmEmitI32Load(body) != 0 || WasmAppendByte(body, 0x21u) != 0
                            || WasmAppendULEB(body, state->scratch4Local) != 0
                            || WasmAppendByte(body, 0x20u) != 0
                            || WasmAppendULEB(body, state->scratch2Local) != 0
                            || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0
                            || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 4) != 0
                            || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0
                            || WasmEmitAddrFromFrame(body, state, state->allocCallTempOffset, 0u)
                                   != 0
                            || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0
                            || WasmAppendByte(body, 0x20u) != 0
                            || WasmAppendULEB(body, state->scratch4Local) != 0
                            || WasmAppendByte(body, 0x11u) != 0
                            || WasmAppendULEB(body, imports->allocatorIndirectTypeIndex) != 0
                            || WasmAppendByte(body, 0x00u) != 0 || WasmAppendByte(body, 0x21u) != 0
                            || WasmAppendULEB(body, state->scratch0Local) != 0)
                        {
                            return -1;
                        }
                        if (WasmAppendByte(body, 0x20u) != 0
                            || WasmAppendULEB(body, state->scratch0Local) != 0
                            || WasmAppendByte(body, 0x45u) != 0 || WasmAppendByte(body, 0x04u) != 0
                            || WasmAppendByte(body, 0x40u) != 0)
                        {
                            return -1;
                        }
                        if (isOptional) {
                            if (WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0
                                || WasmAppendByte(body, 0x21u) != 0
                                || WasmAppendULEB(body, state->scratch0Local) != 0)
                            {
                                return -1;
                            }
                        } else {
                            if (imports->hasWasmMinPanic && strings != NULL
                                && strings->hasAllocNullPanicString)
                            {
                                if (WasmAppendByte(body, 0x41u) != 0
                                    || WasmAppendSLEB32(
                                           body, (int32_t)strings->allocNullPanic.dataOffset)
                                           != 0
                                    || WasmAppendByte(body, 0x41u) != 0
                                    || WasmAppendSLEB32(body, (int32_t)strings->allocNullPanic.len)
                                           != 0
                                    || WasmAppendByte(body, 0x10u) != 0
                                    || WasmAppendULEB(body, imports->wasmMinPanicFuncIndex) != 0)
                                {
                                    return -1;
                                }
                            }
                            if (WasmAppendByte(body, 0x00u) != 0) {
                                return -1;
                            }
                        }
                        if (WasmAppendByte(body, 0x05u) != 0
                            || WasmEmitZeroDynamicLocalAddrRange(
                                   body,
                                   state->scratch0Local,
                                   state->scratch1Local,
                                   state->scratch3Local)
                                   != 0
                            || WasmAppendByte(body, 0x0bu) != 0 || WasmAppendByte(body, 0x0bu) != 0)
                        {
                            return -1;
                        }
                    } else if (
                        WasmAppendByte(body, 0x23u) != 0
                        || WasmAppendULEB(body, imports->heapGlobalIndex) != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0
                        || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 3) != 0
                        || WasmAppendByte(body, 0x6au) != 0 || WasmAppendByte(body, 0x41u) != 0
                        || WasmAppendSLEB32(body, -4) != 0 || WasmAppendByte(body, 0x71u) != 0
                        || WasmAppendByte(body, 0x6au) != 0 || WasmAppendByte(body, 0x24u) != 0
                        || WasmAppendULEB(body, imports->heapGlobalIndex) != 0)
                    {
                        return -1;
                    }
                    if (WasmEmitZeroDynamicLocalAddrRange(
                            body, state->scratch0Local, state->scratch1Local, state->scratch3Local)
                            != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmStackPushEx(state, allocType, allocTypeRef) != 0)
                    {
                        return -1;
                    }
                    break;
                }
                allocSize = WasmAllocatedByteSizeForType(program, allocType, allocTypeRef);
                if (allocSize == 0u || WasmTypeKindIsSlice(allocType)) {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported ALLOC_NEW type";
                    }
                    return -1;
                }
                if (hasAllocArg) {
                    if (state->allocCallTempOffset == UINT32_MAX || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch2Local) != 0
                        || WasmAppendByte(body, 0x45u) != 0 || WasmAppendByte(body, 0x04u) != 0
                        || WasmAppendByte(body, 0x40u) != 0)
                    {
                        return -1;
                    }
                    if (isOptional) {
                        if (WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0
                            || WasmAppendByte(body, 0x21u) != 0
                            || WasmAppendULEB(body, state->scratch0Local) != 0)
                        {
                            return -1;
                        }
                    } else {
                        if (imports->hasWasmMinPanic && strings != NULL
                            && strings->hasAllocNullPanicString)
                        {
                            if (WasmAppendByte(body, 0x41u) != 0
                                || WasmAppendSLEB32(
                                       body, (int32_t)strings->allocNullPanic.dataOffset)
                                       != 0
                                || WasmAppendByte(body, 0x41u) != 0
                                || WasmAppendSLEB32(body, (int32_t)strings->allocNullPanic.len) != 0
                                || WasmAppendByte(body, 0x10u) != 0
                                || WasmAppendULEB(body, imports->wasmMinPanicFuncIndex) != 0)
                            {
                                return -1;
                            }
                        }
                        if (WasmAppendByte(body, 0x00u) != 0) {
                            return -1;
                        }
                    }
                    if (WasmAppendByte(body, 0x05u) != 0
                        || WasmEmitAddrFromFrame(body, state, state->allocCallTempOffset, 0u) != 0
                        || WasmAppendByte(body, 0x41u) != 0
                        || WasmAppendSLEB32(body, (int32_t)allocSize) != 0
                        || WasmEmitI32Store(body) != 0 || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch2Local) != 0
                        || WasmEmitI32Load(body) != 0 || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch3Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch2Local) != 0
                        || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0
                        || WasmAppendByte(body, 0x41u) != 0
                        || WasmAppendSLEB32(
                               body,
                               (int32_t)WasmAllocatedByteAlignForType(
                                   program, allocType, allocTypeRef))
                               != 0
                        || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0
                        || WasmEmitAddrFromFrame(body, state, state->allocCallTempOffset, 0u) != 0
                        || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch3Local) != 0
                        || WasmAppendByte(body, 0x11u) != 0
                        || WasmAppendULEB(body, imports->allocatorIndirectTypeIndex) != 0
                        || WasmAppendByte(body, 0x00u) != 0 || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x45u) != 0 || WasmAppendByte(body, 0x04u) != 0
                        || WasmAppendByte(body, 0x40u) != 0)
                    {
                        return -1;
                    }
                    if (isOptional) {
                        if (WasmAppendByte(body, 0x05u) != 0) {
                            return -1;
                        }
                    } else {
                        if (imports->hasWasmMinPanic && strings != NULL
                            && strings->hasAllocNullPanicString)
                        {
                            if (WasmAppendByte(body, 0x41u) != 0
                                || WasmAppendSLEB32(
                                       body, (int32_t)strings->allocNullPanic.dataOffset)
                                       != 0
                                || WasmAppendByte(body, 0x41u) != 0
                                || WasmAppendSLEB32(body, (int32_t)strings->allocNullPanic.len) != 0
                                || WasmAppendByte(body, 0x10u) != 0
                                || WasmAppendULEB(body, imports->wasmMinPanicFuncIndex) != 0)
                            {
                                return -1;
                            }
                        }
                        if (WasmAppendByte(body, 0x00u) != 0 || WasmAppendByte(body, 0x05u) != 0) {
                            return -1;
                        }
                    }
                    if (WasmEmitZeroLocalAddrRange(body, state->scratch0Local, allocSize) != 0
                        || WasmAppendByte(body, 0x0bu) != 0 || WasmAppendByte(body, 0x0bu) != 0)
                    {
                        return -1;
                    }
                } else if (
                    WasmAppendByte(body, 0x23u) != 0
                    || WasmAppendULEB(body, imports->heapGlobalIndex) != 0
                    || WasmAppendByte(body, 0x21u) != 0
                    || WasmAppendULEB(body, state->scratch0Local) != 0
                    || WasmAppendByte(body, 0x20u) != 0
                    || WasmAppendULEB(body, state->scratch0Local) != 0
                    || WasmAppendByte(body, 0x41u) != 0
                    || WasmAppendSLEB32(body, (int32_t)WasmAlign4(allocSize)) != 0
                    || WasmAppendByte(body, 0x6au) != 0 || WasmAppendByte(body, 0x24u) != 0
                    || WasmAppendULEB(body, imports->heapGlobalIndex) != 0
                    || WasmEmitZeroLocalAddrRange(body, state->scratch0Local, allocSize) != 0)
                {
                    return -1;
                }
                if (WasmAppendByte(body, 0x20u) != 0
                    || WasmAppendULEB(body, state->scratch0Local) != 0
                    || WasmStackPushEx(state, allocType, allocTypeRef) != 0)
                {
                    return -1;
                }
                break;
            }
            case SLMirOp_DROP: {
                uint8_t valueType = 0;
                uint8_t slotCount;
                if (state->stackLen == 0u) {
                    break;
                }
                if (WasmStackPop(state, &valueType) != 0) {
                    return -1;
                }
                slotCount = WasmTypeKindSlotCount(valueType);
                if (slotCount == 0u) {
                    break;
                }
                if (WasmAppendByte(body, 0x1au) != 0) {
                    return -1;
                }
                if (slotCount == 2u && WasmAppendByte(body, 0x1au) != 0) {
                    return -1;
                }
                break;
            }
            case SLMirOp_CALL_FN: {
                uint32_t             argc = SLMirCallArgCountFromTok(inst->tok);
                const SLMirFunction* callee;
                const SLWasmFnSig*   calleeSig;
                uint32_t             argIndex;
                uint32_t             tempOffset = UINT32_MAX;
                if (inst->aux >= program->funcLen) {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_INTERNAL, inst->start, inst->end);
                    return -1;
                }
                callee = &program->funcs[inst->aux];
                calleeSig = &sigs[inst->aux];
                if (SLMirCallTokDropsReceiverArg0(inst->tok) || argc != callee->paramCount) {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported call shape";
                    }
                    return -1;
                }
                for (argIndex = 0; argIndex < argc; argIndex++) {
                    uint8_t argType = 0;
                    if (WasmStackPop(state, &argType) != 0) {
                        return -1;
                    }
                    if (argType != calleeSig->logicalParamKinds[argc - 1u - argIndex]) {
                        WasmSetDiag(
                            diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                        if (diag != NULL) {
                            diag->detail = "call argument type mismatch";
                        }
                        return -1;
                    }
                }
                if (calleeSig->usesSRet) {
                    if (!state->usesFrame
                        || WasmTempOffsetForPc(program, fn, state, pc, &tempOffset) != 0
                        || WasmEmitAddrFromFrame(body, state, tempOffset, 0u) != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0)
                    {
                        WasmSetDiag(
                            diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                        if (diag != NULL && diag->detail == NULL) {
                            diag->detail = "unsupported call shape";
                        }
                        return -1;
                    }
                }
                if (WasmAppendByte(body, 0x10u) != 0
                    || WasmAppendULEB(body, imports->importFuncCount + inst->aux) != 0)
                {
                    return -1;
                }
                if (calleeSig->usesSRet) {
                    if (WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmStackPushEx(
                               state, calleeSig->logicalResultKind, calleeSig->logicalResultTypeRef)
                               != 0)
                    {
                        return -1;
                    }
                } else if (
                    calleeSig->logicalResultKind != SLWasmType_VOID
                    && WasmStackPushEx(
                           state, calleeSig->logicalResultKind, calleeSig->logicalResultTypeRef)
                           != 0)
                {
                    return -1;
                }
                break;
            }
            case SLMirOp_CALL_INDIRECT: {
                uint32_t           argc = SLMirCallArgCountFromTok(inst->tok);
                const SLWasmFnSig* calleeSig = NULL;
                uint32_t           calleeTypeRef = UINT32_MAX;
                uint32_t           calleeFuncIndex = UINT32_MAX;
                uint32_t           totalArgSlots = 0u;
                uint32_t           argIndex;
                uint32_t           scratchIndex;
                uint8_t            calleeType = 0;
                uint32_t           ignoredTypeRef = UINT32_MAX;
                if (imports == NULL || !imports->hasFunctionTable
                    || SLMirCallTokDropsReceiverArg0(inst->tok) || state->stackLen < argc + 1u)
                {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported indirect call shape";
                    }
                    return -1;
                }
                calleeTypeRef = state->stackTypeRefs[state->stackLen - argc - 1u];
                if (calleeTypeRef < program->typeLen) {
                    calleeFuncIndex = SLMirTypeRefFuncRefFunctionIndex(
                        &program->types[calleeTypeRef]);
                    if (calleeFuncIndex < program->funcLen) {
                        calleeSig = &sigs[calleeFuncIndex];
                    }
                }
                if (calleeSig != NULL) {
                    if (argc != calleeSig->logicalParamCount || calleeSig->usesSRet) {
                        WasmSetDiag(
                            diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                        if (diag != NULL) {
                            diag->detail = "unsupported indirect call shape";
                        }
                        return -1;
                    }
                    for (argIndex = 0; argIndex < argc; argIndex++) {
                        totalArgSlots += WasmTypeKindSlotCount(
                            calleeSig->logicalParamKinds[argIndex]);
                    }
                    if (totalArgSlots + 1u > 7u) {
                        WasmSetDiag(
                            diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                        if (diag != NULL) {
                            diag->detail = "unsupported indirect call shape";
                        }
                        return -1;
                    }
                    scratchIndex = totalArgSlots;
                    for (argIndex = argc; argIndex > 0u; argIndex--) {
                        uint8_t  argType = 0;
                        uint32_t slotCount = WasmTypeKindSlotCount(
                            calleeSig->logicalParamKinds[argIndex - 1u]);
                        if (WasmStackPop(state, &argType) != 0
                            || argType != calleeSig->logicalParamKinds[argIndex - 1u])
                        {
                            WasmSetDiag(
                                diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                            if (diag != NULL) {
                                diag->detail = "call argument type mismatch";
                            }
                            return -1;
                        }
                        scratchIndex -= slotCount;
                        if (slotCount == 2u) {
                            if (WasmAppendByte(body, 0x21u) != 0
                                || WasmAppendULEB(body, state->scratch0Local + scratchIndex + 1u)
                                       != 0
                                || WasmAppendByte(body, 0x21u) != 0
                                || WasmAppendULEB(body, state->scratch0Local + scratchIndex) != 0)
                            {
                                return -1;
                            }
                        } else if (
                            WasmAppendByte(body, 0x21u) != 0
                            || WasmAppendULEB(body, state->scratch0Local + scratchIndex) != 0)
                        {
                            return -1;
                        }
                    }
                    if (WasmStackPopEx(state, &calleeType, &ignoredTypeRef) != 0
                        || calleeType != SLWasmType_FUNC_REF || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch0Local + totalArgSlots) != 0)
                    {
                        return -1;
                    }
                    for (scratchIndex = 0u; scratchIndex < totalArgSlots; scratchIndex++) {
                        if (WasmAppendByte(body, 0x20u) != 0
                            || WasmAppendULEB(body, state->scratch0Local + scratchIndex) != 0)
                        {
                            return -1;
                        }
                    }
                    if (WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local + totalArgSlots) != 0
                        || WasmAppendByte(body, 0x11u) != 0
                        || WasmAppendULEB(body, calleeSig->typeIndex) != 0
                        || WasmAppendByte(body, 0x00u) != 0)
                    {
                        return -1;
                    }
                    if (calleeSig->logicalResultKind != SLWasmType_VOID
                        && WasmStackPushEx(
                               state, calleeSig->logicalResultKind, calleeSig->logicalResultTypeRef)
                               != 0)
                    {
                        return -1;
                    }
                    break;
                }
                if (!state->usesFrame || argc != 6u) {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported indirect call shape";
                    }
                    return -1;
                }
                {
                    uint8_t valueType = 0;
                    if (WasmStackPop(state, &valueType) != 0
                        || WasmRequireI32Value(
                               valueType,
                               diag,
                               inst->start,
                               inst->end,
                               "allocator flags must be i32")
                               != 0
                        || WasmStackPop(state, &valueType) != 0
                        || WasmRequirePointerValue(
                               valueType,
                               diag,
                               inst->start,
                               inst->end,
                               "allocator newSizeInOut must be a pointer")
                               != 0
                        || WasmStackPop(state, &valueType) != 0
                        || WasmRequireI32Value(
                               valueType,
                               diag,
                               inst->start,
                               inst->end,
                               "allocator curSize must be i32")
                               != 0
                        || WasmStackPop(state, &valueType) != 0
                        || WasmRequireI32Value(
                               valueType,
                               diag,
                               inst->start,
                               inst->end,
                               "allocator align must be i32")
                               != 0
                        || WasmStackPop(state, &valueType) != 0
                        || WasmRequireI32Value(
                               valueType,
                               diag,
                               inst->start,
                               inst->end,
                               "allocator addr must be i32")
                               != 0
                        || WasmStackPop(state, &valueType) != 0
                        || WasmRequireAllocatorValue(
                               valueType,
                               diag,
                               inst->start,
                               inst->end,
                               "allocator self must be pointer-like")
                               != 0
                        || WasmStackPop(state, &valueType) != 0 || valueType != SLWasmType_FUNC_REF)
                    {
                        if (diag != NULL && diag->code == SLDiag_NONE) {
                            WasmSetDiag(
                                diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                            diag->detail = "allocator indirect callee must be a function value";
                        }
                        return -1;
                    }
                    if (WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch6Local) != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch5Local) != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch4Local) != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch3Local) != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch2Local) != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch2Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch3Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch4Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch5Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch6Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x11u) != 0
                        || WasmAppendULEB(body, imports->allocatorIndirectTypeIndex) != 0
                        || WasmAppendByte(body, 0x00u) != 0
                        || WasmStackPush(state, SLWasmType_I32) != 0)
                    {
                        return -1;
                    }
                }
                break;
            }
            case SLMirOp_CALL_HOST: {
                uint32_t            argc = SLMirCallArgCountFromTok(inst->tok);
                const SLMirHostRef* host;
                uint8_t             argType = 0;
                uint8_t             msgType = 0;
                if (inst->aux >= program->hostLen) {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_INTERNAL, inst->start, inst->end);
                    return -1;
                }
                host = &program->hosts[inst->aux];
                if (host->kind == SLMirHost_GENERIC
                    && host->target == SLMirHostTarget_PLATFORM_EXIT)
                {
                    if (imports == NULL || !imports->hasWasmMinExit || argc != 1u
                        || SLMirCallTokDropsReceiverArg0(inst->tok))
                    {
                        WasmSetDiag(
                            diag, SLDiag_WASM_BACKEND_UNSUPPORTED_HOSTCALL, inst->start, inst->end);
                        if (diag != NULL) {
                            diag->detail = "unsupported wasm-min platform.exit call shape";
                        }
                        return -1;
                    }
                    if (WasmStackPop(state, &argType) != 0
                        || WasmRequireI32Value(
                               argType, diag, inst->start, inst->end, "platform.exit expects i32")
                               != 0
                        || WasmAppendByte(body, 0x10u) != 0
                        || WasmAppendULEB(body, imports->wasmMinExitFuncIndex) != 0
                        || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0
                        || WasmStackPush(state, SLWasmType_I32) != 0)
                    {
                        return -1;
                    }
                    break;
                }
                if (host->kind == SLMirHost_GENERIC
                    && host->target == SLMirHostTarget_PLATFORM_CONSOLE_LOG)
                {
                    if (imports == NULL || strings == NULL || !imports->hasWasmMinConsoleLog
                        || argc != 2u || SLMirCallTokDropsReceiverArg0(inst->tok))
                    {
                        WasmSetDiag(
                            diag, SLDiag_WASM_BACKEND_UNSUPPORTED_HOSTCALL, inst->start, inst->end);
                        if (diag != NULL) {
                            diag->detail = "unsupported wasm-min platform.console_log call shape";
                        }
                        return -1;
                    }
                    if (WasmStackPop(state, &argType) != 0 || WasmStackPop(state, &msgType) != 0
                        || WasmRequireI32Value(
                               argType,
                               diag,
                               inst->start,
                               inst->end,
                               "platform.console_log flags must be i32")
                               != 0)
                    {
                        return -1;
                    }
                    if (msgType != SLWasmType_STR_REF) {
                        WasmSetDiag(
                            diag, SLDiag_WASM_BACKEND_UNSUPPORTED_HOSTCALL, inst->start, inst->end);
                        if (diag != NULL) {
                            diag->detail = "platform.console_log expects &str";
                        }
                        return -1;
                    }
                    if (WasmAppendByte(body, 0x10u) != 0
                        || WasmAppendULEB(body, imports->wasmMinConsoleLogFuncIndex) != 0
                        || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0
                        || WasmStackPush(state, SLWasmType_I32) != 0)
                    {
                        return -1;
                    }
                    break;
                }
                WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_HOSTCALL, inst->start, inst->end);
                if (diag != NULL) {
                    diag->detail = "host calls are not supported in this Wasm mode";
                }
                return -1;
            }
            case SLMirOp_ASSERT: {
                uint8_t condType = 0;
                if (WasmStackPop(state, &condType) != 0
                    || WasmRequireI32Value(
                           condType, diag, inst->start, inst->end, "non-scalar assert condition")
                           != 0)
                {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "non-scalar assert condition";
                    }
                    return -1;
                }
                if (WasmAppendByte(body, 0x45u) != 0 || WasmAppendByte(body, 0x04u) != 0
                    || WasmAppendByte(body, 0x40u) != 0)
                {
                    return -1;
                }
                if (imports != NULL && imports->hasWasmMinPanic && strings != NULL
                    && strings->hasAssertPanicString)
                {
                    if (WasmAppendByte(body, 0x41u) != 0
                        || WasmAppendSLEB32(body, (int32_t)strings->assertPanic.dataOffset) != 0
                        || WasmAppendByte(body, 0x41u) != 0
                        || WasmAppendSLEB32(body, (int32_t)strings->assertPanic.len) != 0
                        || WasmAppendByte(body, 0x10u) != 0
                        || WasmAppendULEB(body, imports->wasmMinPanicFuncIndex) != 0
                        || WasmAppendByte(body, 0x00u) != 0)
                    {
                        return -1;
                    }
                } else if (WasmAppendByte(body, 0x00u) != 0) {
                    return -1;
                }
                if (WasmAppendByte(body, 0x0bu) != 0) {
                    return -1;
                }
                break;
            }
            case SLMirOp_JUMP_IF_FALSE: {
                uint8_t             condType = 0;
                uint32_t            falseTarget = inst->aux;
                uint32_t            thenEnd = falseTarget;
                uint32_t            mergeTarget = falseTarget;
                SLWasmBranchTargets nestedTargets = WasmNestedBranchTargets(branchTargets);
                uint32_t            stackDepthBefore;
                uint32_t            branchDepth = 0;
                int                 hasElse = 0;
                if (falseTarget <= pc + 1u || falseTarget > endPc) {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported control-flow graph shape";
                    }
                    return -1;
                }
                if (WasmStackPop(state, &condType) != 0 || condType != SLWasmType_I32) {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported branch condition type";
                    }
                    return -1;
                }
                if (falseTarget > pc + 1u) {
                    const SLMirInst* maybeElseJump =
                        &program->insts[fn->instStart + falseTarget - 1u];
                    if (maybeElseJump->op == SLMirOp_JUMP) {
                        if (WasmBranchDepthForJump(branchTargets, maybeElseJump->aux, &branchDepth))
                        {
                            thenEnd = falseTarget;
                        } else if (maybeElseJump->aux <= falseTarget) {
                            WasmSetDiag(
                                diag,
                                SLDiag_WASM_BACKEND_UNSUPPORTED_MIR,
                                maybeElseJump->start,
                                maybeElseJump->end);
                            if (diag != NULL) {
                                diag->detail = "loops/backedges are not supported";
                            }
                            return -1;
                        } else if (maybeElseJump->aux > endPc) {
                            WasmSetDiag(
                                diag,
                                SLDiag_WASM_BACKEND_UNSUPPORTED_MIR,
                                maybeElseJump->start,
                                maybeElseJump->end);
                            if (diag != NULL) {
                                diag->detail = "unsupported control-flow graph shape";
                            }
                            return -1;
                        } else {
                            hasElse = 1;
                            thenEnd = falseTarget - 1u;
                            mergeTarget = maybeElseJump->aux;
                        }
                    }
                }
                stackDepthBefore = state->stackLen;
                if (WasmAppendByte(body, 0x04u) != 0 || WasmAppendByte(body, 0x40u) != 0) {
                    return -1;
                }
                if (WasmEmitFunctionRange(
                        program,
                        sigs,
                        imports,
                        strings,
                        fn,
                        body,
                        state,
                        &nestedTargets,
                        resultKind,
                        pc + 1u,
                        thenEnd,
                        diag)
                    != 0)
                {
                    return -1;
                }
                if (state->stackLen != stackDepthBefore) {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "if branch stack mismatch";
                    }
                    return -1;
                }
                if (hasElse) {
                    if (WasmAppendByte(body, 0x05u) != 0) {
                        return -1;
                    }
                    if (WasmEmitFunctionRange(
                            program,
                            sigs,
                            imports,
                            strings,
                            fn,
                            body,
                            state,
                            &nestedTargets,
                            resultKind,
                            falseTarget,
                            mergeTarget,
                            diag)
                        != 0)
                    {
                        return -1;
                    }
                    if (state->stackLen != stackDepthBefore) {
                        WasmSetDiag(
                            diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                        if (diag != NULL) {
                            diag->detail = "else branch stack mismatch";
                        }
                        return -1;
                    }
                }
                if (WasmAppendByte(body, 0x0bu) != 0) {
                    return -1;
                }
                pc = mergeTarget;
                continue;
            }
            case SLMirOp_JUMP: {
                uint32_t branchDepth = 0;
                if (WasmBranchDepthForJump(branchTargets, inst->aux, &branchDepth)) {
                    if (WasmAppendByte(body, 0x0cu) != 0 || WasmAppendULEB(body, branchDepth) != 0)
                    {
                        return -1;
                    }
                    break;
                }
                WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                if (diag != NULL) {
                    diag->detail = inst->aux > pc ? "unsupported forward jump shape"
                                                  : "loops/backedges are not supported";
                }
                return -1;
            }
            case SLMirOp_RETURN: {
                uint8_t  returnType = 0;
                uint32_t returnTypeRef = UINT32_MAX;
                if (resultKind == SLWasmType_VOID) {
                    WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "RETURN used in void function";
                    }
                    return -1;
                }
                if (WasmStackPopEx(state, &returnType, &returnTypeRef) != 0) {
                    return -1;
                }
                if (WasmTypeKindUsesSRet(resultKind)) {
                    uint32_t copySize;
                    if (returnType != resultKind || returnTypeRef == UINT32_MAX
                        || !WasmTypeRefsCompatible(program, fn->typeRef, returnTypeRef))
                    {
                        if (diag != NULL && diag->code == 0) {
                            WasmSetDiag(
                                diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                            diag->detail = "unsupported return value";
                        }
                        return -1;
                    }
                    copySize = (uint32_t)WasmTypeByteSize(program, fn->typeRef);
                    if (!state->usesFrame || copySize == 0u || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmEmitCopyLocalAddrToLocalAddr(
                               body, state->scratch0Local, state->wasmParamValueCount, 0u, copySize)
                               != 0
                        || WasmEmitRestoreFrameAndReturn(body, imports, state, SLWasmType_VOID)
                               != 0)
                    {
                        return -1;
                    }
                    break;
                }
                if (returnType != resultKind) {
                    if (diag != NULL && diag->code == 0) {
                        WasmSetDiag(
                            diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                        diag->detail = "unsupported return value";
                    }
                    return -1;
                }
                if (state->usesFrame) {
                    if (WasmEmitRestoreFrameAndReturn(body, imports, state, resultKind) != 0) {
                        return -1;
                    }
                } else if (WasmAppendByte(body, 0x0fu) != 0) {
                    return -1;
                }
                break;
            }
            case SLMirOp_RETURN_VOID:
                if (resultKind != SLWasmType_VOID) {
                    break;
                }
                if (state->usesFrame) {
                    if (WasmEmitRestoreFrameAndReturn(body, imports, state, SLWasmType_VOID) != 0) {
                        return -1;
                    }
                } else if (WasmAppendByte(body, 0x0fu) != 0) {
                    return -1;
                }
                break;
            case SLMirOp_LOAD_IDENT:
            case SLMirOp_CALL:
                WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                if (diag != NULL) {
                    diag->detail = "MIR still needs dynamic resolution";
                }
                return -1;
            default:
                WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                if (diag != NULL) {
                    diag->detail = WasmMirOpName(inst->op);
                }
                return -1;
        }
        pc++;
    }
    return 0;
}

static int WasmEmitFunctionBody(
    const SLMirProgram*       program,
    const SLWasmFnSig*        sigs,
    const SLWasmImportLayout* imports,
    const SLWasmStringLayout* strings,
    uint32_t                  funcIndex,
    SLWasmBuf*                body,
    SLDiag* _Nullable diag) {
    const SLMirFunction* fn;
    SLWasmEmitState      state;
    uint32_t             localDeclCount = 0;
    uint32_t             nonParamLocalCount;
    uint8_t              resultKind = SLWasmType_VOID;
    enum {
        kWasmFunctionBodyLimit = 4u * 1024u * 1024u,
    };

    if (program == NULL || sigs == NULL || body == NULL || funcIndex >= program->funcLen) {
        return -1;
    }
    fn = &program->funcs[funcIndex];
    body->diag = diag;
    body->maxLen = kWasmFunctionBodyLimit;
    body->limitStart = fn->nameStart;
    body->limitEnd = fn->nameEnd;
    body->limitDetail = "emitted Wasm function body exceeds size limit";
    if (WasmPrepareFunctionState(program, fn, &state, diag) != 0) {
        return -1;
    }
    if (!WasmTypeKindFromMirType(program, fn->typeRef, &resultKind)) {
        WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, fn->nameStart, fn->nameEnd);
        return -1;
    }

    nonParamLocalCount =
        state.usesFrame ? 8u : (uint32_t)(state.wasmLocalValueCount - state.wasmParamValueCount);
    if (nonParamLocalCount != 0u) {
        localDeclCount = 1u;
    }
    if (WasmAppendULEB(body, localDeclCount) != 0) {
        return -1;
    }
    if (localDeclCount != 0) {
        if (WasmAppendULEB(body, nonParamLocalCount) != 0 || WasmAppendByte(body, 0x7fu) != 0) {
            return -1;
        }
    }
    if (state.usesFrame) {
        uint32_t paramIndex;
        if (WasmAppendByte(body, 0x23u) != 0 || WasmAppendULEB(body, imports->frameGlobalIndex) != 0
            || WasmAppendByte(body, 0x21u) != 0 || WasmAppendULEB(body, state.frameBaseLocal) != 0
            || WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, state.frameBaseLocal) != 0
            || WasmAppendByte(body, 0x41u) != 0
            || WasmAppendSLEB32(body, (int32_t)state.frameSize) != 0
            || WasmAppendByte(body, 0x6au) != 0 || WasmAppendByte(body, 0x24u) != 0
            || WasmAppendULEB(body, imports->frameGlobalIndex) != 0)
        {
            return -1;
        }
        for (paramIndex = 0; paramIndex < fn->paramCount; paramIndex++) {
            uint8_t typeKind = state.localKinds[paramIndex];
            if (WasmTypeKindSlotCount(typeKind) == 2u) {
                if (WasmEmitAddrFromFrame(body, &state, state.frameOffsets[paramIndex], 0u) != 0
                    || WasmAppendByte(body, 0x20u) != 0
                    || WasmAppendULEB(body, state.wasmValueIndex[paramIndex]) != 0
                    || WasmEmitI32Store(body) != 0
                    || WasmEmitAddrFromFrame(body, &state, state.frameOffsets[paramIndex], 4u) != 0
                    || WasmAppendByte(body, 0x20u) != 0
                    || WasmAppendULEB(body, state.wasmValueIndex[paramIndex] + 1u) != 0
                    || WasmEmitI32Store(body) != 0)
                {
                    return -1;
                }
            } else {
                if (WasmEmitAddrFromFrame(body, &state, state.frameOffsets[paramIndex], 0u) != 0
                    || WasmAppendByte(body, 0x20u) != 0
                    || WasmAppendULEB(body, state.wasmValueIndex[paramIndex]) != 0
                    || WasmEmitI32Store(body) != 0)
                {
                    return -1;
                }
            }
        }
    }

    if (WasmEmitFunctionRange(
            program,
            sigs,
            imports,
            strings,
            fn,
            body,
            &state,
            &(SLWasmBranchTargets){ 0 },
            resultKind,
            0,
            fn->instLen,
            diag)
        != 0)
    {
        return -1;
    }
    if (WasmAppendByte(body, 0x0bu) != 0) {
        return -1;
    }
    return 0;
}

static int WasmEmitRootAllocThunkBody(
    const SLWasmImportLayout* imports, const SLCodegenOptions* options, SLWasmBuf* body) {
    enum {
        kParamSelf = 0,
        kParamAddr = 1,
        kParamAlign = 2,
        kParamCurSize = 3,
        kParamNewSizePtr = 4,
        kParamFlags = 5,
        kLocalBase = 6,
        kLocalSize = 7,
        kLocalCursor = 8,
        kLocalAlign = 9,
    };
    if (imports == NULL || options == NULL || body == NULL || !imports->needsHeapGlobal) {
        return -1;
    }
    body->options = options;
    if (WasmAppendULEB(body, 1u) != 0 || WasmAppendULEB(body, 4u) != 0
        || WasmAppendByte(body, 0x7fu) != 0)
    {
        return -1;
    }

    if (WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, kParamNewSizePtr) != 0
        || WasmAppendByte(body, 0x45u) != 0 || WasmAppendByte(body, 0x04u) != 0
        || WasmAppendByte(body, 0x40u) != 0 || WasmAppendByte(body, 0x41u) != 0
        || WasmAppendSLEB32(body, 0) != 0 || WasmAppendByte(body, 0x0fu) != 0
        || WasmAppendByte(body, 0x0bu) != 0)
    {
        return -1;
    }
    if (WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, kParamAddr) != 0
        || WasmAppendByte(body, 0x04u) != 0 || WasmAppendByte(body, 0x40u) != 0
        || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0
        || WasmAppendByte(body, 0x0fu) != 0 || WasmAppendByte(body, 0x0bu) != 0)
    {
        return -1;
    }
    if (WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, kParamCurSize) != 0
        || WasmAppendByte(body, 0x04u) != 0 || WasmAppendByte(body, 0x40u) != 0
        || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0
        || WasmAppendByte(body, 0x0fu) != 0 || WasmAppendByte(body, 0x0bu) != 0)
    {
        return -1;
    }
    if (WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, kParamFlags) != 0
        || WasmAppendByte(body, 0x04u) != 0 || WasmAppendByte(body, 0x40u) != 0
        || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0
        || WasmAppendByte(body, 0x0fu) != 0 || WasmAppendByte(body, 0x0bu) != 0)
    {
        return -1;
    }
    if (WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, kParamAlign) != 0
        || WasmAppendByte(body, 0x45u) != 0 || WasmAppendByte(body, 0x04u) != 0
        || WasmAppendByte(body, 0x40u) != 0 || WasmAppendByte(body, 0x41u) != 0
        || WasmAppendSLEB32(body, 0) != 0 || WasmAppendByte(body, 0x0fu) != 0
        || WasmAppendByte(body, 0x0bu) != 0)
    {
        return -1;
    }

    if (WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, kParamNewSizePtr) != 0
        || WasmEmitI32Load(body) != 0 || WasmAppendByte(body, 0x21u) != 0
        || WasmAppendULEB(body, kLocalSize) != 0)
    {
        return -1;
    }
    if (WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, kLocalSize) != 0
        || WasmAppendByte(body, 0x45u) != 0 || WasmAppendByte(body, 0x04u) != 0
        || WasmAppendByte(body, 0x40u) != 0 || WasmAppendByte(body, 0x41u) != 0
        || WasmAppendSLEB32(body, 0) != 0 || WasmAppendByte(body, 0x0fu) != 0
        || WasmAppendByte(body, 0x0bu) != 0)
    {
        return -1;
    }

    if (WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, kParamAlign) != 0
        || WasmAppendByte(body, 0x21u) != 0 || WasmAppendULEB(body, kLocalAlign) != 0
        || WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, kLocalAlign) != 0
        || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 4) != 0
        || WasmAppendByte(body, 0x49u) != 0 || WasmAppendByte(body, 0x04u) != 0
        || WasmAppendByte(body, 0x40u) != 0 || WasmAppendByte(body, 0x41u) != 0
        || WasmAppendSLEB32(body, 4) != 0 || WasmAppendByte(body, 0x21u) != 0
        || WasmAppendULEB(body, kLocalAlign) != 0 || WasmAppendByte(body, 0x0bu) != 0)
    {
        return -1;
    }

    if (WasmAppendByte(body, 0x23u) != 0 || WasmAppendULEB(body, imports->heapGlobalIndex) != 0
        || WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, kLocalAlign) != 0
        || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 1) != 0
        || WasmAppendByte(body, 0x6bu) != 0 || WasmAppendByte(body, 0x6au) != 0
        || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0
        || WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, kLocalAlign) != 0
        || WasmAppendByte(body, 0x6bu) != 0 || WasmAppendByte(body, 0x71u) != 0
        || WasmAppendByte(body, 0x21u) != 0 || WasmAppendULEB(body, kLocalBase) != 0)
    {
        return -1;
    }
    if (WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, kLocalBase) != 0
        || WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, kLocalSize) != 0
        || WasmAppendByte(body, 0x6au) != 0 || WasmAppendByte(body, 0x24u) != 0
        || WasmAppendULEB(body, imports->heapGlobalIndex) != 0)
    {
        return -1;
    }
    if (WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, kLocalBase) != 0
        || WasmAppendByte(body, 0x21u) != 0 || WasmAppendULEB(body, kLocalCursor) != 0)
    {
        return -1;
    }
    if (WasmAppendByte(body, 0x02u) != 0 || WasmAppendByte(body, 0x40u) != 0
        || WasmAppendByte(body, 0x03u) != 0 || WasmAppendByte(body, 0x40u) != 0)
    {
        return -1;
    }
    if (WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, kLocalCursor) != 0
        || WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, kLocalBase) != 0
        || WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, kLocalSize) != 0
        || WasmAppendByte(body, 0x6au) != 0 || WasmAppendByte(body, 0x4fu) != 0
        || WasmAppendByte(body, 0x0du) != 0 || WasmAppendULEB(body, 1u) != 0)
    {
        return -1;
    }
    if (WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, kLocalCursor) != 0
        || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0
        || WasmEmitU8Store(body) != 0 || WasmAppendByte(body, 0x20u) != 0
        || WasmAppendULEB(body, kLocalCursor) != 0 || WasmAppendByte(body, 0x41u) != 0
        || WasmAppendSLEB32(body, 1) != 0 || WasmAppendByte(body, 0x6au) != 0
        || WasmAppendByte(body, 0x21u) != 0 || WasmAppendULEB(body, kLocalCursor) != 0
        || WasmAppendByte(body, 0x0cu) != 0 || WasmAppendULEB(body, 0u) != 0
        || WasmAppendByte(body, 0x0bu) != 0 || WasmAppendByte(body, 0x0bu) != 0)
    {
        return -1;
    }
    if (WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, kLocalBase) != 0
        || WasmAppendByte(body, 0x0bu) != 0)
    {
        return -1;
    }
    return 0;
}

static int WasmEmitEntryWrapperBody(
    const SLMirProgram*       program,
    const SLWasmFnSig*        sigs,
    const SLWasmImportLayout* imports,
    const SLWasmEntryLayout*  entry,
    const SLCodegenOptions*   options,
    SLWasmBuf*                body,
    SLDiag* _Nullable diag) {
    const SLWasmFnSig* sig;
    uint32_t           mainFuncWasmIndex;
    if (program == NULL || sigs == NULL || imports == NULL || entry == NULL || options == NULL
        || body == NULL || !entry->hasWrapper || entry->mainFuncIndex >= program->funcLen)
    {
        return -1;
    }
    sig = &sigs[entry->mainFuncIndex];
    mainFuncWasmIndex = imports->importFuncCount + entry->mainFuncIndex;
    body->options = options;
    if (entry->usesSRet) {
        if (WasmAppendULEB(body, 0u) != 0 || WasmAppendByte(body, 0x41u) != 0
            || WasmAppendSLEB32(body, (int32_t)entry->resultOffset) != 0
            || WasmAppendByte(body, 0x10u) != 0 || WasmAppendULEB(body, mainFuncWasmIndex) != 0
            || WasmAppendByte(body, 0x0bu) != 0)
        {
            return -1;
        }
        return 0;
    }
    if (WasmTypeKindSlotCount(sig->logicalResultKind) == 1u) {
        if (WasmAppendULEB(body, 1u) != 0 || WasmAppendULEB(body, 1u) != 0
            || WasmAppendByte(body, 0x7fu) != 0 || WasmAppendByte(body, 0x10u) != 0
            || WasmAppendULEB(body, mainFuncWasmIndex) != 0 || WasmAppendByte(body, 0x21u) != 0
            || WasmAppendULEB(body, 0u) != 0 || WasmAppendByte(body, 0x41u) != 0
            || WasmAppendSLEB32(body, (int32_t)entry->resultOffset) != 0
            || WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, 0u) != 0
            || WasmEmitI32Store(body) != 0 || WasmAppendByte(body, 0x0bu) != 0)
        {
            return -1;
        }
        return 0;
    }
    if (WasmTypeKindSlotCount(sig->logicalResultKind) != 2u) {
        WasmSetDiag(
            diag,
            SLDiag_WASM_BACKEND_UNSUPPORTED_MIR,
            program->funcs[entry->mainFuncIndex].nameStart,
            program->funcs[entry->mainFuncIndex].nameEnd);
        if (diag != NULL) {
            diag->detail = "unsupported entry wrapper result kind";
        }
        return -1;
    }
    if (WasmAppendULEB(body, 1u) != 0 || WasmAppendULEB(body, 2u) != 0
        || WasmAppendByte(body, 0x7fu) != 0 || WasmAppendByte(body, 0x10u) != 0
        || WasmAppendULEB(body, mainFuncWasmIndex) != 0 || WasmAppendByte(body, 0x21u) != 0
        || WasmAppendULEB(body, 1u) != 0 || WasmAppendByte(body, 0x21u) != 0
        || WasmAppendULEB(body, 0u) != 0 || WasmAppendByte(body, 0x41u) != 0
        || WasmAppendSLEB32(body, (int32_t)entry->resultOffset) != 0
        || WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, 0u) != 0
        || WasmEmitI32Store(body) != 0 || WasmAppendByte(body, 0x41u) != 0
        || WasmAppendSLEB32(body, (int32_t)(entry->resultOffset + 4u)) != 0
        || WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, 1u) != 0
        || WasmEmitI32Store(body) != 0 || WasmAppendByte(body, 0x0bu) != 0)
    {
        return -1;
    }
    return 0;
}

static int EmitWasmBackend(
    const SLCodegenBackend* backend,
    const SLCodegenUnit*    unit,
    const SLCodegenOptions* _Nullable options,
    SLCodegenArtifact* _Nonnull outArtifact,
    SLDiag* _Nullable diag) {
    SLWasmBuf out = { 0 };
    SLWasmBuf typeSec = { 0 };
    SLWasmBuf importSec = { 0 };
    SLWasmBuf funcSec = { 0 };
    SLWasmBuf tableSec = { 0 };
    SLWasmBuf memSec = { 0 };
    SLWasmBuf globalSec = { 0 };
    SLWasmBuf exportSec = { 0 };
    SLWasmBuf elemSec = { 0 };
    SLWasmBuf codeSec = { 0 };
    SLWasmBuf dataSec = { 0 };
    SLWasmFnSig* _Nullable sigs = NULL;
    SLWasmStringLayout strings = { 0 };
    SLWasmImportLayout imports = { 0 };
    SLWasmEntryLayout  entry = { 0 };
    uint32_t           i;
    uint32_t           exportCount = 1u;
    const char         wasmHeader[8] = { '\0', 'a', 's', 'm', 0x01, 0x00, 0x00, 0x00 };
    (void)backend;

    if (diag != NULL) {
        *diag = (SLDiag){ 0 };
    }
    *outArtifact = (SLCodegenArtifact){ 0 };
    if (unit == NULL || outArtifact == NULL || options == NULL || options->arenaGrow == NULL) {
        WasmSetDiag(diag, SLDiag_WASM_BACKEND_INTERNAL, 0, 0);
        return -1;
    }
    if (unit->usesPlatform && !WasmIsWasmMinPlatform(unit)) {
        WasmSetDiag(diag, SLDiag_WASM_BACKEND_PLATFORM_REQUIRED, 0, 0);
        if (diag != NULL) {
            diag->detail = "packages using import \"platform\" require --platform wasm-min";
        }
        return -1;
    }
    if (unit->mirProgram == NULL) {
        WasmSetDiag(diag, SLDiag_WASM_BACKEND_INTERNAL, 0, 0);
        if (diag != NULL) {
            diag->detail = "missing MIR program";
        }
        return -1;
    }
    if (SLMirProgramNeedsDynamicResolution(unit->mirProgram)) {
        WasmSetDiag(diag, SLDiag_WASM_BACKEND_UNSUPPORTED_MIR, 0, 0);
        if (diag != NULL) {
            diag->detail = "MIR still needs dynamic resolution";
        }
        return -1;
    }

    out.unit = unit;
    out.options = options;
    out.diag = diag;
    typeSec.options = options;
    importSec.options = options;
    funcSec.options = options;
    tableSec.options = options;
    memSec.options = options;
    globalSec.options = options;
    exportSec.options = options;
    elemSec.options = options;
    codeSec.options = options;
    dataSec.options = options;

    if (unit->mirProgram->funcLen > 0) {
        uint32_t allocSize = 0;
        sigs = (SLWasmFnSig*)options->arenaGrow(
            options->allocatorCtx, unit->mirProgram->funcLen * (uint32_t)sizeof(*sigs), &allocSize);
        if (sigs == NULL || allocSize < unit->mirProgram->funcLen * sizeof(*sigs)) {
            if (sigs != NULL && options->arenaFree != NULL) {
                options->arenaFree(options->allocatorCtx, sigs, allocSize);
            }
            WasmSetDiag(diag, SLDiag_ARENA_OOM, 0, 0);
            return -1;
        }
    }
    if (WasmBuildFunctionSignatures(unit->mirProgram, sigs, diag) != 0) {
        goto fail;
    }
    if (WasmAnalyzeImports(unit, &imports, diag) != 0) {
        goto fail;
    }
    if (imports.hasFunctionTable) {
        for (i = 0; i < unit->mirProgram->funcLen; i++) {
            if (WasmSigMatchesAllocatorIndirect(&sigs[i])) {
                sigs[i].typeIndex = imports.allocatorIndirectTypeIndex;
            }
        }
    }
    if (WasmBuildStringLayout(unit, &imports, options, &strings, diag) != 0) {
        goto fail;
    }
    if (WasmPlanEntryLayout(unit->mirProgram, sigs, &imports, &strings, &entry, diag) != 0) {
        goto fail;
    }

    if (WasmAppendBytes(&out, wasmHeader, sizeof(wasmHeader)) != 0) {
        goto oom;
    }

    if (WasmAppendULEB(
            &typeSec,
            unit->mirProgram->funcLen + imports.importFuncCount
                + (imports.hasFunctionTable ? 1u : 0u) + (entry.hasWrapper ? 1u : 0u))
        != 0)
    {
        goto oom;
    }
    for (i = 0; i < unit->mirProgram->funcLen; i++) {
        const SLWasmFnSig* sig = &sigs[i];
        if (WasmAppendByte(&typeSec, 0x60u) != 0
            || WasmAppendULEB(&typeSec, sig->wasmParamCount) != 0
            || WasmAppendBytes(&typeSec, sig->wasmParamTypes, sig->wasmParamCount) != 0
            || WasmAppendULEB(&typeSec, sig->wasmResultCount) != 0)
        {
            goto oom;
        }
        if (sig->wasmResultCount != 0u
            && WasmAppendBytes(&typeSec, sig->wasmResultTypes, sig->wasmResultCount) != 0)
        {
            goto oom;
        }
    }
    if (imports.hasWasmMinExit) {
        if (WasmAppendByte(&typeSec, 0x60u) != 0 || WasmAppendULEB(&typeSec, 1u) != 0
            || WasmAppendByte(&typeSec, 0x7fu) != 0 || WasmAppendULEB(&typeSec, 0u) != 0)
        {
            goto oom;
        }
    }
    if (imports.hasWasmMinConsoleLog) {
        if (WasmAppendByte(&typeSec, 0x60u) != 0 || WasmAppendULEB(&typeSec, 3u) != 0
            || WasmAppendByte(&typeSec, 0x7fu) != 0 || WasmAppendByte(&typeSec, 0x7fu) != 0
            || WasmAppendByte(&typeSec, 0x7fu) != 0 || WasmAppendULEB(&typeSec, 0u) != 0)
        {
            goto oom;
        }
    }
    if (imports.hasWasmMinPanic) {
        if (WasmAppendByte(&typeSec, 0x60u) != 0 || WasmAppendULEB(&typeSec, 2u) != 0
            || WasmAppendByte(&typeSec, 0x7fu) != 0 || WasmAppendByte(&typeSec, 0x7fu) != 0
            || WasmAppendULEB(&typeSec, 0u) != 0)
        {
            goto oom;
        }
    }
    if (imports.hasFunctionTable) {
        if (WasmAppendByte(&typeSec, 0x60u) != 0 || WasmAppendULEB(&typeSec, 6u) != 0
            || WasmAppendByte(&typeSec, 0x7fu) != 0 || WasmAppendByte(&typeSec, 0x7fu) != 0
            || WasmAppendByte(&typeSec, 0x7fu) != 0 || WasmAppendByte(&typeSec, 0x7fu) != 0
            || WasmAppendByte(&typeSec, 0x7fu) != 0 || WasmAppendByte(&typeSec, 0x7fu) != 0
            || WasmAppendULEB(&typeSec, 1u) != 0 || WasmAppendByte(&typeSec, 0x7fu) != 0)
        {
            goto oom;
        }
    }
    if (entry.hasWrapper) {
        if (WasmAppendByte(&typeSec, 0x60u) != 0 || WasmAppendULEB(&typeSec, 0u) != 0
            || WasmAppendULEB(&typeSec, 0u) != 0)
        {
            goto oom;
        }
    }
    if (WasmAppendSection(&out, 1u, &typeSec) != 0) {
        goto oom;
    }

    if (imports.importFuncCount != 0u) {
        if (WasmAppendULEB(&importSec, imports.importFuncCount) != 0) {
            goto oom;
        }
        if (imports.hasWasmMinExit) {
            if (WasmAppendULEB(&importSec, 8u) != 0
                || WasmAppendBytes(&importSec, "wasm_min", 8u) != 0
                || WasmAppendULEB(&importSec, 4u) != 0
                || WasmAppendBytes(&importSec, "exit", 4u) != 0
                || WasmAppendByte(&importSec, 0x00u) != 0
                || WasmAppendULEB(&importSec, imports.wasmMinExitTypeIndex) != 0)
            {
                goto oom;
            }
        }
        if (imports.hasWasmMinConsoleLog) {
            if (WasmAppendULEB(&importSec, 8u) != 0
                || WasmAppendBytes(&importSec, "wasm_min", 8u) != 0
                || WasmAppendULEB(&importSec, 11u) != 0
                || WasmAppendBytes(&importSec, "console_log", 11u) != 0
                || WasmAppendByte(&importSec, 0x00u) != 0
                || WasmAppendULEB(&importSec, imports.wasmMinConsoleLogTypeIndex) != 0)
            {
                goto oom;
            }
        }
        if (imports.hasWasmMinPanic) {
            if (WasmAppendULEB(&importSec, 8u) != 0
                || WasmAppendBytes(&importSec, "wasm_min", 8u) != 0
                || WasmAppendULEB(&importSec, 5u) != 0
                || WasmAppendBytes(&importSec, "panic", 5u) != 0
                || WasmAppendByte(&importSec, 0x00u) != 0
                || WasmAppendULEB(&importSec, imports.wasmMinPanicTypeIndex) != 0)
            {
                goto oom;
            }
        }
        if (WasmAppendSection(&out, 2u, &importSec) != 0) {
            goto oom;
        }
    }

    if (WasmAppendULEB(
            &funcSec,
            unit->mirProgram->funcLen + (imports.hasRootAllocThunk ? 1u : 0u)
                + (entry.hasWrapper ? 1u : 0u))
        != 0)
    {
        goto oom;
    }
    for (i = 0; i < unit->mirProgram->funcLen; i++) {
        if (WasmAppendULEB(&funcSec, sigs[i].typeIndex) != 0) {
            goto oom;
        }
    }
    if (imports.hasRootAllocThunk
        && WasmAppendULEB(&funcSec, imports.allocatorIndirectTypeIndex) != 0)
    {
        goto oom;
    }
    if (entry.hasWrapper && WasmAppendULEB(&funcSec, entry.wrapperTypeIndex) != 0) {
        goto oom;
    }
    if (WasmAppendSection(&out, 3u, &funcSec) != 0) {
        goto oom;
    }

    if (imports.hasFunctionTable) {
        if (WasmAppendULEB(&tableSec, 1u) != 0 || WasmAppendByte(&tableSec, 0x70u) != 0
            || WasmAppendByte(&tableSec, 0x00u) != 0
            || WasmAppendULEB(&tableSec, imports.tableFuncCount) != 0
            || WasmAppendSection(&out, 4u, &tableSec) != 0)
        {
            goto oom;
        }
    }

    if (WasmAppendULEB(&memSec, 1u) != 0 || WasmAppendByte(&memSec, 0x00u) != 0
        || WasmAppendULEB(&memSec, (imports.needsFrameGlobal && imports.needsHeapGlobal) ? 2u : 1u)
               != 0)
    {
        goto oom;
    }
    if (WasmAppendSection(&out, 5u, &memSec) != 0) {
        goto oom;
    }
    if (imports.needsFrameGlobal || imports.needsHeapGlobal) {
        uint32_t globalCount =
            (imports.needsFrameGlobal ? 1u : 0u) + (imports.needsHeapGlobal ? 1u : 0u);
        uint32_t staticEnd = WasmStaticDataEnd(&strings, &entry);
        uint32_t heapBaseInit = staticEnd;
        uint32_t frameBaseInit =
            imports.needsHeapGlobal ? (staticEnd > 65536u ? staticEnd : 65536u) : staticEnd;
        if (WasmAppendULEB(&globalSec, globalCount) != 0) {
            goto oom;
        }
        if (imports.needsFrameGlobal) {
            if (WasmAppendByte(&globalSec, 0x7fu) != 0 || WasmAppendByte(&globalSec, 0x01u) != 0
                || WasmAppendByte(&globalSec, 0x41u) != 0
                || WasmAppendSLEB32(&globalSec, (int32_t)frameBaseInit) != 0
                || WasmAppendByte(&globalSec, 0x0bu) != 0)
            {
                goto oom;
            }
        }
        if (imports.needsHeapGlobal) {
            if (WasmAppendByte(&globalSec, 0x7fu) != 0 || WasmAppendByte(&globalSec, 0x01u) != 0
                || WasmAppendByte(&globalSec, 0x41u) != 0
                || WasmAppendSLEB32(&globalSec, (int32_t)heapBaseInit) != 0
                || WasmAppendByte(&globalSec, 0x0bu) != 0)
            {
                goto oom;
            }
        }
        if (WasmAppendSection(&out, 6u, &globalSec) != 0) {
            goto oom;
        }
    }

    for (i = 0; i < unit->mirProgram->funcLen; i++) {
        const SLMirFunction* fn = &unit->mirProgram->funcs[i];
        if (WasmFunctionIsNamedMain(unit->mirProgram, fn)) {
            exportCount++;
        }
    }
    if (WasmAppendULEB(&exportSec, exportCount) != 0) {
        goto oom;
    }
    if (WasmAppendULEB(&exportSec, 6u) != 0 || WasmAppendBytes(&exportSec, "memory", 6u) != 0
        || WasmAppendByte(&exportSec, 0x02u) != 0 || WasmAppendULEB(&exportSec, 0u) != 0)
    {
        goto oom;
    }
    for (i = 0; i < unit->mirProgram->funcLen; i++) {
        const SLMirFunction* fn = &unit->mirProgram->funcs[i];
        if (!WasmFunctionIsNamedMain(unit->mirProgram, fn)) {
            continue;
        }
        if (WasmAppendULEB(&exportSec, 7u) != 0 || WasmAppendBytes(&exportSec, "sl_main", 7u) != 0
            || WasmAppendByte(&exportSec, 0x00u) != 0
            || WasmAppendULEB(
                   &exportSec,
                   entry.hasWrapper && entry.mainFuncIndex == i
                       ? entry.wrapperFuncIndex
                       : imports.importFuncCount + i)
                   != 0)
        {
            goto oom;
        }
    }
    if (WasmAppendSection(&out, 7u, &exportSec) != 0) {
        goto oom;
    }

    if (imports.hasFunctionTable) {
        if (WasmAppendULEB(&elemSec, 1u) != 0 || WasmAppendByte(&elemSec, 0x00u) != 0
            || WasmAppendByte(&elemSec, 0x41u) != 0 || WasmAppendSLEB32(&elemSec, 0) != 0
            || WasmAppendByte(&elemSec, 0x0bu) != 0
            || WasmAppendULEB(&elemSec, imports.tableFuncCount) != 0)
        {
            goto oom;
        }
        for (i = 0; i < unit->mirProgram->funcLen; i++) {
            if (WasmAppendULEB(&elemSec, imports.importFuncCount + i) != 0) {
                goto oom;
            }
        }
        if (imports.hasRootAllocThunk && WasmAppendULEB(&elemSec, imports.rootAllocFuncIndex) != 0)
        {
            goto oom;
        }
        if (WasmAppendSection(&out, 9u, &elemSec) != 0) {
            goto oom;
        }
    }

    if (WasmAppendULEB(
            &codeSec,
            unit->mirProgram->funcLen + (imports.hasRootAllocThunk ? 1u : 0u)
                + (entry.hasWrapper ? 1u : 0u))
        != 0)
    {
        goto oom;
    }
    for (i = 0; i < unit->mirProgram->funcLen; i++) {
        SLWasmBuf body = { .options = options };
        if (WasmEmitFunctionBody(unit->mirProgram, sigs, &imports, &strings, i, &body, diag) != 0) {
            if (body.data != NULL && options->arenaFree != NULL) {
                options->arenaFree(options->allocatorCtx, body.data, body.cap);
            }
            goto fail;
        }
        if (WasmAppendULEB(&codeSec, body.len) != 0
            || WasmAppendBytes(&codeSec, body.data, body.len) != 0)
        {
            if (body.data != NULL && options->arenaFree != NULL) {
                options->arenaFree(options->allocatorCtx, body.data, body.cap);
            }
            goto oom;
        }
        if (body.data != NULL && options->arenaFree != NULL) {
            options->arenaFree(options->allocatorCtx, body.data, body.cap);
        }
    }
    if (imports.hasRootAllocThunk) {
        SLWasmBuf body = { .options = options };
        if (WasmEmitRootAllocThunkBody(&imports, options, &body) != 0) {
            if (body.data != NULL && options->arenaFree != NULL) {
                options->arenaFree(options->allocatorCtx, body.data, body.cap);
            }
            goto oom;
        }
        if (WasmAppendULEB(&codeSec, body.len) != 0
            || WasmAppendBytes(&codeSec, body.data, body.len) != 0)
        {
            if (body.data != NULL && options->arenaFree != NULL) {
                options->arenaFree(options->allocatorCtx, body.data, body.cap);
            }
            goto oom;
        }
        if (body.data != NULL && options->arenaFree != NULL) {
            options->arenaFree(options->allocatorCtx, body.data, body.cap);
        }
    }
    if (entry.hasWrapper) {
        SLWasmBuf body = { .options = options };
        if (WasmEmitEntryWrapperBody(unit->mirProgram, sigs, &imports, &entry, options, &body, diag)
            != 0)
        {
            if (body.data != NULL && options->arenaFree != NULL) {
                options->arenaFree(options->allocatorCtx, body.data, body.cap);
            }
            goto fail;
        }
        if (WasmAppendULEB(&codeSec, body.len) != 0
            || WasmAppendBytes(&codeSec, body.data, body.len) != 0)
        {
            if (body.data != NULL && options->arenaFree != NULL) {
                options->arenaFree(options->allocatorCtx, body.data, body.cap);
            }
            goto oom;
        }
        if (body.data != NULL && options->arenaFree != NULL) {
            options->arenaFree(options->allocatorCtx, body.data, body.cap);
        }
    }
    if (WasmAppendSection(&out, 10u, &codeSec) != 0) {
        goto oom;
    }
    if (strings.data.len != 0u) {
        if (WasmAppendULEB(&dataSec, 1u) != 0 || WasmAppendByte(&dataSec, 0x00u) != 0
            || WasmAppendByte(&dataSec, 0x41u) != 0 || WasmAppendSLEB32(&dataSec, 0) != 0
            || WasmAppendByte(&dataSec, 0x0bu) != 0
            || WasmAppendULEB(&dataSec, strings.data.len) != 0
            || WasmAppendBytes(&dataSec, strings.data.data, strings.data.len) != 0
            || WasmAppendSection(&out, 11u, &dataSec) != 0)
        {
            goto oom;
        }
    }

    outArtifact->data = out.data;
    outArtifact->len = out.len;
    outArtifact->isBinary = 1u;
    if (sigs != NULL && options->arenaFree != NULL) {
        options->arenaFree(options->allocatorCtx, sigs, unit->mirProgram->funcLen * sizeof(*sigs));
    }
    if (typeSec.data != NULL && options->arenaFree != NULL) {
        options->arenaFree(options->allocatorCtx, typeSec.data, typeSec.cap);
    }
    if (importSec.data != NULL && options->arenaFree != NULL) {
        options->arenaFree(options->allocatorCtx, importSec.data, importSec.cap);
    }
    if (funcSec.data != NULL && options->arenaFree != NULL) {
        options->arenaFree(options->allocatorCtx, funcSec.data, funcSec.cap);
    }
    if (tableSec.data != NULL && options->arenaFree != NULL) {
        options->arenaFree(options->allocatorCtx, tableSec.data, tableSec.cap);
    }
    if (memSec.data != NULL && options->arenaFree != NULL) {
        options->arenaFree(options->allocatorCtx, memSec.data, memSec.cap);
    }
    if (globalSec.data != NULL && options->arenaFree != NULL) {
        options->arenaFree(options->allocatorCtx, globalSec.data, globalSec.cap);
    }
    if (exportSec.data != NULL && options->arenaFree != NULL) {
        options->arenaFree(options->allocatorCtx, exportSec.data, exportSec.cap);
    }
    if (elemSec.data != NULL && options->arenaFree != NULL) {
        options->arenaFree(options->allocatorCtx, elemSec.data, elemSec.cap);
    }
    if (codeSec.data != NULL && options->arenaFree != NULL) {
        options->arenaFree(options->allocatorCtx, codeSec.data, codeSec.cap);
    }
    if (dataSec.data != NULL && options->arenaFree != NULL) {
        options->arenaFree(options->allocatorCtx, dataSec.data, dataSec.cap);
    }
    if (strings.data.data != NULL && options->arenaFree != NULL) {
        options->arenaFree(options->allocatorCtx, strings.data.data, strings.data.cap);
    }
    if (strings.constRefs != NULL && options->arenaFree != NULL) {
        options->arenaFree(
            options->allocatorCtx,
            strings.constRefs,
            unit->mirProgram->constLen * sizeof(SLWasmStringRef));
    }
    return 0;

oom:
    WasmSetDiag(diag, SLDiag_ARENA_OOM, 0, 0);
fail:
    if (out.data != NULL && options->arenaFree != NULL) {
        options->arenaFree(options->allocatorCtx, out.data, out.cap);
    }
    if (sigs != NULL && options->arenaFree != NULL) {
        options->arenaFree(options->allocatorCtx, sigs, unit->mirProgram->funcLen * sizeof(*sigs));
    }
    if (typeSec.data != NULL && options->arenaFree != NULL) {
        options->arenaFree(options->allocatorCtx, typeSec.data, typeSec.cap);
    }
    if (importSec.data != NULL && options->arenaFree != NULL) {
        options->arenaFree(options->allocatorCtx, importSec.data, importSec.cap);
    }
    if (funcSec.data != NULL && options->arenaFree != NULL) {
        options->arenaFree(options->allocatorCtx, funcSec.data, funcSec.cap);
    }
    if (tableSec.data != NULL && options->arenaFree != NULL) {
        options->arenaFree(options->allocatorCtx, tableSec.data, tableSec.cap);
    }
    if (memSec.data != NULL && options->arenaFree != NULL) {
        options->arenaFree(options->allocatorCtx, memSec.data, memSec.cap);
    }
    if (globalSec.data != NULL && options->arenaFree != NULL) {
        options->arenaFree(options->allocatorCtx, globalSec.data, globalSec.cap);
    }
    if (exportSec.data != NULL && options->arenaFree != NULL) {
        options->arenaFree(options->allocatorCtx, exportSec.data, exportSec.cap);
    }
    if (elemSec.data != NULL && options->arenaFree != NULL) {
        options->arenaFree(options->allocatorCtx, elemSec.data, elemSec.cap);
    }
    if (codeSec.data != NULL && options->arenaFree != NULL) {
        options->arenaFree(options->allocatorCtx, codeSec.data, codeSec.cap);
    }
    if (dataSec.data != NULL && options->arenaFree != NULL) {
        options->arenaFree(options->allocatorCtx, dataSec.data, dataSec.cap);
    }
    if (strings.data.data != NULL && options->arenaFree != NULL) {
        options->arenaFree(options->allocatorCtx, strings.data.data, strings.data.cap);
    }
    if (strings.constRefs != NULL && options->arenaFree != NULL) {
        options->arenaFree(
            options->allocatorCtx,
            strings.constRefs,
            unit->mirProgram->constLen * sizeof(SLWasmStringRef));
    }
    return -1;
}

const SLCodegenBackend gSLCodegenBackendWasm = {
    .name = "wasm",
    .emit = EmitWasmBackend,
};

SL_API_END
