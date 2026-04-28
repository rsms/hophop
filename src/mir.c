#include "libhop-impl.h"
#include "mir.h"

H2_API_BEGIN

typedef struct {
    const H2Ast* ast;
    H2StrView    src;
    H2MirInst*   v;
    uint32_t     len;
    uint32_t     cap;
    int          supported;
    H2Diag* _Nullable diag;
} H2MirBuilder;

static void H2MirSetDiag(H2Diag* _Nullable diag, H2DiagCode code, uint32_t start, uint32_t end);
static int  H2MirNameEqLiteral(H2StrView src, uint32_t start, uint32_t end, const char* lit);
static int  H2MirNameEqLiteralOrPkgBuiltin(
    H2StrView src, uint32_t start, uint32_t end, const char* lit, const char* pkgPrefix);
static int H2MirNameIsCompilerDiagBuiltin(H2StrView src, uint32_t start, uint32_t end);
static int H2MirNameIsLazyTypeBuiltin(H2StrView src, uint32_t start, uint32_t end);
static int H2MirCallUsesLazyBuiltinLowering(const H2MirBuilder* b, int32_t callNode);
static int H2MirBuiltinTypeSize(const H2MirBuilder* b, int32_t typeNode, int64_t* outSize);
static int H2MirInferExprSize(const H2MirBuilder* b, int32_t exprNode, int64_t* outSize);

static void* _Nullable H2MirArenaGrowArray(
    H2Arena* arena, void* _Nullable oldMem, size_t elemSize, uint32_t oldCap, uint32_t newCap) {
    void* newMem;
    if (arena == NULL || elemSize == 0 || newCap < oldCap) {
        return NULL;
    }
    newMem = H2ArenaAlloc(arena, (uint32_t)(elemSize * newCap), (uint32_t)_Alignof(uint64_t));
    if (newMem == NULL) {
        return NULL;
    }
    if (oldMem != NULL && oldCap > 0) {
        memcpy(newMem, oldMem, elemSize * oldCap);
    }
    return newMem;
}

static int H2MirProgramBuilderEnsureCap(
    H2Arena*  arena,
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
    newMem = H2MirArenaGrowArray(arena, *mem, elemSize, *cap, newCap);
    if (newMem == NULL) {
        return -1;
    }
    *mem = newMem;
    *cap = newCap;
    return 0;
}

static int H2MirNameEqLiteral(H2StrView src, uint32_t start, uint32_t end, const char* lit) {
    size_t litLen = 0;
    if (lit == NULL || end < start || end > src.len) {
        return 0;
    }
    while (lit[litLen] != '\0') {
        litLen++;
    }
    return (size_t)(end - start) == litLen && memcmp(src.ptr + start, lit, litLen) == 0;
}

static int H2MirNameEqLiteralOrPkgBuiltin(
    H2StrView src, uint32_t start, uint32_t end, const char* lit, const char* pkgPrefix) {
    size_t litLen = 0;
    size_t pkgLen = 0;
    size_t i;
    if (H2MirNameEqLiteral(src, start, end, lit)) {
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

static int H2MirNameIsCompilerDiagBuiltin(H2StrView src, uint32_t start, uint32_t end) {
    return H2MirNameEqLiteralOrPkgBuiltin(src, start, end, "error", "compiler")
        || H2MirNameEqLiteralOrPkgBuiltin(src, start, end, "error_at", "compiler")
        || H2MirNameEqLiteralOrPkgBuiltin(src, start, end, "warn", "compiler")
        || H2MirNameEqLiteralOrPkgBuiltin(src, start, end, "warn_at", "compiler");
}

static int H2MirNameIsLazyTypeBuiltin(H2StrView src, uint32_t start, uint32_t end) {
    return H2MirNameEqLiteral(src, start, end, "typeof")
        || H2MirNameEqLiteralOrPkgBuiltin(src, start, end, "kind", "reflect")
        || H2MirNameEqLiteralOrPkgBuiltin(src, start, end, "base", "reflect")
        || H2MirNameEqLiteralOrPkgBuiltin(src, start, end, "is_alias", "reflect")
        || H2MirNameEqLiteralOrPkgBuiltin(src, start, end, "is_const", "reflect")
        || H2MirNameEqLiteralOrPkgBuiltin(src, start, end, "type_name", "reflect")
        || H2MirNameEqLiteral(src, start, end, "ptr")
        || H2MirNameEqLiteral(src, start, end, "slice")
        || H2MirNameEqLiteral(src, start, end, "array");
}

static int H2MirCallUsesLazyBuiltinLowering(const H2MirBuilder* b, int32_t callNode) {
    const H2AstNode* call;
    const H2AstNode* callee;
    int32_t          calleeNode;
    int32_t          recvNode;
    if (b == NULL || b->ast == NULL || callNode < 0 || (uint32_t)callNode >= b->ast->len) {
        return 0;
    }
    call = &b->ast->nodes[callNode];
    calleeNode = call->firstChild;
    callee = calleeNode >= 0 ? &b->ast->nodes[calleeNode] : NULL;
    if (call->kind != H2Ast_CALL || callee == NULL) {
        return 0;
    }
    if (callee->kind == H2Ast_IDENT) {
        return H2MirNameEqLiteralOrPkgBuiltin(
                   b->src, callee->dataStart, callee->dataEnd, "source_location_of", "builtin")
            || H2MirNameIsCompilerDiagBuiltin(b->src, callee->dataStart, callee->dataEnd)
            || H2MirNameIsLazyTypeBuiltin(b->src, callee->dataStart, callee->dataEnd);
    }
    if (callee->kind != H2Ast_FIELD_EXPR) {
        return 0;
    }
    recvNode = callee->firstChild;
    if (recvNode < 0 || (uint32_t)recvNode >= b->ast->len
        || b->ast->nodes[recvNode].kind != H2Ast_IDENT)
    {
        return 0;
    }
    if (H2MirNameEqLiteral(
            b->src, b->ast->nodes[recvNode].dataStart, b->ast->nodes[recvNode].dataEnd, "builtin")
        && H2MirNameEqLiteral(b->src, callee->dataStart, callee->dataEnd, "source_location_of"))
    {
        return 1;
    }
    if (H2MirNameEqLiteral(
            b->src, b->ast->nodes[recvNode].dataStart, b->ast->nodes[recvNode].dataEnd, "reflect")
        && (H2MirNameEqLiteral(b->src, callee->dataStart, callee->dataEnd, "kind")
            || H2MirNameEqLiteral(b->src, callee->dataStart, callee->dataEnd, "base")
            || H2MirNameEqLiteral(b->src, callee->dataStart, callee->dataEnd, "is_alias")
            || H2MirNameEqLiteral(b->src, callee->dataStart, callee->dataEnd, "is_const")
            || H2MirNameEqLiteral(b->src, callee->dataStart, callee->dataEnd, "type_name")))
    {
        return 1;
    }
    if (!H2MirNameEqLiteral(
            b->src, b->ast->nodes[recvNode].dataStart, b->ast->nodes[recvNode].dataEnd, "compiler"))
    {
        return 0;
    }
    return H2MirNameIsCompilerDiagBuiltin(b->src, callee->dataStart, callee->dataEnd);
}

void H2MirProgramBuilderInit(H2MirProgramBuilder* b, H2Arena* arena) {
    if (b == NULL) {
        return;
    }
    memset(b, 0, sizeof(*b));
    b->arena = arena;
    b->openFunc = UINT32_MAX;
}

static int H2MirProgramBuilderAppendElem(
    H2Arena*    arena,
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
    if (H2MirProgramBuilderEnsureCap(arena, mem, elemSize, *len + 1u, cap, 1u) != 0) {
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

int H2MirProgramBuilderAddConst(
    H2MirProgramBuilder* b, const H2MirConst* value, uint32_t* _Nullable outIndex) {
    if (b == NULL) {
        return -1;
    }
    return H2MirProgramBuilderAppendElem(
        b->arena,
        (void**)&b->consts,
        value,
        (uint32_t)sizeof(*value),
        &b->constLen,
        &b->constCap,
        outIndex);
}

int H2MirProgramBuilderAddSource(
    H2MirProgramBuilder* b, const H2MirSourceRef* value, uint32_t* _Nullable outIndex) {
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
    return H2MirProgramBuilderAppendElem(
        b->arena,
        (void**)&b->sources,
        value,
        (uint32_t)sizeof(*value),
        &b->sourceLen,
        &b->sourceCap,
        outIndex);
}

int H2MirProgramBuilderAddLocal(
    H2MirProgramBuilder* b, const H2MirLocal* value, uint32_t* _Nullable outSlot) {
    uint32_t slot = 0;
    if (b == NULL || value == NULL || !b->hasOpenFunc) {
        return -1;
    }
    slot = b->funcs[b->openFunc].localCount;
    if (H2MirProgramBuilderAppendElem(
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

int H2MirProgramBuilderAddField(
    H2MirProgramBuilder* b, const H2MirField* value, uint32_t* _Nullable outIndex) {
    if (b == NULL) {
        return -1;
    }
    return H2MirProgramBuilderAppendElem(
        b->arena,
        (void**)&b->fields,
        value,
        (uint32_t)sizeof(*value),
        &b->fieldLen,
        &b->fieldCap,
        outIndex);
}

int H2MirProgramBuilderAddType(
    H2MirProgramBuilder* b, const H2MirTypeRef* value, uint32_t* _Nullable outIndex) {
    if (b == NULL) {
        return -1;
    }
    return H2MirProgramBuilderAppendElem(
        b->arena,
        (void**)&b->types,
        value,
        (uint32_t)sizeof(*value),
        &b->typeLen,
        &b->typeCap,
        outIndex);
}

int H2MirProgramBuilderAddHost(
    H2MirProgramBuilder* b, const H2MirHostRef* value, uint32_t* _Nullable outIndex) {
    if (b == NULL) {
        return -1;
    }
    return H2MirProgramBuilderAppendElem(
        b->arena,
        (void**)&b->hosts,
        value,
        (uint32_t)sizeof(*value),
        &b->hostLen,
        &b->hostCap,
        outIndex);
}

int H2MirProgramBuilderAddSymbol(
    H2MirProgramBuilder* b, const H2MirSymbolRef* value, uint32_t* _Nullable outIndex) {
    if (b == NULL) {
        return -1;
    }
    return H2MirProgramBuilderAppendElem(
        b->arena,
        (void**)&b->symbols,
        value,
        (uint32_t)sizeof(*value),
        &b->symbolLen,
        &b->symbolCap,
        outIndex);
}

int H2MirProgramBuilderBeginFunction(
    H2MirProgramBuilder* b, const H2MirFunction* value, uint32_t* _Nullable outIndex) {
    H2MirFunction fn;
    uint32_t      index = 0;
    if (b == NULL || value == NULL || b->hasOpenFunc) {
        return -1;
    }
    fn = *value;
    fn.instStart = b->instLen;
    fn.instLen = 0;
    fn.localStart = b->localLen;
    fn.localCount = 0;
    if (H2MirProgramBuilderAppendElem(
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

int H2MirProgramBuilderAppendInst(H2MirProgramBuilder* b, const H2MirInst* value) {
    if (b == NULL || value == NULL || !b->hasOpenFunc) {
        return -1;
    }
    if (H2MirProgramBuilderAppendElem(
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

int H2MirProgramBuilderInsertInst(
    H2MirProgramBuilder* b,
    uint32_t             functionIndex,
    uint32_t             instIndexInFunction,
    const H2MirInst*     value) {
    uint32_t absIndex;
    uint32_t funcIndex;
    if (b == NULL || value == NULL || b->hasOpenFunc || functionIndex >= b->funcLen) {
        return -1;
    }
    if (instIndexInFunction > b->funcs[functionIndex].instLen) {
        return -1;
    }
    if (H2MirProgramBuilderEnsureCap(
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

int H2MirProgramBuilderEndFunction(H2MirProgramBuilder* b) {
    if (b == NULL || !b->hasOpenFunc) {
        return -1;
    }
    b->funcs[b->openFunc].instLen = b->instLen - b->funcs[b->openFunc].instStart;
    b->openFunc = UINT32_MAX;
    b->hasOpenFunc = 0u;
    return 0;
}

void H2MirProgramBuilderFinish(const H2MirProgramBuilder* b, H2MirProgram* outProgram) {
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

int H2MirValidateProgram(const H2MirProgram* program, H2Diag* _Nullable diag) {
    uint32_t funcIndex;
    if (diag != NULL) {
        *diag = (H2Diag){ 0 };
    }
    if (program == NULL) {
        H2MirSetDiag(diag, H2Diag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const H2MirFunction* fn = &program->funcs[funcIndex];
        uint32_t             instIndex;
        if (fn->instStart > program->instLen || fn->instLen > program->instLen - fn->instStart) {
            H2MirSetDiag(diag, H2Diag_UNEXPECTED_TOKEN, fn->nameStart, fn->nameEnd);
            return -1;
        }
        if (fn->localStart > program->localLen
            || fn->localCount > program->localLen - fn->localStart
            || fn->localCount < fn->paramCount)
        {
            H2MirSetDiag(diag, H2Diag_UNEXPECTED_TOKEN, fn->nameStart, fn->nameEnd);
            return -1;
        }
        if (fn->sourceRef >= program->sourceLen) {
            H2MirSetDiag(diag, H2Diag_UNEXPECTED_TOKEN, fn->nameStart, fn->nameEnd);
            return -1;
        }
        if (fn->localCount != 0u) {
            uint32_t localIndex;
            for (localIndex = 0; localIndex < fn->localCount; localIndex++) {
                const H2MirLocal* local = &program->locals[fn->localStart + localIndex];
                if (local->typeRef != UINT32_MAX && local->typeRef >= program->typeLen) {
                    H2MirSetDiag(diag, H2Diag_UNEXPECTED_TOKEN, fn->nameStart, fn->nameEnd);
                    return -1;
                }
            }
        }
        for (instIndex = 0; instIndex < fn->instLen; instIndex++) {
            const H2MirInst* ins = &program->insts[fn->instStart + instIndex];
            switch (ins->op) {
                case H2MirOp_PUSH_CONST:
                    if (ins->aux >= program->constLen) {
                        H2MirSetDiag(diag, H2Diag_UNEXPECTED_TOKEN, ins->start, ins->end);
                        return -1;
                    }
                    break;
                case H2MirOp_LOAD_IDENT:
                case H2MirOp_STORE_IDENT:
                case H2MirOp_CALL:
                    if (program->symbolLen == 0 || ins->aux >= program->symbolLen) {
                        H2MirSetDiag(diag, H2Diag_UNEXPECTED_TOKEN, ins->start, ins->end);
                        return -1;
                    }
                    break;
                case H2MirOp_CAST:
                case H2MirOp_COERCE:
                    if (program->typeLen == 0 || ins->aux >= program->typeLen) {
                        H2MirSetDiag(diag, H2Diag_UNEXPECTED_TOKEN, ins->start, ins->end);
                        return -1;
                    }
                    break;
                case H2MirOp_CALL_HOST:
                    if (program->hostLen == 0 || ins->aux >= program->hostLen) {
                        H2MirSetDiag(diag, H2Diag_UNEXPECTED_TOKEN, ins->start, ins->end);
                        return -1;
                    }
                    break;
                case H2MirOp_AGG_GET:
                case H2MirOp_AGG_ADDR:
                    if (program->fieldLen == 0 || ins->aux >= program->fieldLen) {
                        H2MirSetDiag(diag, H2Diag_UNEXPECTED_TOKEN, ins->start, ins->end);
                        return -1;
                    }
                    break;
                case H2MirOp_LOCAL_ZERO:
                case H2MirOp_LOCAL_LOAD:
                case H2MirOp_LOCAL_STORE:
                case H2MirOp_LOCAL_ADDR:
                    if (ins->aux >= fn->localCount) {
                        H2MirSetDiag(diag, H2Diag_UNEXPECTED_TOKEN, ins->start, ins->end);
                        return -1;
                    }
                    break;
                case H2MirOp_JUMP:
                case H2MirOp_JUMP_IF_FALSE:
                    if (ins->aux >= fn->instLen) {
                        H2MirSetDiag(diag, H2Diag_UNEXPECTED_TOKEN, ins->start, ins->end);
                        return -1;
                    }
                    break;
                case H2MirOp_CALL_FN:
                    if (ins->aux >= program->funcLen) {
                        H2MirSetDiag(diag, H2Diag_UNEXPECTED_TOKEN, ins->start, ins->end);
                        return -1;
                    }
                    break;
                default: break;
            }
        }
    }
    return 0;
}

int H2MirProgramNeedsDynamicResolution(const H2MirProgram* program) {
    uint32_t i;
    if (program == NULL) {
        return 1;
    }
    for (i = 0; i < program->instLen; i++) {
        if (program->insts[i].op == H2MirOp_LOAD_IDENT || program->insts[i].op == H2MirOp_CALL) {
            return 1;
        }
    }
    return 0;
}

int H2MirFindFirstDynamicResolutionInst(
    const H2MirProgram* program,
    uint32_t* _Nullable outFunctionIndex,
    uint32_t* _Nullable outPc,
    const H2MirInst** _Nullable outInst) {
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
        const H2MirFunction* fn = &program->funcs[funcIndex];
        uint32_t             pc;
        for (pc = 0; pc < fn->instLen; pc++) {
            const H2MirInst* inst = &program->insts[fn->instStart + pc];
            if (inst->op != H2MirOp_LOAD_IDENT && inst->op != H2MirOp_CALL) {
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

static void H2MirSetDiag(H2Diag* _Nullable diag, H2DiagCode code, uint32_t start, uint32_t end) {
    if (diag == NULL) {
        return;
    }
    H2DiagReset(diag, code);

    diag->start = start;

    diag->end = end;
}

static int H2MirIsAllowedUnaryToken(H2TokenKind tok) {
    return tok == H2Tok_ADD || tok == H2Tok_SUB || tok == H2Tok_NOT;
}

static int H2MirIsAllowedBinaryToken(H2TokenKind tok) {
    switch (tok) {
        case H2Tok_ADD:
        case H2Tok_SUB:
        case H2Tok_MUL:
        case H2Tok_DIV:
        case H2Tok_MOD:
        case H2Tok_AND:
        case H2Tok_OR:
        case H2Tok_XOR:
        case H2Tok_LSHIFT:
        case H2Tok_RSHIFT:
        case H2Tok_EQ:
        case H2Tok_NEQ:
        case H2Tok_LT:
        case H2Tok_GT:
        case H2Tok_LTE:
        case H2Tok_GTE:
        case H2Tok_LOGICAL_AND:
        case H2Tok_LOGICAL_OR:  return 1;
        default:                return 0;
    }
}

static int H2MirEmitInstEx(
    H2MirBuilder* b, H2MirOp op, H2TokenKind tok, uint32_t aux, uint32_t start, uint32_t end) {
    if (!b->supported) {
        return 0;
    }
    if (b->len >= b->cap) {
        H2MirSetDiag(b->diag, H2Diag_ARENA_OOM, start, end);
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

static int H2MirEmitInst(
    H2MirBuilder* b, H2MirOp op, H2TokenKind tok, uint32_t start, uint32_t end) {
    return H2MirEmitInstEx(b, op, tok, 0, start, end);
}

static int H2MirTypeNameEqCStr(H2MirBuilder* b, const H2AstNode* n, const char* s) {
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

static H2MirCastTarget H2MirClassifyCastTarget(H2MirBuilder* b, int32_t typeNode) {
    const H2AstNode* n;
    int32_t          childNode;
    if (typeNode < 0 || (uint32_t)typeNode >= b->ast->len) {
        return H2MirCastTarget_INVALID;
    }
    n = &b->ast->nodes[typeNode];
    if ((n->kind == H2Ast_TYPE_REF || n->kind == H2Ast_TYPE_PTR) && n->firstChild >= 0
        && (uint32_t)n->firstChild < b->ast->len)
    {
        childNode = n->firstChild;
        if (b->ast->nodes[childNode].kind == H2Ast_TYPE_NAME
            && H2MirTypeNameEqCStr(b, &b->ast->nodes[childNode], "str"))
        {
            return H2MirCastTarget_STR_VIEW;
        }
        return H2MirCastTarget_PTR_LIKE;
    }
    if (n->kind == H2Ast_TYPE_MUTREF) {
        return H2MirCastTarget_PTR_LIKE;
    }
    if (n->kind != H2Ast_TYPE_NAME) {
        return H2MirCastTarget_INVALID;
    }
    if (H2MirTypeNameEqCStr(b, n, "rawptr")) {
        return H2MirCastTarget_PTR_LIKE;
    }
    if (H2MirTypeNameEqCStr(b, n, "bool")) {
        return H2MirCastTarget_BOOL;
    }
    if (H2MirTypeNameEqCStr(b, n, "f32") || H2MirTypeNameEqCStr(b, n, "f64")) {
        return H2MirCastTarget_FLOAT;
    }
    if (H2MirTypeNameEqCStr(b, n, "u8") || H2MirTypeNameEqCStr(b, n, "u16")
        || H2MirTypeNameEqCStr(b, n, "u32") || H2MirTypeNameEqCStr(b, n, "u64")
        || H2MirTypeNameEqCStr(b, n, "uint") || H2MirTypeNameEqCStr(b, n, "i8")
        || H2MirTypeNameEqCStr(b, n, "i16") || H2MirTypeNameEqCStr(b, n, "i32")
        || H2MirTypeNameEqCStr(b, n, "i64") || H2MirTypeNameEqCStr(b, n, "int"))
    {
        return H2MirCastTarget_INT;
    }
    return H2MirCastTarget_INVALID;
}

static int H2MirBuiltinTypeSize(const H2MirBuilder* b, int32_t typeNode, int64_t* outSize) {
    const H2AstNode* type;
    uint32_t         len;
    const char*      name;
    if (b == NULL || outSize == NULL || typeNode < 0 || (uint32_t)typeNode >= b->ast->len) {
        return 0;
    }
    type = &b->ast->nodes[typeNode];
    switch (type->kind) {
        case H2Ast_TYPE_PTR:
        case H2Ast_TYPE_REF:
        case H2Ast_TYPE_MUTREF:
        case H2Ast_TYPE_FN:       *outSize = (int64_t)sizeof(void*); return 1;
        case H2Ast_TYPE_SLICE:
        case H2Ast_TYPE_MUTSLICE: *outSize = (int64_t)(sizeof(void*) * 2u); return 1;
        case H2Ast_TYPE_NAME:     break;
        default:                  return 0;
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

static int H2MirInferExprSize(const H2MirBuilder* b, int32_t exprNode, int64_t* outSize) {
    const H2AstNode* expr;
    int32_t          valueNode;
    int32_t          typeNode;
    if (b == NULL || outSize == NULL || exprNode < 0 || (uint32_t)exprNode >= b->ast->len) {
        return 0;
    }
    expr = &b->ast->nodes[exprNode];
    switch (expr->kind) {
        case H2Ast_INT:    *outSize = (int64_t)sizeof(uintptr_t); return 1;
        case H2Ast_FLOAT:  *outSize = 8; return 1;
        case H2Ast_BOOL:   *outSize = 1; return 1;
        case H2Ast_STRING: *outSize = (int64_t)(sizeof(void*) * 2u); return 1;
        case H2Ast_CAST:
            valueNode = expr->firstChild;
            typeNode = valueNode >= 0 ? b->ast->nodes[valueNode].nextSibling : -1;
            if (valueNode < 0 || typeNode < 0 || b->ast->nodes[typeNode].nextSibling >= 0) {
                return 0;
            }
            return H2MirBuiltinTypeSize(b, typeNode, outSize);
        default: return 0;
    }
}

static int H2MirBuildExprNode(H2MirBuilder* b, int32_t nodeId) {
    const H2AstNode* n;
    if (!b->supported) {
        return 0;
    }
    if (nodeId < 0 || (uint32_t)nodeId >= b->ast->len) {
        H2MirSetDiag(b->diag, H2Diag_EXPECTED_EXPR, 0, 0);
        return -1;
    }
    n = &b->ast->nodes[nodeId];
    switch (n->kind) {
        case H2Ast_INT:
            return H2MirEmitInst(b, H2MirOp_PUSH_INT, H2Tok_INT, n->dataStart, n->dataEnd);
        case H2Ast_RUNE:
            return H2MirEmitInst(b, H2MirOp_PUSH_INT, H2Tok_RUNE, n->dataStart, n->dataEnd);
        case H2Ast_FLOAT:
            return H2MirEmitInst(b, H2MirOp_PUSH_FLOAT, H2Tok_FLOAT, n->dataStart, n->dataEnd);
        case H2Ast_BOOL:
            return H2MirEmitInst(b, H2MirOp_PUSH_BOOL, H2Tok_TRUE, n->dataStart, n->dataEnd);
        case H2Ast_STRING:
            return H2MirEmitInst(b, H2MirOp_PUSH_STRING, H2Tok_STRING, n->dataStart, n->dataEnd);
        case H2Ast_NULL: return H2MirEmitInst(b, H2MirOp_PUSH_NULL, H2Tok_NULL, n->start, n->end);
        case H2Ast_IDENT:
            return H2MirEmitInstEx(
                b, H2MirOp_LOAD_IDENT, H2Tok_IDENT, (uint32_t)nodeId, n->dataStart, n->dataEnd);
        case H2Ast_SIZEOF: {
            int32_t innerNode = n->firstChild;
            int64_t sizeValue = 0;
            if (innerNode < 0 || (uint32_t)innerNode >= b->ast->len) {
                H2MirSetDiag(b->diag, H2Diag_EXPECTED_EXPR, n->start, n->end);
                return -1;
            }
            if (n->flags == 1u) {
                if (!H2MirBuiltinTypeSize(b, innerNode, &sizeValue)) {
                    b->supported = 0;
                    return 0;
                }
            } else if (!H2MirInferExprSize(b, innerNode, &sizeValue)) {
                b->supported = 0;
                return 0;
            }
            return H2MirEmitInstEx(
                b, H2MirOp_PUSH_INT, H2Tok_INVALID, (uint32_t)sizeValue, n->start, n->end);
        }
        case H2Ast_TUPLE_EXPR: {
            int32_t  child = n->firstChild;
            uint32_t elemCount = 0;
            while (child >= 0) {
                if (H2MirBuildExprNode(b, child) != 0) {
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
            return H2MirEmitInstEx(
                b, H2MirOp_TUPLE_MAKE, (H2TokenKind)elemCount, (uint32_t)nodeId, n->start, n->end);
        }
        case H2Ast_ARRAY_LIT: {
            int32_t  child = n->firstChild;
            uint32_t elemCount = 0;
            while (child >= 0) {
                if (H2MirBuildExprNode(b, child) != 0) {
                    return -1;
                }
                if (elemCount == UINT16_MAX) {
                    b->supported = 0;
                    return 0;
                }
                elemCount++;
                child = b->ast->nodes[child].nextSibling;
            }
            return H2MirEmitInstEx(
                b, H2MirOp_TUPLE_MAKE, (H2TokenKind)elemCount, (uint32_t)nodeId, n->start, n->end);
        }
        case H2Ast_CALL: {
            int32_t  callee = b->ast->nodes[nodeId].firstChild;
            int32_t  arg;
            uint32_t argc = 0;
            uint32_t callFlags = 0;
            uint16_t callTokFlags = 0;
            uint32_t callStart;
            uint32_t callEnd;
            int      isBuiltinLen = 0;
            int      isBuiltinCStr = 0;
            int      isLazyBuiltin = H2MirCallUsesLazyBuiltinLowering(b, nodeId);
            if (callee < 0 || (uint32_t)callee >= b->ast->len) {
                b->supported = 0;
                return 0;
            }
            if (b->ast->nodes[callee].kind == H2Ast_IDENT) {
                callStart = b->ast->nodes[callee].dataStart;
                callEnd = b->ast->nodes[callee].dataEnd;
                isBuiltinLen =
                    callEnd == callStart + 3u && memcmp(b->src.ptr + callStart, "len", 3) == 0;
                isBuiltinCStr =
                    callEnd == callStart + 4u && memcmp(b->src.ptr + callStart, "cstr", 4) == 0;
            } else if (b->ast->nodes[callee].kind == H2Ast_FIELD_EXPR) {
                int32_t baseNode = b->ast->nodes[callee].firstChild;
                if (baseNode < 0) {
                    b->supported = 0;
                    return 0;
                }
                if (!isLazyBuiltin) {
                    if (H2MirBuildExprNode(b, baseNode) != 0) {
                        return -1;
                    }
                    argc = 1;
                    callFlags = H2MirSymbolFlag_CALL_RECEIVER_ARG0;
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
                    if (b->ast->nodes[arg].kind == H2Ast_CALL_ARG) {
                        exprNode = b->ast->nodes[arg].firstChild;
                        if (exprNode < 0) {
                            b->supported = 0;
                            return 0;
                        }
                        isSpread = (b->ast->nodes[arg].flags & H2AstFlag_CALL_ARG_SPREAD) != 0;
                    }
                    if (isSpread) {
                        if (b->ast->nodes[arg].nextSibling >= 0
                            || (callTokFlags & H2MirCallArgFlag_SPREAD_LAST) != 0u)
                        {
                            b->supported = 0;
                            return 0;
                        }
                        callTokFlags |= H2MirCallArgFlag_SPREAD_LAST;
                    }
                    if (H2MirBuildExprNode(b, exprNode) != 0) {
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
                return H2MirEmitInstEx(b, H2MirOp_SEQ_LEN, H2Tok_INVALID, 0, callStart, callEnd);
            }
            if (isBuiltinCStr && argc == 1u) {
                return H2MirEmitInstEx(b, H2MirOp_STR_CSTR, H2Tok_INVALID, 0, callStart, callEnd);
            }
            return H2MirEmitInstEx(
                b,
                H2MirOp_CALL,
                (H2TokenKind)((uint16_t)argc | callTokFlags),
                H2MirRawCallAuxPack((uint32_t)nodeId, callFlags),
                callStart,
                callEnd);
        }
        case H2Ast_UNARY: {
            int32_t child = b->ast->nodes[nodeId].firstChild;
            if (!H2MirIsAllowedUnaryToken((H2TokenKind)n->op)) {
                b->supported = 0;
                return 0;
            }
            if (child < 0) {
                H2MirSetDiag(b->diag, H2Diag_EXPECTED_EXPR, n->start, n->end);
                return -1;
            }
            if (H2MirBuildExprNode(b, child) != 0) {
                return -1;
            }
            return H2MirEmitInst(b, H2MirOp_UNARY, (H2TokenKind)n->op, n->start, n->end);
        }
        case H2Ast_BINARY: {
            int32_t lhs = b->ast->nodes[nodeId].firstChild;
            int32_t rhs = lhs >= 0 ? b->ast->nodes[lhs].nextSibling : -1;
            if (!H2MirIsAllowedBinaryToken((H2TokenKind)n->op)) {
                b->supported = 0;
                return 0;
            }
            if (lhs < 0 || rhs < 0) {
                H2MirSetDiag(b->diag, H2Diag_EXPECTED_EXPR, n->start, n->end);
                return -1;
            }
            if (H2MirBuildExprNode(b, lhs) != 0 || H2MirBuildExprNode(b, rhs) != 0) {
                return -1;
            }
            return H2MirEmitInst(b, H2MirOp_BINARY, (H2TokenKind)n->op, n->start, n->end);
        }
        case H2Ast_INDEX: {
            int32_t  baseNode = b->ast->nodes[nodeId].firstChild;
            int32_t  idxNode = baseNode >= 0 ? b->ast->nodes[baseNode].nextSibling : -1;
            int32_t  extraNode = idxNode >= 0 ? b->ast->nodes[idxNode].nextSibling : -1;
            uint16_t sliceFlags =
                (uint16_t)(n->flags & (H2AstFlag_INDEX_HAS_START | H2AstFlag_INDEX_HAS_END));
            if ((n->flags & H2AstFlag_INDEX_SLICE) != 0u) {
                if (baseNode < 0) {
                    H2MirSetDiag(b->diag, H2Diag_EXPECTED_EXPR, n->start, n->end);
                    return -1;
                }
                if (H2MirBuildExprNode(b, baseNode) != 0) {
                    return -1;
                }
                if ((n->flags & H2AstFlag_INDEX_HAS_START) != 0u) {
                    if (idxNode < 0) {
                        H2MirSetDiag(b->diag, H2Diag_EXPECTED_EXPR, n->start, n->end);
                        return -1;
                    }
                    if (H2MirBuildExprNode(b, idxNode) != 0) {
                        return -1;
                    }
                    idxNode = extraNode;
                    extraNode = idxNode >= 0 ? b->ast->nodes[idxNode].nextSibling : -1;
                }
                if ((n->flags & H2AstFlag_INDEX_HAS_END) != 0u) {
                    if (idxNode < 0) {
                        H2MirSetDiag(b->diag, H2Diag_EXPECTED_EXPR, n->start, n->end);
                        return -1;
                    }
                    if (H2MirBuildExprNode(b, idxNode) != 0) {
                        return -1;
                    }
                    idxNode = extraNode;
                    extraNode = idxNode >= 0 ? b->ast->nodes[idxNode].nextSibling : -1;
                }
                if (extraNode >= 0 || idxNode >= 0) {
                    H2MirSetDiag(b->diag, H2Diag_EXPECTED_EXPR, n->start, n->end);
                    return -1;
                }
                return H2MirEmitInstEx(b, H2MirOp_SLICE_MAKE, sliceFlags, 0, n->start, n->end);
            }
            if (baseNode < 0 || idxNode < 0 || extraNode >= 0) {
                H2MirSetDiag(b->diag, H2Diag_EXPECTED_EXPR, n->start, n->end);
                return -1;
            }
            if (H2MirBuildExprNode(b, baseNode) != 0 || H2MirBuildExprNode(b, idxNode) != 0) {
                return -1;
            }
            return H2MirEmitInst(b, H2MirOp_INDEX, H2Tok_INVALID, n->start, n->end);
        }
        case H2Ast_FIELD_EXPR: {
            int32_t baseNode = b->ast->nodes[nodeId].firstChild;
            if (baseNode < 0) {
                H2MirSetDiag(b->diag, H2Diag_EXPECTED_EXPR, n->start, n->end);
                return -1;
            }
            if (H2MirBuildExprNode(b, baseNode) != 0) {
                return -1;
            }
            return H2MirEmitInstEx(b, H2MirOp_AGG_GET, H2Tok_INVALID, 0, n->dataStart, n->dataEnd);
        }
        case H2Ast_CAST: {
            int32_t         valueNode = b->ast->nodes[nodeId].firstChild;
            int32_t         typeNode = valueNode >= 0 ? b->ast->nodes[valueNode].nextSibling : -1;
            int32_t         extraNode = typeNode >= 0 ? b->ast->nodes[typeNode].nextSibling : -1;
            H2MirCastTarget target = H2MirCastTarget_INVALID;
            if (valueNode < 0 || typeNode < 0 || extraNode >= 0) {
                H2MirSetDiag(b->diag, H2Diag_EXPECTED_EXPR, n->start, n->end);
                return -1;
            }
            target = H2MirClassifyCastTarget(b, typeNode);
            if (target == H2MirCastTarget_INVALID) {
                b->supported = 0;
                return 0;
            }
            if (H2MirBuildExprNode(b, valueNode) != 0) {
                return -1;
            }
            return H2MirEmitInstEx(
                b, H2MirOp_CAST, (H2TokenKind)target, (uint32_t)typeNode, n->start, n->end);
        }
        default: b->supported = 0; return 0;
    }
}

int H2MirBuildExpr(
    H2Arena*     arena,
    const H2Ast* ast,
    H2StrView    src,
    int32_t      nodeId,
    H2MirChunk*  outChunk,
    int*         outSupported,
    H2Diag* _Nullable diag) {
    H2MirBuilder b;
    uint32_t     cap;

    if (diag != NULL) {
        *diag = (H2Diag){ 0 };
    }
    if (outChunk == NULL || outSupported == NULL || arena == NULL || ast == NULL
        || ast->nodes == NULL)
    {
        H2MirSetDiag(diag, H2Diag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }

    outChunk->v = NULL;
    outChunk->len = 0;
    *outSupported = 0;

    cap = ast->len * 4u + 8u;
    b.v = (H2MirInst*)H2ArenaAlloc(
        arena, cap * (uint32_t)sizeof(H2MirInst), (uint32_t)_Alignof(H2MirInst));
    if (b.v == NULL) {
        H2MirSetDiag(diag, H2Diag_ARENA_OOM, 0, 0);
        return -1;
    }

    b.ast = ast;
    b.src = src;
    b.len = 0;
    b.cap = cap;
    b.supported = 1;
    b.diag = diag;

    if (H2MirBuildExprNode(&b, nodeId) != 0) {
        return -1;
    }

    if (b.supported) {
        uint32_t start = 0;
        uint32_t end = 0;
        if (nodeId >= 0 && (uint32_t)nodeId < ast->len) {
            start = ast->nodes[nodeId].start;
            end = ast->nodes[nodeId].end;
        }
        if (H2MirEmitInst(&b, H2MirOp_RETURN, H2Tok_EOF, start, end) != 0) {
            return -1;
        }
        outChunk->v = b.v;
        outChunk->len = b.len;
        *outSupported = 1;
    }

    return 0;
}

H2_API_END
