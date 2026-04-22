#include "libhop-impl.h"
#include "mir.h"

HOP_API_BEGIN

typedef struct {
    const HOPAst* ast;
    HOPStrView    src;
    HOPMirInst*   v;
    uint32_t      len;
    uint32_t      cap;
    int           supported;
    HOPDiag* _Nullable diag;
} HOPMirBuilder;

static void HOPMirSetDiag(HOPDiag* _Nullable diag, HOPDiagCode code, uint32_t start, uint32_t end);
static int  HOPMirNameEqLiteral(HOPStrView src, uint32_t start, uint32_t end, const char* lit);
static int  HOPMirNameEqLiteralOrPkgBuiltin(
    HOPStrView src, uint32_t start, uint32_t end, const char* lit, const char* pkgPrefix);
static int HOPMirNameIsCompilerDiagBuiltin(HOPStrView src, uint32_t start, uint32_t end);
static int HOPMirNameIsLazyTypeBuiltin(HOPStrView src, uint32_t start, uint32_t end);
static int HOPMirCallUsesLazyBuiltinLowering(const HOPMirBuilder* b, int32_t callNode);
static int HOPMirBuiltinTypeSize(const HOPMirBuilder* b, int32_t typeNode, int64_t* outSize);
static int HOPMirInferExprSize(const HOPMirBuilder* b, int32_t exprNode, int64_t* outSize);

static void* _Nullable HOPMirArenaGrowArray(
    HOPArena* arena, void* _Nullable oldMem, size_t elemSize, uint32_t oldCap, uint32_t newCap) {
    void* newMem;
    if (arena == NULL || elemSize == 0 || newCap < oldCap) {
        return NULL;
    }
    newMem = HOPArenaAlloc(arena, (uint32_t)(elemSize * newCap), (uint32_t)_Alignof(uint64_t));
    if (newMem == NULL) {
        return NULL;
    }
    if (oldMem != NULL && oldCap > 0) {
        memcpy(newMem, oldMem, elemSize * oldCap);
    }
    return newMem;
}

static int HOPMirProgramBuilderEnsureCap(
    HOPArena* arena,
    void**    mem,
    uint32_t  elemSize,
    uint32_t  needLen,
    uint32_t* cap,
    uint32_t  align) {
    uint32_t newCap;
    void*    newMem;
    (void)align;
    if (arena == NULL || mem == NULL || cap == NULL || elemSize == 0) {
        return -1;
    }
    if (needLen <= *cap) {
        return 0;
    }
    newCap = *cap == 0 ? 8u : *cap;
    while (newCap < needLen) {
        if (newCap > UINT32_MAX / 2u) {
            newCap = needLen;
            break;
        }
        newCap *= 2u;
    }
    newMem = HOPMirArenaGrowArray(arena, *mem, elemSize, *cap, newCap);
    if (newMem == NULL) {
        return -1;
    }
    *mem = newMem;
    *cap = newCap;
    return 0;
}

static int HOPMirNameEqLiteral(HOPStrView src, uint32_t start, uint32_t end, const char* lit) {
    size_t litLen = 0;
    if (lit == NULL || end < start || end > src.len) {
        return 0;
    }
    while (lit[litLen] != '\0') {
        litLen++;
    }
    return (size_t)(end - start) == litLen && memcmp(src.ptr + start, lit, litLen) == 0;
}

static int HOPMirNameEqLiteralOrPkgBuiltin(
    HOPStrView src, uint32_t start, uint32_t end, const char* lit, const char* pkgPrefix) {
    size_t litLen = 0;
    size_t pkgLen = 0;
    size_t i;
    if (HOPMirNameEqLiteral(src, start, end, lit)) {
        return 1;
    }
    if (lit == NULL || pkgPrefix == NULL || end < start || end > src.len) {
        return 0;
    }
    while (lit[litLen] != '\0') {
        litLen++;
    }
    while (pkgPrefix[pkgLen] != '\0') {
        pkgLen++;
    }
    if ((size_t)(end - start) != pkgLen + 2u + litLen) {
        return 0;
    }
    for (i = 0; i < pkgLen; i++) {
        if (src.ptr[start + i] != pkgPrefix[i]) {
            return 0;
        }
    }
    if (src.ptr[start + pkgLen] != '_' || src.ptr[start + pkgLen + 1u] != '_') {
        return 0;
    }
    return memcmp(src.ptr + start + pkgLen + 2u, lit, litLen) == 0;
}

static int HOPMirNameIsCompilerDiagBuiltin(HOPStrView src, uint32_t start, uint32_t end) {
    return HOPMirNameEqLiteralOrPkgBuiltin(src, start, end, "error", "compiler")
        || HOPMirNameEqLiteralOrPkgBuiltin(src, start, end, "error_at", "compiler")
        || HOPMirNameEqLiteralOrPkgBuiltin(src, start, end, "warn", "compiler")
        || HOPMirNameEqLiteralOrPkgBuiltin(src, start, end, "warn_at", "compiler");
}

static int HOPMirNameIsLazyTypeBuiltin(HOPStrView src, uint32_t start, uint32_t end) {
    return HOPMirNameEqLiteral(src, start, end, "typeof")
        || HOPMirNameEqLiteral(src, start, end, "kind")
        || HOPMirNameEqLiteral(src, start, end, "base")
        || HOPMirNameEqLiteral(src, start, end, "is_alias")
        || HOPMirNameEqLiteral(src, start, end, "type_name")
        || HOPMirNameEqLiteral(src, start, end, "ptr")
        || HOPMirNameEqLiteral(src, start, end, "slice")
        || HOPMirNameEqLiteral(src, start, end, "array");
}

static int HOPMirCallUsesLazyBuiltinLowering(const HOPMirBuilder* b, int32_t callNode) {
    const HOPAstNode* call;
    const HOPAstNode* callee;
    int32_t           calleeNode;
    int32_t           recvNode;
    if (b == NULL || b->ast == NULL || callNode < 0 || (uint32_t)callNode >= b->ast->len) {
        return 0;
    }
    call = &b->ast->nodes[callNode];
    calleeNode = call->firstChild;
    callee = calleeNode >= 0 ? &b->ast->nodes[calleeNode] : NULL;
    if (call->kind != HOPAst_CALL || callee == NULL) {
        return 0;
    }
    if (callee->kind == HOPAst_IDENT) {
        return HOPMirNameEqLiteralOrPkgBuiltin(
                   b->src, callee->dataStart, callee->dataEnd, "source_location_of", "builtin")
            || HOPMirNameIsCompilerDiagBuiltin(b->src, callee->dataStart, callee->dataEnd)
            || HOPMirNameIsLazyTypeBuiltin(b->src, callee->dataStart, callee->dataEnd);
    }
    if (callee->kind != HOPAst_FIELD_EXPR) {
        return 0;
    }
    recvNode = callee->firstChild;
    if (recvNode < 0 || (uint32_t)recvNode >= b->ast->len
        || b->ast->nodes[recvNode].kind != HOPAst_IDENT)
    {
        return 0;
    }
    if (HOPMirNameEqLiteral(
            b->src, b->ast->nodes[recvNode].dataStart, b->ast->nodes[recvNode].dataEnd, "builtin")
        && HOPMirNameEqLiteral(b->src, callee->dataStart, callee->dataEnd, "source_location_of"))
    {
        return 1;
    }
    if (HOPMirNameEqLiteral(
            b->src, b->ast->nodes[recvNode].dataStart, b->ast->nodes[recvNode].dataEnd, "reflect")
        && (HOPMirNameEqLiteral(b->src, callee->dataStart, callee->dataEnd, "kind")
            || HOPMirNameEqLiteral(b->src, callee->dataStart, callee->dataEnd, "base")
            || HOPMirNameEqLiteral(b->src, callee->dataStart, callee->dataEnd, "is_alias")
            || HOPMirNameEqLiteral(b->src, callee->dataStart, callee->dataEnd, "type_name")))
    {
        return 1;
    }
    if (!HOPMirNameEqLiteral(
            b->src, b->ast->nodes[recvNode].dataStart, b->ast->nodes[recvNode].dataEnd, "compiler"))
    {
        return 0;
    }
    return HOPMirNameIsCompilerDiagBuiltin(b->src, callee->dataStart, callee->dataEnd);
}

void HOPMirProgramBuilderInit(HOPMirProgramBuilder* b, HOPArena* arena) {
    if (b == NULL) {
        return;
    }
    memset(b, 0, sizeof(*b));
    b->arena = arena;
    b->openFunc = UINT32_MAX;
}

static int HOPMirProgramBuilderAppendElem(
    HOPArena*   arena,
    void**      mem,
    const void* value,
    uint32_t    elemSize,
    uint32_t*   len,
    uint32_t*   cap,
    uint32_t* _Nullable outIndex) {
    uint8_t* dst;
    if (arena == NULL || mem == NULL || value == NULL || len == NULL || cap == NULL) {
        return -1;
    }
    if (HOPMirProgramBuilderEnsureCap(arena, mem, elemSize, *len + 1u, cap, 1u) != 0) {
        return -1;
    }
    dst = (uint8_t*)(*mem) + (size_t)(*len) * elemSize;
    memcpy(dst, value, elemSize);
    if (outIndex != NULL) {
        *outIndex = *len;
    }
    (*len)++;
    return 0;
}

int HOPMirProgramBuilderAddConst(
    HOPMirProgramBuilder* b, const HOPMirConst* value, uint32_t* _Nullable outIndex) {
    if (b == NULL) {
        return -1;
    }
    return HOPMirProgramBuilderAppendElem(
        b->arena,
        (void**)&b->consts,
        value,
        (uint32_t)sizeof(*value),
        &b->constLen,
        &b->constCap,
        outIndex);
}

int HOPMirProgramBuilderAddSource(
    HOPMirProgramBuilder* b, const HOPMirSourceRef* value, uint32_t* _Nullable outIndex) {
    uint32_t i;
    if (b == NULL || value == NULL) {
        return -1;
    }
    for (i = 0; i < b->sourceLen; i++) {
        if (b->sources[i].src.ptr == value->src.ptr && b->sources[i].src.len == value->src.len) {
            if (outIndex != NULL) {
                *outIndex = i;
            }
            return 0;
        }
    }
    return HOPMirProgramBuilderAppendElem(
        b->arena,
        (void**)&b->sources,
        value,
        (uint32_t)sizeof(*value),
        &b->sourceLen,
        &b->sourceCap,
        outIndex);
}

int HOPMirProgramBuilderAddLocal(
    HOPMirProgramBuilder* b, const HOPMirLocal* value, uint32_t* _Nullable outSlot) {
    uint32_t slot = 0;
    if (b == NULL || value == NULL || !b->hasOpenFunc) {
        return -1;
    }
    slot = b->funcs[b->openFunc].localCount;
    if (HOPMirProgramBuilderAppendElem(
            b->arena,
            (void**)&b->locals,
            value,
            (uint32_t)sizeof(*value),
            &b->localLen,
            &b->localCap,
            NULL)
        != 0)
    {
        return -1;
    }
    b->funcs[b->openFunc].localCount++;
    if (outSlot != NULL) {
        *outSlot = slot;
    }
    return 0;
}

int HOPMirProgramBuilderAddField(
    HOPMirProgramBuilder* b, const HOPMirField* value, uint32_t* _Nullable outIndex) {
    if (b == NULL) {
        return -1;
    }
    return HOPMirProgramBuilderAppendElem(
        b->arena,
        (void**)&b->fields,
        value,
        (uint32_t)sizeof(*value),
        &b->fieldLen,
        &b->fieldCap,
        outIndex);
}

int HOPMirProgramBuilderAddType(
    HOPMirProgramBuilder* b, const HOPMirTypeRef* value, uint32_t* _Nullable outIndex) {
    if (b == NULL) {
        return -1;
    }
    return HOPMirProgramBuilderAppendElem(
        b->arena,
        (void**)&b->types,
        value,
        (uint32_t)sizeof(*value),
        &b->typeLen,
        &b->typeCap,
        outIndex);
}

int HOPMirProgramBuilderAddHost(
    HOPMirProgramBuilder* b, const HOPMirHostRef* value, uint32_t* _Nullable outIndex) {
    if (b == NULL) {
        return -1;
    }
    return HOPMirProgramBuilderAppendElem(
        b->arena,
        (void**)&b->hosts,
        value,
        (uint32_t)sizeof(*value),
        &b->hostLen,
        &b->hostCap,
        outIndex);
}

int HOPMirProgramBuilderAddSymbol(
    HOPMirProgramBuilder* b, const HOPMirSymbolRef* value, uint32_t* _Nullable outIndex) {
    if (b == NULL) {
        return -1;
    }
    return HOPMirProgramBuilderAppendElem(
        b->arena,
        (void**)&b->symbols,
        value,
        (uint32_t)sizeof(*value),
        &b->symbolLen,
        &b->symbolCap,
        outIndex);
}

int HOPMirProgramBuilderBeginFunction(
    HOPMirProgramBuilder* b, const HOPMirFunction* value, uint32_t* _Nullable outIndex) {
    HOPMirFunction fn;
    uint32_t       index = 0;
    if (b == NULL || value == NULL || b->hasOpenFunc) {
        return -1;
    }
    fn = *value;
    fn.instStart = b->instLen;
    fn.instLen = 0;
    fn.localStart = b->localLen;
    fn.localCount = 0;
    if (HOPMirProgramBuilderAppendElem(
            b->arena,
            (void**)&b->funcs,
            &fn,
            (uint32_t)sizeof(fn),
            &b->funcLen,
            &b->funcCap,
            &index)
        != 0)
    {
        return -1;
    }
    b->openFunc = index;
    b->hasOpenFunc = 1u;
    if (outIndex != NULL) {
        *outIndex = index;
    }
    return 0;
}

int HOPMirProgramBuilderAppendInst(HOPMirProgramBuilder* b, const HOPMirInst* value) {
    if (b == NULL || value == NULL || !b->hasOpenFunc) {
        return -1;
    }
    if (HOPMirProgramBuilderAppendElem(
            b->arena,
            (void**)&b->insts,
            value,
            (uint32_t)sizeof(*value),
            &b->instLen,
            &b->instCap,
            NULL)
        != 0)
    {
        return -1;
    }
    b->funcs[b->openFunc].instLen = b->instLen - b->funcs[b->openFunc].instStart;
    return 0;
}

int HOPMirProgramBuilderInsertInst(
    HOPMirProgramBuilder* b,
    uint32_t              functionIndex,
    uint32_t              instIndexInFunction,
    const HOPMirInst*     value) {
    uint32_t absIndex;
    uint32_t funcIndex;
    if (b == NULL || value == NULL || b->hasOpenFunc || functionIndex >= b->funcLen) {
        return -1;
    }
    if (instIndexInFunction > b->funcs[functionIndex].instLen) {
        return -1;
    }
    if (HOPMirProgramBuilderEnsureCap(
            b->arena, (void**)&b->insts, (uint32_t)sizeof(*value), b->instLen + 1u, &b->instCap, 1u)
        != 0)
    {
        return -1;
    }
    absIndex = b->funcs[functionIndex].instStart + instIndexInFunction;
    if (absIndex < b->instLen) {
        memmove(
            &b->insts[absIndex + 1u],
            &b->insts[absIndex],
            sizeof(*value) * (size_t)(b->instLen - absIndex));
    }
    b->insts[absIndex] = *value;
    b->instLen++;
    b->funcs[functionIndex].instLen++;
    for (funcIndex = functionIndex + 1u; funcIndex < b->funcLen; funcIndex++) {
        if (b->funcs[funcIndex].instStart >= absIndex) {
            b->funcs[funcIndex].instStart++;
        }
    }
    return 0;
}

int HOPMirProgramBuilderEndFunction(HOPMirProgramBuilder* b) {
    if (b == NULL || !b->hasOpenFunc) {
        return -1;
    }
    b->funcs[b->openFunc].instLen = b->instLen - b->funcs[b->openFunc].instStart;
    b->openFunc = UINT32_MAX;
    b->hasOpenFunc = 0u;
    return 0;
}

void HOPMirProgramBuilderFinish(const HOPMirProgramBuilder* b, HOPMirProgram* outProgram) {
    if (b == NULL || outProgram == NULL) {
        return;
    }
    outProgram->insts = b->insts;
    outProgram->instLen = b->instLen;
    outProgram->consts = b->consts;
    outProgram->constLen = b->constLen;
    outProgram->sources = b->sources;
    outProgram->sourceLen = b->sourceLen;
    outProgram->funcs = b->funcs;
    outProgram->funcLen = b->funcLen;
    outProgram->locals = b->locals;
    outProgram->localLen = b->localLen;
    outProgram->fields = b->fields;
    outProgram->fieldLen = b->fieldLen;
    outProgram->types = b->types;
    outProgram->typeLen = b->typeLen;
    outProgram->hosts = b->hosts;
    outProgram->hostLen = b->hostLen;
    outProgram->symbols = b->symbols;
    outProgram->symbolLen = b->symbolLen;
}

int HOPMirValidateProgram(const HOPMirProgram* program, HOPDiag* _Nullable diag) {
    uint32_t funcIndex;
    if (diag != NULL) {
        *diag = (HOPDiag){ 0 };
    }
    if (program == NULL) {
        HOPMirSetDiag(diag, HOPDiag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const HOPMirFunction* fn = &program->funcs[funcIndex];
        uint32_t              instIndex;
        if (fn->instStart > program->instLen || fn->instLen > program->instLen - fn->instStart) {
            HOPMirSetDiag(diag, HOPDiag_UNEXPECTED_TOKEN, fn->nameStart, fn->nameEnd);
            return -1;
        }
        if (fn->localStart > program->localLen
            || fn->localCount > program->localLen - fn->localStart
            || fn->localCount < fn->paramCount)
        {
            HOPMirSetDiag(diag, HOPDiag_UNEXPECTED_TOKEN, fn->nameStart, fn->nameEnd);
            return -1;
        }
        if (fn->sourceRef >= program->sourceLen) {
            HOPMirSetDiag(diag, HOPDiag_UNEXPECTED_TOKEN, fn->nameStart, fn->nameEnd);
            return -1;
        }
        if (fn->localCount != 0u) {
            uint32_t localIndex;
            for (localIndex = 0; localIndex < fn->localCount; localIndex++) {
                const HOPMirLocal* local = &program->locals[fn->localStart + localIndex];
                if (local->typeRef != UINT32_MAX && local->typeRef >= program->typeLen) {
                    HOPMirSetDiag(diag, HOPDiag_UNEXPECTED_TOKEN, fn->nameStart, fn->nameEnd);
                    return -1;
                }
            }
        }
        for (instIndex = 0; instIndex < fn->instLen; instIndex++) {
            const HOPMirInst* ins = &program->insts[fn->instStart + instIndex];
            switch (ins->op) {
                case HOPMirOp_PUSH_CONST:
                    if (ins->aux >= program->constLen) {
                        HOPMirSetDiag(diag, HOPDiag_UNEXPECTED_TOKEN, ins->start, ins->end);
                        return -1;
                    }
                    break;
                case HOPMirOp_LOAD_IDENT:
                case HOPMirOp_STORE_IDENT:
                case HOPMirOp_CALL:
                    if (program->symbolLen == 0 || ins->aux >= program->symbolLen) {
                        HOPMirSetDiag(diag, HOPDiag_UNEXPECTED_TOKEN, ins->start, ins->end);
                        return -1;
                    }
                    break;
                case HOPMirOp_CAST:
                case HOPMirOp_COERCE:
                    if (program->typeLen == 0 || ins->aux >= program->typeLen) {
                        HOPMirSetDiag(diag, HOPDiag_UNEXPECTED_TOKEN, ins->start, ins->end);
                        return -1;
                    }
                    break;
                case HOPMirOp_CALL_HOST:
                    if (program->hostLen == 0 || ins->aux >= program->hostLen) {
                        HOPMirSetDiag(diag, HOPDiag_UNEXPECTED_TOKEN, ins->start, ins->end);
                        return -1;
                    }
                    break;
                case HOPMirOp_AGG_GET:
                case HOPMirOp_AGG_ADDR:
                    if (program->fieldLen == 0 || ins->aux >= program->fieldLen) {
                        HOPMirSetDiag(diag, HOPDiag_UNEXPECTED_TOKEN, ins->start, ins->end);
                        return -1;
                    }
                    break;
                case HOPMirOp_LOCAL_ZERO:
                case HOPMirOp_LOCAL_LOAD:
                case HOPMirOp_LOCAL_STORE:
                case HOPMirOp_LOCAL_ADDR:
                    if (ins->aux >= fn->localCount) {
                        HOPMirSetDiag(diag, HOPDiag_UNEXPECTED_TOKEN, ins->start, ins->end);
                        return -1;
                    }
                    break;
                case HOPMirOp_JUMP:
                case HOPMirOp_JUMP_IF_FALSE:
                    if (ins->aux >= fn->instLen) {
                        HOPMirSetDiag(diag, HOPDiag_UNEXPECTED_TOKEN, ins->start, ins->end);
                        return -1;
                    }
                    break;
                case HOPMirOp_CALL_FN:
                    if (ins->aux >= program->funcLen) {
                        HOPMirSetDiag(diag, HOPDiag_UNEXPECTED_TOKEN, ins->start, ins->end);
                        return -1;
                    }
                    break;
                default: break;
            }
        }
    }
    return 0;
}

int HOPMirProgramNeedsDynamicResolution(const HOPMirProgram* program) {
    uint32_t i;
    if (program == NULL) {
        return 1;
    }
    for (i = 0; i < program->instLen; i++) {
        if (program->insts[i].op == HOPMirOp_LOAD_IDENT || program->insts[i].op == HOPMirOp_CALL) {
            return 1;
        }
    }
    return 0;
}

int HOPMirFindFirstDynamicResolutionInst(
    const HOPMirProgram* program,
    uint32_t* _Nullable outFunctionIndex,
    uint32_t* _Nullable outPc,
    const HOPMirInst** _Nullable outInst) {
    uint32_t funcIndex;
    if (outFunctionIndex != NULL) {
        *outFunctionIndex = UINT32_MAX;
    }
    if (outPc != NULL) {
        *outPc = UINT32_MAX;
    }
    if (outInst != NULL) {
        *outInst = NULL;
    }
    if (program == NULL) {
        return 0;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const HOPMirFunction* fn = &program->funcs[funcIndex];
        uint32_t              pc;
        for (pc = 0; pc < fn->instLen; pc++) {
            const HOPMirInst* inst = &program->insts[fn->instStart + pc];
            if (inst->op != HOPMirOp_LOAD_IDENT && inst->op != HOPMirOp_CALL) {
                continue;
            }
            if (outFunctionIndex != NULL) {
                *outFunctionIndex = funcIndex;
            }
            if (outPc != NULL) {
                *outPc = pc;
            }
            if (outInst != NULL) {
                *outInst = inst;
            }
            return 1;
        }
    }
    return 0;
}

static void HOPMirSetDiag(HOPDiag* _Nullable diag, HOPDiagCode code, uint32_t start, uint32_t end) {
    if (diag == NULL) {
        return;
    }
    diag->code = code;
    diag->type = HOPDiagTypeOfCode(code);
    diag->start = start;
    diag->end = end;
    diag->argStart = 0;
    diag->argEnd = 0;
}

static int HOPMirIsAllowedUnaryToken(HOPTokenKind tok) {
    return tok == HOPTok_ADD || tok == HOPTok_SUB || tok == HOPTok_NOT;
}

static int HOPMirIsAllowedBinaryToken(HOPTokenKind tok) {
    switch (tok) {
        case HOPTok_ADD:
        case HOPTok_SUB:
        case HOPTok_MUL:
        case HOPTok_DIV:
        case HOPTok_MOD:
        case HOPTok_AND:
        case HOPTok_OR:
        case HOPTok_XOR:
        case HOPTok_LSHIFT:
        case HOPTok_RSHIFT:
        case HOPTok_EQ:
        case HOPTok_NEQ:
        case HOPTok_LT:
        case HOPTok_GT:
        case HOPTok_LTE:
        case HOPTok_GTE:
        case HOPTok_LOGICAL_AND:
        case HOPTok_LOGICAL_OR:  return 1;
        default:                 return 0;
    }
}

static int HOPMirEmitInstEx(
    HOPMirBuilder* b, HOPMirOp op, HOPTokenKind tok, uint32_t aux, uint32_t start, uint32_t end) {
    if (!b->supported) {
        return 0;
    }
    if (b->len >= b->cap) {
        HOPMirSetDiag(b->diag, HOPDiag_ARENA_OOM, start, end);
        return -1;
    }
    b->v[b->len].op = op;
    b->v[b->len].tok = (uint16_t)tok;
    b->v[b->len]._reserved = 0;
    b->v[b->len].aux = aux;
    b->v[b->len].start = start;
    b->v[b->len].end = end;
    b->len++;
    return 0;
}

static int HOPMirEmitInst(
    HOPMirBuilder* b, HOPMirOp op, HOPTokenKind tok, uint32_t start, uint32_t end) {
    return HOPMirEmitInstEx(b, op, tok, 0, start, end);
}

static int HOPMirTypeNameEqCStr(HOPMirBuilder* b, const HOPAstNode* n, const char* s) {
    uint32_t len = 0;
    if (b == NULL || n == NULL || s == NULL || n->dataEnd < n->dataStart || b->src.ptr == NULL) {
        return 0;
    }
    while (s[len] != '\0') {
        len++;
    }
    if (len != n->dataEnd - n->dataStart || n->dataEnd > b->src.len) {
        return 0;
    }
    return memcmp(b->src.ptr + n->dataStart, s, len) == 0;
}

static HOPMirCastTarget HOPMirClassifyCastTarget(HOPMirBuilder* b, int32_t typeNode) {
    const HOPAstNode* n;
    int32_t           childNode;
    if (typeNode < 0 || (uint32_t)typeNode >= b->ast->len) {
        return HOPMirCastTarget_INVALID;
    }
    n = &b->ast->nodes[typeNode];
    if ((n->kind == HOPAst_TYPE_REF || n->kind == HOPAst_TYPE_PTR) && n->firstChild >= 0
        && (uint32_t)n->firstChild < b->ast->len)
    {
        childNode = n->firstChild;
        if (b->ast->nodes[childNode].kind == HOPAst_TYPE_NAME
            && HOPMirTypeNameEqCStr(b, &b->ast->nodes[childNode], "str"))
        {
            return HOPMirCastTarget_STR_VIEW;
        }
        return HOPMirCastTarget_PTR_LIKE;
    }
    if (n->kind == HOPAst_TYPE_MUTREF) {
        return HOPMirCastTarget_PTR_LIKE;
    }
    if (n->kind != HOPAst_TYPE_NAME) {
        return HOPMirCastTarget_INVALID;
    }
    if (HOPMirTypeNameEqCStr(b, n, "rawptr")) {
        return HOPMirCastTarget_PTR_LIKE;
    }
    if (HOPMirTypeNameEqCStr(b, n, "bool")) {
        return HOPMirCastTarget_BOOL;
    }
    if (HOPMirTypeNameEqCStr(b, n, "f32") || HOPMirTypeNameEqCStr(b, n, "f64")) {
        return HOPMirCastTarget_FLOAT;
    }
    if (HOPMirTypeNameEqCStr(b, n, "u8") || HOPMirTypeNameEqCStr(b, n, "u16")
        || HOPMirTypeNameEqCStr(b, n, "u32") || HOPMirTypeNameEqCStr(b, n, "u64")
        || HOPMirTypeNameEqCStr(b, n, "uint") || HOPMirTypeNameEqCStr(b, n, "i8")
        || HOPMirTypeNameEqCStr(b, n, "i16") || HOPMirTypeNameEqCStr(b, n, "i32")
        || HOPMirTypeNameEqCStr(b, n, "i64") || HOPMirTypeNameEqCStr(b, n, "int"))
    {
        return HOPMirCastTarget_INT;
    }
    return HOPMirCastTarget_INVALID;
}

static int HOPMirBuiltinTypeSize(const HOPMirBuilder* b, int32_t typeNode, int64_t* outSize) {
    const HOPAstNode* type;
    uint32_t          len;
    const char*       name;
    if (b == NULL || outSize == NULL || typeNode < 0 || (uint32_t)typeNode >= b->ast->len) {
        return 0;
    }
    type = &b->ast->nodes[typeNode];
    switch (type->kind) {
        case HOPAst_TYPE_PTR:
        case HOPAst_TYPE_REF:
        case HOPAst_TYPE_MUTREF:
        case HOPAst_TYPE_FN:       *outSize = (int64_t)sizeof(void*); return 1;
        case HOPAst_TYPE_SLICE:
        case HOPAst_TYPE_MUTSLICE: *outSize = (int64_t)(sizeof(void*) * 2u); return 1;
        case HOPAst_TYPE_NAME:     break;
        default:                   return 0;
    }
    len = type->dataEnd >= type->dataStart ? type->dataEnd - type->dataStart : 0u;
    name = b->src.ptr + type->dataStart;
    if ((len == 2u && memcmp(name, "u8", 2) == 0) || (len == 2u && memcmp(name, "i8", 2) == 0)
        || (len == 4u && memcmp(name, "bool", 4) == 0))
    {
        *outSize = 1;
        return 1;
    }
    if ((len == 3u && memcmp(name, "u16", 3) == 0) || (len == 3u && memcmp(name, "i16", 3) == 0)) {
        *outSize = 2;
        return 1;
    }
    if ((len == 3u && memcmp(name, "u32", 3) == 0) || (len == 3u && memcmp(name, "i32", 3) == 0)
        || (len == 3u && memcmp(name, "f32", 3) == 0)
        || (len == 4u && memcmp(name, "rune", 4) == 0))
    {
        *outSize = 4;
        return 1;
    }
    if ((len == 3u && memcmp(name, "u64", 3) == 0) || (len == 3u && memcmp(name, "i64", 3) == 0)
        || (len == 3u && memcmp(name, "f64", 3) == 0))
    {
        *outSize = 8;
        return 1;
    }
    if ((len == 3u && memcmp(name, "int", 3) == 0) || (len == 4u && memcmp(name, "uint", 4) == 0)
        || (len == 5u && memcmp(name, "usize", 5) == 0)
        || (len == 5u && memcmp(name, "isize", 5) == 0))
    {
        *outSize = (int64_t)sizeof(uintptr_t);
        return 1;
    }
    if (len == 6u && memcmp(name, "rawptr", 6) == 0) {
        *outSize = (int64_t)sizeof(void*);
        return 1;
    }
    if (len == 3u && memcmp(name, "str", 3) == 0) {
        *outSize = (int64_t)(sizeof(void*) * 2u);
        return 1;
    }
    if (len == 4u && memcmp(name, "type", 4) == 0) {
        *outSize = (int64_t)sizeof(void*);
        return 1;
    }
    return 0;
}

static int HOPMirInferExprSize(const HOPMirBuilder* b, int32_t exprNode, int64_t* outSize) {
    const HOPAstNode* expr;
    int32_t           valueNode;
    int32_t           typeNode;
    if (b == NULL || outSize == NULL || exprNode < 0 || (uint32_t)exprNode >= b->ast->len) {
        return 0;
    }
    expr = &b->ast->nodes[exprNode];
    switch (expr->kind) {
        case HOPAst_INT:    *outSize = (int64_t)sizeof(uintptr_t); return 1;
        case HOPAst_FLOAT:  *outSize = 8; return 1;
        case HOPAst_BOOL:   *outSize = 1; return 1;
        case HOPAst_STRING: *outSize = (int64_t)(sizeof(void*) * 2u); return 1;
        case HOPAst_CAST:
            valueNode = expr->firstChild;
            typeNode = valueNode >= 0 ? b->ast->nodes[valueNode].nextSibling : -1;
            if (valueNode < 0 || typeNode < 0 || b->ast->nodes[typeNode].nextSibling >= 0) {
                return 0;
            }
            return HOPMirBuiltinTypeSize(b, typeNode, outSize);
        default: return 0;
    }
}

static int HOPMirBuildExprNode(HOPMirBuilder* b, int32_t nodeId) {
    const HOPAstNode* n;
    if (!b->supported) {
        return 0;
    }
    if (nodeId < 0 || (uint32_t)nodeId >= b->ast->len) {
        HOPMirSetDiag(b->diag, HOPDiag_EXPECTED_EXPR, 0, 0);
        return -1;
    }
    n = &b->ast->nodes[nodeId];
    switch (n->kind) {
        case HOPAst_INT:
            return HOPMirEmitInst(b, HOPMirOp_PUSH_INT, HOPTok_INT, n->dataStart, n->dataEnd);
        case HOPAst_RUNE:
            return HOPMirEmitInst(b, HOPMirOp_PUSH_INT, HOPTok_RUNE, n->dataStart, n->dataEnd);
        case HOPAst_FLOAT:
            return HOPMirEmitInst(b, HOPMirOp_PUSH_FLOAT, HOPTok_FLOAT, n->dataStart, n->dataEnd);
        case HOPAst_BOOL:
            return HOPMirEmitInst(b, HOPMirOp_PUSH_BOOL, HOPTok_TRUE, n->dataStart, n->dataEnd);
        case HOPAst_STRING:
            return HOPMirEmitInst(b, HOPMirOp_PUSH_STRING, HOPTok_STRING, n->dataStart, n->dataEnd);
        case HOPAst_NULL:
            return HOPMirEmitInst(b, HOPMirOp_PUSH_NULL, HOPTok_NULL, n->start, n->end);
        case HOPAst_IDENT:
            return HOPMirEmitInstEx(
                b, HOPMirOp_LOAD_IDENT, HOPTok_IDENT, (uint32_t)nodeId, n->dataStart, n->dataEnd);
        case HOPAst_SIZEOF: {
            int32_t innerNode = n->firstChild;
            int64_t sizeValue = 0;
            if (innerNode < 0 || (uint32_t)innerNode >= b->ast->len) {
                HOPMirSetDiag(b->diag, HOPDiag_EXPECTED_EXPR, n->start, n->end);
                return -1;
            }
            if (n->flags == 1u) {
                if (!HOPMirBuiltinTypeSize(b, innerNode, &sizeValue)) {
                    b->supported = 0;
                    return 0;
                }
            } else if (!HOPMirInferExprSize(b, innerNode, &sizeValue)) {
                b->supported = 0;
                return 0;
            }
            return HOPMirEmitInstEx(
                b, HOPMirOp_PUSH_INT, HOPTok_INVALID, (uint32_t)sizeValue, n->start, n->end);
        }
        case HOPAst_TUPLE_EXPR: {
            int32_t  child = n->firstChild;
            uint32_t elemCount = 0;
            while (child >= 0) {
                if (HOPMirBuildExprNode(b, child) != 0) {
                    return -1;
                }
                if (elemCount == UINT16_MAX) {
                    b->supported = 0;
                    return 0;
                }
                elemCount++;
                child = b->ast->nodes[child].nextSibling;
            }
            if (elemCount == 0u) {
                b->supported = 0;
                return 0;
            }
            return HOPMirEmitInstEx(
                b,
                HOPMirOp_TUPLE_MAKE,
                (HOPTokenKind)elemCount,
                (uint32_t)nodeId,
                n->start,
                n->end);
        }
        case HOPAst_CALL: {
            int32_t  callee = b->ast->nodes[nodeId].firstChild;
            int32_t  arg;
            uint32_t argc = 0;
            uint32_t callFlags = 0;
            uint16_t callTokFlags = 0;
            uint32_t callStart;
            uint32_t callEnd;
            int      isBuiltinLen = 0;
            int      isBuiltinCStr = 0;
            int      isLazyBuiltin = HOPMirCallUsesLazyBuiltinLowering(b, nodeId);
            if (callee < 0 || (uint32_t)callee >= b->ast->len) {
                b->supported = 0;
                return 0;
            }
            if (b->ast->nodes[callee].kind == HOPAst_IDENT) {
                callStart = b->ast->nodes[callee].dataStart;
                callEnd = b->ast->nodes[callee].dataEnd;
                isBuiltinLen =
                    callEnd == callStart + 3u && memcmp(b->src.ptr + callStart, "len", 3) == 0;
                isBuiltinCStr =
                    callEnd == callStart + 4u && memcmp(b->src.ptr + callStart, "cstr", 4) == 0;
            } else if (b->ast->nodes[callee].kind == HOPAst_FIELD_EXPR) {
                int32_t baseNode = b->ast->nodes[callee].firstChild;
                if (baseNode < 0) {
                    b->supported = 0;
                    return 0;
                }
                if (!isLazyBuiltin) {
                    if (HOPMirBuildExprNode(b, baseNode) != 0) {
                        return -1;
                    }
                    argc = 1;
                    callFlags = HOPMirSymbolFlag_CALL_RECEIVER_ARG0;
                }
                callStart = b->ast->nodes[callee].dataStart;
                callEnd = b->ast->nodes[callee].dataEnd;
                isBuiltinCStr =
                    callEnd == callStart + 4u && memcmp(b->src.ptr + callStart, "cstr", 4) == 0;
            } else {
                b->supported = 0;
                return 0;
            }
            arg = b->ast->nodes[callee].nextSibling;
            if (!isLazyBuiltin) {
                while (arg >= 0) {
                    int32_t exprNode = arg;
                    int     isSpread = 0;
                    if (b->ast->nodes[arg].kind == HOPAst_CALL_ARG) {
                        exprNode = b->ast->nodes[arg].firstChild;
                        if (exprNode < 0) {
                            b->supported = 0;
                            return 0;
                        }
                        isSpread = (b->ast->nodes[arg].flags & HOPAstFlag_CALL_ARG_SPREAD) != 0;
                    }
                    if (isSpread) {
                        if (b->ast->nodes[arg].nextSibling >= 0
                            || (callTokFlags & HOPMirCallArgFlag_SPREAD_LAST) != 0u)
                        {
                            b->supported = 0;
                            return 0;
                        }
                        callTokFlags |= HOPMirCallArgFlag_SPREAD_LAST;
                    }
                    if (HOPMirBuildExprNode(b, exprNode) != 0) {
                        return -1;
                    }
                    if (argc == UINT16_MAX) {
                        b->supported = 0;
                        return 0;
                    }
                    argc++;
                    arg = b->ast->nodes[arg].nextSibling;
                }
            }
            if (isBuiltinLen && callFlags == 0u && argc == 1u) {
                return HOPMirEmitInstEx(b, HOPMirOp_SEQ_LEN, HOPTok_INVALID, 0, callStart, callEnd);
            }
            if (isBuiltinCStr && argc == 1u) {
                return HOPMirEmitInstEx(
                    b, HOPMirOp_STR_CSTR, HOPTok_INVALID, 0, callStart, callEnd);
            }
            return HOPMirEmitInstEx(
                b,
                HOPMirOp_CALL,
                (HOPTokenKind)((uint16_t)argc | callTokFlags),
                HOPMirRawCallAuxPack((uint32_t)nodeId, callFlags),
                callStart,
                callEnd);
        }
        case HOPAst_UNARY: {
            int32_t child = b->ast->nodes[nodeId].firstChild;
            if (!HOPMirIsAllowedUnaryToken((HOPTokenKind)n->op)) {
                b->supported = 0;
                return 0;
            }
            if (child < 0) {
                HOPMirSetDiag(b->diag, HOPDiag_EXPECTED_EXPR, n->start, n->end);
                return -1;
            }
            if (HOPMirBuildExprNode(b, child) != 0) {
                return -1;
            }
            return HOPMirEmitInst(b, HOPMirOp_UNARY, (HOPTokenKind)n->op, n->start, n->end);
        }
        case HOPAst_BINARY: {
            int32_t lhs = b->ast->nodes[nodeId].firstChild;
            int32_t rhs = lhs >= 0 ? b->ast->nodes[lhs].nextSibling : -1;
            if (!HOPMirIsAllowedBinaryToken((HOPTokenKind)n->op)) {
                b->supported = 0;
                return 0;
            }
            if (lhs < 0 || rhs < 0) {
                HOPMirSetDiag(b->diag, HOPDiag_EXPECTED_EXPR, n->start, n->end);
                return -1;
            }
            if (HOPMirBuildExprNode(b, lhs) != 0 || HOPMirBuildExprNode(b, rhs) != 0) {
                return -1;
            }
            return HOPMirEmitInst(b, HOPMirOp_BINARY, (HOPTokenKind)n->op, n->start, n->end);
        }
        case HOPAst_INDEX: {
            int32_t  baseNode = b->ast->nodes[nodeId].firstChild;
            int32_t  idxNode = baseNode >= 0 ? b->ast->nodes[baseNode].nextSibling : -1;
            int32_t  extraNode = idxNode >= 0 ? b->ast->nodes[idxNode].nextSibling : -1;
            uint16_t sliceFlags =
                (uint16_t)(n->flags & (HOPAstFlag_INDEX_HAS_START | HOPAstFlag_INDEX_HAS_END));
            if ((n->flags & HOPAstFlag_INDEX_SLICE) != 0u) {
                if (baseNode < 0) {
                    HOPMirSetDiag(b->diag, HOPDiag_EXPECTED_EXPR, n->start, n->end);
                    return -1;
                }
                if (HOPMirBuildExprNode(b, baseNode) != 0) {
                    return -1;
                }
                if ((n->flags & HOPAstFlag_INDEX_HAS_START) != 0u) {
                    if (idxNode < 0) {
                        HOPMirSetDiag(b->diag, HOPDiag_EXPECTED_EXPR, n->start, n->end);
                        return -1;
                    }
                    if (HOPMirBuildExprNode(b, idxNode) != 0) {
                        return -1;
                    }
                    idxNode = extraNode;
                    extraNode = idxNode >= 0 ? b->ast->nodes[idxNode].nextSibling : -1;
                }
                if ((n->flags & HOPAstFlag_INDEX_HAS_END) != 0u) {
                    if (idxNode < 0) {
                        HOPMirSetDiag(b->diag, HOPDiag_EXPECTED_EXPR, n->start, n->end);
                        return -1;
                    }
                    if (HOPMirBuildExprNode(b, idxNode) != 0) {
                        return -1;
                    }
                    idxNode = extraNode;
                    extraNode = idxNode >= 0 ? b->ast->nodes[idxNode].nextSibling : -1;
                }
                if (extraNode >= 0 || idxNode >= 0) {
                    HOPMirSetDiag(b->diag, HOPDiag_EXPECTED_EXPR, n->start, n->end);
                    return -1;
                }
                return HOPMirEmitInstEx(b, HOPMirOp_SLICE_MAKE, sliceFlags, 0, n->start, n->end);
            }
            if (baseNode < 0 || idxNode < 0 || extraNode >= 0) {
                HOPMirSetDiag(b->diag, HOPDiag_EXPECTED_EXPR, n->start, n->end);
                return -1;
            }
            if (HOPMirBuildExprNode(b, baseNode) != 0 || HOPMirBuildExprNode(b, idxNode) != 0) {
                return -1;
            }
            return HOPMirEmitInst(b, HOPMirOp_INDEX, HOPTok_INVALID, n->start, n->end);
        }
        case HOPAst_FIELD_EXPR: {
            int32_t baseNode = b->ast->nodes[nodeId].firstChild;
            if (baseNode < 0) {
                HOPMirSetDiag(b->diag, HOPDiag_EXPECTED_EXPR, n->start, n->end);
                return -1;
            }
            if (HOPMirBuildExprNode(b, baseNode) != 0) {
                return -1;
            }
            return HOPMirEmitInstEx(
                b, HOPMirOp_AGG_GET, HOPTok_INVALID, 0, n->dataStart, n->dataEnd);
        }
        case HOPAst_CAST: {
            int32_t          valueNode = b->ast->nodes[nodeId].firstChild;
            int32_t          typeNode = valueNode >= 0 ? b->ast->nodes[valueNode].nextSibling : -1;
            int32_t          extraNode = typeNode >= 0 ? b->ast->nodes[typeNode].nextSibling : -1;
            HOPMirCastTarget target = HOPMirCastTarget_INVALID;
            if (valueNode < 0 || typeNode < 0 || extraNode >= 0) {
                HOPMirSetDiag(b->diag, HOPDiag_EXPECTED_EXPR, n->start, n->end);
                return -1;
            }
            target = HOPMirClassifyCastTarget(b, typeNode);
            if (target == HOPMirCastTarget_INVALID) {
                b->supported = 0;
                return 0;
            }
            if (HOPMirBuildExprNode(b, valueNode) != 0) {
                return -1;
            }
            return HOPMirEmitInstEx(
                b, HOPMirOp_CAST, (HOPTokenKind)target, (uint32_t)typeNode, n->start, n->end);
        }
        default: b->supported = 0; return 0;
    }
}

int HOPMirBuildExpr(
    HOPArena*     arena,
    const HOPAst* ast,
    HOPStrView    src,
    int32_t       nodeId,
    HOPMirChunk*  outChunk,
    int*          outSupported,
    HOPDiag* _Nullable diag) {
    HOPMirBuilder b;
    uint32_t      cap;

    if (diag != NULL) {
        *diag = (HOPDiag){ 0 };
    }
    if (outChunk == NULL || outSupported == NULL || arena == NULL || ast == NULL
        || ast->nodes == NULL)
    {
        HOPMirSetDiag(diag, HOPDiag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }

    outChunk->v = NULL;
    outChunk->len = 0;
    *outSupported = 0;

    cap = ast->len * 4u + 8u;
    b.v = (HOPMirInst*)HOPArenaAlloc(
        arena, cap * (uint32_t)sizeof(HOPMirInst), (uint32_t)_Alignof(HOPMirInst));
    if (b.v == NULL) {
        HOPMirSetDiag(diag, HOPDiag_ARENA_OOM, 0, 0);
        return -1;
    }

    b.ast = ast;
    b.src = src;
    b.len = 0;
    b.cap = cap;
    b.supported = 1;
    b.diag = diag;

    if (HOPMirBuildExprNode(&b, nodeId) != 0) {
        return -1;
    }

    if (b.supported) {
        uint32_t start = 0;
        uint32_t end = 0;
        if (nodeId >= 0 && (uint32_t)nodeId < ast->len) {
            start = ast->nodes[nodeId].start;
            end = ast->nodes[nodeId].end;
        }
        if (HOPMirEmitInst(&b, HOPMirOp_RETURN, HOPTok_EOF, start, end) != 0) {
            return -1;
        }
        outChunk->v = b.v;
        outChunk->len = b.len;
        *outSupported = 1;
    }

    return 0;
}

HOP_API_END
