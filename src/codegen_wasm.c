#include "libhop-impl.h"
#include "codegen.h"
#include <stdbool.h>
#if H2_LIBC
    #include <stdlib.h>
#endif

H2_API_BEGIN

typedef struct {
    const H2CodegenUnit*    unit;
    const H2CodegenOptions* options;
    H2Diag* _Nullable diag;
    const char* _Nullable limitDetail;
    uint8_t* _Nullable data;
    uint32_t len;
    uint32_t cap;
    uint32_t maxLen;
    uint32_t limitStart;
    uint32_t limitEnd;
} HOPWasmBuf;

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
} HOPWasmFnSig;

typedef struct {
    uint16_t wasmValueIndex[256];
    uint16_t wasmParamValueCount;
    uint16_t wasmLocalValueCount;
    uint16_t hiddenLocalStart;
    uint16_t directCallScratchI32Start;
    uint16_t directCallScratchI64Start;
    uint16_t frameBaseLocal;
    uint16_t scratch0Local;
    uint16_t scratch1Local;
    uint16_t scratch2Local;
    uint16_t scratch3Local;
    uint16_t scratch4Local;
    uint16_t scratch5Local;
    uint16_t scratch6Local;
    uint16_t scratchI64Local;
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
} HOPWasmEmitState;

enum {
    HOPWasmDirectCallScratchI32Count = 64u,
    HOPWasmDirectCallScratchI64Count = 32u,
};

typedef struct {
    uint32_t objectOffset;
    uint32_t dataOffset;
    uint32_t len;
} HOPWasmStringRef;

typedef struct {
    HOPWasmStringRef* _Nullable constRefs;
    HOPWasmBuf       data;
    HOPWasmStringRef assertPanic;
    HOPWasmStringRef allocNullPanic;
    HOPWasmStringRef invalidAllocatorPanic;
    uint32_t         rootAllocatorOffset;
    uint8_t          hasAssertPanicString;
    uint8_t          hasAllocNullPanicString;
    uint8_t          hasInvalidAllocatorPanicString;
    uint8_t          hasRootAllocator;
} HOPWasmStringLayout;

typedef struct {
    int      hasContinue;
    uint32_t continueTargetPc;
    uint32_t continueDepth;
    int      hasBreak;
    uint32_t breakTargetPc;
    uint32_t breakDepth;
} HOPWasmBranchTargets;

typedef struct {
    uint32_t headerPc;
    uint32_t condBranchPc;
    uint32_t bodyStartPc;
    uint32_t tailStartPc;
    uint32_t backedgePc;
    uint32_t exitPc;
} HOPWasmLoopRegion;

typedef struct {
    uint32_t importFuncCount;
    uint32_t importGlobalCount;
    uint32_t definedFuncCount;
    uint32_t frameGlobalIndex;
    uint32_t heapGlobalIndex;
    uint32_t allocatorIndirectTypeIndex;
    uint32_t rootAllocFuncIndex;
    uint32_t tableFuncCount;
    uint32_t rootAllocTableIndex;
    uint32_t platformPanicFuncIndex;
    uint32_t* _Nullable funcWasmIndices;
    uint32_t* _Nullable globalImportIndices;
    uint8_t* _Nullable importedFunctions;
    uint8_t* _Nullable reachableFunctions;
    uint32_t funcWasmIndicesAllocSize;
    uint32_t globalImportIndicesAllocSize;
    uint32_t importedFunctionsAllocSize;
    uint32_t reachableFunctionsAllocSize;
    uint8_t  hasFunctionTable;
    uint8_t  hasRootAllocThunk;
    uint8_t  hasPlatformPanicImport;
    uint8_t  needsFrameGlobal;
    uint8_t  needsHeapGlobal;
    uint8_t  _reserved[2];
} HOPWasmImportLayout;

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
} HOPWasmEntryLayout;

static int WasmEmitPlatformPanicCall(
    HOPWasmBuf*                body,
    const HOPWasmImportLayout* imports,
    int32_t                    dataOffset,
    int32_t                    len,
    int32_t                    flags);

enum {
    HOPWasmType_VOID = 0,
    HOPWasmType_I32 = 1,
    HOPWasmType_I64 = 2,
    HOPWasmType_STR_REF = 3,
    HOPWasmType_STR_PTR = 4,
    HOPWasmType_U8_PTR = 5,
    HOPWasmType_I32_PTR = 6,
    HOPWasmType_I8_PTR = 7,
    HOPWasmType_U16_PTR = 8,
    HOPWasmType_I16_PTR = 9,
    HOPWasmType_U32_PTR = 10,
    HOPWasmType_OPAQUE_PTR = 11,
    HOPWasmType_ARRAY_VIEW_U8 = 12,
    HOPWasmType_ARRAY_VIEW_I8 = 13,
    HOPWasmType_ARRAY_VIEW_U16 = 14,
    HOPWasmType_ARRAY_VIEW_I16 = 15,
    HOPWasmType_ARRAY_VIEW_U32 = 16,
    HOPWasmType_ARRAY_VIEW_I32 = 17,
    HOPWasmType_SLICE_U8 = 18,
    HOPWasmType_SLICE_I8 = 19,
    HOPWasmType_SLICE_U16 = 20,
    HOPWasmType_SLICE_I16 = 21,
    HOPWasmType_SLICE_U32 = 22,
    HOPWasmType_SLICE_I32 = 23,
    HOPWasmType_SLICE_AGG = 24,
    HOPWasmType_AGG_REF = 25,
    HOPWasmType_FUNC_REF = 26,
    HOPWasmType_FUNC_REF_PTR = 27,
};

enum {
    HOPWasmLocalStorage_PLAIN = 0,
    HOPWasmLocalStorage_ARRAY = 1,
    HOPWasmLocalStorage_AGG = 2,
};

static void WasmSetDiag(H2Diag* _Nullable diag, H2DiagCode code, uint32_t start, uint32_t end) {
    if (diag == NULL) {
        return;
    }
    diag->code = code;
    diag->type = H2DiagTypeOfCode(code);
    diag->start = start;
    diag->end = end;
    diag->argStart = 0;
    diag->argEnd = 0;
    diag->argText = NULL;
    diag->argTextLen = 0;
    diag->relatedStart = 0;
    diag->relatedEnd = 0;
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

static size_t WasmCStrLen(const char* s) {
    size_t n = 0u;
    if (s == NULL) {
        return 0u;
    }
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static const char* _Nullable WasmCopyDiagDetail(
    const H2CodegenOptions* options, const char* prefix, H2StrView name, const char* suffix) {
    char*    buf;
    uint32_t allocSize = 0u;
    size_t   prefixLen = WasmCStrLen(prefix);
    size_t   suffixLen = WasmCStrLen(suffix);
    size_t   totalLen = prefixLen + name.len + suffixLen;
    if (options == NULL || options->arenaGrow == NULL) {
        return NULL;
    }
    buf = (char*)options->arenaGrow(options->allocatorCtx, (uint32_t)totalLen + 1u, &allocSize);
    if (buf == NULL) {
        return NULL;
    }
    if (prefixLen != 0u) {
        memcpy(buf, prefix, prefixLen);
    }
    if (name.len != 0u && name.ptr != NULL) {
        memcpy(buf + prefixLen, name.ptr, name.len);
    }
    if (suffixLen != 0u) {
        memcpy(buf + prefixLen + name.len, suffix, suffixLen);
    }
    buf[totalLen] = '\0';
    return buf;
}

static const char* _Nullable WasmDynamicResolutionDetail(
    const H2CodegenUnit*    unit,
    const H2CodegenOptions* options,
    uint32_t                funcIndex,
    const H2MirInst*        inst) {
    const H2MirProgram*   program;
    const H2MirSymbolRef* sym;
    const H2MirFunction*  fn;
    const char*           src;
    H2StrView             name = { 0 };
    const char*           prefix = NULL;
    const char*           suffix = " (needs MIR resolution)";
    if (unit == NULL || inst == NULL || unit->mirProgram == NULL) {
        return NULL;
    }
    program = unit->mirProgram;
    if (funcIndex >= program->funcLen) {
        return "MIR still needs dynamic resolution";
    }
    fn = &program->funcs[funcIndex];
    if (fn->sourceRef >= program->sourceLen) {
        return "MIR still needs dynamic resolution";
    }
    src = program->sources[fn->sourceRef].src.ptr;
    if (inst->op == H2MirOp_LOAD_IDENT) {
        name.ptr = src + inst->start;
        name.len = inst->end >= inst->start ? inst->end - inst->start : 0u;
        prefix = "unresolved ident ";
        return WasmCopyDiagDetail(options, prefix, name, suffix);
    }
    if (inst->op != H2MirOp_CALL || inst->aux >= program->symbolLen) {
        return "MIR still needs dynamic resolution";
    }
    sym = &program->symbols[inst->aux];
    name.ptr = src + sym->nameStart;
    name.len = sym->nameEnd >= sym->nameStart ? sym->nameEnd - sym->nameStart : 0u;
    prefix = "unresolved call ";
    return WasmCopyDiagDetail(options, prefix, name, suffix);
}

static const char* _Nullable WasmFunctionDetail(
    const H2CodegenOptions* options,
    const H2MirProgram*     program,
    const H2MirFunction*    fn,
    const char*             prefix,
    const char*             suffix) {
    H2StrView name = { 0 };
    if (program == NULL || fn == NULL || fn->sourceRef >= program->sourceLen) {
        return NULL;
    }
    name.ptr = program->sources[fn->sourceRef].src.ptr + fn->nameStart;
    name.len = fn->nameEnd >= fn->nameStart ? fn->nameEnd - fn->nameStart : 0u;
    return WasmCopyDiagDetail(options, prefix, name, suffix);
}

static bool WasmFunctionIsNamedMain(const H2MirProgram* program, const H2MirFunction* fn) {
    return program != NULL && fn != NULL && fn->sourceRef < program->sourceLen
        && WasmSliceEqLiteral(
               program->sources[fn->sourceRef].src.ptr, fn->nameStart, fn->nameEnd, "main");
}

static bool WasmLocalNameEq(
    const H2MirProgram*  program,
    const H2MirFunction* fn,
    const H2MirLocal*    local,
    const char*          name) {
    if (program == NULL || fn == NULL || local == NULL || name == NULL
        || fn->sourceRef >= program->sourceLen)
    {
        return false;
    }
    return WasmSliceEqLiteral(
        program->sources[fn->sourceRef].src.ptr, local->nameStart, local->nameEnd, name);
}

static bool     WasmTypeRefIsSourceLocation(const H2MirProgram* program, uint32_t typeRefIndex);
static uint32_t WasmFindMemAllocatorTypeRef(const H2MirProgram* program);

static bool WasmLocalIsSourceLocation(
    const H2MirProgram* program, const H2MirFunction* fn, const H2MirLocal* local) {
    return (local != NULL && WasmTypeRefIsSourceLocation(program, local->typeRef))
        || WasmLocalNameEq(program, fn, local, "_srcLoc")
        || WasmLocalNameEq(program, fn, local, "srcLoc");
}

static bool WasmProgramNeedsFunctionTable(const H2MirProgram* program);
static bool WasmProgramNeedsRootAllocator(
    const H2MirProgram* program, const uint8_t* _Nullable reachableFunctions);
static bool WasmProgramHasAssert(
    const H2MirProgram* program, const uint8_t* _Nullable reachableFunctions);
static bool WasmProgramHasAllocNullPanic(
    const H2MirProgram* program, const uint8_t* _Nullable reachableFunctions);
static bool WasmProgramHasInvalidAllocatorPanic(
    const H2MirProgram* program, const uint8_t* _Nullable reachableFunctions);

typedef struct {
    const H2ForeignLinkageEntry* _Nullable entries;
    uint32_t len;
} HOPWasmForeignMetadata;

static int WasmBuildReachableFunctionSet(
    const H2CodegenUnit*          unit,
    const HOPWasmForeignMetadata* foreign,
    const H2CodegenOptions*       options,
    HOPWasmImportLayout*          imports,
    H2Diag* _Nullable diag) {
    const H2MirProgram* program;
    uint32_t            allocSize = 0u;
    uint32_t*           queue = NULL;
    uint32_t            queueAllocSize = 0u;
    uint32_t            readIndex = 0u;
    uint32_t            writeIndex = 0u;
    uint32_t            i;
    if (unit == NULL || unit->mirProgram == NULL || options == NULL || options->arenaGrow == NULL
        || imports == NULL)
    {
        return -1;
    }
    program = unit->mirProgram;
    if (program->funcLen == 0u) {
        return 0;
    }
    imports->reachableFunctions = (uint8_t*)options->arenaGrow(
        options->allocatorCtx, program->funcLen, &allocSize);
    imports->reachableFunctionsAllocSize = allocSize;
    if (imports->reachableFunctions == NULL
        || imports->reachableFunctionsAllocSize < program->funcLen)
    {
        WasmSetDiag(diag, H2Diag_ARENA_OOM, 0, 0);
        return -1;
    }
    memset(imports->reachableFunctions, 0, program->funcLen);
    if (WasmProgramNeedsFunctionTable(program) || WasmProgramNeedsRootAllocator(program, NULL)) {
        memset(imports->reachableFunctions, 1, program->funcLen);
        return 0;
    }
    queue = (uint32_t*)options->arenaGrow(
        options->allocatorCtx, program->funcLen * (uint32_t)sizeof(*queue), &queueAllocSize);
    if (queue == NULL || queueAllocSize < program->funcLen * sizeof(*queue)) {
        WasmSetDiag(diag, H2Diag_ARENA_OOM, 0, 0);
        return -1;
    }
    for (i = 0; i < program->funcLen; i++) {
        if (WasmFunctionIsNamedMain(program, &program->funcs[i])) {
            imports->reachableFunctions[i] = 1u;
            queue[writeIndex++] = i;
        }
    }
    if (foreign != NULL && foreign->entries != NULL) {
        for (i = 0; i < foreign->len; i++) {
            if (foreign->entries[i].kind == H2ForeignLinkage_EXPORT_FN
                && foreign->entries[i].functionIndex < program->funcLen
                && imports->reachableFunctions[foreign->entries[i].functionIndex] == 0u)
            {
                imports->reachableFunctions[foreign->entries[i].functionIndex] = 1u;
                queue[writeIndex++] = foreign->entries[i].functionIndex;
            }
        }
    }
    while (readIndex < writeIndex) {
        uint32_t             functionIndex = queue[readIndex++];
        const H2MirFunction* fn = &program->funcs[functionIndex];
        uint32_t             pc;
        for (pc = 0u; pc < fn->instLen; pc++) {
            const H2MirInst* inst = &program->insts[fn->instStart + pc];
            if (inst->op == H2MirOp_CALL_FN && inst->aux < program->funcLen
                && imports->reachableFunctions[inst->aux] == 0u)
            {
                imports->reachableFunctions[inst->aux] = 1u;
                queue[writeIndex++] = inst->aux;
            }
        }
    }
    if (foreign != NULL && foreign->entries != NULL
        && (WasmProgramHasAssert(program, imports->reachableFunctions)
            || WasmProgramHasAllocNullPanic(program, imports->reachableFunctions)
            || WasmProgramHasInvalidAllocatorPanic(program, imports->reachableFunctions)))
    {
        for (i = 0; i < foreign->len; i++) {
            if (foreign->entries[i].kind == H2ForeignLinkage_WASM_IMPORT_FN
                && (foreign->entries[i].flags & H2ForeignLinkageFlag_PLATFORM_PANIC) != 0u
                && foreign->entries[i].functionIndex < program->funcLen)
            {
                imports->reachableFunctions[foreign->entries[i].functionIndex] = 1u;
            }
        }
    }
    return 0;
}

static uint32_t WasmFunctionWasmIndex(const HOPWasmImportLayout* imports, uint32_t functionIndex) {
    if (imports == NULL || imports->funcWasmIndices == NULL) {
        return UINT32_MAX;
    }
    return imports->funcWasmIndices[functionIndex];
}

static int WasmCollectForeignDirectives(
    const H2CodegenUnit* unit, HOPWasmForeignMetadata* meta, H2Diag* _Nullable diag) {
    (void)diag;
    if (meta != NULL) {
        *meta = (HOPWasmForeignMetadata){ 0 };
    }
    if (meta == NULL || unit == NULL) {
        return -1;
    }
    if (unit->foreignLinkage != NULL) {
        meta->entries = unit->foreignLinkage->entries;
        meta->len = unit->foreignLinkage->len;
    }
    return 0;
}

static void WasmFreeImportLayout(
    const H2CodegenOptions* _Nullable options, HOPWasmImportLayout* _Nullable imports) {
    if (imports == NULL || options == NULL || options->arenaFree == NULL) {
        return;
    }
    if (imports->funcWasmIndices != NULL) {
        options->arenaFree(
            options->allocatorCtx, imports->funcWasmIndices, imports->funcWasmIndicesAllocSize);
    }
    if (imports->globalImportIndices != NULL) {
        options->arenaFree(
            options->allocatorCtx,
            imports->globalImportIndices,
            imports->globalImportIndicesAllocSize);
    }
    if (imports->importedFunctions != NULL) {
        options->arenaFree(
            options->allocatorCtx, imports->importedFunctions, imports->importedFunctionsAllocSize);
    }
    if (imports->reachableFunctions != NULL) {
        options->arenaFree(
            options->allocatorCtx,
            imports->reachableFunctions,
            imports->reachableFunctionsAllocSize);
    }
    *imports = (HOPWasmImportLayout){ 0 };
}

static int WasmReserve(HOPWasmBuf* b, uint32_t extra) {
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
        WasmSetDiag(b->diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, b->limitStart, b->limitEnd);
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

static int WasmAppendByte(HOPWasmBuf* b, uint8_t v) {
    if (WasmReserve(b, 1u) != 0) {
        return -1;
    }
    b->data[b->len++] = v;
    return 0;
}

static int WasmAppendBytes(HOPWasmBuf* b, const void* p, uint32_t len) {
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

static int WasmAppendULEB(HOPWasmBuf* b, uint32_t v) {
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

static int WasmAppendSLEB32(HOPWasmBuf* b, int32_t v) {
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

static int WasmAppendSLEB64(HOPWasmBuf* b, int64_t v) {
    int more = 1;
    while (more) {
        uint8_t byte = (uint8_t)(v & 0x7f);
        int64_t sign = byte & 0x40u;
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

static int WasmAppendU32LE(HOPWasmBuf* b, uint32_t v) {
    uint8_t bytes[4];
    bytes[0] = (uint8_t)(v & 0xffu);
    bytes[1] = (uint8_t)((v >> 8u) & 0xffu);
    bytes[2] = (uint8_t)((v >> 16u) & 0xffu);
    bytes[3] = (uint8_t)((v >> 24u) & 0xffu);
    return WasmAppendBytes(b, bytes, 4u);
}

static int WasmAppendSection(HOPWasmBuf* out, uint8_t id, const HOPWasmBuf* section) {
    if (WasmAppendByte(out, id) != 0 || WasmAppendULEB(out, section->len) != 0
        || WasmAppendBytes(out, section->data, section->len) != 0)
    {
        return -1;
    }
    return 0;
}

static const char* WasmMirOpName(H2MirOp op) {
    switch (op) {
        case H2MirOp_PUSH_CONST:    return "PUSH_CONST";
        case H2MirOp_UNARY:         return "UNARY";
        case H2MirOp_BINARY:        return "BINARY";
        case H2MirOp_CAST:          return "CAST";
        case H2MirOp_COERCE:        return "COERCE";
        case H2MirOp_AGG_MAKE:      return "AGG_MAKE";
        case H2MirOp_AGG_ZERO:      return "AGG_ZERO";
        case H2MirOp_AGG_SET:       return "AGG_SET";
        case H2MirOp_LOCAL_ZERO:    return "LOCAL_ZERO";
        case H2MirOp_LOCAL_LOAD:    return "LOCAL_LOAD";
        case H2MirOp_LOCAL_STORE:   return "LOCAL_STORE";
        case H2MirOp_ARRAY_ADDR:    return "ARRAY_ADDR";
        case H2MirOp_DROP:          return "DROP";
        case H2MirOp_CALL_FN:       return "CALL_FN";
        case H2MirOp_RETURN:        return "RETURN";
        case H2MirOp_RETURN_VOID:   return "RETURN_VOID";
        case H2MirOp_LOAD_IDENT:    return "LOAD_IDENT";
        case H2MirOp_CALL:          return "CALL";
        case H2MirOp_CALL_HOST:     return "CALL_HOST";
        case H2MirOp_CALL_INDIRECT: return "CALL_INDIRECT";
        case H2MirOp_CTX_GET:       return "CTX_GET";
        case H2MirOp_CTX_ADDR:      return "CTX_ADDR";
        case H2MirOp_JUMP:          return "JUMP";
        case H2MirOp_JUMP_IF_FALSE: return "JUMP_IF_FALSE";
        case H2MirOp_ASSERT:        return "ASSERT";
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

static bool WasmIsWasmMinPlatform(const H2CodegenUnit* unit) {
    return unit != NULL && unit->platformTarget != NULL
        && WasmStrEq(unit->platformTarget, "wasm-min");
}

static bool WasmIsPlaybitPlatform(const H2CodegenUnit* unit) {
    return unit != NULL && unit->platformTarget != NULL
        && WasmStrEq(unit->platformTarget, "playbit");
}

static bool WasmSupportsPlatformImports(const H2CodegenUnit* unit) {
    return WasmIsWasmMinPlatform(unit) || WasmIsPlaybitPlatform(unit);
}

static bool WasmProgramHasAssert(
    const H2MirProgram* program, const uint8_t* _Nullable reachableFunctions) {
    uint32_t funcIndex;
    if (program == NULL) {
        return false;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const H2MirFunction* fn;
        uint32_t             pc;
        if (reachableFunctions != NULL && reachableFunctions[funcIndex] == 0u) {
            continue;
        }
        fn = &program->funcs[funcIndex];
        for (pc = 0; pc < fn->instLen; pc++) {
            if (program->insts[fn->instStart + pc].op == H2MirOp_ASSERT) {
                return true;
            }
        }
    }
    return false;
}

static bool WasmProgramNeedsRootAllocator(
    const H2MirProgram* program, const uint8_t* _Nullable reachableFunctions) {
    uint32_t funcIndex;
    if (program == NULL) {
        return false;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const H2MirFunction* fn;
        uint32_t             pc;
        if (reachableFunctions != NULL && reachableFunctions[funcIndex] == 0u) {
            continue;
        }
        fn = &program->funcs[funcIndex];
        for (pc = 0; pc < fn->instLen; pc++) {
            const H2MirInst* inst = &program->insts[fn->instStart + pc];
            if ((inst->op == H2MirOp_CTX_GET || inst->op == H2MirOp_CTX_ADDR)
                && (inst->aux == H2MirContextField_ALLOCATOR
                    || inst->aux == H2MirContextField_TEMP_ALLOCATOR))
            {
                return true;
            }
        }
    }
    return false;
}

static bool WasmProgramHasAllocNullPanic(
    const H2MirProgram* program, const uint8_t* _Nullable reachableFunctions) {
    uint32_t funcIndex;
    bool     usesRootAllocator;
    if (program == NULL) {
        return false;
    }
    usesRootAllocator = WasmProgramNeedsRootAllocator(program, reachableFunctions);
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const H2MirFunction* fn = &program->funcs[funcIndex];
        uint32_t             pc;
        if (reachableFunctions != NULL && reachableFunctions[funcIndex] == 0u) {
            continue;
        }
        for (pc = 0; pc + 1u < fn->instLen; pc++) {
            const H2MirInst* allocInst = &program->insts[fn->instStart + pc];
            const H2MirInst* nextInst = &program->insts[fn->instStart + pc + 1u];
            uint32_t         typeRef;
            if (allocInst->op != H2MirOp_ALLOC_NEW || nextInst->op != H2MirOp_LOCAL_STORE
                || nextInst->aux >= fn->localCount)
            {
                continue;
            }
            if ((allocInst->tok & H2AstFlag_NEW_HAS_ALLOC) == 0u && !usesRootAllocator) {
                continue;
            }
            typeRef = program->locals[fn->localStart + nextInst->aux].typeRef;
            if (typeRef < program->typeLen && !H2MirTypeRefIsOptional(&program->types[typeRef])) {
                return true;
            }
        }
    }
    return false;
}

static bool WasmProgramHasInvalidAllocatorPanic(
    const H2MirProgram* program, const uint8_t* _Nullable reachableFunctions) {
    uint32_t funcIndex;
    bool     usesRootAllocator;
    if (program == NULL) {
        return false;
    }
    usesRootAllocator = WasmProgramNeedsRootAllocator(program, reachableFunctions);
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const H2MirFunction* fn = &program->funcs[funcIndex];
        uint32_t             pc;
        if (reachableFunctions != NULL && reachableFunctions[funcIndex] == 0u) {
            continue;
        }
        for (pc = 0; pc < fn->instLen; pc++) {
            const H2MirInst* inst = &program->insts[fn->instStart + pc];
            if (inst->op == H2MirOp_ALLOC_NEW
                && ((inst->tok & H2AstFlag_NEW_HAS_ALLOC) != 0u || usesRootAllocator))
            {
                return true;
            }
        }
    }
    return false;
}

static bool WasmProgramNeedsFunctionTable(const H2MirProgram* program) {
    uint32_t i;
    if (program == NULL) {
        return false;
    }
    for (i = 0; i < program->constLen; i++) {
        if (program->consts[i].kind == H2MirConst_FUNCTION) {
            return true;
        }
    }
    for (i = 0; i < program->instLen; i++) {
        if (program->insts[i].op == H2MirOp_CALL_INDIRECT
            || (program->insts[i].op == H2MirOp_ALLOC_NEW
                && (program->insts[i].tok & H2AstFlag_NEW_HAS_ALLOC) != 0u))
        {
            return true;
        }
    }
    return false;
}

static bool WasmProgramNeedsFrameMemory(
    const H2MirProgram* program, const uint8_t* _Nullable reachableFunctions);
static bool WasmProgramNeedsHeapMemory(
    const H2MirProgram* program, const uint8_t* _Nullable reachableFunctions);
static bool WasmFunctionNeedsIndirectScratch(const H2MirProgram* program, const H2MirFunction* fn);
static int  WasmEmitAddrFromFrame(
    HOPWasmBuf* body, const HOPWasmEmitState* state, uint32_t offset, uint16_t addend);
static int WasmTypeByteSize(const H2MirProgram* program, uint32_t typeRefIndex);
static int WasmTypeByteAlign(const H2MirProgram* program, uint32_t typeRefIndex);
static int WasmFindAggregateField(
    const H2MirProgram* program,
    uint32_t            ownerTypeRef,
    const H2MirField*   fieldRef,
    uint32_t*           outFieldIndex,
    uint32_t*           outOffset);
static bool WasmAggregateHasDynamicLayout(const H2MirProgram* program, uint32_t ownerTypeRef);
static int  WasmTempTypeRefForInst(const H2MirProgram* program, const H2MirInst* inst);
static int  WasmTempOffsetForPc(
    const H2MirProgram*     program,
    const H2MirFunction*    fn,
    const HOPWasmEmitState* state,
    uint32_t                pc,
    uint32_t* _Nonnull outOffset);

static uint8_t WasmTypeKindSlotCount(uint8_t typeKind) {
    switch (typeKind) {
        case HOPWasmType_I32:
        case HOPWasmType_I64:
        case HOPWasmType_STR_PTR:
        case HOPWasmType_U8_PTR:
        case HOPWasmType_I32_PTR:
        case HOPWasmType_I8_PTR:
        case HOPWasmType_U16_PTR:
        case HOPWasmType_I16_PTR:
        case HOPWasmType_U32_PTR:
        case HOPWasmType_OPAQUE_PTR:
        case HOPWasmType_ARRAY_VIEW_U8:
        case HOPWasmType_ARRAY_VIEW_I8:
        case HOPWasmType_ARRAY_VIEW_U16:
        case HOPWasmType_ARRAY_VIEW_I16:
        case HOPWasmType_ARRAY_VIEW_U32:
        case HOPWasmType_ARRAY_VIEW_I32:
        case HOPWasmType_AGG_REF:
        case HOPWasmType_FUNC_REF:
        case HOPWasmType_FUNC_REF_PTR:   return 1u;
        case HOPWasmType_STR_REF:
        case HOPWasmType_SLICE_U8:
        case HOPWasmType_SLICE_I8:
        case HOPWasmType_SLICE_U16:
        case HOPWasmType_SLICE_I16:
        case HOPWasmType_SLICE_U32:
        case HOPWasmType_SLICE_I32:
        case HOPWasmType_SLICE_AGG:      return 2u;
        default:                         return 0u;
    }
}

static bool WasmTypeKindIsSupported(uint8_t typeKind) {
    return WasmTypeKindSlotCount(typeKind) != 0u;
}

static bool WasmTypeKindFromMirType(
    const H2MirProgram* program, uint32_t typeRefIndex, uint8_t* outTypeKind) {
    const H2MirTypeRef* typeRef;
    if (outTypeKind == NULL) {
        return false;
    }
    *outTypeKind = HOPWasmType_VOID;
    if (typeRefIndex == UINT32_MAX) {
        return true;
    }
    if (program == NULL || typeRefIndex >= program->typeLen) {
        return false;
    }
    typeRef = &program->types[typeRefIndex];
    if (WasmTypeRefIsSourceLocation(program, typeRefIndex)) {
        *outTypeKind = HOPWasmType_I32;
        return true;
    }
    if (H2MirTypeRefIsStrRef(typeRef)) {
        *outTypeKind = HOPWasmType_STR_REF;
        return true;
    }
    if (H2MirTypeRefIsStrObj(typeRef)) {
        *outTypeKind = HOPWasmType_STR_PTR;
        return true;
    }
    if (H2MirTypeRefIsStrPtr(typeRef)) {
        *outTypeKind = HOPWasmType_STR_PTR;
        return true;
    }
    if (H2MirTypeRefIsU8Ptr(typeRef)) {
        *outTypeKind = HOPWasmType_U8_PTR;
        return true;
    }
    if (H2MirTypeRefIsI8Ptr(typeRef)) {
        *outTypeKind = HOPWasmType_I8_PTR;
        return true;
    }
    if (H2MirTypeRefIsU16Ptr(typeRef)) {
        *outTypeKind = HOPWasmType_U16_PTR;
        return true;
    }
    if (H2MirTypeRefIsI16Ptr(typeRef)) {
        *outTypeKind = HOPWasmType_I16_PTR;
        return true;
    }
    if (H2MirTypeRefIsU32Ptr(typeRef)) {
        *outTypeKind = HOPWasmType_U32_PTR;
        return true;
    }
    if (H2MirTypeRefIsFixedArray(typeRef)) {
        switch (H2MirTypeRefIntKind(typeRef)) {
            case H2MirIntKind_U8:   *outTypeKind = HOPWasmType_ARRAY_VIEW_U8; return true;
            case H2MirIntKind_I8:   *outTypeKind = HOPWasmType_ARRAY_VIEW_I8; return true;
            case H2MirIntKind_U16:  *outTypeKind = HOPWasmType_ARRAY_VIEW_U16; return true;
            case H2MirIntKind_I16:  *outTypeKind = HOPWasmType_ARRAY_VIEW_I16; return true;
            case H2MirIntKind_U32:  *outTypeKind = HOPWasmType_ARRAY_VIEW_U32; return true;
            case H2MirIntKind_BOOL:
            case H2MirIntKind_I32:  *outTypeKind = HOPWasmType_ARRAY_VIEW_I32; return true;
            default:                return false;
        }
    }
    if (H2MirTypeRefIsFixedArrayView(typeRef)) {
        switch (H2MirTypeRefIntKind(typeRef)) {
            case H2MirIntKind_U8:   *outTypeKind = HOPWasmType_ARRAY_VIEW_U8; return true;
            case H2MirIntKind_I8:   *outTypeKind = HOPWasmType_ARRAY_VIEW_I8; return true;
            case H2MirIntKind_U16:  *outTypeKind = HOPWasmType_ARRAY_VIEW_U16; return true;
            case H2MirIntKind_I16:  *outTypeKind = HOPWasmType_ARRAY_VIEW_I16; return true;
            case H2MirIntKind_U32:  *outTypeKind = HOPWasmType_ARRAY_VIEW_U32; return true;
            case H2MirIntKind_BOOL:
            case H2MirIntKind_I32:  *outTypeKind = HOPWasmType_ARRAY_VIEW_I32; return true;
            default:                return false;
        }
    }
    if (H2MirTypeRefIsSliceView(typeRef)) {
        switch (H2MirTypeRefIntKind(typeRef)) {
            case H2MirIntKind_U8:   *outTypeKind = HOPWasmType_SLICE_U8; return true;
            case H2MirIntKind_I8:   *outTypeKind = HOPWasmType_SLICE_I8; return true;
            case H2MirIntKind_U16:  *outTypeKind = HOPWasmType_SLICE_U16; return true;
            case H2MirIntKind_I16:  *outTypeKind = HOPWasmType_SLICE_I16; return true;
            case H2MirIntKind_U32:  *outTypeKind = HOPWasmType_SLICE_U32; return true;
            case H2MirIntKind_BOOL:
            case H2MirIntKind_I32:  *outTypeKind = HOPWasmType_SLICE_I32; return true;
            default:                return false;
        }
    }
    if (H2MirTypeRefIsVArrayView(typeRef)) {
        switch (H2MirTypeRefIntKind(typeRef)) {
            case H2MirIntKind_U8:   *outTypeKind = HOPWasmType_SLICE_U8; return true;
            case H2MirIntKind_I8:   *outTypeKind = HOPWasmType_SLICE_I8; return true;
            case H2MirIntKind_U16:  *outTypeKind = HOPWasmType_SLICE_U16; return true;
            case H2MirIntKind_I16:  *outTypeKind = HOPWasmType_SLICE_I16; return true;
            case H2MirIntKind_U32:  *outTypeKind = HOPWasmType_SLICE_U32; return true;
            case H2MirIntKind_BOOL:
            case H2MirIntKind_I32:  *outTypeKind = HOPWasmType_SLICE_I32; return true;
            default:                return false;
        }
    }
    if (H2MirTypeRefIsAggSliceView(typeRef)) {
        *outTypeKind = HOPWasmType_SLICE_AGG;
        return true;
    }
    if (H2MirTypeRefIsAggregate(typeRef)) {
        *outTypeKind = HOPWasmType_AGG_REF;
        return true;
    }
    if (H2MirTypeRefIsFuncRef(typeRef)) {
        *outTypeKind = HOPWasmType_FUNC_REF;
        return true;
    }
    if (H2MirTypeRefIsOpaquePtr(typeRef)) {
        *outTypeKind = HOPWasmType_OPAQUE_PTR;
        return true;
    }
    if (H2MirTypeRefIsI32Ptr(typeRef)) {
        *outTypeKind = HOPWasmType_I32_PTR;
        return true;
    }
    if (H2MirTypeRefScalarKind(typeRef) == H2MirTypeScalar_I64) {
        *outTypeKind = HOPWasmType_I64;
        return true;
    }
    if (H2MirTypeRefScalarKind(typeRef) == H2MirTypeScalar_I32) {
        *outTypeKind = HOPWasmType_I32;
        return true;
    }
    return false;
}

static bool WasmTypeKindIsPointer(uint8_t typeKind) {
    return typeKind == HOPWasmType_STR_PTR || typeKind == HOPWasmType_U8_PTR
        || typeKind == HOPWasmType_I32_PTR || typeKind == HOPWasmType_I8_PTR
        || typeKind == HOPWasmType_U16_PTR || typeKind == HOPWasmType_I16_PTR
        || typeKind == HOPWasmType_U32_PTR || typeKind == HOPWasmType_OPAQUE_PTR
        || typeKind == HOPWasmType_FUNC_REF_PTR;
}

static bool WasmTypeKindIsArrayView(uint8_t typeKind) {
    return typeKind == HOPWasmType_ARRAY_VIEW_U8 || typeKind == HOPWasmType_ARRAY_VIEW_I8
        || typeKind == HOPWasmType_ARRAY_VIEW_U16 || typeKind == HOPWasmType_ARRAY_VIEW_I16
        || typeKind == HOPWasmType_ARRAY_VIEW_U32 || typeKind == HOPWasmType_ARRAY_VIEW_I32;
}

static bool WasmTypeKindUsesSRet(uint8_t typeKind) {
    return typeKind == HOPWasmType_AGG_REF || WasmTypeKindIsArrayView(typeKind);
}

static bool WasmTypeKindIsSlice(uint8_t typeKind) {
    return typeKind == HOPWasmType_SLICE_U8 || typeKind == HOPWasmType_SLICE_I8
        || typeKind == HOPWasmType_SLICE_U16 || typeKind == HOPWasmType_SLICE_I16
        || typeKind == HOPWasmType_SLICE_U32 || typeKind == HOPWasmType_SLICE_I32
        || typeKind == HOPWasmType_SLICE_AGG;
}

static bool WasmTypeKindIsRawSingleSlot(uint8_t typeKind) {
    return typeKind == HOPWasmType_I32 || typeKind == HOPWasmType_I64
        || WasmTypeKindIsPointer(typeKind) || WasmTypeKindIsArrayView(typeKind)
        || typeKind == HOPWasmType_FUNC_REF;
}

static bool WasmTypeRefsCompatible(
    const H2MirProgram* program, uint32_t expectedTypeRef, uint32_t actualTypeRef);

static bool WasmI32LikeValueUsesUnsignedExtend(
    const H2MirProgram* program, uint8_t valueTypeKind, uint32_t valueTypeRef) {
    const H2MirTypeRef* typeRef;
    if (WasmTypeKindIsPointer(valueTypeKind) || WasmTypeKindIsArrayView(valueTypeKind)) {
        return true;
    }
    if (program == NULL || valueTypeRef == UINT32_MAX || valueTypeRef >= program->typeLen) {
        return false;
    }
    typeRef = &program->types[valueTypeRef];
    if (H2MirTypeRefScalarKind(typeRef) != H2MirTypeScalar_I32) {
        return false;
    }
    switch (H2MirTypeRefIntKind(typeRef)) {
        case H2MirIntKind_U8:
        case H2MirIntKind_U16:
        case H2MirIntKind_U32: return true;
        default:               return false;
    }
}

static bool WasmValueKindCompatibleWithLocal(
    const H2MirProgram* program,
    uint8_t             expectedTypeKind,
    uint8_t             actualTypeKind,
    uint32_t            expectedTypeRef,
    uint32_t            actualTypeRef) {
    if (actualTypeKind == expectedTypeKind) {
        if (expectedTypeKind == HOPWasmType_AGG_REF && expectedTypeRef < UINT32_MAX
            && actualTypeRef < UINT32_MAX)
        {
            return WasmTypeRefsCompatible(program, expectedTypeRef, actualTypeRef);
        }
        return true;
    }
    if (actualTypeKind == HOPWasmType_STR_REF && expectedTypeKind == HOPWasmType_STR_PTR) {
        return true;
    }
    if (actualTypeKind == HOPWasmType_I32 && WasmTypeKindIsPointer(expectedTypeKind)) {
        return true;
    }
    if (actualTypeKind == HOPWasmType_STR_REF && expectedTypeKind == HOPWasmType_SLICE_U8) {
        return true;
    }
    return false;
}

static int WasmAdaptCallArgValue(
    HOPWasmBuf*         body,
    const H2MirProgram* program,
    uint8_t             expectedTypeKind,
    uint8_t             actualTypeKind,
    uint32_t            actualTypeRef) {
    if (actualTypeKind == expectedTypeKind) {
        return 0;
    }
    if (actualTypeKind == HOPWasmType_I32 && WasmTypeKindIsPointer(expectedTypeKind)) {
        return 0;
    }
    if ((actualTypeKind == HOPWasmType_I32 || WasmTypeKindIsPointer(actualTypeKind)
         || WasmTypeKindIsArrayView(actualTypeKind))
        && expectedTypeKind == HOPWasmType_I64)
    {
        return WasmAppendByte(
            body,
            WasmI32LikeValueUsesUnsignedExtend(program, actualTypeKind, actualTypeRef)
                ? 0xadu
                : 0xacu);
    }
    return -1;
}

static uint8_t WasmTypeKindWasmValueType(uint8_t typeKind) {
    return typeKind == HOPWasmType_I64 ? 0x7eu : 0x7fu;
}

static uint32_t WasmTypeKindElementSize(uint8_t typeKind) {
    switch (typeKind) {
        case HOPWasmType_U8_PTR:
        case HOPWasmType_I8_PTR:
        case HOPWasmType_ARRAY_VIEW_U8:
        case HOPWasmType_ARRAY_VIEW_I8:
        case HOPWasmType_SLICE_U8:
        case HOPWasmType_SLICE_I8:       return 1u;
        case HOPWasmType_U16_PTR:
        case HOPWasmType_I16_PTR:
        case HOPWasmType_ARRAY_VIEW_U16:
        case HOPWasmType_ARRAY_VIEW_I16:
        case HOPWasmType_SLICE_U16:
        case HOPWasmType_SLICE_I16:      return 2u;
        case HOPWasmType_U32_PTR:
        case HOPWasmType_I32_PTR:
        case HOPWasmType_ARRAY_VIEW_U32:
        case HOPWasmType_ARRAY_VIEW_I32:
        case HOPWasmType_SLICE_U32:
        case HOPWasmType_SLICE_I32:
        case HOPWasmType_STR_PTR:        return 4u;
        default:                         return 0u;
    }
}

static uint32_t WasmScalarByteWidth(uint8_t intKind) {
    switch ((H2MirIntKind)intKind) {
        case H2MirIntKind_U8:
        case H2MirIntKind_I8:  return 1u;
        case H2MirIntKind_U16:
        case H2MirIntKind_I16: return 2u;
        default:               return 4u;
    }
}

static uint32_t WasmIntKindByteWidth(H2MirIntKind intKind) {
    switch (intKind) {
        case H2MirIntKind_U8:
        case H2MirIntKind_I8:  return 1u;
        case H2MirIntKind_U16:
        case H2MirIntKind_I16: return 2u;
        default:               return 4u;
    }
}

static uint8_t WasmLocalAddressTypeKind(const HOPWasmEmitState* state, uint32_t localIndex) {
    uint8_t intKind;
    if (state == NULL || localIndex >= 256u) {
        return HOPWasmType_VOID;
    }
    if (state->localKinds[localIndex] == HOPWasmType_STR_REF) {
        return HOPWasmType_STR_PTR;
    }
    if (state->localKinds[localIndex] != HOPWasmType_I32) {
        return HOPWasmType_VOID;
    }
    intKind = state->localIntKinds[localIndex];
    switch ((H2MirIntKind)intKind) {
        case H2MirIntKind_U8:  return HOPWasmType_U8_PTR;
        case H2MirIntKind_I8:  return HOPWasmType_I8_PTR;
        case H2MirIntKind_U16: return HOPWasmType_U16_PTR;
        case H2MirIntKind_I16: return HOPWasmType_I16_PTR;
        case H2MirIntKind_U32: return HOPWasmType_U32_PTR;
        default:               return HOPWasmType_I32_PTR;
    }
}

static uint32_t WasmAlign4(uint32_t n) {
    return (n + 3u) & ~3u;
}

static uint32_t WasmOpaquePtrPointeeTypeRef(const H2MirProgram* program, uint32_t typeRefIndex) {
    if (program == NULL || typeRefIndex >= program->typeLen) {
        return UINT32_MAX;
    }
    if (H2MirTypeRefIsAggregate(&program->types[typeRefIndex])) {
        return typeRefIndex;
    }
    return H2MirTypeRefOpaquePointeeTypeRef(&program->types[typeRefIndex]);
}

static uint32_t WasmAggSliceElemTypeRef(const H2MirProgram* program, uint32_t typeRefIndex) {
    if (program == NULL || typeRefIndex >= program->typeLen) {
        return UINT32_MAX;
    }
    return H2MirTypeRefAggSliceElemTypeRef(&program->types[typeRefIndex]);
}

static uint32_t WasmTypeElementSize(
    const H2MirProgram* program, uint8_t typeKind, uint32_t typeRefIndex) {
    uint32_t elemTypeRef;
    uint32_t elemSize;
    elemSize = WasmTypeKindElementSize(typeKind);
    if (elemSize != 0u || typeKind != HOPWasmType_SLICE_AGG) {
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
    const H2MirProgram* program, uint8_t typeKind, uint32_t typeRefIndex) {
    uint32_t pointeeTypeRef;
    if (program == NULL || typeRefIndex == UINT32_MAX || typeRefIndex >= program->typeLen) {
        return 0u;
    }
    if (typeKind == HOPWasmType_STR_PTR) {
        return 8u;
    }
    if (typeKind == HOPWasmType_OPAQUE_PTR) {
        pointeeTypeRef = WasmOpaquePtrPointeeTypeRef(program, typeRefIndex);
        return pointeeTypeRef == UINT32_MAX
                 ? 0u
                 : (uint32_t)WasmTypeByteSize(program, pointeeTypeRef);
    }
    if (H2MirTypeRefIsFixedArray(&program->types[typeRefIndex])
        || H2MirTypeRefIsFixedArrayView(&program->types[typeRefIndex]))
    {
        return WasmIntKindByteWidth(H2MirTypeRefIntKind(&program->types[typeRefIndex]))
             * H2MirTypeRefFixedArrayCount(&program->types[typeRefIndex]);
    }
    switch (typeKind) {
        case HOPWasmType_U8_PTR:
        case HOPWasmType_I8_PTR:  return 1u;
        case HOPWasmType_U16_PTR:
        case HOPWasmType_I16_PTR: return 2u;
        case HOPWasmType_U32_PTR:
        case HOPWasmType_I32_PTR: return 4u;
        default:                  return 0u;
    }
}

static uint32_t WasmAllocatedByteAlignForType(
    const H2MirProgram* program, uint8_t typeKind, uint32_t typeRefIndex) {
    uint32_t pointeeTypeRef;
    if (program == NULL || typeRefIndex == UINT32_MAX || typeRefIndex >= program->typeLen) {
        return 0u;
    }
    if (typeKind == HOPWasmType_STR_PTR) {
        return 4u;
    }
    if (typeKind == HOPWasmType_OPAQUE_PTR) {
        pointeeTypeRef = WasmOpaquePtrPointeeTypeRef(program, typeRefIndex);
        return pointeeTypeRef == UINT32_MAX
                 ? 0u
                 : (uint32_t)WasmTypeByteAlign(program, pointeeTypeRef);
    }
    if (H2MirTypeRefIsFixedArray(&program->types[typeRefIndex])
        || H2MirTypeRefIsFixedArrayView(&program->types[typeRefIndex]))
    {
        return WasmIntKindByteWidth(H2MirTypeRefIntKind(&program->types[typeRefIndex]));
    }
    switch (typeKind) {
        case HOPWasmType_U8_PTR:
        case HOPWasmType_I8_PTR:  return 1u;
        case HOPWasmType_U16_PTR:
        case HOPWasmType_I16_PTR: return 2u;
        case HOPWasmType_U32_PTR:
        case HOPWasmType_I32_PTR: return 4u;
        default:                  return 0u;
    }
}

static uint32_t WasmFrameSlotSize(uint8_t typeKind) {
    switch (typeKind) {
        case HOPWasmType_I64:            return 8u;
        case HOPWasmType_STR_REF:
        case HOPWasmType_SLICE_U8:
        case HOPWasmType_SLICE_I8:
        case HOPWasmType_SLICE_U16:
        case HOPWasmType_SLICE_I16:
        case HOPWasmType_SLICE_U32:
        case HOPWasmType_SLICE_I32:
        case HOPWasmType_SLICE_AGG:      return 8u;
        case HOPWasmType_I32:
        case HOPWasmType_STR_PTR:
        case HOPWasmType_U8_PTR:
        case HOPWasmType_I32_PTR:
        case HOPWasmType_I8_PTR:
        case HOPWasmType_U16_PTR:
        case HOPWasmType_I16_PTR:
        case HOPWasmType_U32_PTR:
        case HOPWasmType_OPAQUE_PTR:
        case HOPWasmType_ARRAY_VIEW_U8:
        case HOPWasmType_ARRAY_VIEW_I8:
        case HOPWasmType_ARRAY_VIEW_U16:
        case HOPWasmType_ARRAY_VIEW_I16:
        case HOPWasmType_ARRAY_VIEW_U32:
        case HOPWasmType_ARRAY_VIEW_I32:
        case HOPWasmType_AGG_REF:
        case HOPWasmType_FUNC_REF:
        case HOPWasmType_FUNC_REF_PTR:   return 4u;
        default:                         return 0u;
    }
}

static int WasmAnalyzeImports(
    const H2CodegenUnit*          unit,
    const HOPWasmForeignMetadata* foreign,
    const H2CodegenOptions*       options,
    HOPWasmImportLayout*          imports,
    H2Diag* _Nullable diag) {
    uint8_t* _Nullable reachableFunctions = NULL;
    uint32_t reachableFunctionsAllocSize = 0u;
    uint32_t i;
    if (imports == NULL) {
        return -1;
    }
    reachableFunctions = imports->reachableFunctions;
    reachableFunctionsAllocSize = imports->reachableFunctionsAllocSize;
    *imports = (HOPWasmImportLayout){ 0 };
    imports->reachableFunctions = reachableFunctions;
    imports->reachableFunctionsAllocSize = reachableFunctionsAllocSize;
    if (unit == NULL || unit->mirProgram == NULL) {
        return -1;
    }
    imports->platformPanicFuncIndex = UINT32_MAX;
    imports->needsFrameGlobal =
        WasmProgramNeedsFrameMemory(unit->mirProgram, imports->reachableFunctions) ? 1u : 0u;
    imports->needsHeapGlobal =
        WasmProgramNeedsHeapMemory(unit->mirProgram, imports->reachableFunctions) ? 1u : 0u;
    if (unit->mirProgram->funcLen != 0u) {
        uint32_t allocSize = 0;
        uint32_t funcBytes =
            unit->mirProgram->funcLen * (uint32_t)sizeof(*imports->funcWasmIndices);
        uint32_t globalBytes =
            unit->mirProgram->funcLen * (uint32_t)sizeof(*imports->globalImportIndices);
        uint32_t importBytes =
            unit->mirProgram->funcLen * (uint32_t)sizeof(*imports->importedFunctions);
        if (options == NULL || options->arenaGrow == NULL) {
            return -1;
        }
        imports->funcWasmIndices = (uint32_t*)options->arenaGrow(
            options->allocatorCtx, funcBytes, &allocSize);
        imports->funcWasmIndicesAllocSize = allocSize;
        allocSize = 0;
        imports->globalImportIndices = (uint32_t*)options->arenaGrow(
            options->allocatorCtx, globalBytes, &allocSize);
        imports->globalImportIndicesAllocSize = allocSize;
        allocSize = 0;
        imports->importedFunctions = (uint8_t*)options->arenaGrow(
            options->allocatorCtx, importBytes, &allocSize);
        imports->importedFunctionsAllocSize = allocSize;
        if (imports->funcWasmIndices == NULL || imports->funcWasmIndicesAllocSize < funcBytes
            || imports->globalImportIndices == NULL
            || imports->globalImportIndicesAllocSize < globalBytes
            || imports->importedFunctions == NULL
            || imports->importedFunctionsAllocSize < importBytes)
        {
            WasmSetDiag(diag, H2Diag_ARENA_OOM, 0, 0);
            return -1;
        }
        memset(imports->funcWasmIndices, 0, funcBytes);
        memset(imports->globalImportIndices, 0, globalBytes);
        memset(imports->importedFunctions, 0, importBytes);
        for (i = 0; i < unit->mirProgram->funcLen; i++) {
            imports->funcWasmIndices[i] = UINT32_MAX;
            imports->globalImportIndices[i] = UINT32_MAX;
        }
    }
    imports->hasRootAllocThunk =
        WasmProgramNeedsRootAllocator(unit->mirProgram, imports->reachableFunctions) ? 1u : 0u;
    imports->hasFunctionTable =
        (imports->hasRootAllocThunk || WasmProgramNeedsFunctionTable(unit->mirProgram)) ? 1u : 0u;
    for (i = 0; i < unit->mirProgram->funcLen; i++) {
        const H2MirFunction* fn;
        uint32_t             pc;
        if (imports->reachableFunctions != NULL && imports->reachableFunctions[i] == 0u) {
            continue;
        }
        fn = &unit->mirProgram->funcs[i];
        for (pc = 0u; pc < fn->instLen; pc++) {
            const H2MirInst* inst = &unit->mirProgram->insts[fn->instStart + pc];
            if (inst->op != H2MirOp_CALL_HOST) {
                continue;
            }
            WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_HOSTCALL, inst->start, inst->end);
            if (diag != NULL) {
                diag->detail = "host calls are not supported in this Wasm mode";
            }
            return -1;
        }
    }
    if (foreign != NULL && foreign->entries != NULL) {
        for (i = 0; i < foreign->len; i++) {
            const H2ForeignLinkageEntry* entry = &foreign->entries[i];
            if (entry->functionIndex >= unit->mirProgram->funcLen) {
                WasmSetDiag(diag, H2Diag_WASM_BACKEND_INTERNAL, entry->start, entry->end);
                return -1;
            }
            if (entry->kind == H2ForeignLinkage_WASM_IMPORT_FN
                && (imports->reachableFunctions == NULL
                    || imports->reachableFunctions[entry->functionIndex] != 0u))
            {
                imports->importedFunctions[entry->functionIndex] = 1u;
                if ((entry->flags & H2ForeignLinkageFlag_PLATFORM_PANIC) != 0u) {
                    imports->hasPlatformPanicImport = 1u;
                }
            } else if (
                entry->kind == H2ForeignLinkage_WASM_IMPORT_CONST
                || entry->kind == H2ForeignLinkage_WASM_IMPORT_VAR)
            {
                if (imports->reachableFunctions != NULL
                    && imports->reachableFunctions[entry->functionIndex] == 0u)
                {
                    continue;
                }
                uint8_t globalType = 0;
                if (!WasmTypeKindFromMirType(
                        unit->mirProgram,
                        unit->mirProgram->funcs[entry->functionIndex].typeRef,
                        &globalType)
                    || globalType != HOPWasmType_I32)
                {
                    WasmSetDiag(
                        diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, entry->start, entry->end);
                    if (diag != NULL) {
                        diag->detail =
                            "direct Wasm imported globals currently support only i32-like values";
                    }
                    return -1;
                }
                imports->globalImportIndices[entry->functionIndex] = imports->importGlobalCount++;
            }
        }
    }
    if (foreign != NULL && foreign->entries != NULL) {
        for (i = 0; i < foreign->len; i++) {
            const H2ForeignLinkageEntry* entry = &foreign->entries[i];
            if (entry->kind != H2ForeignLinkage_WASM_IMPORT_FN) {
                continue;
            }
            if (imports->reachableFunctions != NULL
                && imports->reachableFunctions[entry->functionIndex] == 0u)
            {
                continue;
            }
            if (imports->funcWasmIndices != NULL && imports->importedFunctions != NULL
                && imports->importedFunctions[entry->functionIndex] != 0u
                && imports->funcWasmIndices[entry->functionIndex] == UINT32_MAX)
            {
                imports->funcWasmIndices[entry->functionIndex] = imports->importFuncCount;
            }
            if ((entry->flags & H2ForeignLinkageFlag_PLATFORM_PANIC) != 0u) {
                imports->platformPanicFuncIndex = imports->importFuncCount;
            }
            imports->importFuncCount++;
        }
    }
    for (i = 0; i < unit->mirProgram->funcLen; i++) {
        if (imports->reachableFunctions != NULL && imports->reachableFunctions[i] == 0u) {
            continue;
        }
        if (imports->importedFunctions == NULL || imports->importedFunctions[i] == 0u) {
            imports->funcWasmIndices[i] = imports->importFuncCount + imports->definedFuncCount++;
        }
    }
    if ((WasmProgramHasAssert(unit->mirProgram, imports->reachableFunctions)
         || WasmProgramHasAllocNullPanic(unit->mirProgram, imports->reachableFunctions)
         || WasmProgramHasInvalidAllocatorPanic(unit->mirProgram, imports->reachableFunctions))
        && !imports->hasPlatformPanicImport)
    {
        WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, 0, 0);
        if (diag != NULL) {
            diag->detail = "selected platform does not provide imported panic for direct Wasm";
        }
        return -1;
    }
    imports->frameGlobalIndex = imports->importGlobalCount;
    imports->heapGlobalIndex = imports->importGlobalCount + (imports->needsFrameGlobal ? 1u : 0u);
    if (imports->hasFunctionTable) {
        imports->allocatorIndirectTypeIndex = unit->mirProgram->funcLen;
        imports->tableFuncCount =
            unit->mirProgram->funcLen + (imports->hasRootAllocThunk ? 1u : 0u);
        imports->rootAllocTableIndex = unit->mirProgram->funcLen;
        if (imports->hasRootAllocThunk) {
            imports->rootAllocFuncIndex = imports->importFuncCount + imports->definedFuncCount;
        }
    }
    return 0;
}

static int WasmBuildStringLayout(
    const H2CodegenUnit*       unit,
    const HOPWasmImportLayout* imports,
    const H2CodegenOptions*    options,
    HOPWasmStringLayout* _Nonnull strings,
    H2Diag* _Nullable diag) {
    static const char panicMessage[] = "assertion failed";
    static const char allocPanicMessage[] = "unwrap: null value";
    static const char invalidAllocatorPanicMessage[] = "invalid allocator";
    uint32_t          i;
    uint32_t          allocSize = 0;
    if (unit == NULL || unit->mirProgram == NULL || options == NULL || strings == NULL) {
        return -1;
    }
    memset(strings, 0, sizeof(*strings));
    strings->data.options = options;
    if (unit->mirProgram->constLen != 0u) {
        strings->constRefs = (HOPWasmStringRef*)options->arenaGrow(
            options->allocatorCtx,
            unit->mirProgram->constLen * (uint32_t)sizeof(HOPWasmStringRef),
            &allocSize);
        if (strings->constRefs == NULL
            || allocSize < unit->mirProgram->constLen * sizeof(HOPWasmStringRef))
        {
            if (strings->constRefs != NULL && options->arenaFree != NULL) {
                options->arenaFree(options->allocatorCtx, strings->constRefs, allocSize);
            }
            WasmSetDiag(diag, H2Diag_ARENA_OOM, 0, 0);
            return -1;
        }
        for (i = 0; i < unit->mirProgram->constLen; i++) {
            strings->constRefs[i] = (HOPWasmStringRef){
                .objectOffset = UINT32_MAX, .dataOffset = UINT32_MAX, .len = 0u
            };
        }
    }
    for (i = 0; i < unit->mirProgram->constLen; i++) {
        const H2MirConst* c = &unit->mirProgram->consts[i];
        if (c->kind != H2MirConst_STRING) {
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
            WasmSetDiag(diag, H2Diag_ARENA_OOM, 0, 0);
            return -1;
        }
    }
    if (WasmProgramNeedsRootAllocator(unit->mirProgram, imports->reachableFunctions)) {
        while (strings->data.len < 4u) {
            if (WasmAppendByte(&strings->data, 0u) != 0) {
                WasmSetDiag(diag, H2Diag_ARENA_OOM, 0, 0);
                return -1;
            }
        }
        strings->hasRootAllocator = 1u;
        strings->rootAllocatorOffset = strings->data.len;
        if (WasmAppendU32LE(&strings->data, imports != NULL ? imports->rootAllocTableIndex : 0u)
                != 0
            || WasmAppendU32LE(&strings->data, 0u) != 0)
        {
            WasmSetDiag(diag, H2Diag_ARENA_OOM, 0, 0);
            return -1;
        }
    } else {
        strings->rootAllocatorOffset = UINT32_MAX;
    }
    if (imports != NULL && imports->hasPlatformPanicImport
        && WasmProgramHasAssert(unit->mirProgram, imports->reachableFunctions))
    {
        strings->hasAssertPanicString = 1u;
        strings->assertPanic.objectOffset = UINT32_MAX;
        strings->assertPanic.dataOffset = strings->data.len;
        strings->assertPanic.len = (uint32_t)(sizeof(panicMessage) - 1u);
        if (WasmAppendBytes(&strings->data, panicMessage, strings->assertPanic.len) != 0
            || WasmAppendByte(&strings->data, 0u) != 0)
        {
            WasmSetDiag(diag, H2Diag_ARENA_OOM, 0, 0);
            return -1;
        }
    }
    if (imports != NULL && imports->hasPlatformPanicImport
        && WasmProgramHasAllocNullPanic(unit->mirProgram, imports->reachableFunctions))
    {
        strings->hasAllocNullPanicString = 1u;
        strings->allocNullPanic.objectOffset = UINT32_MAX;
        strings->allocNullPanic.dataOffset = strings->data.len;
        strings->allocNullPanic.len = (uint32_t)(sizeof(allocPanicMessage) - 1u);
        if (WasmAppendBytes(&strings->data, allocPanicMessage, strings->allocNullPanic.len) != 0
            || WasmAppendByte(&strings->data, 0u) != 0)
        {
            WasmSetDiag(diag, H2Diag_ARENA_OOM, 0, 0);
            return -1;
        }
    }
    if (imports != NULL && imports->hasPlatformPanicImport
        && WasmProgramHasInvalidAllocatorPanic(unit->mirProgram, imports->reachableFunctions))
    {
        strings->hasInvalidAllocatorPanicString = 1u;
        strings->invalidAllocatorPanic.objectOffset = UINT32_MAX;
        strings->invalidAllocatorPanic.dataOffset = strings->data.len;
        strings->invalidAllocatorPanic.len = (uint32_t)(sizeof(invalidAllocatorPanicMessage) - 1u);
        if (WasmAppendBytes(
                &strings->data, invalidAllocatorPanicMessage, strings->invalidAllocatorPanic.len)
                != 0
            || WasmAppendByte(&strings->data, 0u) != 0)
        {
            WasmSetDiag(diag, H2Diag_ARENA_OOM, 0, 0);
            return -1;
        }
    }
    return 0;
}

static uint32_t WasmStaticDataEnd(
    const HOPWasmStringLayout* strings, const HOPWasmEntryLayout* entry) {
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
    const H2MirProgram*        program,
    const HOPWasmFnSig*        sigs,
    const HOPWasmImportLayout* imports,
    const HOPWasmStringLayout* strings,
    HOPWasmEntryLayout* _Nonnull entry,
    H2Diag* _Nullable diag) {
    uint32_t i;
    if (program == NULL || sigs == NULL || imports == NULL || strings == NULL || entry == NULL) {
        return -1;
    }
    *entry = (HOPWasmEntryLayout){
        .mainFuncIndex = UINT32_MAX,
        .wrapperTypeIndex = UINT32_MAX,
        .wrapperFuncIndex = UINT32_MAX,
        .resultOffset = UINT32_MAX,
    };
    for (i = 0; i < program->funcLen; i++) {
        const H2MirFunction* fn = &program->funcs[i];
        const HOPWasmFnSig*  sig = &sigs[i];
        if (!WasmFunctionIsNamedMain(program, fn)) {
            continue;
        }
        entry->mainFuncIndex = i;
        if (sig->logicalResultKind == HOPWasmType_VOID || sig->logicalResultKind == HOPWasmType_I32)
        {
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
            WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, fn->nameStart, fn->nameEnd);
            if (diag != NULL) {
                diag->detail = "unsupported exported main return kind";
            }
            return -1;
        }
        entry->resultOffset = WasmAlign4(strings->data.len);
        entry->wrapperTypeIndex = program->funcLen + (imports->hasFunctionTable ? 1u : 0u);
        entry->wrapperFuncIndex =
            imports->importFuncCount + imports->definedFuncCount
            + (imports->hasRootAllocThunk ? 1u : 0u);
        return 0;
    }
    return 0;
}

static bool WasmFunctionNeedsFrameMemory(const H2MirProgram* program, const H2MirFunction* fn) {
    uint32_t i;
    if (program == NULL || fn == NULL) {
        return false;
    }
    for (i = 0; i < fn->localCount; i++) {
        uint8_t typeKind = 0;
        if (WasmTypeKindFromMirType(program, program->locals[fn->localStart + i].typeRef, &typeKind)
            && (typeKind == HOPWasmType_STR_PTR || typeKind == HOPWasmType_AGG_REF
                || WasmTypeKindIsPointer(typeKind) || WasmTypeKindIsArrayView(typeKind)
                || WasmTypeKindIsSlice(typeKind)))
        {
            if (typeKind == HOPWasmType_AGG_REF
                && WasmTypeByteSize(program, program->locals[fn->localStart + i].typeRef) == 0)
            {
                continue;
            }
            return true;
        }
    }
    for (i = 0; i < fn->instLen; i++) {
        switch (program->insts[fn->instStart + i].op) {
            case H2MirOp_AGG_ZERO:
            case H2MirOp_AGG_SET:
            case H2MirOp_LOCAL_ADDR:
            case H2MirOp_ARRAY_ADDR:
            case H2MirOp_DEREF_LOAD:
            case H2MirOp_DEREF_STORE:
            case H2MirOp_SEQ_LEN:
            case H2MirOp_STR_CSTR:
            case H2MirOp_INDEX:
            case H2MirOp_AGG_GET:
            case H2MirOp_AGG_ADDR:
            case H2MirOp_ALLOC_NEW:
            case H2MirOp_CTX_GET:
            case H2MirOp_CTX_ADDR:
            case H2MirOp_SLICE_MAKE:  return true;
            default:                  break;
        }
    }
    return false;
}

static bool WasmProgramNeedsFrameMemory(
    const H2MirProgram* program, const uint8_t* _Nullable reachableFunctions) {
    uint32_t i;
    if (program == NULL) {
        return false;
    }
    for (i = 0; i < program->funcLen; i++) {
        if (reachableFunctions != NULL && reachableFunctions[i] == 0u) {
            continue;
        }
        if (WasmFunctionNeedsFrameMemory(program, &program->funcs[i])) {
            return true;
        }
    }
    return false;
}

static bool WasmProgramNeedsHeapMemory(
    const H2MirProgram* program, const uint8_t* _Nullable reachableFunctions) {
    uint32_t funcIndex;
    if (program == NULL) {
        return false;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const H2MirFunction* fn;
        uint32_t             pc;
        if (reachableFunctions != NULL && reachableFunctions[funcIndex] == 0u) {
            continue;
        }
        fn = &program->funcs[funcIndex];
        for (pc = 0; pc < fn->instLen; pc++) {
            if (program->insts[fn->instStart + pc].op == H2MirOp_ALLOC_NEW) {
                return true;
            }
        }
    }
    return false;
}

static bool WasmFunctionNeedsIndirectScratch(const H2MirProgram* program, const H2MirFunction* fn) {
    uint32_t pc;
    if (program == NULL || fn == NULL) {
        return false;
    }
    for (pc = 0; pc < fn->instLen; pc++) {
        if (program->insts[fn->instStart + pc].op == H2MirOp_CALL_INDIRECT) {
            return true;
        }
    }
    return false;
}

static bool WasmFunctionNeedsDirectCallScratch(
    const H2MirProgram* program, const H2MirFunction* fn) {
    uint32_t pc;
    if (program == NULL || fn == NULL) {
        return false;
    }
    for (pc = 0; pc < fn->instLen; pc++) {
        const H2MirInst* inst = &program->insts[fn->instStart + pc];
        if (inst->op == H2MirOp_CALL_FN && H2MirCallArgCountFromTok(inst->tok) > 0u) {
            return true;
        }
    }
    return false;
}

static bool WasmSigMatchesAllocatorIndirect(const HOPWasmFnSig* sig) {
    if (sig == NULL || sig->wasmParamCount != 7u || sig->wasmResultCount != 1u
        || sig->wasmResultTypes[0] != 0x7fu)
    {
        return false;
    }
    return sig->wasmParamTypes[0] == 0x7fu && sig->wasmParamTypes[1] == 0x7fu
        && sig->wasmParamTypes[2] == 0x7fu && sig->wasmParamTypes[3] == 0x7fu
        && sig->wasmParamTypes[4] == 0x7fu && sig->wasmParamTypes[5] == 0x7fu
        && sig->wasmParamTypes[6] == 0x7fu;
}

static bool WasmSigSameShape(const HOPWasmFnSig* a, const HOPWasmFnSig* b) {
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

static uint32_t WasmFindFunctionValueTypeRef(const H2MirProgram* program, uint32_t functionIndex) {
    uint32_t i;
    if (program == NULL || functionIndex >= program->funcLen) {
        return UINT32_MAX;
    }
    for (i = 0; i < program->typeLen; i++) {
        if (H2MirTypeRefFuncRefFunctionIndex(&program->types[i]) == functionIndex) {
            return i;
        }
    }
    return UINT32_MAX;
}

static int WasmBuildFunctionSignatures(
    const H2MirProgram*     program,
    const H2CodegenOptions* options,
    const uint8_t* _Nullable reachableFunctions,
    HOPWasmFnSig* sigs,
    H2Diag* _Nullable diag) {
    uint32_t i;
    if (program == NULL || sigs == NULL) {
        return -1;
    }
    for (i = 0; i < program->funcLen; i++) {
        const H2MirFunction* fn = &program->funcs[i];
        HOPWasmFnSig*        sig = &sigs[i];
        uint32_t             j;
        memset(sig, 0, sizeof(*sig));
        sig->typeIndex = i;
        sig->logicalResultTypeRef = fn->typeRef;
        if (reachableFunctions != NULL && reachableFunctions[i] == 0u) {
            continue;
        }
        if (fn->paramCount > sizeof(sig->logicalParamKinds)) {
            WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, fn->nameStart, fn->nameEnd);
            if (diag != NULL) {
                diag->detail = "too many Wasm parameters";
            }
            return -1;
        }
        for (j = 0; j < fn->paramCount; j++) {
            const H2MirLocal* local;
            uint8_t           paramType = 0;
            if (fn->localStart + j >= program->localLen) {
                WasmSetDiag(diag, H2Diag_WASM_BACKEND_INTERNAL, fn->nameStart, fn->nameEnd);
                return -1;
            }
            local = &program->locals[fn->localStart + j];
            if (WasmLocalIsSourceLocation(program, fn, local)) {
                paramType = HOPWasmType_I32;
            } else if (
                !WasmTypeKindFromMirType(program, local->typeRef, &paramType)
                || !WasmTypeKindIsSupported(paramType))
            {
                paramType = HOPWasmType_VOID;
            }
            if (!WasmTypeKindIsSupported(paramType)) {
                WasmSetDiag(
                    diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, local->nameStart, local->nameEnd);
                if (diag != NULL) {
                    diag->detail = WasmFunctionDetail(
                        options,
                        program,
                        fn,
                        "unsupported parameter type in ",
                        ": only scalar, pointer-like, array-view, slice, and &str parameters are "
                        "supported");
                    if (diag->detail == NULL) {
                        diag->detail = "only scalar, pointer-like, array-view, slice, and &str "
                                       "parameters are supported";
                    }
                }
                return -1;
            }
            if (sig->wasmParamCount + WasmTypeKindSlotCount(paramType)
                > sizeof(sig->wasmParamTypes))
            {
                WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, fn->nameStart, fn->nameEnd);
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
                sig->wasmParamTypes[sig->wasmParamCount++] = WasmTypeKindWasmValueType(paramType);
            }
        }
        sig->logicalParamCount = (uint8_t)fn->paramCount;
        if (!WasmTypeKindFromMirType(program, fn->typeRef, &sig->logicalResultKind)) {
            WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, fn->nameStart, fn->nameEnd);
            if (diag != NULL) {
                diag->detail = WasmFunctionDetail(
                    options,
                    program,
                    fn,
                    "unsupported return type in ",
                    ": only void, scalar, pointer-like, slice, and &str returns are supported");
                if (diag->detail == NULL) {
                    diag->detail =
                        "only void, scalar, pointer-like, slice, and &str returns are supported";
                }
            }
            return -1;
        }
        sig->usesSRet = WasmTypeKindUsesSRet(sig->logicalResultKind) ? 1u : 0u;
        if (sig->usesSRet) {
            if (sig->wasmParamCount + 1u > sizeof(sig->wasmParamTypes)) {
                WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, fn->nameStart, fn->nameEnd);
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
        } else if (sig->logicalResultKind != HOPWasmType_VOID) {
            sig->wasmResultTypes[0] = WasmTypeKindWasmValueType(sig->logicalResultKind);
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

static int WasmStackPushEx(HOPWasmEmitState* state, uint8_t typeKind, uint32_t typeRef) {
    if (state == NULL || state->stackLen >= sizeof(state->stackKinds)) {
        return -1;
    }
    state->stackKinds[state->stackLen++] = typeKind;
    state->stackTypeRefs[state->stackLen - 1u] = typeRef;
    return 0;
}

static int WasmStackPopEx(
    HOPWasmEmitState* state, uint8_t* _Nullable outTypeKind, uint32_t* _Nullable outTypeRef) {
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

static int WasmStackPush(HOPWasmEmitState* state, uint8_t typeKind) {
    return WasmStackPushEx(state, typeKind, UINT32_MAX);
}

static int WasmStackPop(HOPWasmEmitState* state, uint8_t* outTypeKind) {
    return WasmStackPopEx(state, outTypeKind, NULL);
}

static int WasmPrepareFunctionState(
    const H2MirProgram*  program,
    const H2MirFunction* fn,
    HOPWasmEmitState*    state,
    H2Diag* _Nullable diag) {
    uint32_t i;
    uint16_t nextValueIndex = 0;
    uint8_t  resultKind = HOPWasmType_VOID;
    if (program == NULL || fn == NULL || state == NULL) {
        return -1;
    }
    memset(state, 0, sizeof(*state));
    state->allocCallTempOffset = UINT32_MAX;
    state->directCallScratchI32Start = UINT16_MAX;
    state->directCallScratchI64Start = UINT16_MAX;
    state->scratchI64Local = UINT16_MAX;
    if (fn->localCount > sizeof(state->localKinds)) {
        WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, fn->nameStart, fn->nameEnd);
        if (diag != NULL) {
            diag->detail = "too many locals";
        }
        return -1;
    }
    state->usesFrame = WasmFunctionNeedsFrameMemory(program, fn) ? 1u : 0u;
    for (i = 0; i < fn->localCount; i++) {
        const H2MirLocal* local = &program->locals[fn->localStart + i];
        uint8_t           typeKind = 0;
        if (WasmLocalIsSourceLocation(program, fn, local)) {
            typeKind = HOPWasmType_I32;
        } else if (
            !WasmTypeKindFromMirType(program, local->typeRef, &typeKind)
            || !WasmTypeKindIsSupported(typeKind))
        {
            typeKind = HOPWasmType_VOID;
        }
        if (!WasmTypeKindIsSupported(typeKind)) {
            WasmSetDiag(
                diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, local->nameStart, local->nameEnd);
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
            WasmLocalIsSourceLocation(program, fn, local) ? HOPWasmLocalStorage_PLAIN
            : (local->typeRef < program->typeLen
               && H2MirTypeRefIsFixedArray(&program->types[local->typeRef]))
                ? HOPWasmLocalStorage_ARRAY
            : (local->typeRef < program->typeLen
               && H2MirTypeRefIsAggregate(&program->types[local->typeRef]))
                ? HOPWasmLocalStorage_AGG
                : HOPWasmLocalStorage_PLAIN;
        state->localIntKinds[i] =
            (uint8_t)((local->typeRef < program->typeLen)
                          ? H2MirTypeRefIntKind(&program->types[local->typeRef])
                          : H2MirIntKind_NONE);
        state->arrayCounts[i] =
            (local->typeRef < program->typeLen)
                ? H2MirTypeRefFixedArrayCount(&program->types[local->typeRef])
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
            if (state->localStorage[i] == HOPWasmLocalStorage_ARRAY) {
                slotSize = WasmTypeKindElementSize(typeKind) * state->arrayCounts[i];
            } else if (state->localStorage[i] == HOPWasmLocalStorage_AGG) {
                int byteSize = WasmTypeByteSize(program, local->typeRef);
                if (byteSize < 0) {
                    WasmSetDiag(
                        diag,
                        H2Diag_WASM_BACKEND_UNSUPPORTED_MIR,
                        local->nameStart,
                        local->nameEnd);
                    if (diag != NULL) {
                        diag->detail = "unsupported local layout";
                    }
                    return -1;
                }
                slotSize = byteSize == 0 ? 1u : (uint32_t)byteSize;
            } else if (typeKind == HOPWasmType_I32) {
                slotSize = WasmScalarByteWidth(state->localIntKinds[i]);
            }
            if (slotSize == 0u) {
                WasmSetDiag(
                    diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, local->nameStart, local->nameEnd);
                if (diag != NULL) {
                    diag->detail = "unsupported local layout";
                }
                return -1;
            }
            state->frameSize = state->frameOffsets[i] + slotSize;
        }
        if (state->usesFrame && typeKind == HOPWasmType_STR_PTR) {
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
            const H2MirInst* inst = &program->insts[fn->instStart + tempPc];
            int              typeRef = WasmTempTypeRefForInst(program, inst);
            if (typeRef < 0) {
                continue;
            }
            state->frameSize = WasmAlign4(state->frameSize);
            {
                int byteSize = WasmTypeByteSize(program, (uint32_t)typeRef);
                if (byteSize < 0) {
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported temporary layout";
                    }
                    return -1;
                }
                state->frameSize += byteSize == 0 ? 1u : (uint32_t)byteSize;
            }
        }
        for (tempPc = 0; tempPc < fn->instLen; tempPc++) {
            const H2MirInst* inst = &program->insts[fn->instStart + tempPc];
            if (inst->op == H2MirOp_ALLOC_NEW
                && ((inst->tok & H2AstFlag_NEW_HAS_ALLOC) != 0u
                    || WasmProgramNeedsRootAllocator(program, NULL)))
            {
                state->frameSize = WasmAlign4(state->frameSize);
                state->allocCallTempOffset = state->frameSize;
                state->frameSize += 4u;
                break;
            }
        }
        state->frameSize = WasmAlign4(state->frameSize);
        if (!WasmTypeKindFromMirType(program, fn->typeRef, &resultKind)) {
            WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, fn->nameStart, fn->nameEnd);
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
        state->scratchI64Local = (uint16_t)(state->hiddenLocalStart + 8u);
        state->wasmLocalValueCount = (uint16_t)(state->hiddenLocalStart + 9u);
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
        state->scratchI64Local = (uint16_t)(state->hiddenLocalStart + 7u);
        state->wasmLocalValueCount = (uint16_t)(nextValueIndex + 8u);
    } else {
        state->wasmLocalValueCount = nextValueIndex;
    }
    if (WasmFunctionNeedsDirectCallScratch(program, fn)) {
        if ((uint32_t)state->wasmLocalValueCount + HOPWasmDirectCallScratchI32Count
                + HOPWasmDirectCallScratchI64Count
            > UINT16_MAX)
        {
            WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, fn->nameStart, fn->nameEnd);
            if (diag != NULL) {
                diag->detail = "too many Wasm locals";
            }
            return -1;
        }
        state->directCallScratchI32Start = state->wasmLocalValueCount;
        state->directCallScratchI64Start =
            (uint16_t)(state->directCallScratchI32Start + HOPWasmDirectCallScratchI32Count);
        state->wasmLocalValueCount =
            (uint16_t)(state->directCallScratchI64Start + HOPWasmDirectCallScratchI64Count);
    }
    return 0;
}

static int WasmRequireI32Value(
    uint8_t valueTypeKind,
    H2Diag* _Nullable diag,
    uint32_t start,
    uint32_t end,
    const char* _Nonnull detail) {
    if (valueTypeKind == HOPWasmType_I32) {
        return 0;
    }
    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, start, end);
    if (diag != NULL) {
        diag->detail = detail;
    }
    return -1;
}

static int WasmRequireAllocatorValue(
    uint8_t valueTypeKind,
    H2Diag* _Nullable diag,
    uint32_t start,
    uint32_t end,
    const char* _Nonnull detail) {
    if (valueTypeKind == HOPWasmType_I32 || WasmTypeKindIsPointer(valueTypeKind)
        || valueTypeKind == HOPWasmType_AGG_REF)
    {
        return 0;
    }
    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, start, end);
    if (diag != NULL) {
        diag->detail = detail;
    }
    return -1;
}

static int WasmEmitInvalidAllocatorTrap(
    HOPWasmBuf* _Nonnull body,
    const HOPWasmImportLayout* _Nullable imports,
    const HOPWasmStringLayout* _Nullable strings) {
    if (imports != NULL && strings != NULL && imports->hasPlatformPanicImport
        && strings->hasInvalidAllocatorPanicString)
    {
        if (WasmEmitPlatformPanicCall(
                body,
                imports,
                (int32_t)strings->invalidAllocatorPanic.dataOffset,
                (int32_t)strings->invalidAllocatorPanic.len,
                0)
            != 0)
        {
            return -1;
        }
    }
    return WasmAppendByte(body, 0x00u);
}

static int WasmRequirePointerValue(
    uint8_t valueTypeKind,
    H2Diag* _Nullable diag,
    uint32_t start,
    uint32_t end,
    const char* _Nonnull detail) {
    if (WasmTypeKindIsPointer(valueTypeKind)) {
        return 0;
    }
    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, start, end);
    if (diag != NULL) {
        diag->detail = detail;
    }
    return -1;
}

static int WasmAppendMemArg(HOPWasmBuf* body, uint32_t alignLog2, uint32_t offset) {
    return WasmAppendULEB(body, alignLog2) != 0 || WasmAppendULEB(body, offset) != 0 ? -1 : 0;
}

static int WasmEmitI32Load(HOPWasmBuf* body) {
    return WasmAppendByte(body, 0x28u) != 0 || WasmAppendMemArg(body, 2u, 0u) != 0 ? -1 : 0;
}

static int WasmEmitI32Store(HOPWasmBuf* body) {
    return WasmAppendByte(body, 0x36u) != 0 || WasmAppendMemArg(body, 2u, 0u) != 0 ? -1 : 0;
}

static int WasmEmitI64Load(HOPWasmBuf* body) {
    return WasmAppendByte(body, 0x29u) != 0 || WasmAppendMemArg(body, 3u, 0u) != 0 ? -1 : 0;
}

static int WasmEmitI64Store(HOPWasmBuf* body) {
    return WasmAppendByte(body, 0x37u) != 0 || WasmAppendMemArg(body, 3u, 0u) != 0 ? -1 : 0;
}

static int WasmEmitU8Load(HOPWasmBuf* body) {
    return WasmAppendByte(body, 0x2du) != 0 || WasmAppendMemArg(body, 0u, 0u) != 0 ? -1 : 0;
}

static int WasmEmitI8Load(HOPWasmBuf* body) {
    return WasmAppendByte(body, 0x2cu) != 0 || WasmAppendMemArg(body, 0u, 0u) != 0 ? -1 : 0;
}

static int WasmEmitU16Load(HOPWasmBuf* body) {
    return WasmAppendByte(body, 0x2fu) != 0 || WasmAppendMemArg(body, 1u, 0u) != 0 ? -1 : 0;
}

static int WasmEmitI16Load(HOPWasmBuf* body) {
    return WasmAppendByte(body, 0x2eu) != 0 || WasmAppendMemArg(body, 1u, 0u) != 0 ? -1 : 0;
}

static int WasmEmitU8Store(HOPWasmBuf* body) {
    return WasmAppendByte(body, 0x3au) != 0 || WasmAppendMemArg(body, 0u, 0u) != 0 ? -1 : 0;
}

static int WasmEmitU16Store(HOPWasmBuf* body) {
    return WasmAppendByte(body, 0x3bu) != 0 || WasmAppendMemArg(body, 1u, 0u) != 0 ? -1 : 0;
}

static int WasmEmitTypedLoad(HOPWasmBuf* body, uint8_t typeKind) {
    switch (typeKind) {
        case HOPWasmType_U8_PTR:
        case HOPWasmType_ARRAY_VIEW_U8:
        case HOPWasmType_SLICE_U8:       return WasmEmitU8Load(body);
        case HOPWasmType_I8_PTR:
        case HOPWasmType_ARRAY_VIEW_I8:
        case HOPWasmType_SLICE_I8:       return WasmEmitI8Load(body);
        case HOPWasmType_U16_PTR:
        case HOPWasmType_ARRAY_VIEW_U16:
        case HOPWasmType_SLICE_U16:      return WasmEmitU16Load(body);
        case HOPWasmType_I16_PTR:
        case HOPWasmType_ARRAY_VIEW_I16:
        case HOPWasmType_SLICE_I16:      return WasmEmitI16Load(body);
        case HOPWasmType_U32_PTR:
        case HOPWasmType_I32_PTR:
        case HOPWasmType_ARRAY_VIEW_U32:
        case HOPWasmType_ARRAY_VIEW_I32:
        case HOPWasmType_SLICE_U32:
        case HOPWasmType_SLICE_I32:
        case HOPWasmType_FUNC_REF_PTR:   return WasmEmitI32Load(body);
        default:                         return -1;
    }
}

static int WasmEmitTypedStore(HOPWasmBuf* body, uint8_t typeKind) {
    switch (typeKind) {
        case HOPWasmType_U8_PTR:
        case HOPWasmType_I8_PTR:
        case HOPWasmType_ARRAY_VIEW_U8:
        case HOPWasmType_ARRAY_VIEW_I8:  return WasmEmitU8Store(body);
        case HOPWasmType_U16_PTR:
        case HOPWasmType_I16_PTR:
        case HOPWasmType_ARRAY_VIEW_U16:
        case HOPWasmType_ARRAY_VIEW_I16: return WasmEmitU16Store(body);
        case HOPWasmType_U32_PTR:
        case HOPWasmType_I32_PTR:
        case HOPWasmType_ARRAY_VIEW_U32:
        case HOPWasmType_ARRAY_VIEW_I32:
        case HOPWasmType_FUNC_REF_PTR:   return WasmEmitI32Store(body);
        default:                         return -1;
    }
}

static int WasmEmitScaleIndex(HOPWasmBuf* body, uint32_t elemSize) {
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
    HOPWasmBuf* body, const HOPWasmEmitState* state, uint32_t offset, uint32_t size) {
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
    HOPWasmBuf*             body,
    const HOPWasmEmitState* state,
    uint16_t                srcLocal,
    uint32_t                dstOffset,
    uint32_t                size) {
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
    HOPWasmBuf* body, uint16_t srcLocal, uint16_t dstLocal, uint32_t dstAddend, uint32_t size) {
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

static int WasmEmitZeroLocalAddrRange(HOPWasmBuf* body, uint16_t dstLocal, uint32_t size) {
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
    HOPWasmBuf* body, uint16_t dstLocal, uint16_t sizeLocal, uint16_t cursorLocal) {
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
    const H2MirProgram* program, uint8_t baseType, uint32_t baseTypeRef) {
    if (program == NULL || baseTypeRef == UINT32_MAX) {
        return UINT32_MAX;
    }
    if (baseType == HOPWasmType_AGG_REF) {
        return baseTypeRef;
    }
    if (baseType == HOPWasmType_OPAQUE_PTR) {
        return WasmOpaquePtrPointeeTypeRef(program, baseTypeRef);
    }
    return UINT32_MAX;
}

static int WasmSourceSliceEq(
    const H2MirProgram* program,
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
    const H2MirProgram* program, const H2MirField* fieldRef, const char* name) {
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

static bool WasmFieldIsEmbeddedAllocator(const H2MirProgram* program, const H2MirField* fieldRef) {
    return WasmFieldNameEq(program, fieldRef, "MemAllocator");
}

static uint32_t WasmAggregateFieldCount(const H2MirProgram* program, uint32_t typeRefIndex) {
    uint32_t i;
    uint32_t count = 0u;
    if (program == NULL || typeRefIndex >= program->typeLen) {
        return 0u;
    }
    for (i = 0; i < program->fieldLen; i++) {
        if (program->fields[i].ownerTypeRef == typeRefIndex) {
            count++;
        }
    }
    return count;
}

static bool WasmAggregateHasNamedField(
    const H2MirProgram* program, uint32_t typeRefIndex, const char* name) {
    uint32_t i;
    if (program == NULL || name == NULL || typeRefIndex >= program->typeLen) {
        return false;
    }
    for (i = 0; i < program->fieldLen; i++) {
        const H2MirField* field = &program->fields[i];
        if (field->ownerTypeRef == typeRefIndex && WasmFieldNameEq(program, field, name)) {
            return true;
        }
    }
    return false;
}

static bool WasmTypeRefIsSourceLocation(const H2MirProgram* program, uint32_t typeRefIndex) {
    return program != NULL && typeRefIndex < program->typeLen
        && H2MirTypeRefIsAggregate(&program->types[typeRefIndex])
        && WasmAggregateFieldCount(program, typeRefIndex) == 5u
        && WasmAggregateHasNamedField(program, typeRefIndex, "file")
        && WasmAggregateHasNamedField(program, typeRefIndex, "start_line")
        && WasmAggregateHasNamedField(program, typeRefIndex, "start_column")
        && WasmAggregateHasNamedField(program, typeRefIndex, "end_line")
        && WasmAggregateHasNamedField(program, typeRefIndex, "end_column");
}

static bool WasmTypeRefIsMemAllocator(const H2MirProgram* program, uint32_t typeRefIndex) {
    return program != NULL && typeRefIndex < program->typeLen
        && H2MirTypeRefIsAggregate(&program->types[typeRefIndex])
        && WasmAggregateFieldCount(program, typeRefIndex) == 2u
        && WasmAggregateHasNamedField(program, typeRefIndex, "handler")
        && WasmAggregateHasNamedField(program, typeRefIndex, "data");
}

static uint32_t WasmFindMemAllocatorTypeRef(const H2MirProgram* program) {
    uint32_t i;
    if (program == NULL) {
        return UINT32_MAX;
    }
    for (i = 0; i < program->typeLen; i++) {
        if (WasmTypeRefIsMemAllocator(program, i)) {
            return i;
        }
    }
    return UINT32_MAX;
}

static int WasmTypeByteAlign(const H2MirProgram* program, uint32_t typeRefIndex);

static int WasmTypeByteSize(const H2MirProgram* program, uint32_t typeRefIndex) {
    const H2MirTypeRef* typeRef;
    uint32_t            i;
    uint32_t            offset = 0u;
    uint32_t            maxAlign = 1u;
    if (program == NULL || typeRefIndex == UINT32_MAX || typeRefIndex >= program->typeLen) {
        return -1;
    }
    typeRef = &program->types[typeRefIndex];
    if (H2MirTypeRefIsAggregate(typeRef)) {
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
    if (H2MirTypeRefIsStrObj(typeRef)) {
        return 8;
    }
    if (H2MirTypeRefIsFixedArray(typeRef)) {
        return (int)(WasmIntKindByteWidth(H2MirTypeRefIntKind(typeRef))
                     * H2MirTypeRefFixedArrayCount(typeRef));
    }
    if (H2MirTypeRefIsVArrayView(typeRef)) {
        return 0;
    }
    if (H2MirTypeRefIsStrRef(typeRef) || H2MirTypeRefIsSliceView(typeRef)
        || H2MirTypeRefIsAggSliceView(typeRef))
    {
        return 8;
    }
    if (H2MirTypeRefIsStrPtr(typeRef) || H2MirTypeRefIsOpaquePtr(typeRef)
        || H2MirTypeRefIsFixedArrayView(typeRef) || H2MirTypeRefIsU8Ptr(typeRef)
        || H2MirTypeRefIsI8Ptr(typeRef) || H2MirTypeRefIsU16Ptr(typeRef)
        || H2MirTypeRefIsI16Ptr(typeRef) || H2MirTypeRefIsU32Ptr(typeRef)
        || H2MirTypeRefIsI32Ptr(typeRef) || H2MirTypeRefIsFuncRef(typeRef))
    {
        return 4;
    }
    if (H2MirTypeRefScalarKind(typeRef) == H2MirTypeScalar_I32) {
        return (int)WasmScalarByteWidth((uint8_t)H2MirTypeRefIntKind(typeRef));
    }
    if (H2MirTypeRefScalarKind(typeRef) == H2MirTypeScalar_I64) {
        return 8;
    }
    return -1;
}

static int WasmTypeByteAlign(const H2MirProgram* program, uint32_t typeRefIndex) {
    const H2MirTypeRef* typeRef;
    int                 size = WasmTypeByteSize(program, typeRefIndex);
    if (program == NULL || typeRefIndex == UINT32_MAX || typeRefIndex >= program->typeLen) {
        return -1;
    }
    typeRef = &program->types[typeRefIndex];
    if (H2MirTypeRefIsVArrayView(typeRef)) {
        uint32_t elemSize = WasmIntKindByteWidth(H2MirTypeRefIntKind(typeRef));
        return elemSize >= 4u ? 4 : (int)elemSize;
    }
    if (H2MirTypeRefScalarKind(typeRef) == H2MirTypeScalar_I64) {
        return 8;
    }
    if (size <= 0) {
        return -1;
    }
    return size >= 4 ? 4 : size;
}

static bool WasmAggregateHasDynamicLayout(const H2MirProgram* program, uint32_t ownerTypeRef) {
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
        if (H2MirTypeRefIsStrObj(&program->types[program->fields[i].typeRef])
            || H2MirTypeRefIsVArrayView(&program->types[program->fields[i].typeRef]))
        {
            return true;
        }
    }
    return false;
}

static int WasmEmitAggregateFieldAddress(
    HOPWasmBuf*             body,
    const HOPWasmEmitState* state,
    const H2MirProgram*     program,
    uint32_t                ownerTypeRef,
    uint32_t                fieldIndex,
    uint32_t                fieldOffset) {
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
        const H2MirField*   fieldRef;
        const H2MirTypeRef* fieldType;
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
        if (H2MirTypeRefIsStrObj(fieldType)) {
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
        if (H2MirTypeRefIsVArrayView(fieldType)) {
            uint32_t countFieldRef = H2MirTypeRefVArrayCountField(fieldType);
            uint32_t countFieldIndex = UINT32_MAX;
            uint32_t countFieldOffset = UINT32_MAX;
            uint32_t elemSize = WasmIntKindByteWidth(H2MirTypeRefIntKind(fieldType));
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
    const H2MirProgram* program, uint32_t expectedTypeRef, uint32_t actualTypeRef) {
    const H2MirTypeRef* expected;
    const H2MirTypeRef* actual;
    if (program == NULL || expectedTypeRef >= program->typeLen || actualTypeRef >= program->typeLen)
    {
        return false;
    }
    if (expectedTypeRef == actualTypeRef) {
        return true;
    }
    expected = &program->types[expectedTypeRef];
    actual = &program->types[actualTypeRef];
    if (!H2MirTypeRefIsAggregate(expected) || !H2MirTypeRefIsAggregate(actual)) {
        return expected->flags == actual->flags && expected->aux == actual->aux;
    }
    return WasmTypeByteSize(program, expectedTypeRef) == WasmTypeByteSize(program, actualTypeRef)
        && WasmTypeByteAlign(program, expectedTypeRef) == WasmTypeByteAlign(program, actualTypeRef);
}

static int WasmTempTypeRefForInst(const H2MirProgram* program, const H2MirInst* inst) {
    uint8_t typeKind = HOPWasmType_VOID;
    if (program == NULL || inst == NULL) {
        return -1;
    }
    if (inst->op == H2MirOp_AGG_ZERO) {
        return (inst->aux < program->typeLen && H2MirTypeRefIsAggregate(&program->types[inst->aux]))
                 ? (int)inst->aux
                 : -1;
    }
    if (inst->op == H2MirOp_CALL_FN && inst->aux < program->funcLen) {
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
    const H2MirProgram*     program,
    const H2MirFunction*    fn,
    const HOPWasmEmitState* state,
    uint32_t                pc,
    uint32_t* _Nonnull outOffset) {
    uint32_t scanPc;
    uint32_t offset;
    if (program == NULL || fn == NULL || state == NULL || outOffset == NULL || pc >= fn->instLen) {
        return -1;
    }
    offset = state->tempFrameStart;
    for (scanPc = 0; scanPc <= pc; scanPc++) {
        const H2MirInst* inst = &program->insts[fn->instStart + scanPc];
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

static int WasmFindAggregateFieldDepth(
    const H2MirProgram* program,
    uint32_t            ownerTypeRef,
    const H2MirField*   fieldRef,
    uint32_t            depth,
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
    if (program == NULL || fieldRef == NULL || depth > 16u) {
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
    offset = 0u;
    for (i = 0; i < program->fieldLen; i++) {
        int      fieldSize;
        int      fieldAlign;
        uint32_t fieldOffset;
        uint32_t nestedFieldIndex = UINT32_MAX;
        uint32_t nestedOffset = UINT32_MAX;
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
        if (program->fields[i].typeRef < program->typeLen
            && H2MirTypeRefIsAggregate(&program->types[program->fields[i].typeRef])
            && WasmFindAggregateFieldDepth(
                program,
                program->fields[i].typeRef,
                fieldRef,
                depth + 1u,
                &nestedFieldIndex,
                &nestedOffset))
        {
            if (outFieldIndex != NULL) {
                *outFieldIndex = nestedFieldIndex;
            }
            if (outOffset != NULL) {
                *outOffset = fieldOffset + nestedOffset;
            }
            return 1;
        }
        offset = fieldOffset + (uint32_t)fieldSize;
    }
    return 0;
}

static int WasmFindAggregateField(
    const H2MirProgram* program,
    uint32_t            ownerTypeRef,
    const H2MirField*   fieldRef,
    uint32_t*           outFieldIndex,
    uint32_t*           outOffset) {
    return WasmFindAggregateFieldDepth(
        program, ownerTypeRef, fieldRef, 0u, outFieldIndex, outOffset);
}

static uint8_t WasmAddressTypeFromTypeRef(const H2MirProgram* program, uint32_t typeRefIndex) {
    const H2MirTypeRef* typeRef;
    if (program == NULL || typeRefIndex == UINT32_MAX || typeRefIndex >= program->typeLen) {
        return HOPWasmType_VOID;
    }
    typeRef = &program->types[typeRefIndex];
    if (H2MirTypeRefIsStrRef(typeRef)) {
        return HOPWasmType_STR_PTR;
    }
    if (H2MirTypeRefIsAggSliceView(typeRef)) {
        return HOPWasmType_SLICE_AGG;
    }
    if (H2MirTypeRefIsStrObj(typeRef)) {
        return HOPWasmType_STR_PTR;
    }
    if (H2MirTypeRefIsFuncRef(typeRef)) {
        return HOPWasmType_FUNC_REF_PTR;
    }
    if (H2MirTypeRefIsOpaquePtr(typeRef)) {
        return HOPWasmType_OPAQUE_PTR;
    }
    if (H2MirTypeRefIsAggregate(typeRef)) {
        // `AGG_ADDR` forms an address to aggregate storage, so callers treat it like `*T`.
        return HOPWasmType_OPAQUE_PTR;
    }
    if (H2MirTypeRefIsFixedArray(typeRef) || H2MirTypeRefIsFixedArrayView(typeRef)) {
        uint8_t typeKind = HOPWasmType_VOID;
        if (WasmTypeKindFromMirType(program, typeRefIndex, &typeKind)) {
            return typeKind;
        }
        return HOPWasmType_VOID;
    }
    if (H2MirTypeRefScalarKind(typeRef) == H2MirTypeScalar_I32) {
        switch (H2MirTypeRefIntKind(typeRef)) {
            case H2MirIntKind_U8:  return HOPWasmType_U8_PTR;
            case H2MirIntKind_I8:  return HOPWasmType_I8_PTR;
            case H2MirIntKind_U16: return HOPWasmType_U16_PTR;
            case H2MirIntKind_I16: return HOPWasmType_I16_PTR;
            case H2MirIntKind_U32: return HOPWasmType_U32_PTR;
            default:               return HOPWasmType_I32_PTR;
        }
    }
    if (H2MirTypeRefIsStrPtr(typeRef)) {
        return HOPWasmType_STR_PTR;
    }
    if (H2MirTypeRefIsU8Ptr(typeRef)) {
        return HOPWasmType_U8_PTR;
    }
    if (H2MirTypeRefIsI8Ptr(typeRef)) {
        return HOPWasmType_I8_PTR;
    }
    if (H2MirTypeRefIsU16Ptr(typeRef)) {
        return HOPWasmType_U16_PTR;
    }
    if (H2MirTypeRefIsI16Ptr(typeRef)) {
        return HOPWasmType_I16_PTR;
    }
    if (H2MirTypeRefIsU32Ptr(typeRef)) {
        return HOPWasmType_U32_PTR;
    }
    if (H2MirTypeRefIsI32Ptr(typeRef)) {
        return HOPWasmType_I32_PTR;
    }
    return HOPWasmType_VOID;
}

static int WasmEmitAddrFromFrame(
    HOPWasmBuf* body, const HOPWasmEmitState* state, uint32_t offset, uint16_t addend) {
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
    HOPWasmBuf*                body,
    const HOPWasmImportLayout* imports,
    const HOPWasmEmitState*    state,
    uint8_t                    resultKind) {
    if (body == NULL || imports == NULL || state == NULL) {
        return -1;
    }
    if (WasmTypeKindSlotCount(resultKind) == 1u) {
        uint16_t resultLocal =
            resultKind == HOPWasmType_I64 ? state->scratchI64Local : state->scratch0Local;
        if (WasmAppendByte(body, 0x21u) != 0 || WasmAppendULEB(body, resultLocal) != 0) {
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
        uint16_t resultLocal =
            resultKind == HOPWasmType_I64 ? state->scratchI64Local : state->scratch0Local;
        if (WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, resultLocal) != 0) {
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
    const HOPWasmBranchTargets* targets, uint32_t targetPc, uint32_t* outDepth) {
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

static HOPWasmBranchTargets WasmNestedBranchTargets(const HOPWasmBranchTargets* targets) {
    HOPWasmBranchTargets nested = { 0 };
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
    const H2MirProgram*  program,
    const H2MirFunction* fn,
    uint32_t             startPc,
    uint32_t             endPc,
    HOPWasmLoopRegion*   out) {
    uint32_t branchPc;
    if (program == NULL || fn == NULL || out == NULL || startPc >= endPc) {
        return false;
    }
    for (branchPc = startPc; branchPc < endPc; branchPc++) {
        const H2MirInst* branchInst = &program->insts[fn->instStart + branchPc];
        uint32_t         falseTarget;
        const H2MirInst* backedgeInst;
        uint32_t         scanPc;
        uint32_t         tailStartPc = 0;
        if (branchInst->op != H2MirOp_JUMP_IF_FALSE) {
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
        if (backedgeInst->op != H2MirOp_JUMP || backedgeInst->aux != startPc) {
            continue;
        }
        for (scanPc = branchPc + 1u; scanPc + 1u < falseTarget; scanPc++) {
            const H2MirInst* inst = &program->insts[fn->instStart + scanPc];
            if (inst->op == H2MirOp_JUMP && inst->aux > scanPc && inst->aux < falseTarget
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
    const H2MirProgram*  program,
    const H2MirFunction* fn,
    uint32_t             startPc,
    uint32_t             endPc,
    HOPWasmLoopRegion*   out) {
    uint32_t backedgePc;
    if (program == NULL || fn == NULL || out == NULL || startPc + 1u >= endPc) {
        return false;
    }
    for (backedgePc = endPc; backedgePc-- > startPc + 1u;) {
        const H2MirInst* backedgeInst = &program->insts[fn->instStart + backedgePc];
        uint32_t         scanPc;
        if (backedgeInst->op != H2MirOp_JUMP || backedgeInst->aux != startPc) {
            continue;
        }
        for (scanPc = startPc; scanPc < backedgePc; scanPc++) {
            const H2MirInst* inst = &program->insts[fn->instStart + scanPc];
            switch (inst->op) {
                case H2MirOp_JUMP:
                    if (inst->aux != startPc && inst->aux != backedgePc + 1u
                        && (inst->aux <= scanPc || inst->aux > backedgePc))
                    {
                        goto next_backedge;
                    }
                    break;
                case H2MirOp_JUMP_IF_FALSE:
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
    HOPWasmBuf* body, uint16_t tok, uint32_t start, uint32_t end, H2Diag* diag) {
    uint8_t opcode = 0;
    switch ((H2TokenKind)tok) {
        case H2Tok_ADD: opcode = 0x6au; break;
        case H2Tok_SUB: opcode = 0x6bu; break;
        case H2Tok_MUL: opcode = 0x6cu; break;
        case H2Tok_AND: opcode = 0x71u; break;
        case H2Tok_EQ:  opcode = 0x46u; break;
        case H2Tok_NEQ: opcode = 0x47u; break;
        case H2Tok_LT:  opcode = 0x48u; break;
        case H2Tok_GT:  opcode = 0x4au; break;
        case H2Tok_LTE: opcode = 0x4cu; break;
        case H2Tok_GTE: opcode = 0x4eu; break;
        default:
            WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, start, end);
            if (diag != NULL) {
                diag->detail = "unsupported i32 binary op";
            }
            return -1;
    }
    return WasmAppendByte(body, opcode);
}

static int WasmEmitBinaryI64(
    HOPWasmBuf* body, uint16_t tok, uint32_t start, uint32_t end, H2Diag* diag) {
    uint8_t opcode = 0;
    switch ((H2TokenKind)tok) {
        case H2Tok_ADD: opcode = 0x7cu; break;
        case H2Tok_SUB: opcode = 0x7du; break;
        case H2Tok_MUL: opcode = 0x7eu; break;
        case H2Tok_AND: opcode = 0x83u; break;
        case H2Tok_EQ:  opcode = 0x51u; break;
        case H2Tok_NEQ: opcode = 0x52u; break;
        case H2Tok_LT:  opcode = 0x55u; break;
        case H2Tok_GT:  opcode = 0x57u; break;
        case H2Tok_LTE: opcode = 0x59u; break;
        case H2Tok_GTE: opcode = 0x5bu; break;
        default:
            WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, start, end);
            if (diag != NULL) {
                diag->detail = "unsupported i64 binary op";
            }
            return -1;
    }
    return WasmAppendByte(body, opcode);
}

static int WasmEmitUnaryI32(
    HOPWasmBuf* body, uint16_t tok, uint32_t start, uint32_t end, H2Diag* diag) {
    switch ((H2TokenKind)tok) {
        case H2Tok_ADD: return 0;
        case H2Tok_NOT: return WasmAppendByte(body, 0x45u);
        case H2Tok_SUB:
            if (WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, -1) != 0
                || WasmAppendByte(body, 0x6cu) != 0)
            {
                return -1;
            }
            return 0;
        default:
            WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, start, end);
            if (diag != NULL) {
                diag->detail = "unsupported i32 unary op";
            }
            return -1;
    }
}

static int WasmEmitUnaryI64(
    HOPWasmBuf* body, uint16_t tok, uint32_t start, uint32_t end, H2Diag* diag) {
    switch ((H2TokenKind)tok) {
        case H2Tok_ADD: return 0;
        case H2Tok_NOT: return WasmAppendByte(body, 0x50u);
        case H2Tok_SUB:
            if (WasmAppendByte(body, 0x42u) != 0 || WasmAppendSLEB64(body, -1) != 0
                || WasmAppendByte(body, 0x7eu) != 0)
            {
                return -1;
            }
            return 0;
        default:
            WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, start, end);
            if (diag != NULL) {
                diag->detail = "unsupported i64 unary op";
            }
            return -1;
    }
}

static int WasmEmitFunctionRange(
    const H2MirProgram*         program,
    const HOPWasmFnSig*         sigs,
    const HOPWasmImportLayout*  imports,
    const HOPWasmStringLayout*  strings,
    const H2MirFunction*        fn,
    HOPWasmBuf*                 body,
    HOPWasmEmitState*           state,
    const HOPWasmBranchTargets* branchTargets,
    uint8_t                     resultKind,
    uint32_t                    startPc,
    uint32_t                    endPc,
    H2Diag* _Nullable diag) {
    uint32_t pc = startPc;
    uint32_t stepCount = 0u;
    uint32_t maxSteps = ((endPc > startPc ? (endPc - startPc) : 1u) * 8u) + 32u;
    while (pc < endPc) {
        int allowLoopRecognition = !(
            branchTargets != NULL && branchTargets->hasContinue
            && branchTargets->continueTargetPc == pc);
        if (++stepCount > maxSteps) {
            WasmSetDiag(diag, H2Diag_WASM_BACKEND_INTERNAL, fn->nameStart, fn->nameEnd);
            if (diag != NULL) {
                diag->detail = "Wasm emitter range exceeded progress limit";
            }
            return -1;
        }
        HOPWasmLoopRegion loop;
        if (allowLoopRecognition && WasmFindLoopRegion(program, fn, pc, endPc, &loop)) {
            HOPWasmBranchTargets condTargets = { 0 };
            HOPWasmBranchTargets bodyTargets = { 0 };
            HOPWasmBranchTargets tailTargets = { 0 };
            const H2MirInst*     branchInst = &program->insts[fn->instStart + loop.condBranchPc];
            uint8_t              condType = 0;
            uint32_t             stackDepthBefore = state->stackLen;
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
            if (WasmStackPop(state, &condType) != 0 || condType != HOPWasmType_I32) {
                WasmSetDiag(
                    diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, branchInst->start, branchInst->end);
                if (diag != NULL) {
                    diag->detail = "unsupported branch condition type";
                }
                return -1;
            }
            if (state->stackLen != stackDepthBefore) {
                WasmSetDiag(
                    diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, branchInst->start, branchInst->end);
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
                    diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, branchInst->start, branchInst->end);
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
                    diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, branchInst->start, branchInst->end);
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
            HOPWasmBranchTargets loopTargets = { 0 };
            uint32_t             stackDepthBefore = state->stackLen;
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
                WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, fn->nameStart, fn->nameEnd);
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
        const H2MirInst* inst = &program->insts[fn->instStart + pc];
        switch (inst->op) {
            case H2MirOp_PUSH_CONST:
                if (inst->aux >= program->constLen) {
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_INTERNAL, inst->start, inst->end);
                    return -1;
                }
                switch (program->consts[inst->aux].kind) {
                    case H2MirConst_INT:
                        if (WasmAppendByte(body, 0x41u) != 0
                            || WasmAppendSLEB32(body, (int32_t)program->consts[inst->aux].bits) != 0
                            || WasmStackPushEx(state, HOPWasmType_I32, UINT32_MAX) != 0)
                        {
                            return -1;
                        }
                        break;
                    case H2MirConst_BOOL:
                        if (WasmAppendByte(body, 0x41u) != 0
                            || WasmAppendSLEB32(body, program->consts[inst->aux].bits != 0 ? 1 : 0)
                                   != 0
                            || WasmStackPushEx(state, HOPWasmType_I32, UINT32_MAX) != 0)
                        {
                            return -1;
                        }
                        break;
                    case H2MirConst_NULL:
                        if (WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0
                            || WasmStackPushEx(state, HOPWasmType_I32, UINT32_MAX) != 0)
                        {
                            return -1;
                        }
                        break;
                    case H2MirConst_STRING:
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
                            || WasmStackPushEx(state, HOPWasmType_STR_REF, UINT32_MAX) != 0)
                        {
                            WasmSetDiag(
                                diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                            if (diag != NULL) {
                                diag->detail = "unsupported string constant use";
                            }
                            return -1;
                        }
                        break;
                    case H2MirConst_FUNCTION:
                        if (imports == NULL || !imports->hasFunctionTable
                            || program->consts[inst->aux].aux >= program->funcLen
                            || WasmAppendByte(body, 0x41u) != 0
                            || WasmAppendSLEB32(body, (int32_t)program->consts[inst->aux].aux) != 0
                            || WasmStackPushEx(
                                   state,
                                   HOPWasmType_FUNC_REF,
                                   WasmFindFunctionValueTypeRef(
                                       program, program->consts[inst->aux].aux))
                                   != 0)
                        {
                            WasmSetDiag(
                                diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                            if (diag != NULL) {
                                diag->detail = "unsupported function constant use";
                            }
                            return -1;
                        }
                        break;
                    default:
                        WasmSetDiag(
                            diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                        if (diag != NULL) {
                            diag->detail = "unsupported constant kind";
                        }
                        return -1;
                }
                break;
            case H2MirOp_UNARY: {
                uint8_t  operandType = 0;
                uint32_t operandTypeRef = UINT32_MAX;
                if (WasmStackPopEx(state, &operandType, &operandTypeRef) != 0) {
                    return -1;
                }
                if (operandType == HOPWasmType_I64) {
                    uint8_t resultType = inst->tok == H2Tok_NOT ? HOPWasmType_I32 : HOPWasmType_I64;
                    if (WasmEmitUnaryI64(body, inst->tok, inst->start, inst->end, diag) != 0
                        || WasmStackPushEx(state, resultType, UINT32_MAX) != 0)
                    {
                        return -1;
                    }
                    break;
                }
                if (WasmRequireI32Value(
                        operandType, diag, inst->start, inst->end, "unsupported unary operand")
                        != 0
                    || WasmEmitUnaryI32(body, inst->tok, inst->start, inst->end, diag) != 0
                    || WasmStackPushEx(state, HOPWasmType_I32, UINT32_MAX) != 0)
                {
                    return -1;
                }
                break;
            }
            case H2MirOp_BINARY: {
                uint8_t  rhsType = 0;
                uint8_t  lhsType = 0;
                uint32_t rhsTypeRef = UINT32_MAX;
                uint32_t lhsTypeRef = UINT32_MAX;
                if (WasmStackPopEx(state, &rhsType, &rhsTypeRef) != 0
                    || WasmStackPopEx(state, &lhsType, &lhsTypeRef) != 0)
                {
                    return -1;
                }
                if ((inst->tok == H2Tok_EQ || inst->tok == H2Tok_NEQ) && lhsType == rhsType
                    && WasmTypeKindIsRawSingleSlot(lhsType))
                {
                    uint8_t resultType = HOPWasmType_I32;
                    int rc = lhsType == HOPWasmType_I64
                               ? WasmEmitBinaryI64(body, inst->tok, inst->start, inst->end, diag)
                               : WasmEmitBinaryI32(body, inst->tok, inst->start, inst->end, diag);
                    if (rc != 0 || WasmStackPushEx(state, resultType, UINT32_MAX) != 0) {
                        return -1;
                    }
                    break;
                }
                if (lhsType == rhsType && lhsType == HOPWasmType_I64) {
                    uint8_t resultType =
                        (inst->tok == H2Tok_EQ || inst->tok == H2Tok_NEQ || inst->tok == H2Tok_LT
                         || inst->tok == H2Tok_GT || inst->tok == H2Tok_LTE
                         || inst->tok == H2Tok_GTE)
                            ? HOPWasmType_I32
                            : HOPWasmType_I64;
                    if (WasmEmitBinaryI64(body, inst->tok, inst->start, inst->end, diag) != 0
                        || WasmStackPushEx(state, resultType, UINT32_MAX) != 0)
                    {
                        return -1;
                    }
                    break;
                }
                if ((inst->tok == H2Tok_EQ || inst->tok == H2Tok_NEQ)
                    && ((WasmTypeKindIsRawSingleSlot(lhsType) && rhsType == HOPWasmType_I32)
                        || (lhsType == HOPWasmType_I32 && WasmTypeKindIsRawSingleSlot(rhsType))))
                {
                    if (lhsType == HOPWasmType_I64 && rhsType == HOPWasmType_I32) {
                        if (WasmAdaptCallArgValue(
                                body, program, HOPWasmType_I64, rhsType, rhsTypeRef)
                                != 0
                            || WasmEmitBinaryI64(body, inst->tok, inst->start, inst->end, diag) != 0
                            || WasmStackPushEx(state, HOPWasmType_I32, UINT32_MAX) != 0)
                        {
                            return -1;
                        }
                        break;
                    }
                    if (lhsType == HOPWasmType_I32 && rhsType == HOPWasmType_I64) {
                        if (state->scratchI64Local >= state->wasmLocalValueCount) {
                            WasmSetDiag(
                                diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                            if (diag != NULL) {
                                diag->detail = "unsupported mixed i32/i64 comparison";
                            }
                            return -1;
                        }
                        if (WasmAppendByte(body, 0x21u) != 0
                            || WasmAppendULEB(body, state->scratchI64Local) != 0
                            || WasmAdaptCallArgValue(
                                   body, program, HOPWasmType_I64, lhsType, lhsTypeRef)
                                   != 0
                            || WasmAppendByte(body, 0x20u) != 0
                            || WasmAppendULEB(body, state->scratchI64Local) != 0
                            || WasmEmitBinaryI64(body, inst->tok, inst->start, inst->end, diag) != 0
                            || WasmStackPushEx(state, HOPWasmType_I32, UINT32_MAX) != 0)
                        {
                            return -1;
                        }
                        break;
                    }
                    if (WasmEmitBinaryI32(body, inst->tok, inst->start, inst->end, diag) != 0
                        || WasmStackPushEx(state, HOPWasmType_I32, UINT32_MAX) != 0)
                    {
                        return -1;
                    }
                    break;
                }
                if ((inst->tok == H2Tok_EQ || inst->tok == H2Tok_NEQ)
                    && WasmTypeKindIsSlice(lhsType) && rhsType == HOPWasmType_I32)
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
                        || WasmStackPushEx(state, HOPWasmType_I32, UINT32_MAX) != 0)
                    {
                        return -1;
                    }
                    break;
                }
                if ((inst->tok == H2Tok_EQ || inst->tok == H2Tok_NEQ) && lhsType == HOPWasmType_I32
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
                        || WasmStackPushEx(state, HOPWasmType_I32, UINT32_MAX) != 0)
                    {
                        return -1;
                    }
                    break;
                }
                if ((inst->tok == H2Tok_EQ || inst->tok == H2Tok_NEQ)
                    && lhsType == HOPWasmType_STR_REF && rhsType == HOPWasmType_STR_REF)
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
                    if (inst->tok == H2Tok_NEQ && WasmAppendByte(body, 0x45u) != 0) {
                        return -1;
                    }
                    if (WasmStackPushEx(state, HOPWasmType_I32, UINT32_MAX) != 0) {
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
                    || WasmStackPushEx(state, HOPWasmType_I32, UINT32_MAX) != 0)
                {
                    return -1;
                }
                break;
            }
            case H2MirOp_CAST:
            case H2MirOp_COERCE: {
                uint8_t  valueType = 0;
                uint8_t  targetType = 0;
                uint32_t valueTypeRef = UINT32_MAX;
                if (WasmStackPopEx(state, &valueType, &valueTypeRef) != 0
                    || !WasmTypeKindFromMirType(program, inst->aux, &targetType))
                {
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported cast/coerce";
                    }
                    return -1;
                }
                if ((valueType == HOPWasmType_I32 && targetType == HOPWasmType_I32)
                    || (valueType == HOPWasmType_I64 && targetType == HOPWasmType_I64)
                    || (valueType == HOPWasmType_I32
                        && (WasmTypeKindIsPointer(targetType)
                            || WasmTypeKindIsArrayView(targetType)))
                    || ((WasmTypeKindIsPointer(valueType) || WasmTypeKindIsArrayView(valueType))
                        && (WasmTypeKindIsPointer(targetType)
                            || WasmTypeKindIsArrayView(targetType)))
                    || ((WasmTypeKindIsPointer(valueType) || WasmTypeKindIsArrayView(valueType))
                        && targetType == HOPWasmType_I32)
                    || (valueType == HOPWasmType_STR_REF && targetType == HOPWasmType_STR_PTR)
                    || (valueType == HOPWasmType_AGG_REF && targetType == HOPWasmType_AGG_REF))
                {
                    if (WasmStackPushEx(state, targetType, inst->aux) != 0) {
                        return -1;
                    }
                    break;
                }
                if ((valueType == HOPWasmType_I32 || WasmTypeKindIsPointer(valueType)
                     || WasmTypeKindIsArrayView(valueType))
                    && targetType == HOPWasmType_I64)
                {
                    if (WasmAppendByte(
                            body,
                            WasmI32LikeValueUsesUnsignedExtend(program, valueType, valueTypeRef)
                                ? 0xadu
                                : 0xacu)
                            != 0
                        || WasmStackPushEx(state, targetType, inst->aux) != 0)
                    {
                        return -1;
                    }
                    break;
                }
                if (valueType == HOPWasmType_I64 && targetType == HOPWasmType_I32) {
                    if (WasmAppendByte(body, 0xa7u) != 0
                        || WasmStackPushEx(state, targetType, inst->aux) != 0)
                    {
                        return -1;
                    }
                    break;
                }
                WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                if (diag != NULL) {
                    diag->detail = "unsupported cast/coerce";
                }
                return -1;
            }
            case H2MirOp_AGG_ZERO: {
                uint32_t tempOffset = UINT32_MAX;
                if (!state->usesFrame || inst->aux >= program->typeLen
                    || !H2MirTypeRefIsAggregate(&program->types[inst->aux])
                    || WasmTempOffsetForPc(program, fn, state, pc, &tempOffset) != 0
                    || WasmEmitZeroFrameRange(
                           body, state, tempOffset, (uint32_t)WasmTypeByteSize(program, inst->aux))
                           != 0
                    || WasmEmitAddrFromFrame(body, state, tempOffset, 0u) != 0
                    || WasmStackPushEx(state, HOPWasmType_AGG_REF, inst->aux) != 0)
                {
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL && diag->detail == NULL) {
                        diag->detail = "unsupported aggregate zero-init";
                    }
                    return -1;
                }
                break;
            }
            case H2MirOp_LOCAL_ZERO:
                if (inst->aux >= fn->localCount) {
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported LOCAL_ZERO type";
                    }
                    return -1;
                }
                if (state->usesFrame) {
                    if (state->localStorage[inst->aux] == HOPWasmLocalStorage_ARRAY) {
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
                    } else if (state->localStorage[inst->aux] == HOPWasmLocalStorage_AGG) {
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
                            || (state->localKinds[inst->aux] == HOPWasmType_I64
                                    ? (WasmAppendByte(body, 0x42u) != 0
                                       || WasmAppendSLEB64(body, 0) != 0)
                                    : (WasmAppendByte(body, 0x41u) != 0
                                       || WasmAppendSLEB32(body, 0) != 0))
                            || (state->localKinds[inst->aux] == HOPWasmType_I32
                                    ? WasmEmitTypedStore(
                                          body, WasmLocalAddressTypeKind(state, inst->aux))
                                    : (state->localKinds[inst->aux] == HOPWasmType_I64
                                           ? WasmEmitI64Store(body)
                                           : WasmEmitI32Store(body)))
                                   != 0)
                        {
                            return -1;
                        }
                    } else if (
                        state->localKinds[inst->aux] == HOPWasmType_STR_REF
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
                            diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                        if (diag != NULL) {
                            diag->detail = "unsupported LOCAL_ZERO type";
                        }
                        return -1;
                    }
                    break;
                }
                if (WasmTypeKindIsRawSingleSlot(state->localKinds[inst->aux])) {
                    if ((state->localKinds[inst->aux] == HOPWasmType_I64
                             ? (WasmAppendByte(body, 0x42u) != 0 || WasmAppendSLEB64(body, 0) != 0)
                             : (WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0))
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->wasmValueIndex[inst->aux]) != 0)
                    {
                        return -1;
                    }
                } else if (
                    state->localKinds[inst->aux] == HOPWasmType_STR_REF
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
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported LOCAL_ZERO type";
                    }
                    return -1;
                }
                break;
            case H2MirOp_LOCAL_LOAD:
                if (inst->aux >= fn->localCount) {
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_INTERNAL, inst->start, inst->end);
                    return -1;
                }
                if (state->usesFrame) {
                    if (state->localStorage[inst->aux] == HOPWasmLocalStorage_ARRAY) {
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
                    } else if (state->localStorage[inst->aux] == HOPWasmLocalStorage_AGG) {
                        if (WasmEmitAddrFromFrame(body, state, state->frameOffsets[inst->aux], 0u)
                                != 0
                            || WasmStackPushEx(
                                   state, HOPWasmType_AGG_REF, state->localTypeRefs[inst->aux])
                                   != 0)
                        {
                            return -1;
                        }
                    } else if (WasmTypeKindIsRawSingleSlot(state->localKinds[inst->aux])) {
                        if (WasmEmitAddrFromFrame(body, state, state->frameOffsets[inst->aux], 0u)
                                != 0
                            || (state->localKinds[inst->aux] == HOPWasmType_I32
                                    ? WasmEmitTypedLoad(
                                          body, WasmLocalAddressTypeKind(state, inst->aux))
                                    : (state->localKinds[inst->aux] == HOPWasmType_I64
                                           ? WasmEmitI64Load(body)
                                           : WasmEmitI32Load(body)))
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
                        state->localKinds[inst->aux] == HOPWasmType_STR_REF
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
                            diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
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
                    state->localKinds[inst->aux] == HOPWasmType_STR_REF
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
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported local load type";
                    }
                    return -1;
                }
                break;
            case H2MirOp_LOCAL_STORE: {
                uint8_t  valueType = 0;
                uint32_t valueTypeRef = UINT32_MAX;
                if (inst->aux >= fn->localCount
                    || WasmStackPopEx(state, &valueType, &valueTypeRef) != 0
                    || !WasmValueKindCompatibleWithLocal(
                        program,
                        state->localKinds[inst->aux],
                        valueType,
                        state->localTypeRefs[inst->aux],
                        valueTypeRef))
                {
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "local store type mismatch";
                    }
                    return -1;
                }
                if (state->usesFrame) {
                    if (state->localStorage[inst->aux] == HOPWasmLocalStorage_ARRAY) {
                        uint32_t copySize;
                        if (!WasmTypeKindIsArrayView(valueType) || valueTypeRef == UINT32_MAX
                            || (state->localTypeRefs[inst->aux] != UINT32_MAX
                                && !WasmTypeRefsCompatible(
                                    program, state->localTypeRefs[inst->aux], valueTypeRef)))
                        {
                            WasmSetDiag(
                                diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
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
                                diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                            if (diag != NULL && diag->detail == NULL) {
                                diag->detail = "array local store is not supported";
                            }
                            return -1;
                        }
                        break;
                    }
                    if (state->localStorage[inst->aux] == HOPWasmLocalStorage_AGG) {
                        uint32_t copySize;
                        if (valueType != HOPWasmType_AGG_REF || valueTypeRef == UINT32_MAX) {
                            WasmSetDiag(
                                diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
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
                                diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
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
                        if (state->localKinds[inst->aux] == HOPWasmType_STR_PTR
                            && valueType == HOPWasmType_STR_REF)
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
                            || WasmAppendULEB(
                                   body,
                                   state->localKinds[inst->aux] == HOPWasmType_I64
                                       ? state->scratchI64Local
                                       : state->scratch0Local)
                                   != 0
                            || WasmEmitAddrFromFrame(
                                   body, state, state->frameOffsets[inst->aux], 0u)
                                   != 0
                            || WasmAppendByte(body, 0x20u) != 0
                            || WasmAppendULEB(
                                   body,
                                   state->localKinds[inst->aux] == HOPWasmType_I64
                                       ? state->scratchI64Local
                                       : state->scratch0Local)
                                   != 0
                            || (state->localKinds[inst->aux] == HOPWasmType_I32
                                    ? WasmEmitTypedStore(
                                          body, WasmLocalAddressTypeKind(state, inst->aux))
                                    : (state->localKinds[inst->aux] == HOPWasmType_I64
                                           ? WasmEmitI64Store(body)
                                           : WasmEmitI32Store(body)))
                                   != 0)
                        {
                            return -1;
                        }
                    } else if (
                        state->localKinds[inst->aux] == HOPWasmType_STR_REF
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
                            diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
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
                } else if (valueType == HOPWasmType_STR_REF || WasmTypeKindIsSlice(valueType)) {
                    if (WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->wasmValueIndex[inst->aux] + 1u) != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->wasmValueIndex[inst->aux]) != 0)
                    {
                        return -1;
                    }
                } else {
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported local store type";
                    }
                    return -1;
                }
                break;
            }
            case H2MirOp_LOCAL_ADDR:
                if (!state->usesFrame || inst->aux >= fn->localCount) {
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported local address";
                    }
                    return -1;
                }
                if (state->localKinds[inst->aux] == HOPWasmType_I32) {
                    uint8_t addrType = WasmLocalAddressTypeKind(state, inst->aux);
                    if (addrType == HOPWasmType_VOID) {
                        WasmSetDiag(
                            diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
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
                } else if (state->localKinds[inst->aux] == HOPWasmType_STR_REF) {
                    if (WasmEmitAddrFromFrame(body, state, state->frameOffsets[inst->aux], 0u) != 0
                        || WasmStackPushEx(state, HOPWasmType_STR_PTR, UINT32_MAX) != 0)
                    {
                        return -1;
                    }
                } else if (state->localStorage[inst->aux] == HOPWasmLocalStorage_ARRAY) {
                    if (WasmEmitAddrFromFrame(body, state, state->frameOffsets[inst->aux], 0u) != 0
                        || WasmStackPushEx(
                               state, state->localKinds[inst->aux], state->localTypeRefs[inst->aux])
                               != 0)
                    {
                        return -1;
                    }
                } else if (state->localStorage[inst->aux] == HOPWasmLocalStorage_AGG) {
                    if (WasmEmitAddrFromFrame(body, state, state->frameOffsets[inst->aux], 0u) != 0
                        || WasmStackPushEx(state, HOPWasmType_I32, UINT32_MAX) != 0)
                    {
                        return -1;
                    }
                } else {
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported local address type";
                    }
                    return -1;
                }
                break;
            case H2MirOp_ARRAY_ADDR: {
                uint8_t  indexType = 0;
                uint8_t  baseType = 0;
                uint32_t baseTypeRef = UINT32_MAX;
                uint8_t  ptrType = HOPWasmType_VOID;
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
                    case HOPWasmType_ARRAY_VIEW_U8:  ptrType = HOPWasmType_U8_PTR; break;
                    case HOPWasmType_ARRAY_VIEW_I8:  ptrType = HOPWasmType_I8_PTR; break;
                    case HOPWasmType_ARRAY_VIEW_U16: ptrType = HOPWasmType_U16_PTR; break;
                    case HOPWasmType_ARRAY_VIEW_I16: ptrType = HOPWasmType_I16_PTR; break;
                    case HOPWasmType_ARRAY_VIEW_U32: ptrType = HOPWasmType_U32_PTR; break;
                    case HOPWasmType_ARRAY_VIEW_I32: ptrType = HOPWasmType_I32_PTR; break;
                    default:                         break;
                }
                if (ptrType == HOPWasmType_VOID) {
                    switch (baseType) {
                        case HOPWasmType_SLICE_U8:  ptrType = HOPWasmType_U8_PTR; break;
                        case HOPWasmType_SLICE_I8:  ptrType = HOPWasmType_I8_PTR; break;
                        case HOPWasmType_SLICE_U16: ptrType = HOPWasmType_U16_PTR; break;
                        case HOPWasmType_SLICE_I16: ptrType = HOPWasmType_I16_PTR; break;
                        case HOPWasmType_SLICE_U32: ptrType = HOPWasmType_U32_PTR; break;
                        case HOPWasmType_SLICE_I32: ptrType = HOPWasmType_I32_PTR; break;
                        case HOPWasmType_SLICE_AGG: ptrType = HOPWasmType_OPAQUE_PTR; break;
                        default:                    break;
                    }
                }
                if (ptrType == HOPWasmType_VOID) {
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported array address base";
                    }
                    return -1;
                }
                elemSize = WasmTypeElementSize(program, baseType, baseTypeRef);
                if (elemSize == 0u) {
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
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
                               ptrType == HOPWasmType_OPAQUE_PTR
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
            case H2MirOp_AGG_SET: {
                uint8_t           valueType = 0;
                uint8_t           baseType = 0;
                uint32_t          valueTypeRef = UINT32_MAX;
                uint32_t          baseTypeRef = UINT32_MAX;
                uint32_t          ownerTypeRef = UINT32_MAX;
                uint32_t          fieldIndex = UINT32_MAX;
                uint32_t          fieldOffset = UINT32_MAX;
                uint32_t          fieldTypeRef = UINT32_MAX;
                uint8_t           fieldType = HOPWasmType_VOID;
                uint8_t           fieldAddrType = HOPWasmType_VOID;
                const H2MirField* fieldRef;
                if (inst->aux >= program->fieldLen
                    || WasmStackPopEx(state, &valueType, &valueTypeRef) != 0
                    || WasmStackPopEx(state, &baseType, &baseTypeRef) != 0)
                {
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported aggregate set base";
                    }
                    return -1;
                }
                fieldRef = &program->fields[inst->aux];
                if (baseType == HOPWasmType_STR_PTR && WasmFieldNameEq(program, fieldRef, "len")) {
                    if (valueType != HOPWasmType_I32 || WasmAppendByte(body, 0x21u) != 0
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
                            diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                        if (diag != NULL && diag->detail == NULL) {
                            diag->detail = "unsupported aggregate field set";
                        }
                        return -1;
                    }
                    break;
                }
                ownerTypeRef = WasmAggregateOwnerTypeRef(program, baseType, baseTypeRef);
                if (ownerTypeRef == UINT32_MAX) {
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported aggregate set base";
                    }
                    return -1;
                }
                if (!WasmFindAggregateField(
                        program, ownerTypeRef, fieldRef, &fieldIndex, &fieldOffset))
                {
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported aggregate field set";
                    }
                    return -1;
                }
                fieldTypeRef = program->fields[fieldIndex].typeRef;
                if (!WasmTypeKindFromMirType(program, fieldTypeRef, &fieldType)) {
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported aggregate field set";
                    }
                    return -1;
                }
                fieldAddrType = WasmAddressTypeFromTypeRef(program, fieldTypeRef);
                if (fieldType == HOPWasmType_STR_PTR
                    && H2MirTypeRefIsStrObj(&program->types[fieldTypeRef]))
                {
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported aggregate field set";
                    }
                    return -1;
                }
                if (WasmTypeKindIsRawSingleSlot(fieldType)) {
                    uint16_t valueLocal =
                        fieldType == HOPWasmType_I64 ? state->scratchI64Local
                        : state->usesFrame && WasmAggregateHasDynamicLayout(program, ownerTypeRef)
                            ? state->scratch4Local
                            : state->scratch1Local;
                    if ((valueType != fieldType
                         && WasmAdaptCallArgValue(body, program, fieldType, valueType, valueTypeRef)
                                != 0)
                        || WasmAppendByte(body, 0x21u) != 0 || WasmAppendULEB(body, valueLocal) != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmEmitAggregateFieldAddress(
                               body, state, program, ownerTypeRef, fieldIndex, fieldOffset)
                               != 0
                        || WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, valueLocal) != 0
                        || (fieldType == HOPWasmType_I64 ? WasmEmitI64Store(body)
                            : fieldType == HOPWasmType_I32
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
                if (fieldType == HOPWasmType_STR_REF || WasmTypeKindIsSlice(fieldType)) {
                    bool isVArrayView = H2MirTypeRefIsVArrayView(&program->types[fieldTypeRef]);
                    if (isVArrayView) {
                        WasmSetDiag(
                            diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
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
                if (fieldType == HOPWasmType_AGG_REF) {
                    uint32_t copySize = (uint32_t)WasmTypeByteSize(program, fieldTypeRef);
                    if (valueType != HOPWasmType_AGG_REF || valueTypeRef == UINT32_MAX
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
                            diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                        if (diag != NULL && diag->detail == NULL) {
                            diag->detail = "unsupported aggregate field set";
                        }
                        return -1;
                    }
                    break;
                }
                WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                if (diag != NULL) {
                    diag->detail = "unsupported aggregate field set";
                }
                return -1;
            }
            case H2MirOp_AGG_ADDR: {
                uint8_t           baseType = 0;
                uint32_t          baseTypeRef = UINT32_MAX;
                uint32_t          ownerTypeRef = UINT32_MAX;
                uint32_t          fieldIndex = UINT32_MAX;
                uint32_t          fieldOffset = UINT32_MAX;
                uint8_t           addrType = HOPWasmType_VOID;
                const H2MirField* fieldRef;
                if (inst->aux >= program->fieldLen
                    || WasmStackPopEx(state, &baseType, &baseTypeRef) != 0)
                {
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported aggregate address base";
                    }
                    return -1;
                }
                fieldRef = &program->fields[inst->aux];
                if (WasmFieldNameEq(program, fieldRef, "impl")
                    || WasmFieldNameEq(program, fieldRef, "handler"))
                {
                    if (baseType != HOPWasmType_I32 && !WasmTypeKindIsPointer(baseType)
                        && baseType != HOPWasmType_AGG_REF)
                    {
                        WasmSetDiag(
                            diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                        if (diag != NULL) {
                            diag->detail = "unsupported aggregate field address type";
                        }
                        return -1;
                    }
                    if (WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0
                        || WasmAppendByte(body, 0x6au) != 0
                        || WasmStackPushEx(state, HOPWasmType_FUNC_REF_PTR, UINT32_MAX) != 0)
                    {
                        return -1;
                    }
                    break;
                }
                if ((ownerTypeRef = WasmAggregateOwnerTypeRef(program, baseType, baseTypeRef))
                    == UINT32_MAX)
                {
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported aggregate address base";
                    }
                    return -1;
                }
                if (!WasmFindAggregateField(
                        program, ownerTypeRef, fieldRef, &fieldIndex, &fieldOffset)
                    || fieldIndex >= program->fieldLen)
                {
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unknown aggregate field";
                    }
                    return -1;
                }
                addrType = WasmAddressTypeFromTypeRef(program, program->fields[fieldIndex].typeRef);
                if (addrType == HOPWasmType_VOID) {
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
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
            case H2MirOp_AGG_GET: {
                uint8_t           baseType = 0;
                uint32_t          baseTypeRef = UINT32_MAX;
                uint32_t          ownerTypeRef = UINT32_MAX;
                uint32_t          fieldIndex = UINT32_MAX;
                uint32_t          fieldOffset = UINT32_MAX;
                uint8_t           fieldType = HOPWasmType_VOID;
                const H2MirField* fieldRef;
                if (inst->aux >= program->fieldLen
                    || WasmStackPopEx(state, &baseType, &baseTypeRef) != 0)
                {
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported aggregate get base";
                    }
                    return -1;
                }
                fieldRef = &program->fields[inst->aux];
                if (WasmFieldNameEq(program, fieldRef, "impl")
                    || WasmFieldNameEq(program, fieldRef, "handler"))
                {
                    if (WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmEmitI32Load(body) != 0
                        || WasmStackPushEx(state, HOPWasmType_FUNC_REF, UINT32_MAX) != 0)
                    {
                        return -1;
                    }
                    break;
                }
                if (baseType == HOPWasmType_STR_PTR && WasmFieldNameEq(program, fieldRef, "len")) {
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
                        || WasmStackPushEx(state, HOPWasmType_I32, UINT32_MAX) != 0)
                    {
                        return -1;
                    }
                    break;
                }
                if (baseType == HOPWasmType_STR_REF && WasmFieldNameEq(program, fieldRef, "len")) {
                    if (WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x1au) != 0 || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmStackPushEx(state, HOPWasmType_I32, UINT32_MAX) != 0)
                    {
                        return -1;
                    }
                    break;
                }
                if ((ownerTypeRef = WasmAggregateOwnerTypeRef(program, baseType, baseTypeRef))
                    == UINT32_MAX)
                {
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
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
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
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
                if (fieldType == HOPWasmType_STR_PTR
                    && H2MirTypeRefIsStrObj(&program->types[program->fields[fieldIndex].typeRef]))
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
                if (fieldType == HOPWasmType_STR_REF || WasmTypeKindIsSlice(fieldType)) {
                    bool isVArrayView = H2MirTypeRefIsVArrayView(
                        &program->types[program->fields[fieldIndex].typeRef]);
                    uint32_t countFieldRef = H2MirTypeRefVArrayCountField(
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
                            diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
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
                if (fieldType == HOPWasmType_AGG_REF) {
                    if (WasmEmitAggregateFieldAddress(
                            body, state, program, ownerTypeRef, fieldIndex, fieldOffset)
                            != 0
                        || WasmStackPushEx(
                               state, HOPWasmType_AGG_REF, program->fields[fieldIndex].typeRef)
                               != 0)
                    {
                        return -1;
                    }
                    break;
                }
                if (WasmEmitAggregateFieldAddress(
                        body, state, program, ownerTypeRef, fieldIndex, fieldOffset)
                        != 0
                    || ((fieldType == HOPWasmType_I32)
                            ? WasmEmitTypedLoad(
                                  body,
                                  WasmAddressTypeFromTypeRef(
                                      program, program->fields[fieldIndex].typeRef))
                        : (fieldType == HOPWasmType_I64)
                            ? WasmEmitI64Load(body)
                            : WasmEmitI32Load(body))
                           != 0
                    || WasmStackPushEx(state, fieldType, program->fields[fieldIndex].typeRef) != 0)
                {
                    return -1;
                }
                break;
            }
            case H2MirOp_DEREF_LOAD: {
                uint8_t refType = 0;
                if (WasmStackPop(state, &refType) != 0) {
                    return -1;
                }
                if (refType == HOPWasmType_I32_PTR || refType == HOPWasmType_U8_PTR
                    || refType == HOPWasmType_I8_PTR || refType == HOPWasmType_U16_PTR
                    || refType == HOPWasmType_I16_PTR || refType == HOPWasmType_U32_PTR)
                {
                    if (WasmEmitTypedLoad(body, refType) != 0
                        || WasmStackPush(state, HOPWasmType_I32) != 0)
                    {
                        return -1;
                    }
                } else if (refType == HOPWasmType_FUNC_REF_PTR) {
                    if (WasmEmitTypedLoad(body, refType) != 0
                        || WasmStackPush(state, HOPWasmType_FUNC_REF) != 0)
                    {
                        return -1;
                    }
                } else if (refType == HOPWasmType_STR_PTR) {
                    if (WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmEmitI32Load(body) != 0 || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 4) != 0
                        || WasmAppendByte(body, 0x6au) != 0 || WasmEmitI32Load(body) != 0
                        || WasmStackPush(state, HOPWasmType_STR_REF) != 0)
                    {
                        return -1;
                    }
                } else {
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported dereference load";
                    }
                    return -1;
                }
                break;
            }
            case H2MirOp_DEREF_STORE: {
                uint8_t  refType = 0;
                uint8_t  valueType = 0;
                uint32_t refTypeRef = UINT32_MAX;
                uint32_t valueTypeRef = UINT32_MAX;
                if (WasmStackPopEx(state, &refType, &refTypeRef) != 0
                    || WasmStackPopEx(state, &valueType, &valueTypeRef) != 0)
                {
                    return -1;
                }
                if ((refType == HOPWasmType_I32_PTR || refType == HOPWasmType_U8_PTR
                     || refType == HOPWasmType_I8_PTR || refType == HOPWasmType_U16_PTR
                     || refType == HOPWasmType_I16_PTR || refType == HOPWasmType_U32_PTR)
                    && (valueType == HOPWasmType_I32 || WasmTypeKindIsPointer(valueType)))
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
                } else if (refType == HOPWasmType_FUNC_REF_PTR && valueType == HOPWasmType_FUNC_REF)
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
                } else if (
                    refType == HOPWasmType_OPAQUE_PTR && WasmTypeKindIsRawSingleSlot(valueType))
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
                } else if (
                    (refType == HOPWasmType_OPAQUE_PTR || refType == HOPWasmType_AGG_REF)
                    && valueType == HOPWasmType_AGG_REF && valueTypeRef < program->typeLen
                    && (refType != HOPWasmType_AGG_REF
                        || (refTypeRef < program->typeLen
                            && WasmTypeRefsCompatible(program, refTypeRef, valueTypeRef))))
                {
                    uint32_t copySize = (uint32_t)WasmTypeByteSize(program, valueTypeRef);
                    if (copySize == 0u) {
                        WasmSetDiag(
                            diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                        if (diag != NULL) {
                            diag->detail = "unsupported aggregate dereference store";
                        }
                        return -1;
                    }
                    if (WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmEmitCopyLocalAddrToLocalAddr(
                               body, state->scratch0Local, state->scratch1Local, 0u, copySize)
                               != 0)
                    {
                        return -1;
                    }
                } else if (refType == HOPWasmType_STR_PTR && valueType == HOPWasmType_STR_REF) {
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
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported dereference store";
                    }
                    return -1;
                }
                break;
            }
            case H2MirOp_SEQ_LEN: {
                uint8_t  valueType = 0;
                uint32_t valueTypeRef = UINT32_MAX;
                if (WasmStackPopEx(state, &valueType, &valueTypeRef) != 0) {
                    return -1;
                }
                if (valueType == HOPWasmType_STR_REF) {
                    if (WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x1au) != 0 || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmStackPush(state, HOPWasmType_I32) != 0)
                    {
                        return -1;
                    }
                } else if (WasmTypeKindIsArrayView(valueType) && valueTypeRef < program->typeLen) {
                    if (WasmAppendByte(body, 0x1au) != 0 || WasmAppendByte(body, 0x41u) != 0
                        || WasmAppendSLEB32(
                               body,
                               (int32_t)H2MirTypeRefFixedArrayCount(&program->types[valueTypeRef]))
                               != 0
                        || WasmStackPushEx(state, HOPWasmType_I32, UINT32_MAX) != 0)
                    {
                        return -1;
                    }
                } else if (WasmTypeKindIsSlice(valueType)) {
                    if (WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x1au) != 0 || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmStackPushEx(state, HOPWasmType_I32, UINT32_MAX) != 0)
                    {
                        return -1;
                    }
                } else if (valueType == HOPWasmType_STR_PTR) {
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
                        || WasmStackPush(state, HOPWasmType_I32) != 0)
                    {
                        return -1;
                    }
                } else {
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported seq len operand";
                    }
                    return -1;
                }
                break;
            }
            case H2MirOp_STR_CSTR: {
                uint8_t valueType = 0;
                if (WasmStackPop(state, &valueType) != 0) {
                    return -1;
                }
                if (valueType == HOPWasmType_STR_REF) {
                    if (WasmAppendByte(body, 0x1au) != 0
                        || WasmStackPush(state, HOPWasmType_U8_PTR) != 0)
                    {
                        return -1;
                    }
                } else if (valueType == HOPWasmType_STR_PTR) {
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
                        || WasmStackPush(state, HOPWasmType_U8_PTR) != 0)
                    {
                        return -1;
                    }
                } else {
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported cstr operand";
                    }
                    return -1;
                }
                break;
            }
            case H2MirOp_INDEX: {
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
                if (baseType == HOPWasmType_U8_PTR) {
                    if (WasmAppendByte(body, 0x6au) != 0 || WasmEmitU8Load(body) != 0
                        || WasmStackPush(state, HOPWasmType_I32) != 0)
                    {
                        return -1;
                    }
                } else if (baseType == HOPWasmType_STR_REF) {
                    if (WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0
                        || WasmAppendByte(body, 0x1au) != 0 || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0
                        || WasmAppendByte(body, 0x6au) != 0 || WasmEmitU8Load(body) != 0
                        || WasmStackPush(state, HOPWasmType_I32) != 0)
                    {
                        return -1;
                    }
                } else if (
                    baseType == HOPWasmType_I8_PTR || baseType == HOPWasmType_U16_PTR
                    || baseType == HOPWasmType_I16_PTR || baseType == HOPWasmType_U32_PTR
                    || baseType == HOPWasmType_I32_PTR || WasmTypeKindIsArrayView(baseType))
                {
                    uint32_t elemSize = WasmTypeKindElementSize(baseType);
                    if (WasmEmitScaleIndex(body, elemSize) != 0 || WasmAppendByte(body, 0x6au) != 0
                        || WasmEmitTypedLoad(body, baseType) != 0
                        || WasmStackPush(state, HOPWasmType_I32) != 0)
                    {
                        return -1;
                    }
                } else if (WasmTypeKindIsSlice(baseType)) {
                    uint32_t elemSize = WasmTypeElementSize(program, baseType, baseTypeRef);
                    if (elemSize == 0u) {
                        WasmSetDiag(
                            diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
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
                    if (baseType == HOPWasmType_SLICE_AGG) {
                        if (WasmStackPushEx(
                                state,
                                HOPWasmType_OPAQUE_PTR,
                                WasmAggSliceElemTypeRef(program, baseTypeRef))
                            != 0)
                        {
                            return -1;
                        }
                    } else if (
                        WasmEmitTypedLoad(body, baseType) != 0
                        || WasmStackPushEx(state, HOPWasmType_I32, UINT32_MAX) != 0)
                    {
                        return -1;
                    }
                } else if (baseType == HOPWasmType_STR_PTR) {
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
                        || WasmStackPush(state, HOPWasmType_I32) != 0)
                    {
                        return -1;
                    }
                } else {
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported index base";
                    }
                    return -1;
                }
                break;
            }
            case H2MirOp_SLICE_MAKE: {
                uint8_t  baseType = 0;
                uint32_t baseTypeRef = UINT32_MAX;
                uint32_t totalLen = 0u;
                uint8_t  resultType = HOPWasmType_VOID;
                int      hasStart = (inst->tok & H2AstFlag_INDEX_HAS_START) != 0u;
                int      hasEnd = (inst->tok & H2AstFlag_INDEX_HAS_END) != 0u;
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
                                 ? H2MirTypeRefFixedArrayCount(&program->types[baseTypeRef])
                                 : 0u;
                    if (totalLen == 0u) {
                        WasmSetDiag(
                            diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                        if (diag != NULL) {
                            diag->detail = "unsupported slice base";
                        }
                        return -1;
                    }
                    resultType =
                        (uint8_t)(baseType + (HOPWasmType_SLICE_U8 - HOPWasmType_ARRAY_VIEW_U8));
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
                WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                if (diag != NULL) {
                    diag->detail = "unsupported slice base";
                }
                return -1;
            }
            case H2MirOp_CTX_GET:
            case H2MirOp_CTX_ADDR:
                if ((inst->aux == H2MirContextField_ALLOCATOR
                     || inst->aux == H2MirContextField_TEMP_ALLOCATOR)
                    && strings != NULL && strings->rootAllocatorOffset != UINT32_MAX)
                {
                    uint32_t memAllocatorTypeRef = WasmFindMemAllocatorTypeRef(program);
                    if (WasmAppendByte(body, 0x41u) != 0
                        || WasmAppendSLEB32(body, (int32_t)strings->rootAllocatorOffset) != 0
                        || WasmStackPushEx(
                               state,
                               memAllocatorTypeRef < program->typeLen
                                   ? HOPWasmType_AGG_REF
                                   : HOPWasmType_OPAQUE_PTR,
                               memAllocatorTypeRef)
                               != 0)
                    {
                        return -1;
                    }
                    break;
                }
                WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                if (diag != NULL) {
                    diag->detail = "unsupported context access";
                }
                return -1;
            case H2MirOp_ALLOC_NEW: {
                const H2MirInst* nextInst;
                uint32_t         localIndex;
                uint8_t          allocType = HOPWasmType_VOID;
                uint32_t         allocTypeRef = UINT32_MAX;
                uint32_t         allocSize = 0u;
                uint8_t          allocArgType = HOPWasmType_VOID;
                bool             hasAllocArg = (inst->tok & H2AstFlag_NEW_HAS_ALLOC) != 0u;
                bool             useContextAllocator =
                    !hasAllocArg && strings != NULL && strings->rootAllocatorOffset != UINT32_MAX;
                bool useAllocator = hasAllocArg || useContextAllocator;
                bool useFixedCountAlloc = false;
                bool isOptional = false;
                if (imports == NULL || !imports->needsHeapGlobal
                    || (inst->tok & H2AstFlag_NEW_HAS_INIT) != 0 || pc + 1u >= fn->instLen)
                {
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported ALLOC_NEW shape";
                    }
                    return -1;
                }
                nextInst = &program->insts[fn->instStart + pc + 1u];
                if (nextInst->op != H2MirOp_LOCAL_STORE || nextInst->aux >= fn->localCount) {
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported ALLOC_NEW shape";
                    }
                    return -1;
                }
                localIndex = nextInst->aux;
                allocType = state->localKinds[localIndex];
                allocTypeRef = state->localTypeRefs[localIndex];
                useFixedCountAlloc =
                    (inst->tok & H2AstFlag_NEW_HAS_COUNT) != 0u && allocTypeRef < program->typeLen
                    && (H2MirTypeRefIsFixedArray(&program->types[allocTypeRef])
                        || H2MirTypeRefIsFixedArrayView(&program->types[allocTypeRef]));
                isOptional = allocTypeRef < program->typeLen
                          && H2MirTypeRefIsOptional(&program->types[allocTypeRef]);
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
                } else if (useContextAllocator) {
                    if (WasmAppendByte(body, 0x41u) != 0
                        || WasmAppendSLEB32(body, (int32_t)strings->rootAllocatorOffset) != 0
                        || WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch2Local) != 0)
                    {
                        return -1;
                    }
                }
                if (WasmTypeKindIsSlice(allocType)) {
                    uint32_t elemSize = WasmTypeElementSize(program, allocType, allocTypeRef);
                    uint8_t  countType = 0;
                    if ((inst->tok & H2AstFlag_NEW_HAS_COUNT) == 0u || elemSize == 0u
                        || WasmStackPop(state, &countType) != 0
                        || WasmRequireI32Value(
                               countType,
                               diag,
                               inst->start,
                               inst->end,
                               "unsupported dynamic ALLOC_NEW count")
                               != 0)
                    {
                        if (diag != NULL && diag->code == H2Diag_NONE) {
                            WasmSetDiag(
                                diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                            diag->detail = "unsupported dynamic ALLOC_NEW type";
                        }
                        return -1;
                    }
                    if (WasmAppendByte(body, 0x21u) != 0
                        || WasmAppendULEB(body, state->scratch1Local) != 0)
                    {
                        return -1;
                    }
                    if (useAllocator) {
                        if (state->allocCallTempOffset == UINT32_MAX
                            || WasmAppendByte(body, 0x20u) != 0
                            || WasmAppendULEB(body, state->scratch2Local) != 0
                            || WasmAppendByte(body, 0x45u) != 0 || WasmAppendByte(body, 0x04u) != 0
                            || WasmAppendByte(body, 0x40u) != 0)
                        {
                            return -1;
                        }
                        if (WasmEmitInvalidAllocatorTrap(body, imports, strings) != 0) {
                            return -1;
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
                            || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0
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
                            if (imports->hasPlatformPanicImport && strings != NULL
                                && strings->hasAllocNullPanicString)
                            {
                                if (WasmEmitPlatformPanicCall(
                                        body,
                                        imports,
                                        (int32_t)strings->allocNullPanic.dataOffset,
                                        (int32_t)strings->allocNullPanic.len,
                                        0)
                                    != 0)
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
                if ((inst->tok & H2AstFlag_NEW_HAS_COUNT) != 0u && !useFixedCountAlloc) {
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
                    if (useAllocator) {
                        if (state->allocCallTempOffset == UINT32_MAX
                            || WasmAppendByte(body, 0x20u) != 0
                            || WasmAppendULEB(body, state->scratch2Local) != 0
                            || WasmAppendByte(body, 0x45u) != 0 || WasmAppendByte(body, 0x04u) != 0
                            || WasmAppendByte(body, 0x40u) != 0)
                        {
                            return -1;
                        }
                        if (WasmEmitInvalidAllocatorTrap(body, imports, strings) != 0) {
                            return -1;
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
                            || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0
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
                            if (imports->hasPlatformPanicImport && strings != NULL
                                && strings->hasAllocNullPanicString)
                            {
                                if (WasmEmitPlatformPanicCall(
                                        body,
                                        imports,
                                        (int32_t)strings->allocNullPanic.dataOffset,
                                        (int32_t)strings->allocNullPanic.len,
                                        0)
                                    != 0)
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
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported ALLOC_NEW type";
                    }
                    return -1;
                }
                if (useAllocator) {
                    if (state->allocCallTempOffset == UINT32_MAX || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch2Local) != 0
                        || WasmAppendByte(body, 0x45u) != 0 || WasmAppendByte(body, 0x04u) != 0
                        || WasmAppendByte(body, 0x40u) != 0)
                    {
                        return -1;
                    }
                    if (WasmEmitInvalidAllocatorTrap(body, imports, strings) != 0) {
                        return -1;
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
                        || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0
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
                        if (imports->hasPlatformPanicImport && strings != NULL
                            && strings->hasAllocNullPanicString)
                        {
                            if (WasmEmitPlatformPanicCall(
                                    body,
                                    imports,
                                    (int32_t)strings->allocNullPanic.dataOffset,
                                    (int32_t)strings->allocNullPanic.len,
                                    0)
                                != 0)
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
            case H2MirOp_DROP: {
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
            case H2MirOp_CALL_FN: {
                uint32_t             argc = H2MirCallArgCountFromTok(inst->tok);
                const H2MirFunction* callee;
                const HOPWasmFnSig*  calleeSig;
                uint32_t             argIndex;
                uint16_t             argLocal[32];
                uint8_t              argSlotCount[32];
                uint32_t             nextI32Scratch = 0u;
                uint32_t             nextI64Scratch = 0u;
                uint32_t             tempOffset = UINT32_MAX;
                if (inst->aux >= program->funcLen) {
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_INTERNAL, inst->start, inst->end);
                    return -1;
                }
                callee = &program->funcs[inst->aux];
                calleeSig = &sigs[inst->aux];
                if (H2MirCallTokDropsReceiverArg0(inst->tok) || argc != callee->paramCount) {
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported call shape";
                    }
                    return -1;
                }
                if (argc > sizeof(argLocal) / sizeof(argLocal[0])
                    || (argc > 0u
                        && (state->directCallScratchI32Start == UINT16_MAX
                            || state->directCallScratchI64Start == UINT16_MAX)))
                {
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported call shape";
                    }
                    return -1;
                }
                for (argIndex = argc; argIndex > 0u; argIndex--) {
                    uint8_t  argType = 0;
                    uint32_t argTypeRef = UINT32_MAX;
                    uint8_t  expectedType = calleeSig->logicalParamKinds[argIndex - 1u];
                    uint32_t expectedSlotCount = WasmTypeKindSlotCount(expectedType);
                    uint32_t actualSlotCount = 0u;
                    if (WasmStackPopEx(state, &argType, &argTypeRef) != 0
                        || (actualSlotCount = WasmTypeKindSlotCount(argType)) == 0u
                        || expectedSlotCount == 0u
                        || (actualSlotCount != expectedSlotCount
                            && !(actualSlotCount == 1u && expectedSlotCount == 1u))
                        || WasmAdaptCallArgValue(body, program, expectedType, argType, argTypeRef)
                               != 0)
                    {
                        WasmSetDiag(
                            diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                        if (diag != NULL) {
                            diag->detail = "call argument type mismatch";
                        }
                        return -1;
                    }
                    argSlotCount[argIndex - 1u] = (uint8_t)expectedSlotCount;
                    if (expectedType == HOPWasmType_I64) {
                        if (nextI64Scratch >= HOPWasmDirectCallScratchI64Count
                            || WasmAppendByte(body, 0x21u) != 0
                            || WasmAppendULEB(
                                   body,
                                   (uint16_t)(state->directCallScratchI64Start + nextI64Scratch))
                                   != 0)
                        {
                            return -1;
                        }
                        argLocal[argIndex - 1u] =
                            (uint16_t)(state->directCallScratchI64Start + nextI64Scratch);
                        nextI64Scratch++;
                    } else if (expectedSlotCount == 2u) {
                        if (nextI32Scratch + 1u >= HOPWasmDirectCallScratchI32Count
                            || WasmAppendByte(body, 0x21u) != 0
                            || WasmAppendULEB(
                                   body,
                                   (uint16_t)(state->directCallScratchI32Start + nextI32Scratch
                                              + 1u))
                                   != 0
                            || WasmAppendByte(body, 0x21u) != 0
                            || WasmAppendULEB(
                                   body,
                                   (uint16_t)(state->directCallScratchI32Start + nextI32Scratch))
                                   != 0)
                        {
                            return -1;
                        }
                        argLocal[argIndex - 1u] =
                            (uint16_t)(state->directCallScratchI32Start + nextI32Scratch);
                        nextI32Scratch += 2u;
                    } else {
                        if (nextI32Scratch >= HOPWasmDirectCallScratchI32Count
                            || WasmAppendByte(body, 0x21u) != 0
                            || WasmAppendULEB(
                                   body,
                                   (uint16_t)(state->directCallScratchI32Start + nextI32Scratch))
                                   != 0)
                        {
                            return -1;
                        }
                        argLocal[argIndex - 1u] =
                            (uint16_t)(state->directCallScratchI32Start + nextI32Scratch);
                        nextI32Scratch++;
                    }
                }
                for (argIndex = 0; argIndex < argc; argIndex++) {
                    if (argSlotCount[argIndex] == 2u) {
                        if (WasmAppendByte(body, 0x20u) != 0
                            || WasmAppendULEB(body, argLocal[argIndex]) != 0
                            || WasmAppendByte(body, 0x20u) != 0
                            || WasmAppendULEB(body, (uint16_t)(argLocal[argIndex] + 1u)) != 0)
                        {
                            return -1;
                        }
                    } else if (
                        WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, argLocal[argIndex]) != 0)
                    {
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
                            diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                        if (diag != NULL && diag->detail == NULL) {
                            diag->detail = "unsupported call shape";
                        }
                        return -1;
                    }
                }
                if (WasmAppendByte(body, 0x10u) != 0
                    || WasmAppendULEB(body, WasmFunctionWasmIndex(imports, inst->aux)) != 0)
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
                    calleeSig->logicalResultKind != HOPWasmType_VOID
                    && WasmStackPushEx(
                           state, calleeSig->logicalResultKind, calleeSig->logicalResultTypeRef)
                           != 0)
                {
                    return -1;
                }
                break;
            }
            case H2MirOp_CALL_INDIRECT: {
                uint32_t            argc = H2MirCallArgCountFromTok(inst->tok);
                const HOPWasmFnSig* calleeSig = NULL;
                uint32_t            calleeTypeRef = UINT32_MAX;
                uint32_t            calleeFuncIndex = UINT32_MAX;
                uint32_t            totalArgSlots = 0u;
                uint32_t            argIndex;
                uint32_t            scratchIndex;
                uint8_t             calleeType = 0;
                uint32_t            ignoredTypeRef = UINT32_MAX;
                if (imports == NULL || !imports->hasFunctionTable
                    || H2MirCallTokDropsReceiverArg0(inst->tok) || state->stackLen < argc + 1u)
                {
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported indirect call shape";
                    }
                    return -1;
                }
                calleeTypeRef = state->stackTypeRefs[state->stackLen - argc - 1u];
                if (calleeTypeRef < program->typeLen) {
                    calleeFuncIndex = H2MirTypeRefFuncRefFunctionIndex(
                        &program->types[calleeTypeRef]);
                    if (calleeFuncIndex < program->funcLen) {
                        calleeSig = &sigs[calleeFuncIndex];
                    }
                }
                if (calleeSig != NULL && !WasmSigMatchesAllocatorIndirect(calleeSig)) {
                    if (argc != calleeSig->logicalParamCount || calleeSig->usesSRet) {
                        WasmSetDiag(
                            diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
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
                            diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                        if (diag != NULL) {
                            diag->detail = "unsupported indirect call shape";
                        }
                        return -1;
                    }
                    scratchIndex = totalArgSlots;
                    for (argIndex = argc; argIndex > 0u; argIndex--) {
                        uint8_t  argType = 0;
                        uint32_t argTypeRef = UINT32_MAX;
                        uint8_t  expectedType = calleeSig->logicalParamKinds[argIndex - 1u];
                        uint32_t slotCount = WasmTypeKindSlotCount(expectedType);
                        if (WasmStackPopEx(state, &argType, &argTypeRef) != 0
                            || WasmAdaptCallArgValue(
                                   body, program, expectedType, argType, argTypeRef)
                                   != 0)
                        {
                            WasmSetDiag(
                                diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
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
                        || calleeType != HOPWasmType_FUNC_REF || WasmAppendByte(body, 0x21u) != 0
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
                    if (calleeSig->logicalResultKind != HOPWasmType_VOID
                        && WasmStackPushEx(
                               state, calleeSig->logicalResultKind, calleeSig->logicalResultTypeRef)
                               != 0)
                    {
                        return -1;
                    }
                    break;
                }
                if (!state->usesFrame || argc != 7u) {
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
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
                               "allocator source location must be i32")
                               != 0
                        || WasmStackPop(state, &valueType) != 0
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
                        || WasmRequireAllocatorValue(
                               valueType,
                               diag,
                               inst->start,
                               inst->end,
                               "allocator addr must be pointer-like")
                               != 0
                        || WasmStackPop(state, &valueType) != 0
                        || WasmRequireAllocatorValue(
                               valueType,
                               diag,
                               inst->start,
                               inst->end,
                               "allocator self must be pointer-like")
                               != 0
                        || WasmStackPop(state, &valueType) != 0
                        || valueType != HOPWasmType_FUNC_REF)
                    {
                        if (diag != NULL && diag->code == H2Diag_NONE) {
                            WasmSetDiag(
                                diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                            diag->detail = "allocator indirect callee must be a function value";
                        }
                        return -1;
                    }
                    if (WasmAppendByte(body, 0x1au) != 0 || WasmAppendByte(body, 0x21u) != 0
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
                        || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, 0) != 0
                        || WasmAppendByte(body, 0x20u) != 0
                        || WasmAppendULEB(body, state->scratch0Local) != 0
                        || WasmAppendByte(body, 0x11u) != 0
                        || WasmAppendULEB(body, imports->allocatorIndirectTypeIndex) != 0
                        || WasmAppendByte(body, 0x00u) != 0
                        || WasmStackPushEx(state, HOPWasmType_OPAQUE_PTR, UINT32_MAX) != 0)
                    {
                        return -1;
                    }
                }
                break;
            }
            case H2MirOp_CALL_HOST: {
                WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_HOSTCALL, inst->start, inst->end);
                if (diag != NULL) {
                    diag->detail = "host calls are not supported in this Wasm mode";
                }
                return -1;
            }
            case H2MirOp_ASSERT: {
                uint8_t condType = 0;
                if (WasmStackPop(state, &condType) != 0
                    || WasmRequireI32Value(
                           condType, diag, inst->start, inst->end, "non-scalar assert condition")
                           != 0)
                {
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
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
                if (imports != NULL && imports->hasPlatformPanicImport && strings != NULL
                    && strings->hasAssertPanicString)
                {
                    if (WasmEmitPlatformPanicCall(
                            body,
                            imports,
                            (int32_t)strings->assertPanic.dataOffset,
                            (int32_t)strings->assertPanic.len,
                            0)
                            != 0
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
            case H2MirOp_JUMP_IF_FALSE: {
                uint8_t              condType = 0;
                uint32_t             falseTarget = inst->aux;
                uint32_t             thenEnd = falseTarget;
                uint32_t             mergeTarget = falseTarget;
                HOPWasmBranchTargets nestedTargets = WasmNestedBranchTargets(branchTargets);
                uint32_t             stackDepthBefore;
                uint32_t             branchDepth = 0;
                int                  hasElse = 0;
                if (falseTarget <= pc + 1u || falseTarget > endPc) {
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported control-flow graph shape";
                    }
                    return -1;
                }
                if (WasmStackPop(state, &condType) != 0 || condType != HOPWasmType_I32) {
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                    if (diag != NULL) {
                        diag->detail = "unsupported branch condition type";
                    }
                    return -1;
                }
                if (falseTarget > pc + 1u) {
                    const H2MirInst* maybeElseJump =
                        &program->insts[fn->instStart + falseTarget - 1u];
                    if (maybeElseJump->op == H2MirOp_JUMP) {
                        if (WasmBranchDepthForJump(branchTargets, maybeElseJump->aux, &branchDepth))
                        {
                            thenEnd = falseTarget;
                        } else if (maybeElseJump->aux <= falseTarget) {
                            WasmSetDiag(
                                diag,
                                H2Diag_WASM_BACKEND_UNSUPPORTED_MIR,
                                maybeElseJump->start,
                                maybeElseJump->end);
                            if (diag != NULL) {
                                diag->detail = "loops/backedges are not supported";
                            }
                            return -1;
                        } else if (maybeElseJump->aux > endPc) {
                            WasmSetDiag(
                                diag,
                                H2Diag_WASM_BACKEND_UNSUPPORTED_MIR,
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
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
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
                            diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
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
            case H2MirOp_JUMP: {
                uint32_t branchDepth = 0;
                if (WasmBranchDepthForJump(branchTargets, inst->aux, &branchDepth)) {
                    if (WasmAppendByte(body, 0x0cu) != 0 || WasmAppendULEB(body, branchDepth) != 0)
                    {
                        return -1;
                    }
                    break;
                }
                WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                if (diag != NULL) {
                    diag->detail = inst->aux > pc ? "unsupported forward jump shape"
                                                  : "loops/backedges are not supported";
                }
                return -1;
            }
            case H2MirOp_RETURN: {
                uint8_t  returnType = 0;
                uint32_t returnTypeRef = UINT32_MAX;
                if (resultKind == HOPWasmType_VOID) {
                    WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
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
                                diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
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
                        || WasmEmitRestoreFrameAndReturn(body, imports, state, HOPWasmType_VOID)
                               != 0)
                    {
                        return -1;
                    }
                    break;
                }
                if (returnType != resultKind
                    && WasmAdaptCallArgValue(body, program, resultKind, returnType, returnTypeRef)
                           != 0)
                {
                    if (diag != NULL && diag->code == 0) {
                        WasmSetDiag(
                            diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                        diag->detail = "unsupported return value";
                    }
                    return -1;
                }
                if (state->usesFrame) {
                    if (WasmEmitRestoreFrameAndReturn(body, imports, state, resultKind) != 0) {
                        WasmSetDiag(
                            diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                        if (diag != NULL) {
                            diag->detail = "failed emitting frame return";
                        }
                        return -1;
                    }
                } else if (WasmAppendByte(body, 0x0fu) != 0) {
                    return -1;
                }
                break;
            }
            case H2MirOp_RETURN_VOID:
                if (resultKind != HOPWasmType_VOID) {
                    if (WasmAppendByte(body, 0x00u) != 0) {
                        return -1;
                    }
                    break;
                }
                if (state->usesFrame) {
                    if (WasmEmitRestoreFrameAndReturn(body, imports, state, HOPWasmType_VOID) != 0)
                    {
                        return -1;
                    }
                } else if (WasmAppendByte(body, 0x0fu) != 0) {
                    return -1;
                }
                break;
            case H2MirOp_LOAD_IDENT:
            case H2MirOp_CALL:
                WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                if (diag != NULL) {
                    diag->detail = "MIR still needs dynamic resolution";
                }
                return -1;
            default:
                WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, inst->start, inst->end);
                if (diag != NULL) {
                    diag->detail = WasmMirOpName(inst->op);
                }
                return -1;
        }
        pc++;
    }
    return 0;
}

static int WasmAppendLocalDecls(
    HOPWasmBuf* body, const uint8_t* valueTypes, uint32_t valueTypeLen) {
    uint32_t groupCount = 0u;
    uint32_t i = 0u;
    if (body == NULL) {
        return -1;
    }
    while (i < valueTypeLen) {
        uint8_t  valueType = valueTypes[i];
        uint32_t runLen = 1u;
        while (i + runLen < valueTypeLen && valueTypes[i + runLen] == valueType) {
            runLen++;
        }
        groupCount++;
        i += runLen;
    }
    if (WasmAppendULEB(body, groupCount) != 0) {
        return -1;
    }
    i = 0u;
    while (i < valueTypeLen) {
        uint8_t  valueType = valueTypes[i];
        uint32_t runLen = 1u;
        while (i + runLen < valueTypeLen && valueTypes[i + runLen] == valueType) {
            runLen++;
        }
        if (WasmAppendULEB(body, runLen) != 0 || WasmAppendByte(body, valueType) != 0) {
            return -1;
        }
        i += runLen;
    }
    return 0;
}

static int WasmEmitFunctionBody(
    const H2MirProgram*        program,
    const HOPWasmFnSig*        sigs,
    const HOPWasmImportLayout* imports,
    const HOPWasmStringLayout* strings,
    uint32_t                   funcIndex,
    HOPWasmBuf*                body,
    H2Diag* _Nullable diag) {
    const H2MirFunction* fn;
    HOPWasmEmitState     state;
    uint8_t              localValueTypes[512];
    uint32_t             localValueTypeLen = 0u;
    uint8_t              resultKind = HOPWasmType_VOID;
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
    if (imports != NULL && imports->globalImportIndices != NULL
        && imports->globalImportIndices[funcIndex] != UINT32_MAX)
    {
        if (WasmAppendULEB(body, 0u) != 0 || WasmAppendByte(body, 0x23u) != 0
            || WasmAppendULEB(body, imports->globalImportIndices[funcIndex]) != 0
            || WasmAppendByte(body, 0x0bu) != 0)
        {
            return -1;
        }
        return 0;
    }
    if (WasmPrepareFunctionState(program, fn, &state, diag) != 0) {
        return -1;
    }
    if (!WasmTypeKindFromMirType(program, fn->typeRef, &resultKind)) {
        WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, fn->nameStart, fn->nameEnd);
        return -1;
    }

    if (state.usesFrame) {
        for (localValueTypeLen = 0u; localValueTypeLen < 8u; localValueTypeLen++) {
            localValueTypes[localValueTypeLen] = 0x7fu;
        }
        localValueTypes[localValueTypeLen++] = 0x7eu;
    } else {
        uint32_t localIndex;
        for (localIndex = fn->paramCount; localIndex < fn->localCount; localIndex++) {
            uint8_t  typeKind = state.localKinds[localIndex];
            uint32_t slotCount = WasmTypeKindSlotCount(typeKind);
            if (slotCount == 0u || localValueTypeLen + slotCount > sizeof(localValueTypes)) {
                WasmSetDiag(
                    diag,
                    H2Diag_WASM_BACKEND_UNSUPPORTED_MIR,
                    program->locals[fn->localStart + localIndex].nameStart,
                    program->locals[fn->localStart + localIndex].nameEnd);
                if (diag != NULL) {
                    diag->detail = "unsupported local value type";
                }
                return -1;
            }
            if (slotCount == 2u) {
                localValueTypes[localValueTypeLen++] = 0x7fu;
                localValueTypes[localValueTypeLen++] = 0x7fu;
            } else {
                localValueTypes[localValueTypeLen++] = WasmTypeKindWasmValueType(typeKind);
            }
        }
        if (WasmFunctionNeedsIndirectScratch(program, fn)) {
            uint32_t scratchIndex;
            if (localValueTypeLen + 8u > sizeof(localValueTypes)) {
                WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, fn->nameStart, fn->nameEnd);
                if (diag != NULL) {
                    diag->detail = "too many Wasm locals";
                }
                return -1;
            }
            for (scratchIndex = 0u; scratchIndex < 7u; scratchIndex++) {
                localValueTypes[localValueTypeLen++] = 0x7fu;
            }
            localValueTypes[localValueTypeLen++] = 0x7eu;
        }
    }
    if (state.directCallScratchI32Start != UINT16_MAX) {
        uint32_t scratchIndex;
        if (localValueTypeLen + HOPWasmDirectCallScratchI32Count + HOPWasmDirectCallScratchI64Count
            > sizeof(localValueTypes))
        {
            WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, fn->nameStart, fn->nameEnd);
            if (diag != NULL) {
                diag->detail = "too many Wasm locals";
            }
            return -1;
        }
        for (scratchIndex = 0u; scratchIndex < HOPWasmDirectCallScratchI32Count; scratchIndex++) {
            localValueTypes[localValueTypeLen++] = 0x7fu;
        }
        for (scratchIndex = 0u; scratchIndex < HOPWasmDirectCallScratchI64Count; scratchIndex++) {
            localValueTypes[localValueTypeLen++] = 0x7eu;
        }
    }
    if (WasmAppendLocalDecls(body, localValueTypes, localValueTypeLen) != 0) {
        return -1;
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
            WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, fn->nameStart, fn->nameEnd);
            if (diag != NULL) {
                diag->detail = "failed emitting Wasm frame prologue";
            }
            return -1;
        }
        for (paramIndex = 0; paramIndex < fn->paramCount; paramIndex++) {
            uint8_t typeKind = state.localKinds[paramIndex];
            if (state.localStorage[paramIndex] == HOPWasmLocalStorage_AGG) {
                uint32_t copySize = (uint32_t)WasmTypeByteSize(
                    program, state.localTypeRefs[paramIndex]);
                if (copySize == 0u || WasmAppendByte(body, 0x20u) != 0
                    || WasmAppendULEB(body, state.wasmValueIndex[paramIndex]) != 0
                    || WasmAppendByte(body, 0x21u) != 0
                    || WasmAppendULEB(body, state.scratch0Local) != 0
                    || WasmEmitCopyLocalAddrToFrame(
                           body,
                           &state,
                           state.scratch0Local,
                           state.frameOffsets[paramIndex],
                           copySize)
                           != 0)
                {
                    return -1;
                }
            } else if (WasmTypeKindSlotCount(typeKind) == 2u) {
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
                    || (typeKind == HOPWasmType_I64
                            ? WasmEmitI64Store(body)
                            : WasmEmitI32Store(body))
                           != 0)
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
            &(HOPWasmBranchTargets){ 0 },
            resultKind,
            0,
            fn->instLen,
            diag)
        != 0)
    {
        return -1;
    }
    if (WasmAppendByte(body, 0x0bu) != 0) {
        WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, fn->nameStart, fn->nameEnd);
        if (diag != NULL) {
            diag->detail = "failed finalizing Wasm function body";
        }
        return -1;
    }
    return 0;
}

static int WasmEmitRootAllocThunkBody(
    const HOPWasmImportLayout* imports, const H2CodegenOptions* options, HOPWasmBuf* body) {
    enum {
        kParamSelf = 0,
        kParamAddr = 1,
        kParamAlign = 2,
        kParamCurSize = 3,
        kParamNewSizePtr = 4,
        kParamFlags = 5,
        kParamSrcLoc = 6,
        kLocalBase = 7,
        kLocalSize = 8,
        kLocalCursor = 9,
        kLocalAlign = 10,
    };
    (void)kParamSrcLoc;
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
    const H2MirProgram*        program,
    const HOPWasmFnSig*        sigs,
    const HOPWasmImportLayout* imports,
    const HOPWasmEntryLayout*  entry,
    const H2CodegenOptions*    options,
    HOPWasmBuf*                body,
    H2Diag* _Nullable diag) {
    const HOPWasmFnSig* sig;
    uint32_t            mainFuncWasmIndex;
    if (program == NULL || sigs == NULL || imports == NULL || entry == NULL || options == NULL
        || body == NULL || !entry->hasWrapper || entry->mainFuncIndex >= program->funcLen)
    {
        return -1;
    }
    sig = &sigs[entry->mainFuncIndex];
    mainFuncWasmIndex = WasmFunctionWasmIndex(imports, entry->mainFuncIndex);
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
            || WasmAppendByte(body, WasmTypeKindWasmValueType(sig->logicalResultKind)) != 0
            || WasmAppendByte(body, 0x10u) != 0 || WasmAppendULEB(body, mainFuncWasmIndex) != 0
            || WasmAppendByte(body, 0x21u) != 0 || WasmAppendULEB(body, 0u) != 0
            || WasmAppendByte(body, 0x41u) != 0
            || WasmAppendSLEB32(body, (int32_t)entry->resultOffset) != 0
            || WasmAppendByte(body, 0x20u) != 0 || WasmAppendULEB(body, 0u) != 0
            || (sig->logicalResultKind == HOPWasmType_I64
                    ? WasmEmitI64Store(body)
                    : WasmEmitI32Store(body))
                   != 0
            || WasmAppendByte(body, 0x0bu) != 0)
        {
            return -1;
        }
        return 0;
    }
    if (WasmTypeKindSlotCount(sig->logicalResultKind) != 2u) {
        WasmSetDiag(
            diag,
            H2Diag_WASM_BACKEND_UNSUPPORTED_MIR,
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

static int WasmEmitPlatformPanicCall(
    HOPWasmBuf*                body,
    const HOPWasmImportLayout* imports,
    int32_t                    dataOffset,
    int32_t                    len,
    int32_t                    flags) {
    if (body == NULL || imports == NULL || imports->platformPanicFuncIndex == UINT32_MAX) {
        return -1;
    }
    if (WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, dataOffset) != 0
        || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, len) != 0
        || WasmAppendByte(body, 0x41u) != 0 || WasmAppendSLEB32(body, flags) != 0
        || WasmAppendByte(body, 0x10u) != 0
        || WasmAppendULEB(body, imports->platformPanicFuncIndex) != 0)
    {
        return -1;
    }
    return 0;
}

static int EmitWasmBackend(
    const H2CodegenBackend* backend,
    const H2CodegenUnit*    unit,
    const H2CodegenOptions* _Nullable options,
    H2CodegenArtifact* _Nonnull outArtifact,
    H2Diag* _Nullable diag) {
    HOPWasmBuf out = { 0 };
    HOPWasmBuf typeSec = { 0 };
    HOPWasmBuf importSec = { 0 };
    HOPWasmBuf funcSec = { 0 };
    HOPWasmBuf tableSec = { 0 };
    HOPWasmBuf memSec = { 0 };
    HOPWasmBuf globalSec = { 0 };
    HOPWasmBuf exportSec = { 0 };
    HOPWasmBuf elemSec = { 0 };
    HOPWasmBuf codeSec = { 0 };
    HOPWasmBuf dataSec = { 0 };
    HOPWasmFnSig* _Nullable sigs = NULL;
    HOPWasmStringLayout    strings = { 0 };
    HOPWasmImportLayout    imports = { 0 };
    HOPWasmEntryLayout     entry = { 0 };
    HOPWasmForeignMetadata foreign = { 0 };
    uint32_t               i;
    bool                   importMemory = WasmIsPlaybitPlatform(unit);
    uint32_t               exportCount = importMemory ? 0u : 1u;
    const char             wasmHeader[8] = { '\0', 'a', 's', 'm', 0x01, 0x00, 0x00, 0x00 };
    (void)backend;

    if (diag != NULL) {
        *diag = (H2Diag){ 0 };
    }
    *outArtifact = (H2CodegenArtifact){ 0 };
    if (unit == NULL || outArtifact == NULL || options == NULL || options->arenaGrow == NULL) {
        WasmSetDiag(diag, H2Diag_WASM_BACKEND_INTERNAL, 0, 0);
        return -1;
    }
    if (unit->usesPlatform && !WasmSupportsPlatformImports(unit)) {
        WasmSetDiag(diag, H2Diag_WASM_BACKEND_PLATFORM_REQUIRED, 0, 0);
        if (diag != NULL) {
            diag->detail =
                "packages using import \"platform\" require --platform wasm-min or playbit";
        }
        return -1;
    }
    if (unit->mirProgram == NULL) {
        WasmSetDiag(diag, H2Diag_WASM_BACKEND_INTERNAL, 0, 0);
        if (diag != NULL) {
            diag->detail = "missing MIR program";
        }
        return -1;
    }
    if (WasmCollectForeignDirectives(unit, &foreign, diag) != 0) {
        return -1;
    }
    if (H2MirProgramNeedsDynamicResolution(unit->mirProgram)) {
        uint32_t         funcIndex = UINT32_MAX;
        uint32_t         pc = UINT32_MAX;
        const H2MirInst* inst = NULL;
        WasmSetDiag(diag, H2Diag_WASM_BACKEND_UNSUPPORTED_MIR, 0, 0);
        H2MirFindFirstDynamicResolutionInst(unit->mirProgram, &funcIndex, &pc, &inst);
        if (diag != NULL) {
            diag->detail = WasmDynamicResolutionDetail(unit, options, funcIndex, inst);
            if (diag->detail == NULL) {
                diag->detail = "MIR still needs dynamic resolution";
            }
            if (funcIndex < unit->mirProgram->funcLen && pc != UINT32_MAX) {
                const H2MirFunction* fn = &unit->mirProgram->funcs[funcIndex];
                if (fn->sourceRef < unit->mirProgram->sourceLen && inst != NULL) {
                    diag->start = inst->start;
                    diag->end = inst->end;
                }
            }
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
        sigs = (HOPWasmFnSig*)options->arenaGrow(
            options->allocatorCtx, unit->mirProgram->funcLen * (uint32_t)sizeof(*sigs), &allocSize);
        if (sigs == NULL || allocSize < unit->mirProgram->funcLen * sizeof(*sigs)) {
            if (sigs != NULL && options->arenaFree != NULL) {
                options->arenaFree(options->allocatorCtx, sigs, allocSize);
            }
            WasmSetDiag(diag, H2Diag_ARENA_OOM, 0, 0);
            return -1;
        }
    }
    if (WasmBuildReachableFunctionSet(unit, &foreign, options, &imports, diag) != 0) {
        goto fail;
    }
    if (WasmBuildFunctionSignatures(
            unit->mirProgram, options, imports.reachableFunctions, sigs, diag)
        != 0)
    {
        goto fail;
    }
    if (WasmAnalyzeImports(unit, &foreign, options, &imports, diag) != 0) {
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
            unit->mirProgram->funcLen + (imports.hasFunctionTable ? 1u : 0u)
                + (entry.hasWrapper ? 1u : 0u))
        != 0)
    {
        goto oom;
    }
    for (i = 0; i < unit->mirProgram->funcLen; i++) {
        const HOPWasmFnSig* sig = &sigs[i];
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
    if (imports.hasFunctionTable) {
        if (WasmAppendByte(&typeSec, 0x60u) != 0 || WasmAppendULEB(&typeSec, 7u) != 0
            || WasmAppendByte(&typeSec, 0x7fu) != 0 || WasmAppendByte(&typeSec, 0x7fu) != 0
            || WasmAppendByte(&typeSec, 0x7fu) != 0 || WasmAppendByte(&typeSec, 0x7fu) != 0
            || WasmAppendByte(&typeSec, 0x7fu) != 0 || WasmAppendByte(&typeSec, 0x7fu) != 0
            || WasmAppendByte(&typeSec, 0x7fu) != 0 || WasmAppendULEB(&typeSec, 1u) != 0
            || WasmAppendByte(&typeSec, 0x7fu) != 0)
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

    if (imports.importFuncCount != 0u || imports.importGlobalCount != 0u || importMemory) {
        uint32_t importDeclCount =
            imports.importFuncCount + imports.importGlobalCount + (importMemory ? 1u : 0u);
        if (WasmAppendULEB(&importSec, importDeclCount) != 0) {
            goto oom;
        }
        if (importMemory) {
            if (WasmAppendULEB(&importSec, 3u) != 0 || WasmAppendBytes(&importSec, "env", 3u) != 0
                || WasmAppendULEB(&importSec, 6u) != 0
                || WasmAppendBytes(&importSec, "memory", 6u) != 0
                || WasmAppendByte(&importSec, 0x02u) != 0 || WasmAppendByte(&importSec, 0x03u) != 0
                || WasmAppendULEB(&importSec, 512u) != 0 || WasmAppendULEB(&importSec, 65536u) != 0)
            {
                goto oom;
            }
        }
        for (i = 0; i < foreign.len; i++) {
            const H2ForeignLinkageEntry* entry = &foreign.entries[i];
            if (entry->kind == H2ForeignLinkage_WASM_IMPORT_FN) {
                if (imports.reachableFunctions != NULL
                    && imports.reachableFunctions[entry->functionIndex] == 0u)
                {
                    continue;
                }
                if (WasmAppendULEB(&importSec, entry->arg0.len) != 0
                    || WasmAppendBytes(&importSec, entry->arg0.bytes, entry->arg0.len) != 0
                    || WasmAppendULEB(&importSec, entry->arg1.len) != 0
                    || WasmAppendBytes(&importSec, entry->arg1.bytes, entry->arg1.len) != 0
                    || WasmAppendByte(&importSec, 0x00u) != 0
                    || WasmAppendULEB(&importSec, sigs[entry->functionIndex].typeIndex) != 0)
                {
                    goto oom;
                }
            }
        }
        for (i = 0; i < foreign.len; i++) {
            const H2ForeignLinkageEntry* entry = &foreign.entries[i];
            if (entry->kind == H2ForeignLinkage_WASM_IMPORT_CONST
                || entry->kind == H2ForeignLinkage_WASM_IMPORT_VAR)
            {
                if (imports.reachableFunctions != NULL
                    && imports.reachableFunctions[entry->functionIndex] == 0u)
                {
                    continue;
                }
                if (WasmAppendULEB(&importSec, entry->arg0.len) != 0
                    || WasmAppendBytes(&importSec, entry->arg0.bytes, entry->arg0.len) != 0
                    || WasmAppendULEB(&importSec, entry->arg1.len) != 0
                    || WasmAppendBytes(&importSec, entry->arg1.bytes, entry->arg1.len) != 0
                    || WasmAppendByte(&importSec, 0x03u) != 0
                    || WasmAppendByte(&importSec, 0x7fu) != 0
                    || WasmAppendByte(
                           &importSec,
                           entry->kind == H2ForeignLinkage_WASM_IMPORT_VAR ? 0x01u : 0x00u)
                           != 0)
                {
                    goto oom;
                }
            }
        }
        if (WasmAppendSection(&out, 2u, &importSec) != 0) {
            goto oom;
        }
    }

    if (WasmAppendULEB(
            &funcSec,
            imports.definedFuncCount + (imports.hasRootAllocThunk ? 1u : 0u)
                + (entry.hasWrapper ? 1u : 0u))
        != 0)
    {
        goto oom;
    }
    for (i = 0; i < unit->mirProgram->funcLen; i++) {
        if (imports.reachableFunctions != NULL && imports.reachableFunctions[i] == 0u) {
            continue;
        }
        if (imports.importedFunctions != NULL && imports.importedFunctions[i] != 0u) {
            continue;
        }
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

    if (!importMemory) {
        if (WasmAppendULEB(&memSec, 1u) != 0 || WasmAppendByte(&memSec, 0x00u) != 0
            || WasmAppendULEB(
                   &memSec, (imports.needsFrameGlobal && imports.needsHeapGlobal) ? 2u : 1u)
                   != 0)
        {
            goto oom;
        }
        if (WasmAppendSection(&out, 5u, &memSec) != 0) {
            goto oom;
        }
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
        const H2MirFunction* fn = &unit->mirProgram->funcs[i];
        if (imports.reachableFunctions != NULL && imports.reachableFunctions[i] == 0u) {
            continue;
        }
        if (WasmFunctionIsNamedMain(unit->mirProgram, fn)) {
            exportCount++;
        }
    }
    for (i = 0; i < foreign.len; i++) {
        if (foreign.entries[i].kind == H2ForeignLinkage_EXPORT_FN
            && (imports.reachableFunctions == NULL
                || imports.reachableFunctions[foreign.entries[i].functionIndex] != 0u))
        {
            exportCount++;
        }
    }
    if (WasmAppendULEB(&exportSec, exportCount) != 0) {
        goto oom;
    }
    if (!importMemory) {
        if (WasmAppendULEB(&exportSec, 6u) != 0 || WasmAppendBytes(&exportSec, "memory", 6u) != 0
            || WasmAppendByte(&exportSec, 0x02u) != 0 || WasmAppendULEB(&exportSec, 0u) != 0)
        {
            goto oom;
        }
    }
    for (i = 0; i < unit->mirProgram->funcLen; i++) {
        const H2MirFunction* fn = &unit->mirProgram->funcs[i];
        if (imports.reachableFunctions != NULL && imports.reachableFunctions[i] == 0u) {
            continue;
        }
        if (!WasmFunctionIsNamedMain(unit->mirProgram, fn)) {
            continue;
        }
        if (WasmAppendULEB(&exportSec, 8u) != 0 || WasmAppendBytes(&exportSec, "hop_main", 8u) != 0
            || WasmAppendByte(&exportSec, 0x00u) != 0
            || WasmAppendULEB(
                   &exportSec,
                   entry.hasWrapper && entry.mainFuncIndex == i
                       ? entry.wrapperFuncIndex
                       : WasmFunctionWasmIndex(&imports, i))
                   != 0)
        {
            goto oom;
        }
    }
    for (i = 0; i < foreign.len; i++) {
        const H2ForeignLinkageEntry* entryInfo = &foreign.entries[i];
        uint32_t                     wasmFuncIndex;
        if (entryInfo->kind != H2ForeignLinkage_EXPORT_FN) {
            continue;
        }
        if (imports.reachableFunctions != NULL
            && imports.reachableFunctions[entryInfo->functionIndex] == 0u)
        {
            continue;
        }
        wasmFuncIndex =
            entry.hasWrapper && entry.mainFuncIndex == entryInfo->functionIndex
                ? entry.wrapperFuncIndex
                : WasmFunctionWasmIndex(&imports, entryInfo->functionIndex);
        if (WasmAppendULEB(&exportSec, entryInfo->arg0.len) != 0
            || WasmAppendBytes(&exportSec, entryInfo->arg0.bytes, entryInfo->arg0.len) != 0
            || WasmAppendByte(&exportSec, 0x00u) != 0
            || WasmAppendULEB(&exportSec, wasmFuncIndex) != 0)
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
            if (WasmAppendULEB(&elemSec, WasmFunctionWasmIndex(&imports, i)) != 0) {
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
            imports.definedFuncCount + (imports.hasRootAllocThunk ? 1u : 0u)
                + (entry.hasWrapper ? 1u : 0u))
        != 0)
    {
        goto oom;
    }
    for (i = 0; i < unit->mirProgram->funcLen; i++) {
        HOPWasmBuf body = { .options = options };
        if (imports.reachableFunctions != NULL && imports.reachableFunctions[i] == 0u) {
            continue;
        }
        if (imports.importedFunctions != NULL && imports.importedFunctions[i] != 0u) {
            continue;
        }
        if (WasmEmitFunctionBody(unit->mirProgram, sigs, &imports, &strings, i, &body, diag) != 0) {
            if (diag != NULL && diag->code == 0) {
                WasmSetDiag(
                    diag,
                    H2Diag_WASM_BACKEND_UNSUPPORTED_MIR,
                    unit->mirProgram->funcs[i].nameStart,
                    unit->mirProgram->funcs[i].nameEnd);
                diag->detail = "failed emitting Wasm function body";
            }
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
        HOPWasmBuf body = { .options = options };
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
        HOPWasmBuf body = { .options = options };
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
            unit->mirProgram->constLen * sizeof(HOPWasmStringRef));
    }
    WasmFreeImportLayout(options, &imports);
    return 0;

oom:
    WasmSetDiag(diag, H2Diag_ARENA_OOM, 0, 0);
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
            unit->mirProgram->constLen * sizeof(HOPWasmStringRef));
    }
    WasmFreeImportLayout(options, &imports);
    return -1;
}

const H2CodegenBackend gHOPCodegenBackendWasm = {
    .name = "wasm",
    .emit = EmitWasmBackend,
};

H2_API_END
