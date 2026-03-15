#include "libsl-impl.h"
#include "mir.h"

SL_API_BEGIN

typedef struct {
    const SLAst* ast;
    SLStrView    src;
    SLMirInst*   v;
    uint32_t     len;
    uint32_t     cap;
    int          supported;
    SLDiag*      diag;
} SLMirBuilder;

static void SLMirSetDiag(SLDiag* diag, SLDiagCode code, uint32_t start, uint32_t end);

static void* _Nullable SLMirArenaGrowArray(
    SLArena* arena, void* _Nullable oldMem, size_t elemSize, uint32_t oldCap, uint32_t newCap) {
    void* newMem;
    if (arena == NULL || elemSize == 0 || newCap < oldCap) {
        return NULL;
    }
    newMem = SLArenaAlloc(arena, (uint32_t)(elemSize * newCap), (uint32_t)_Alignof(uint64_t));
    if (newMem == NULL) {
        return NULL;
    }
    if (oldMem != NULL && oldCap > 0) {
        memcpy(newMem, oldMem, elemSize * oldCap);
    }
    return newMem;
}

static int SLMirProgramBuilderEnsureCap(
    SLArena*  arena,
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
    newMem = SLMirArenaGrowArray(arena, *mem, elemSize, *cap, newCap);
    if (newMem == NULL) {
        return -1;
    }
    *mem = newMem;
    *cap = newCap;
    return 0;
}

void SLMirProgramBuilderInit(SLMirProgramBuilder* b, SLArena* arena) {
    if (b == NULL) {
        return;
    }
    memset(b, 0, sizeof(*b));
    b->arena = arena;
    b->openFunc = UINT32_MAX;
}

static int SLMirProgramBuilderAppendElem(
    SLArena*    arena,
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
    if (SLMirProgramBuilderEnsureCap(arena, mem, elemSize, *len + 1u, cap, 1u) != 0) {
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

int SLMirProgramBuilderAddConst(
    SLMirProgramBuilder* b, const SLMirConst* value, uint32_t* _Nullable outIndex) {
    if (b == NULL) {
        return -1;
    }
    return SLMirProgramBuilderAppendElem(
        b->arena,
        (void**)&b->consts,
        value,
        (uint32_t)sizeof(*value),
        &b->constLen,
        &b->constCap,
        outIndex);
}

int SLMirProgramBuilderAddSource(
    SLMirProgramBuilder* b, const SLMirSourceRef* value, uint32_t* _Nullable outIndex) {
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
    return SLMirProgramBuilderAppendElem(
        b->arena,
        (void**)&b->sources,
        value,
        (uint32_t)sizeof(*value),
        &b->sourceLen,
        &b->sourceCap,
        outIndex);
}

int SLMirProgramBuilderAddLocal(
    SLMirProgramBuilder* b, const SLMirLocal* value, uint32_t* _Nullable outSlot) {
    uint32_t slot = 0;
    if (b == NULL || value == NULL || !b->hasOpenFunc) {
        return -1;
    }
    slot = b->funcs[b->openFunc].localCount;
    if (SLMirProgramBuilderAppendElem(
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

int SLMirProgramBuilderAddField(
    SLMirProgramBuilder* b, const SLMirField* value, uint32_t* _Nullable outIndex) {
    if (b == NULL) {
        return -1;
    }
    return SLMirProgramBuilderAppendElem(
        b->arena,
        (void**)&b->fields,
        value,
        (uint32_t)sizeof(*value),
        &b->fieldLen,
        &b->fieldCap,
        outIndex);
}

int SLMirProgramBuilderAddType(
    SLMirProgramBuilder* b, const SLMirTypeRef* value, uint32_t* _Nullable outIndex) {
    if (b == NULL) {
        return -1;
    }
    return SLMirProgramBuilderAppendElem(
        b->arena,
        (void**)&b->types,
        value,
        (uint32_t)sizeof(*value),
        &b->typeLen,
        &b->typeCap,
        outIndex);
}

int SLMirProgramBuilderAddHost(
    SLMirProgramBuilder* b, const SLMirHostRef* value, uint32_t* _Nullable outIndex) {
    if (b == NULL) {
        return -1;
    }
    return SLMirProgramBuilderAppendElem(
        b->arena,
        (void**)&b->hosts,
        value,
        (uint32_t)sizeof(*value),
        &b->hostLen,
        &b->hostCap,
        outIndex);
}

int SLMirProgramBuilderAddSymbol(
    SLMirProgramBuilder* b, const SLMirSymbolRef* value, uint32_t* _Nullable outIndex) {
    if (b == NULL) {
        return -1;
    }
    return SLMirProgramBuilderAppendElem(
        b->arena,
        (void**)&b->symbols,
        value,
        (uint32_t)sizeof(*value),
        &b->symbolLen,
        &b->symbolCap,
        outIndex);
}

int SLMirProgramBuilderBeginFunction(
    SLMirProgramBuilder* b, const SLMirFunction* value, uint32_t* _Nullable outIndex) {
    SLMirFunction fn;
    uint32_t      index = 0;
    if (b == NULL || value == NULL || b->hasOpenFunc) {
        return -1;
    }
    fn = *value;
    fn.instStart = b->instLen;
    fn.instLen = 0;
    fn.localStart = b->localLen;
    fn.localCount = 0;
    if (SLMirProgramBuilderAppendElem(
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

int SLMirProgramBuilderAppendInst(SLMirProgramBuilder* b, const SLMirInst* value) {
    if (b == NULL || value == NULL || !b->hasOpenFunc) {
        return -1;
    }
    if (SLMirProgramBuilderAppendElem(
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

int SLMirProgramBuilderInsertInst(
    SLMirProgramBuilder* b,
    uint32_t             functionIndex,
    uint32_t             instIndexInFunction,
    const SLMirInst*     value) {
    uint32_t absIndex;
    uint32_t funcIndex;
    if (b == NULL || value == NULL || b->hasOpenFunc || functionIndex >= b->funcLen) {
        return -1;
    }
    if (instIndexInFunction > b->funcs[functionIndex].instLen) {
        return -1;
    }
    if (SLMirProgramBuilderEnsureCap(
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

int SLMirProgramBuilderEndFunction(SLMirProgramBuilder* b) {
    if (b == NULL || !b->hasOpenFunc) {
        return -1;
    }
    b->funcs[b->openFunc].instLen = b->instLen - b->funcs[b->openFunc].instStart;
    b->openFunc = UINT32_MAX;
    b->hasOpenFunc = 0u;
    return 0;
}

void SLMirProgramBuilderFinish(const SLMirProgramBuilder* b, SLMirProgram* outProgram) {
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

int SLMirValidateProgram(const SLMirProgram* program, SLDiag* _Nullable diag) {
    uint32_t funcIndex;
    if (diag != NULL) {
        *diag = (SLDiag){ 0 };
    }
    if (program == NULL) {
        SLMirSetDiag(diag, SLDiag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const SLMirFunction* fn = &program->funcs[funcIndex];
        uint32_t             instIndex;
        if (fn->instStart > program->instLen || fn->instLen > program->instLen - fn->instStart) {
            SLMirSetDiag(diag, SLDiag_UNEXPECTED_TOKEN, fn->nameStart, fn->nameEnd);
            return -1;
        }
        if (fn->localStart > program->localLen
            || fn->localCount > program->localLen - fn->localStart
            || fn->localCount < fn->paramCount)
        {
            SLMirSetDiag(diag, SLDiag_UNEXPECTED_TOKEN, fn->nameStart, fn->nameEnd);
            return -1;
        }
        if (fn->sourceRef >= program->sourceLen) {
            SLMirSetDiag(diag, SLDiag_UNEXPECTED_TOKEN, fn->nameStart, fn->nameEnd);
            return -1;
        }
        if (fn->localCount != 0u) {
            uint32_t localIndex;
            for (localIndex = 0; localIndex < fn->localCount; localIndex++) {
                const SLMirLocal* local = &program->locals[fn->localStart + localIndex];
                if (local->typeRef != UINT32_MAX && local->typeRef >= program->typeLen) {
                    SLMirSetDiag(diag, SLDiag_UNEXPECTED_TOKEN, fn->nameStart, fn->nameEnd);
                    return -1;
                }
            }
        }
        for (instIndex = 0; instIndex < fn->instLen; instIndex++) {
            const SLMirInst* ins = &program->insts[fn->instStart + instIndex];
            switch (ins->op) {
                case SLMirOp_PUSH_CONST:
                    if (ins->aux >= program->constLen) {
                        SLMirSetDiag(diag, SLDiag_UNEXPECTED_TOKEN, ins->start, ins->end);
                        return -1;
                    }
                    break;
                case SLMirOp_LOAD_IDENT:
                case SLMirOp_CALL:
                    if (program->symbolLen != 0 && ins->aux >= program->symbolLen) {
                        SLMirSetDiag(diag, SLDiag_UNEXPECTED_TOKEN, ins->start, ins->end);
                        return -1;
                    }
                    break;
                case SLMirOp_CAST:
                    if (program->typeLen != 0 && ins->aux >= program->typeLen) {
                        SLMirSetDiag(diag, SLDiag_UNEXPECTED_TOKEN, ins->start, ins->end);
                        return -1;
                    }
                    break;
                case SLMirOp_CALL_HOST:
                    if (program->hostLen == 0 || ins->aux >= program->hostLen) {
                        SLMirSetDiag(diag, SLDiag_UNEXPECTED_TOKEN, ins->start, ins->end);
                        return -1;
                    }
                    break;
                case SLMirOp_AGG_GET:
                case SLMirOp_AGG_ADDR:
                    if (program->fieldLen != 0 && ins->aux >= program->fieldLen) {
                        SLMirSetDiag(diag, SLDiag_UNEXPECTED_TOKEN, ins->start, ins->end);
                        return -1;
                    }
                    break;
                case SLMirOp_LOCAL_ZERO:
                case SLMirOp_LOCAL_LOAD:
                case SLMirOp_LOCAL_STORE:
                case SLMirOp_LOCAL_ADDR:
                    if (ins->aux >= fn->localCount) {
                        SLMirSetDiag(diag, SLDiag_UNEXPECTED_TOKEN, ins->start, ins->end);
                        return -1;
                    }
                    break;
                case SLMirOp_JUMP:
                case SLMirOp_JUMP_IF_FALSE:
                    if (ins->aux >= fn->instLen) {
                        SLMirSetDiag(diag, SLDiag_UNEXPECTED_TOKEN, ins->start, ins->end);
                        return -1;
                    }
                    break;
                case SLMirOp_CALL_FN:
                    if (ins->aux >= program->funcLen) {
                        SLMirSetDiag(diag, SLDiag_UNEXPECTED_TOKEN, ins->start, ins->end);
                        return -1;
                    }
                    break;
                default: break;
            }
        }
    }
    return 0;
}

static void SLMirSetDiag(SLDiag* diag, SLDiagCode code, uint32_t start, uint32_t end) {
    if (diag == NULL) {
        return;
    }
    diag->code = code;
    diag->type = SLDiagTypeOfCode(code);
    diag->start = start;
    diag->end = end;
    diag->argStart = 0;
    diag->argEnd = 0;
}

static int SLMirIsAllowedUnaryToken(SLTokenKind tok) {
    return tok == SLTok_ADD || tok == SLTok_SUB || tok == SLTok_NOT;
}

static int SLMirIsAllowedBinaryToken(SLTokenKind tok) {
    switch (tok) {
        case SLTok_ADD:
        case SLTok_SUB:
        case SLTok_MUL:
        case SLTok_DIV:
        case SLTok_MOD:
        case SLTok_AND:
        case SLTok_OR:
        case SLTok_XOR:
        case SLTok_LSHIFT:
        case SLTok_RSHIFT:
        case SLTok_EQ:
        case SLTok_NEQ:
        case SLTok_LT:
        case SLTok_GT:
        case SLTok_LTE:
        case SLTok_GTE:
        case SLTok_LOGICAL_AND:
        case SLTok_LOGICAL_OR:  return 1;
        default:                return 0;
    }
}

static int SLMirEmitInstEx(
    SLMirBuilder* b, SLMirOp op, SLTokenKind tok, uint32_t aux, uint32_t start, uint32_t end) {
    if (!b->supported) {
        return 0;
    }
    if (b->len >= b->cap) {
        SLMirSetDiag(b->diag, SLDiag_ARENA_OOM, start, end);
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

static int SLMirEmitInst(
    SLMirBuilder* b, SLMirOp op, SLTokenKind tok, uint32_t start, uint32_t end) {
    return SLMirEmitInstEx(b, op, tok, 0, start, end);
}

static int SLMirTypeNameEqCStr(SLMirBuilder* b, const SLAstNode* n, const char* s) {
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

static SLMirCastTarget SLMirClassifyCastTarget(SLMirBuilder* b, int32_t typeNode) {
    const SLAstNode* n;
    if (typeNode < 0 || (uint32_t)typeNode >= b->ast->len) {
        return SLMirCastTarget_INVALID;
    }
    n = &b->ast->nodes[typeNode];
    if (n->kind != SLAst_TYPE_NAME) {
        return SLMirCastTarget_INVALID;
    }
    if (SLMirTypeNameEqCStr(b, n, "bool")) {
        return SLMirCastTarget_BOOL;
    }
    if (SLMirTypeNameEqCStr(b, n, "f32") || SLMirTypeNameEqCStr(b, n, "f64")) {
        return SLMirCastTarget_FLOAT;
    }
    if (SLMirTypeNameEqCStr(b, n, "u8") || SLMirTypeNameEqCStr(b, n, "u16")
        || SLMirTypeNameEqCStr(b, n, "u32") || SLMirTypeNameEqCStr(b, n, "u64")
        || SLMirTypeNameEqCStr(b, n, "uint") || SLMirTypeNameEqCStr(b, n, "i8")
        || SLMirTypeNameEqCStr(b, n, "i16") || SLMirTypeNameEqCStr(b, n, "i32")
        || SLMirTypeNameEqCStr(b, n, "i64") || SLMirTypeNameEqCStr(b, n, "int"))
    {
        return SLMirCastTarget_INT;
    }
    return SLMirCastTarget_INVALID;
}

static int SLMirBuildExprNode(SLMirBuilder* b, int32_t nodeId) {
    const SLAstNode* n;
    if (!b->supported) {
        return 0;
    }
    if (nodeId < 0 || (uint32_t)nodeId >= b->ast->len) {
        SLMirSetDiag(b->diag, SLDiag_EXPECTED_EXPR, 0, 0);
        return -1;
    }
    n = &b->ast->nodes[nodeId];
    switch (n->kind) {
        case SLAst_INT:
            return SLMirEmitInst(b, SLMirOp_PUSH_INT, SLTok_INT, n->dataStart, n->dataEnd);
        case SLAst_RUNE:
            return SLMirEmitInst(b, SLMirOp_PUSH_INT, SLTok_RUNE, n->dataStart, n->dataEnd);
        case SLAst_FLOAT:
            return SLMirEmitInst(b, SLMirOp_PUSH_FLOAT, SLTok_FLOAT, n->dataStart, n->dataEnd);
        case SLAst_BOOL:
            return SLMirEmitInst(b, SLMirOp_PUSH_BOOL, SLTok_TRUE, n->dataStart, n->dataEnd);
        case SLAst_STRING:
            return SLMirEmitInst(b, SLMirOp_PUSH_STRING, SLTok_STRING, n->dataStart, n->dataEnd);
        case SLAst_NULL: return SLMirEmitInst(b, SLMirOp_PUSH_NULL, SLTok_NULL, n->start, n->end);
        case SLAst_IDENT:
            return SLMirEmitInst(b, SLMirOp_LOAD_IDENT, SLTok_IDENT, n->dataStart, n->dataEnd);
        case SLAst_TUPLE_EXPR: {
            int32_t  child = n->firstChild;
            uint32_t elemCount = 0;
            while (child >= 0) {
                if (SLMirBuildExprNode(b, child) != 0) {
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
            return SLMirEmitInstEx(
                b, SLMirOp_TUPLE_MAKE, (SLTokenKind)elemCount, (uint32_t)nodeId, n->start, n->end);
        }
        case SLAst_CALL: {
            int32_t  callee = b->ast->nodes[nodeId].firstChild;
            int32_t  arg;
            uint32_t argc = 0;
            uint32_t callFlags = 0;
            uint32_t callStart;
            uint32_t callEnd;
            int      isBuiltinLen = 0;
            int      isBuiltinCStr = 0;
            if (callee < 0 || (uint32_t)callee >= b->ast->len) {
                b->supported = 0;
                return 0;
            }
            if (b->ast->nodes[callee].kind == SLAst_IDENT) {
                callStart = b->ast->nodes[callee].dataStart;
                callEnd = b->ast->nodes[callee].dataEnd;
                isBuiltinLen =
                    callEnd == callStart + 3u && memcmp(b->src.ptr + callStart, "len", 3) == 0;
                isBuiltinCStr =
                    callEnd == callStart + 4u && memcmp(b->src.ptr + callStart, "cstr", 4) == 0;
            } else if (b->ast->nodes[callee].kind == SLAst_FIELD_EXPR) {
                int32_t baseNode = b->ast->nodes[callee].firstChild;
                if (baseNode < 0) {
                    b->supported = 0;
                    return 0;
                }
                if (SLMirBuildExprNode(b, baseNode) != 0) {
                    return -1;
                }
                argc = 1;
                callFlags = SLMirSymbolFlag_CALL_RECEIVER_ARG0;
                callStart = b->ast->nodes[callee].dataStart;
                callEnd = b->ast->nodes[callee].dataEnd;
                isBuiltinCStr =
                    callEnd == callStart + 4u && memcmp(b->src.ptr + callStart, "cstr", 4) == 0;
            } else {
                b->supported = 0;
                return 0;
            }
            arg = b->ast->nodes[callee].nextSibling;
            while (arg >= 0) {
                int32_t exprNode = arg;
                if (b->ast->nodes[arg].kind == SLAst_CALL_ARG) {
                    exprNode = b->ast->nodes[arg].firstChild;
                    if (exprNode < 0) {
                        b->supported = 0;
                        return 0;
                    }
                }
                if (SLMirBuildExprNode(b, exprNode) != 0) {
                    return -1;
                }
                if (argc == UINT16_MAX) {
                    b->supported = 0;
                    return 0;
                }
                argc++;
                arg = b->ast->nodes[arg].nextSibling;
            }
            if (isBuiltinLen && callFlags == 0u && argc == 1u) {
                return SLMirEmitInstEx(b, SLMirOp_SEQ_LEN, SLTok_INVALID, 0, callStart, callEnd);
            }
            if (isBuiltinCStr && argc == 1u) {
                return SLMirEmitInstEx(b, SLMirOp_STR_CSTR, SLTok_INVALID, 0, callStart, callEnd);
            }
            return SLMirEmitInstEx(
                b, SLMirOp_CALL, (SLTokenKind)argc, callFlags, callStart, callEnd);
        }
        case SLAst_UNARY: {
            int32_t child = b->ast->nodes[nodeId].firstChild;
            if (!SLMirIsAllowedUnaryToken((SLTokenKind)n->op)) {
                b->supported = 0;
                return 0;
            }
            if (child < 0) {
                SLMirSetDiag(b->diag, SLDiag_EXPECTED_EXPR, n->start, n->end);
                return -1;
            }
            if (SLMirBuildExprNode(b, child) != 0) {
                return -1;
            }
            return SLMirEmitInst(b, SLMirOp_UNARY, (SLTokenKind)n->op, n->start, n->end);
        }
        case SLAst_BINARY: {
            int32_t lhs = b->ast->nodes[nodeId].firstChild;
            int32_t rhs = lhs >= 0 ? b->ast->nodes[lhs].nextSibling : -1;
            if (!SLMirIsAllowedBinaryToken((SLTokenKind)n->op)) {
                b->supported = 0;
                return 0;
            }
            if (lhs < 0 || rhs < 0) {
                SLMirSetDiag(b->diag, SLDiag_EXPECTED_EXPR, n->start, n->end);
                return -1;
            }
            if (SLMirBuildExprNode(b, lhs) != 0 || SLMirBuildExprNode(b, rhs) != 0) {
                return -1;
            }
            return SLMirEmitInst(b, SLMirOp_BINARY, (SLTokenKind)n->op, n->start, n->end);
        }
        case SLAst_INDEX: {
            int32_t baseNode = b->ast->nodes[nodeId].firstChild;
            int32_t idxNode = baseNode >= 0 ? b->ast->nodes[baseNode].nextSibling : -1;
            int32_t extraNode = idxNode >= 0 ? b->ast->nodes[idxNode].nextSibling : -1;
            if ((n->flags & SLAstFlag_INDEX_SLICE) != 0) {
                b->supported = 0;
                return 0;
            }
            if (baseNode < 0 || idxNode < 0 || extraNode >= 0) {
                SLMirSetDiag(b->diag, SLDiag_EXPECTED_EXPR, n->start, n->end);
                return -1;
            }
            if (SLMirBuildExprNode(b, baseNode) != 0 || SLMirBuildExprNode(b, idxNode) != 0) {
                return -1;
            }
            return SLMirEmitInst(b, SLMirOp_INDEX, SLTok_INVALID, n->start, n->end);
        }
        case SLAst_FIELD_EXPR: {
            int32_t baseNode = b->ast->nodes[nodeId].firstChild;
            if (baseNode < 0) {
                SLMirSetDiag(b->diag, SLDiag_EXPECTED_EXPR, n->start, n->end);
                return -1;
            }
            if (SLMirBuildExprNode(b, baseNode) != 0) {
                return -1;
            }
            return SLMirEmitInstEx(b, SLMirOp_AGG_GET, SLTok_INVALID, 0, n->dataStart, n->dataEnd);
        }
        case SLAst_CAST: {
            int32_t         valueNode = b->ast->nodes[nodeId].firstChild;
            int32_t         typeNode = valueNode >= 0 ? b->ast->nodes[valueNode].nextSibling : -1;
            int32_t         extraNode = typeNode >= 0 ? b->ast->nodes[typeNode].nextSibling : -1;
            SLMirCastTarget target = SLMirCastTarget_INVALID;
            if (valueNode < 0 || typeNode < 0 || extraNode >= 0) {
                SLMirSetDiag(b->diag, SLDiag_EXPECTED_EXPR, n->start, n->end);
                return -1;
            }
            target = SLMirClassifyCastTarget(b, typeNode);
            if (target == SLMirCastTarget_INVALID) {
                b->supported = 0;
                return 0;
            }
            if (SLMirBuildExprNode(b, valueNode) != 0) {
                return -1;
            }
            return SLMirEmitInstEx(
                b, SLMirOp_CAST, (SLTokenKind)target, (uint32_t)typeNode, n->start, n->end);
        }
        default: b->supported = 0; return 0;
    }
}

int SLMirBuildExpr(
    SLArena*     arena,
    const SLAst* ast,
    SLStrView    src,
    int32_t      nodeId,
    SLMirChunk*  outChunk,
    int*         outSupported,
    SLDiag* _Nullable diag) {
    SLMirBuilder b;
    uint32_t     cap;

    if (diag != NULL) {
        *diag = (SLDiag){ 0 };
    }
    if (outChunk == NULL || outSupported == NULL || arena == NULL || ast == NULL
        || ast->nodes == NULL)
    {
        SLMirSetDiag(diag, SLDiag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }

    outChunk->v = NULL;
    outChunk->len = 0;
    *outSupported = 0;

    cap = ast->len * 4u + 8u;
    b.v = (SLMirInst*)SLArenaAlloc(
        arena, cap * (uint32_t)sizeof(SLMirInst), (uint32_t)_Alignof(SLMirInst));
    if (b.v == NULL) {
        SLMirSetDiag(diag, SLDiag_ARENA_OOM, 0, 0);
        return -1;
    }

    b.ast = ast;
    b.src = src;
    b.len = 0;
    b.cap = cap;
    b.supported = 1;
    b.diag = diag;

    if (SLMirBuildExprNode(&b, nodeId) != 0) {
        return -1;
    }

    if (b.supported) {
        uint32_t start = 0;
        uint32_t end = 0;
        if (nodeId >= 0 && (uint32_t)nodeId < ast->len) {
            start = ast->nodes[nodeId].start;
            end = ast->nodes[nodeId].end;
        }
        if (SLMirEmitInst(&b, SLMirOp_RETURN, SLTok_EOF, start, end) != 0) {
            return -1;
        }
        outChunk->v = b.v;
        outChunk->len = b.len;
        *outSupported = 1;
    }

    return 0;
}

SL_API_END
