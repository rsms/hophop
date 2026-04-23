#include "internal.h"

HOP_API_BEGIN

static int32_t HOPTCFindNamedTypeIndexByTypeId(HOPTypeCheckCtx* c, int32_t typeId);

void HOPTCSetDiag(HOPDiag* diag, HOPDiagCode code, uint32_t start, uint32_t end) {
    if (diag == NULL) {
        return;
    }
    diag->code = code;
    diag->type = HOPDiagTypeOfCode(code);
    diag->start = start;
    diag->end = end;
    diag->argStart = 0;
    diag->argEnd = 0;
    diag->relatedStart = 0;
    diag->relatedEnd = 0;
    diag->detail = NULL;
    diag->hintOverride = NULL;
}

void HOPTCSetDiagWithArg(
    HOPDiag*    diag,
    HOPDiagCode code,
    uint32_t    start,
    uint32_t    end,
    uint32_t    argStart,
    uint32_t    argEnd) {
    if (diag == NULL) {
        return;
    }
    diag->code = code;
    diag->type = HOPDiagTypeOfCode(code);
    diag->start = start;
    diag->end = end;
    diag->argStart = argStart;
    diag->argEnd = argEnd;
    diag->relatedStart = 0;
    diag->relatedEnd = 0;
    diag->detail = NULL;
    diag->hintOverride = NULL;
}

int HOPTCFailSpan(HOPTypeCheckCtx* c, HOPDiagCode code, uint32_t start, uint32_t end) {
    HOPTCSetDiag(c->diag, code, start, end);
    return -1;
}

int HOPTCFailDuplicateDefinition(
    HOPTypeCheckCtx* c,
    uint32_t         nameStart,
    uint32_t         nameEnd,
    uint32_t         otherStart,
    uint32_t         otherEnd) {
    const char prefix[] = "other declaration of '";
    const char suffix[] = "'";
    uint32_t   nameLen = (nameEnd > nameStart && nameEnd <= c->src.len) ? nameEnd - nameStart : 0;
    uint32_t hintLen = (uint32_t)(sizeof(prefix) - 1u) + nameLen + (uint32_t)(sizeof(suffix) - 1u);
    char*    hint;
    HOPTCSetDiagWithArg(c->diag, HOPDiag_DUPLICATE_SYMBOL, nameStart, nameEnd, nameStart, nameEnd);
    if (c->diag == NULL) {
        return -1;
    }
    c->diag->relatedStart = otherStart;
    c->diag->relatedEnd = otherEnd;
    hint = (char*)HOPArenaAlloc(c->arena, hintLen + 1u, 1u);
    if (hint != NULL) {
        memcpy(hint, prefix, sizeof(prefix) - 1u);
        if (nameLen > 0) {
            memcpy(hint + sizeof(prefix) - 1u, c->src.ptr + nameStart, nameLen);
        }
        memcpy(hint + sizeof(prefix) - 1u + nameLen, suffix, sizeof(suffix));
        c->diag->hintOverride = hint;
    }
    return -1;
}

int HOPTCFailNode(HOPTypeCheckCtx* c, int32_t nodeId, HOPDiagCode code) {
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return HOPTCFailSpan(c, code, 0, 0);
    }
    return HOPTCFailSpan(c, code, c->ast->nodes[nodeId].start, c->ast->nodes[nodeId].end);
}

const char* _Nullable HOPTCAllocCStringBytes(
    HOPTypeCheckCtx* c, const uint8_t* bytes, uint32_t len) {
    char* s;
    if (c == NULL) {
        return NULL;
    }
    s = (char*)HOPArenaAlloc(c->arena, len + 1u, 1u);
    if (s == NULL) {
        return NULL;
    }
    if (len > 0u && bytes != NULL) {
        memcpy(s, bytes, len);
    }
    s[len] = '\0';
    return s;
}

int HOPTCStrEqNullable(const char* _Nullable a, const char* _Nullable b) {
    if (a == b) {
        return 1;
    }
    if (a == NULL || b == NULL) {
        return 0;
    }
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

int HOPTCEmitWarningDiag(HOPTypeCheckCtx* c, const HOPDiag* diag) {
    uint32_t i;
    if (c == NULL || diag == NULL || diag->type != HOPDiagType_WARNING) {
        return 0;
    }
    for (i = 0; i < c->warningDedupLen; i++) {
        const HOPTCWarningDedup* seen = &c->warningDedup[i];
        if (seen->start == diag->start && seen->end == diag->end
            && HOPTCStrEqNullable(seen->message, diag->detail))
        {
            return 0;
        }
    }
    if (c->warningDedupLen < c->warningDedupCap && c->warningDedup != NULL) {
        c->warningDedup[c->warningDedupLen].start = diag->start;
        c->warningDedup[c->warningDedupLen].end = diag->end;
        c->warningDedup[c->warningDedupLen].message = diag->detail;
        c->warningDedupLen++;
    }
    if (c->diagSink.onDiag != NULL) {
        c->diagSink.onDiag(c->diagSink.ctx, diag);
    }
    return 0;
}

int HOPTCRecordConstDiagUse(HOPTypeCheckCtx* c, int32_t nodeId) {
    uint32_t i;
    if (c == NULL || nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return -1;
    }
    for (i = 0; i < c->constDiagUseLen; i++) {
        if (c->constDiagUses[i].nodeId == nodeId) {
            return 0;
        }
    }
    if (c->constDiagUseLen >= c->constDiagUseCap || c->constDiagUses == NULL) {
        return HOPTCFailNode(c, nodeId, HOPDiag_ARENA_OOM);
    }
    c->constDiagUses[c->constDiagUseLen].nodeId = nodeId;
    c->constDiagUses[c->constDiagUseLen].ownerFnIndex = c->currentFunctionIndex;
    c->constDiagUses[c->constDiagUseLen].executed = 0;
    c->constDiagUseLen++;
    return 0;
}

void HOPTCMarkConstDiagUseExecuted(HOPTypeCheckCtx* c, int32_t nodeId) {
    uint32_t i;
    if (c == NULL || nodeId < 0) {
        return;
    }
    for (i = 0; i < c->constDiagUseLen; i++) {
        if (c->constDiagUses[i].nodeId == nodeId) {
            c->constDiagUses[i].executed = 1;
            return;
        }
    }
}

void HOPTCMarkConstDiagFnInvoked(HOPTypeCheckCtx* c, int32_t fnIndex) {
    if (c == NULL || fnIndex < 0 || (uint32_t)fnIndex >= c->constDiagFnInvokedCap
        || c->constDiagFnInvoked == NULL)
    {
        return;
    }
    c->constDiagFnInvoked[fnIndex] = 1;
}

int HOPTCValidateConstDiagUses(HOPTypeCheckCtx* c) {
    uint32_t i;
    if (c == NULL) {
        return -1;
    }
    for (i = 0; i < c->constDiagUseLen; i++) {
        const HOPTCConstDiagUse* use = &c->constDiagUses[i];
        if (use->ownerFnIndex >= 0) {
            if ((uint32_t)use->ownerFnIndex < c->constDiagFnInvokedCap
                && c->constDiagFnInvoked != NULL && c->constDiagFnInvoked[use->ownerFnIndex])
            {
                continue;
            }
        } else if (use->executed) {
            continue;
        }
        return HOPTCFailNode(c, use->nodeId, HOPDiag_CONSTEVAL_DIAG_NON_CONST_CONTEXT);
    }
    return 0;
}

int HOPTCRecordCallTarget(HOPTypeCheckCtx* c, int32_t callNode, int32_t targetFnIndex) {
    uint32_t i;
    if (c == NULL || callNode < 0 || targetFnIndex < 0 || (uint32_t)callNode >= c->ast->len
        || (uint32_t)targetFnIndex >= c->funcLen)
    {
        return -1;
    }
    for (i = 0; i < c->callTargetLen; i++) {
        HOPTCCallTarget* target = &c->callTargets[i];
        if (target->callNode == callNode && target->ownerFnIndex == c->currentFunctionIndex) {
            target->targetFnIndex = targetFnIndex;
            return 0;
        }
    }
    if (c->callTargetLen >= c->callTargetCap || c->callTargets == NULL) {
        return HOPTCFailNode(c, callNode, HOPDiag_ARENA_OOM);
    }
    c->callTargets[c->callTargetLen++] = (HOPTCCallTarget){
        .callNode = callNode,
        .ownerFnIndex = c->currentFunctionIndex,
        .targetFnIndex = targetFnIndex,
    };
    return 0;
}

int HOPTCFindCallTarget(
    const HOPTypeCheckCtx* c, int32_t ownerFnIndex, int32_t callNode, int32_t* outTargetFnIndex) {
    uint32_t i;
    if (outTargetFnIndex != NULL) {
        *outTargetFnIndex = -1;
    }
    if (c == NULL || callNode < 0 || outTargetFnIndex == NULL) {
        return 0;
    }
    for (i = 0; i < c->callTargetLen; i++) {
        const HOPTCCallTarget* target = &c->callTargets[i];
        if (target->callNode == callNode && target->ownerFnIndex == ownerFnIndex) {
            *outTargetFnIndex = target->targetFnIndex;
            return 1;
        }
    }
    return 0;
}

static HOPTCLocalUse* _Nullable HOPTCGetLocalUseByIndex(HOPTypeCheckCtx* c, int32_t localIdx) {
    uint32_t useIdx;
    if (c == NULL || localIdx < 0 || (uint32_t)localIdx >= c->localLen || c->locals == NULL
        || c->localUses == NULL)
    {
        return NULL;
    }
    useIdx = c->locals[localIdx].useIndex;
    if (useIdx >= c->localUseLen) {
        return NULL;
    }
    return &c->localUses[useIdx];
}

void HOPTCMarkFunctionUsed(HOPTypeCheckCtx* c, int32_t fnIndex) {
    const HOPTCFunction* fn;
    uint32_t             i;
    if (c == NULL || fnIndex < 0 || (uint32_t)fnIndex >= c->funcUsedCap || c->funcUsed == NULL) {
        return;
    }
    c->funcUsed[fnIndex] = 1;
    if ((uint32_t)fnIndex >= c->funcLen) {
        return;
    }
    fn = &c->funcs[fnIndex];
    if ((fn->flags & HOPTCFunctionFlag_TEMPLATE_INSTANCE) == 0) {
        return;
    }
    for (i = 0; i < c->funcLen; i++) {
        const HOPTCFunction* cand = &c->funcs[i];
        if (cand->declNode == fn->declNode
            && (cand->flags & HOPTCFunctionFlag_TEMPLATE_INSTANCE) == 0)
        {
            c->funcUsed[i] = 1;
        }
    }
}

void HOPTCMarkLocalRead(HOPTypeCheckCtx* c, int32_t localIdx) {
    HOPTCLocalUse* use = HOPTCGetLocalUseByIndex(c, localIdx);
    if (use == NULL) {
        return;
    }
    if (use->readCount < UINT32_MAX) {
        use->readCount++;
    }
}

void HOPTCMarkLocalWrite(HOPTypeCheckCtx* c, int32_t localIdx) {
    HOPTCLocalUse* use = HOPTCGetLocalUseByIndex(c, localIdx);
    if (use == NULL) {
        return;
    }
    if (use->writeCount < UINT32_MAX) {
        use->writeCount++;
    }
}

int HOPTCTypeIsTrackedPtrRef(HOPTypeCheckCtx* c, int32_t typeId) {
    int32_t          resolvedType;
    int32_t          baseType;
    const HOPTCType* type;
    if (c == NULL || typeId < 0) {
        return 0;
    }
    resolvedType = HOPTCResolveAliasBaseType(c, typeId);
    if (resolvedType < 0 || (uint32_t)resolvedType >= c->typeLen) {
        return 0;
    }
    type = &c->types[resolvedType];
    if (type->kind == HOPTCType_REF && !HOPTCTypeIsMutable(type)) {
        baseType = HOPTCResolveAliasBaseType(c, type->baseType);
        if (baseType == c->typeStr) {
            return 0;
        }
        if (baseType >= 0 && (uint32_t)baseType < c->typeLen
            && c->types[baseType].kind == HOPTCType_SLICE
            && !HOPTCTypeIsMutable(&c->types[baseType]))
        {
            return 0;
        }
    }
    return type->kind == HOPTCType_PTR || type->kind == HOPTCType_REF;
}

void HOPTCMarkLocalInitialized(HOPTypeCheckCtx* c, int32_t localIdx) {
    if (c == NULL || localIdx < 0 || (uint32_t)localIdx >= c->localLen || c->locals == NULL) {
        return;
    }
    if (c->locals[localIdx].initState != HOPTCLocalInit_UNTRACKED) {
        c->locals[localIdx].initState = HOPTCLocalInit_INIT;
    }
}

int HOPTCCheckLocalInitialized(HOPTypeCheckCtx* c, int32_t localIdx, uint32_t start, uint32_t end) {
    HOPTCLocal* local;
    HOPDiagCode code;
    if (c == NULL || localIdx < 0 || (uint32_t)localIdx >= c->localLen || c->locals == NULL) {
        return 0;
    }
    local = &c->locals[localIdx];
    if (local->initState == HOPTCLocalInit_UNTRACKED || local->initState == HOPTCLocalInit_INIT) {
        return 0;
    }
    code = local->initState == HOPTCLocalInit_MAYBE
             ? HOPDiag_LOCAL_PTR_REF_MAYBE_UNINIT
             : HOPDiag_LOCAL_PTR_REF_UNINIT;
    HOPTCSetDiagWithArg(c->diag, code, start, end, local->nameStart, local->nameEnd);
    return -1;
}

int HOPTCFailTopLevelPtrRefMissingInitializer(
    HOPTypeCheckCtx* c, uint32_t start, uint32_t end, uint32_t nameStart, uint32_t nameEnd) {
    HOPTCSetDiagWithArg(
        c->diag, HOPDiag_TOPLEVEL_PTR_REF_INIT_REQUIRED, start, end, nameStart, nameEnd);
    return -1;
}

void HOPTCUnmarkLocalRead(HOPTypeCheckCtx* c, int32_t localIdx) {
    HOPTCLocalUse* use = HOPTCGetLocalUseByIndex(c, localIdx);
    if (use == NULL || use->readCount == 0) {
        return;
    }
    use->readCount--;
}

void HOPTCSetLocalUsageKind(HOPTypeCheckCtx* c, int32_t localIdx, uint8_t kind) {
    HOPTCLocalUse* use = HOPTCGetLocalUseByIndex(c, localIdx);
    if (use == NULL) {
        return;
    }
    use->kind = kind;
}

void HOPTCSetLocalUsageSuppress(HOPTypeCheckCtx* c, int32_t localIdx, int suppress) {
    HOPTCLocalUse* use = HOPTCGetLocalUseByIndex(c, localIdx);
    if (use == NULL) {
        return;
    }
    use->suppressWarning = suppress ? 1u : 0u;
}

int HOPTCEmitUnusedSymbolWarnings(HOPTypeCheckCtx* c) {
    uint32_t i;
    if (c == NULL) {
        return -1;
    }
    for (i = 0; i < c->localUseLen; i++) {
        const HOPTCLocalUse* use = &c->localUses[i];
        HOPDiagCode          code;
        HOPDiag              warning;
        if (use->ownerFnIndex < 0 || use->suppressWarning || use->readCount > 0) {
            continue;
        }
        if (use->kind == HOPTCLocalUseKind_PARAM) {
            code = use->writeCount > 0
                     ? HOPDiag_UNUSED_PARAMETER_NEVER_READ
                     : HOPDiag_UNUSED_PARAMETER;
        } else {
            code =
                use->writeCount > 0 ? HOPDiag_UNUSED_VARIABLE_NEVER_READ : HOPDiag_UNUSED_VARIABLE;
        }
        warning = (HOPDiag){ 0 };
        HOPTCSetDiagWithArg(
            &warning, code, use->nameStart, use->nameEnd, use->nameStart, use->nameEnd);
        warning.type = HOPDiagType_WARNING;
        if (HOPTCEmitWarningDiag(c, &warning) != 0) {
            return -1;
        }
    }
    for (i = 0; i < c->funcLen; i++) {
        const HOPTCFunction* fn = &c->funcs[i];
        int32_t              fnNode;
        HOPDiag              warning;
        if (fn->defNode < 0) {
            continue;
        }
        if ((fn->flags & HOPTCFunctionFlag_TEMPLATE_INSTANCE) != 0) {
            continue;
        }
        if (c->funcUsed != NULL && i < c->funcUsedCap && c->funcUsed[i]) {
            continue;
        }
        if (HOPTCIsMainFunction(c, fn)) {
            continue;
        }
        fnNode = fn->defNode >= 0 ? fn->defNode : fn->declNode;
        if (fnNode < 0 || (uint32_t)fnNode >= c->ast->len) {
            continue;
        }
        if ((c->ast->nodes[fnNode].flags & HOPAstFlag_PUB) != 0) {
            continue;
        }
        warning = (HOPDiag){ 0 };
        HOPTCSetDiagWithArg(
            &warning,
            HOPDiag_UNUSED_FUNCTION,
            fn->nameStart,
            fn->nameEnd,
            fn->nameStart,
            fn->nameEnd);
        warning.type = HOPDiagType_WARNING;
        if (HOPTCEmitWarningDiag(c, &warning) != 0) {
            return -1;
        }
    }
    return 0;
}

void HOPTCOffsetToLineCol(
    const char* src, uint32_t srcLen, uint32_t offset, uint32_t* outLine, uint32_t* outColumn) {
    uint32_t i = 0;
    uint32_t line = 1;
    uint32_t col = 1;
    if (src == NULL || outLine == NULL || outColumn == NULL) {
        return;
    }
    if (offset > srcLen) {
        offset = srcLen;
    }
    while (i < offset) {
        if (src[i] == '\n') {
            line++;
            col = 1;
        } else {
            col++;
        }
        i++;
    }
    *outLine = line;
    *outColumn = col;
}

int HOPTCLineColToOffset(
    const char* src, uint32_t srcLen, uint32_t line, uint32_t column, uint32_t* outOffset) {
    uint32_t i = 0;
    uint32_t curLine = 1;
    uint32_t curCol = 1;
    if (src == NULL || outOffset == NULL || line == 0 || column == 0) {
        return -1;
    }
    while (i < srcLen) {
        if (curLine == line && curCol == column) {
            *outOffset = i;
            return 0;
        }
        if (src[i] == '\n') {
            curLine++;
            curCol = 1;
        } else {
            curCol++;
        }
        i++;
    }
    if (curLine == line && curCol == column) {
        *outOffset = srcLen;
        return 0;
    }
    return -1;
}

void HOPTCTextBufInit(HOPTCTextBuf* b, char* ptr, uint32_t cap) {
    b->ptr = ptr;
    b->cap = cap;
    b->len = 0;
    if (cap > 0) {
        ptr[0] = '\0';
    }
}

void HOPTCTextBufAppendChar(HOPTCTextBuf* b, char ch) {
    if (b->cap == 0 || b->ptr == NULL) {
        return;
    }
    if (b->len + 1u < b->cap) {
        b->ptr[b->len++] = ch;
        b->ptr[b->len] = '\0';
    }
}

void HOPTCTextBufAppendCStr(HOPTCTextBuf* b, const char* s) {
    uint32_t i = 0;
    if (s == NULL) {
        return;
    }
    while (s[i] != '\0') {
        HOPTCTextBufAppendChar(b, s[i]);
        i++;
    }
}

void HOPTCTextBufAppendSlice(HOPTCTextBuf* b, HOPStrView src, uint32_t start, uint32_t end) {
    uint32_t i;
    if (end < start || end > src.len) {
        return;
    }
    for (i = start; i < end; i++) {
        HOPTCTextBufAppendChar(b, src.ptr[i]);
    }
}

void HOPTCTextBufAppendU32(HOPTCTextBuf* b, uint32_t v) {
    char     tmp[16];
    uint32_t n = 0;
    if (v == 0) {
        HOPTCTextBufAppendChar(b, '0');
        return;
    }
    while (v > 0 && n < (uint32_t)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0) {
        n--;
        HOPTCTextBufAppendChar(b, tmp[n]);
    }
}

void HOPTCTextBufAppendHexU64(HOPTCTextBuf* b, uint64_t v) {
    char     tmp[16];
    uint32_t n = 0;
    if (v == 0u) {
        HOPTCTextBufAppendChar(b, '0');
        return;
    }
    while (v > 0u && n < (uint32_t)sizeof(tmp)) {
        uint32_t digit = (uint32_t)(v & 0xFu);
        tmp[n++] = (char)(digit < 10u ? ('0' + digit) : ('a' + (digit - 10u)));
        v >>= 4u;
    }
    while (n > 0u) {
        n--;
        HOPTCTextBufAppendChar(b, tmp[n]);
    }
}

const char* HOPTCBuiltinName(HOPTypeCheckCtx* c, int32_t typeId, HOPBuiltinKind kind) {
    switch (kind) {
        case HOPBuiltin_VOID:   return "void";
        case HOPBuiltin_BOOL:   return "bool";
        case HOPBuiltin_TYPE:   return "type";
        case HOPBuiltin_U8:     return "u8";
        case HOPBuiltin_U16:    return "u16";
        case HOPBuiltin_U32:    return "u32";
        case HOPBuiltin_U64:    return "u64";
        case HOPBuiltin_I8:     return "i8";
        case HOPBuiltin_I16:    return "i16";
        case HOPBuiltin_I32:    return "i32";
        case HOPBuiltin_I64:    return "i64";
        case HOPBuiltin_USIZE:  return "uint";
        case HOPBuiltin_ISIZE:  return "int";
        case HOPBuiltin_RAWPTR: return "rawptr";
        case HOPBuiltin_F32:    return "f32";
        case HOPBuiltin_F64:    return "f64";
        case HOPBuiltin_STR:    return "str";
        case HOPBuiltin_INVALID:
            if (typeId >= 0 && typeId == c->typeStr) {
                return "str";
            }
            return "<builtin>";
    }
    return "<builtin>";
}

void HOPTCFormatTypeRec(HOPTypeCheckCtx* c, int32_t typeId, HOPTCTextBuf* b, uint32_t depth) {
    const HOPTCType* t;
    if (depth > 32u) {
        HOPTCTextBufAppendCStr(b, "...");
        return;
    }
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        HOPTCTextBufAppendCStr(b, "<invalid>");
        return;
    }
    t = &c->types[typeId];
    switch (t->kind) {
        case HOPTCType_BUILTIN:
            HOPTCTextBufAppendCStr(b, HOPTCBuiltinName(c, typeId, t->builtin));
            return;
        case HOPTCType_NAMED:
        case HOPTCType_ALIAS:
            if (typeId == c->typeRune && (t->nameEnd <= t->nameStart || t->nameEnd > c->src.len)) {
                HOPTCTextBufAppendCStr(b, "rune");
                return;
            }
            if (t->nameEnd > t->nameStart && t->nameEnd <= c->src.len) {
                uint32_t nameStart = t->nameStart;
                if (t->nameEnd - t->nameStart > 9u
                    && memcmp(c->src.ptr + t->nameStart, "builtin__", 9u) == 0)
                {
                    nameStart += 9u;
                }
                HOPTCTextBufAppendSlice(b, c->src, nameStart, t->nameEnd);
            } else {
                HOPTCTextBufAppendCStr(b, "<unnamed>");
            }
            {
                int32_t namedIndex = HOPTCFindNamedTypeIndexByTypeId(c, typeId);
                if (namedIndex >= 0 && c->namedTypes[(uint32_t)namedIndex].templateArgCount > 0) {
                    uint16_t i;
                    HOPTCTextBufAppendChar(b, '[');
                    for (i = 0; i < c->namedTypes[(uint32_t)namedIndex].templateArgCount; i++) {
                        if (i > 0) {
                            HOPTCTextBufAppendCStr(b, ", ");
                        }
                        HOPTCFormatTypeRec(
                            c,
                            c->genericArgTypes
                                [c->namedTypes[(uint32_t)namedIndex].templateArgStart + i],
                            b,
                            depth + 1u);
                    }
                    HOPTCTextBufAppendChar(b, ']');
                }
            }
            return;
        case HOPTCType_PTR:
            HOPTCTextBufAppendChar(b, '*');
            HOPTCFormatTypeRec(c, t->baseType, b, depth + 1u);
            return;
        case HOPTCType_REF:
            HOPTCTextBufAppendChar(b, (t->flags & HOPTCTypeFlag_MUTABLE) != 0 ? '*' : '&');
            HOPTCFormatTypeRec(c, t->baseType, b, depth + 1u);
            return;
        case HOPTCType_ARRAY:
            HOPTCTextBufAppendChar(b, '[');
            HOPTCFormatTypeRec(c, t->baseType, b, depth + 1u);
            HOPTCTextBufAppendChar(b, ' ');
            HOPTCTextBufAppendU32(b, t->arrayLen);
            HOPTCTextBufAppendChar(b, ']');
            return;
        case HOPTCType_SLICE:
            HOPTCTextBufAppendChar(b, '[');
            HOPTCFormatTypeRec(c, t->baseType, b, depth + 1u);
            HOPTCTextBufAppendChar(b, ']');
            return;
        case HOPTCType_OPTIONAL:
            HOPTCTextBufAppendChar(b, '?');
            HOPTCFormatTypeRec(c, t->baseType, b, depth + 1u);
            return;
        case HOPTCType_TUPLE: {
            uint32_t i;
            HOPTCTextBufAppendChar(b, '(');
            for (i = 0; i < t->fieldCount; i++) {
                if (i > 0) {
                    HOPTCTextBufAppendCStr(b, ", ");
                }
                HOPTCFormatTypeRec(c, c->funcParamTypes[t->fieldStart + i], b, depth + 1u);
            }
            HOPTCTextBufAppendChar(b, ')');
            return;
        }
        case HOPTCType_PACK: {
            uint32_t i;
            HOPTCTextBufAppendCStr(b, "pack(");
            for (i = 0; i < t->fieldCount; i++) {
                if (i > 0) {
                    HOPTCTextBufAppendCStr(b, ", ");
                }
                HOPTCFormatTypeRec(c, c->funcParamTypes[t->fieldStart + i], b, depth + 1u);
            }
            HOPTCTextBufAppendChar(b, ')');
            return;
        }
        case HOPTCType_TYPE_PARAM:
            if (t->nameEnd > t->nameStart && t->nameEnd <= c->src.len) {
                HOPTCTextBufAppendSlice(b, c->src, t->nameStart, t->nameEnd);
            } else {
                HOPTCTextBufAppendCStr(b, "<type-param>");
            }
            return;
        case HOPTCType_ANYTYPE:       HOPTCTextBufAppendCStr(b, "anytype"); return;
        case HOPTCType_UNTYPED_INT:   HOPTCTextBufAppendCStr(b, "const_int"); return;
        case HOPTCType_UNTYPED_FLOAT: HOPTCTextBufAppendCStr(b, "const_float"); return;
        case HOPTCType_NULL:          HOPTCTextBufAppendCStr(b, "null"); return;
        case HOPTCType_FUNCTION:      HOPTCTextBufAppendCStr(b, "fn(...)"); return;
        case HOPTCType_ANON_STRUCT:   HOPTCTextBufAppendCStr(b, "struct{...}"); return;
        case HOPTCType_ANON_UNION:    HOPTCTextBufAppendCStr(b, "union{...}"); return;
        case HOPTCType_INVALID:
        default:                      HOPTCTextBufAppendCStr(b, "<type>"); return;
    }
}

int HOPTCExprIsStringConstant(HOPTypeCheckCtx* c, int32_t nodeId) {
    const HOPAstNode* n;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return 0;
    }
    n = &c->ast->nodes[nodeId];
    if (n->kind == HOPAst_STRING) {
        return 1;
    }
    if (n->kind == HOPAst_BINARY && (HOPTokenKind)n->op == HOPTok_ADD
        && HOPIsStringLiteralConcatChain(c->ast, nodeId))
    {
        return 1;
    }
    return 0;
}

void HOPTCFormatExprSubject(HOPTypeCheckCtx* c, int32_t nodeId, HOPTCTextBuf* b) {
    const HOPAstNode* n;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        HOPTCTextBufAppendCStr(b, "expression");
        return;
    }
    n = &c->ast->nodes[nodeId];
    if (n->kind == HOPAst_IDENT && n->dataEnd > n->dataStart && n->dataEnd <= c->src.len) {
        HOPTCTextBufAppendChar(b, '\'');
        HOPTCTextBufAppendSlice(b, c->src, n->dataStart, n->dataEnd);
        HOPTCTextBufAppendChar(b, '\'');
        return;
    }
    if (HOPTCExprIsStringConstant(c, nodeId)) {
        HOPTCTextBufAppendCStr(b, "string constant");
        return;
    }
    HOPTCTextBufAppendCStr(b, "expression");
}

char* _Nullable HOPTCAllocDiagText(HOPTypeCheckCtx* c, const char* text) {
    uint32_t len;
    char*    p;
    if (text == NULL) {
        return NULL;
    }
    len = 0;
    while (text[len] != '\0') {
        if (len == UINT32_MAX) {
            return NULL;
        }
        len++;
    }
    if (len > UINT32_MAX - 1u) {
        return NULL;
    }
    p = (char*)HOPArenaAlloc(c->arena, len + 1u, 1u);
    if (p == NULL) {
        return NULL;
    }
    memcpy(p, text, len + 1u);
    return p;
}

int HOPTCFailTypeMismatchDetail(
    HOPTypeCheckCtx* c, int32_t failNode, int32_t exprNode, int32_t srcType, int32_t dstType) {
    uint32_t     start = 0;
    uint32_t     end = 0;
    char         subjectBuf[HOPTC_DIAG_TEXT_CAP];
    char         srcTypeBuf[HOPTC_DIAG_TEXT_CAP];
    char         dstTypeBuf[HOPTC_DIAG_TEXT_CAP];
    char         detailBuf[384];
    HOPTCTextBuf subject;
    HOPTCTextBuf srcTypeText;
    HOPTCTextBuf dstTypeText;
    HOPTCTextBuf detailText;
    char*        detail;
    int          sourceIsStringLike = 0;

    if (failNode >= 0 && (uint32_t)failNode < c->ast->len) {
        start = c->ast->nodes[failNode].start;
        end = c->ast->nodes[failNode].end;
    }
    HOPTCSetDiag(c->diag, HOPDiag_TYPE_MISMATCH, start, end);
    if (c->diag == NULL) {
        return -1;
    }

    HOPTCTextBufInit(&subject, subjectBuf, (uint32_t)sizeof(subjectBuf));
    HOPTCTextBufInit(&srcTypeText, srcTypeBuf, (uint32_t)sizeof(srcTypeBuf));
    HOPTCTextBufInit(&dstTypeText, dstTypeBuf, (uint32_t)sizeof(dstTypeBuf));
    HOPTCFormatExprSubject(c, exprNode, &subject);
    HOPTCFormatTypeRec(c, srcType, &srcTypeText, 0);
    HOPTCFormatTypeRec(c, dstType, &dstTypeText, 0);

    HOPTCTextBufInit(&detailText, detailBuf, (uint32_t)sizeof(detailBuf));
    HOPTCTextBufAppendCStr(&detailText, "cannot use ");
    HOPTCTextBufAppendCStr(&detailText, subjectBuf);
    HOPTCTextBufAppendCStr(&detailText, " of type ");
    HOPTCTextBufAppendCStr(&detailText, srcTypeBuf);
    HOPTCTextBufAppendCStr(&detailText, " as ");
    HOPTCTextBufAppendCStr(&detailText, dstTypeBuf);
    detail = HOPTCAllocDiagText(c, detailBuf);
    if (detail != NULL) {
        c->diag->detail = detail;
    }

    if (srcType >= 0 && (uint32_t)srcType < c->typeLen) {
        const HOPTCType* src = &c->types[srcType];
        sourceIsStringLike =
            srcType == c->typeStr
            || ((src->kind == HOPTCType_PTR || src->kind == HOPTCType_REF)
                && src->baseType == c->typeStr);
    }

    if (dstType == c->typeStr && HOPTCExprIsStringConstant(c, exprNode) && sourceIsStringLike) {
        c->diag->hintOverride = HOPTCAllocDiagText(c, "change type to &str or *str");
    }
    return -1;
}

int HOPTCFailDiagText(HOPTypeCheckCtx* c, int32_t nodeId, HOPDiagCode code, const char* detail) {
    int rc = HOPTCFailNode(c, nodeId, code);
    if (rc != -1 || c == NULL || c->diag == NULL || detail == NULL) {
        return rc;
    }
    c->diag->detail = HOPTCAllocDiagText(c, detail);
    return rc;
}

int HOPTCFailTypeMismatchText(HOPTypeCheckCtx* c, int32_t nodeId, const char* detail) {
    return HOPTCFailDiagText(c, nodeId, HOPDiag_TYPE_MISMATCH, detail);
}

int HOPTCFailVarSizeByValue(
    HOPTypeCheckCtx* c, int32_t nodeId, int32_t typeId, const char* position) {
    char         typeBuf[HOPTC_DIAG_TEXT_CAP];
    char         detailBuf[256];
    HOPTCTextBuf typeText;
    HOPTCTextBuf detailText;
    const char*  where = position != NULL && position[0] != 0 ? position : "here";

    HOPTCTextBufInit(&typeText, typeBuf, (uint32_t)sizeof(typeBuf));
    HOPTCFormatTypeRec(c, typeId, &typeText, 0);

    HOPTCTextBufInit(&detailText, detailBuf, (uint32_t)sizeof(detailBuf));
    HOPTCTextBufAppendCStr(&detailText, "type ");
    HOPTCTextBufAppendCStr(&detailText, typeBuf);
    HOPTCTextBufAppendCStr(&detailText, " is not allowed by value in ");
    HOPTCTextBufAppendCStr(&detailText, where);
    HOPTCTextBufAppendCStr(&detailText, "; use &");
    HOPTCTextBufAppendCStr(&detailText, typeBuf);
    HOPTCTextBufAppendCStr(&detailText, " or *");
    HOPTCTextBufAppendCStr(&detailText, typeBuf);
    return HOPTCFailDiagText(c, nodeId, HOPDiag_VAR_SIZE_BYVALUE_FORBIDDEN, detailBuf);
}

int HOPTCFailAssignToConst(HOPTypeCheckCtx* c, int32_t lhsNode) {
    int rc = HOPTCFailNode(c, lhsNode, HOPDiag_TYPE_MISMATCH);
    c->diag->detail = HOPTCAllocDiagText(c, "assignment target is const");
    return rc;
}

int HOPTCFailAssignTargetNotAssignable(HOPTypeCheckCtx* c, int32_t lhsNode) {
    int rc = HOPTCFailNode(c, lhsNode, HOPDiag_TYPE_MISMATCH);
    if (rc != -1 || c == NULL || c->diag == NULL) {
        return rc;
    }
    c->diag->detail = HOPTCAllocDiagText(c, "assignment target is not assignable");
    return rc;
}

int HOPTCFailSwitchMissingCases(
    HOPTypeCheckCtx* c,
    int32_t          failNode,
    int32_t          subjectType,
    int32_t          subjectEnumType,
    uint32_t         enumVariantCount,
    const uint32_t* _Nullable enumVariantStarts,
    const uint32_t* _Nullable enumVariantEnds,
    const uint8_t* _Nullable enumCovered,
    int boolCoveredTrue,
    int boolCoveredFalse) {
    uint32_t          start = 0;
    uint32_t          end = 0;
    char              typeBuf[HOPTC_DIAG_TEXT_CAP];
    HOPTCTextBuf      typeText;
    uint32_t          missingCount = 0;
    uint32_t          missingNameLen = 0;
    uint32_t          detailLen;
    uint32_t          i;
    char*             detail;
    HOPTCTextBuf      detailText;
    static const char prefix[] = "of type ";

    if (failNode >= 0 && (uint32_t)failNode < c->ast->len) {
        start = c->ast->nodes[failNode].start;
        end = c->ast->nodes[failNode].end;
    }
    HOPTCSetDiag(c->diag, HOPDiag_SWITCH_MISSING_CASES, start, end);
    if (c->diag == NULL) {
        return -1;
    }

    HOPTCTextBufInit(&typeText, typeBuf, (uint32_t)sizeof(typeBuf));
    HOPTCFormatTypeRec(c, subjectType, &typeText, 0);

    if (subjectEnumType >= 0) {
        for (i = 0; i < enumVariantCount; i++) {
            if (enumCovered[i]) {
                continue;
            }
            if (enumVariantEnds[i] >= enumVariantStarts[i]) {
                uint32_t nameLen = enumVariantEnds[i] - enumVariantStarts[i];
                if (UINT32_MAX - missingNameLen < nameLen) {
                    return -1;
                }
                missingNameLen += nameLen;
            }
            missingCount++;
        }
    } else {
        if (!boolCoveredTrue) {
            missingNameLen += 4u;
            missingCount++;
        }
        if (!boolCoveredFalse) {
            missingNameLen += 5u;
            missingCount++;
        }
    }

    if (missingCount > 1u) {
        if (UINT32_MAX - missingNameLen < (missingCount - 1u) * 2u) {
            return -1;
        }
        missingNameLen += (missingCount - 1u) * 2u;
    }

    detailLen = (uint32_t)(sizeof(prefix) - 1u);
    if (UINT32_MAX - detailLen < typeText.len) {
        return -1;
    }
    detailLen += typeText.len;
    if (UINT32_MAX - detailLen < 2u) {
        return -1;
    }
    detailLen += 2u;
    if (UINT32_MAX - detailLen < missingNameLen) {
        return -1;
    }
    detailLen += missingNameLen;
    if (UINT32_MAX - detailLen < 1u) {
        return -1;
    }
    detailLen += 1u;

    detail = (char*)HOPArenaAlloc(c->arena, detailLen, 1u);
    if (detail == NULL) {
        return -1;
    }

    HOPTCTextBufInit(&detailText, detail, detailLen);
    HOPTCTextBufAppendCStr(&detailText, prefix);
    HOPTCTextBufAppendCStr(&detailText, typeBuf);
    HOPTCTextBufAppendCStr(&detailText, ": ");

    if (subjectEnumType >= 0) {
        int first = 1;
        for (i = 0; i < enumVariantCount; i++) {
            if (enumCovered[i]) {
                continue;
            }
            if (!first) {
                HOPTCTextBufAppendCStr(&detailText, ", ");
            }
            first = 0;
            HOPTCTextBufAppendSlice(&detailText, c->src, enumVariantStarts[i], enumVariantEnds[i]);
        }
    } else {
        int first = 1;
        if (!boolCoveredTrue) {
            HOPTCTextBufAppendCStr(&detailText, "true");
            first = 0;
        }
        if (!boolCoveredFalse) {
            if (!first) {
                HOPTCTextBufAppendCStr(&detailText, ", ");
            }
            HOPTCTextBufAppendCStr(&detailText, "false");
        }
    }

    c->diag->detail = detail;
    return -1;
}

int32_t HOPAstFirstChild(const HOPAst* ast, int32_t nodeId) {
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    return ast->nodes[nodeId].firstChild;
}

int32_t HOPAstNextSibling(const HOPAst* ast, int32_t nodeId) {
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    return ast->nodes[nodeId].nextSibling;
}

int HOPNameEqSlice(HOPStrView src, uint32_t aStart, uint32_t aEnd, uint32_t bStart, uint32_t bEnd) {
    uint32_t i;
    uint32_t len;
    if (aEnd < aStart || bEnd < bStart) {
        return 0;
    }
    len = aEnd - aStart;
    if (len != (bEnd - bStart)) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        if (src.ptr[aStart + i] != src.ptr[bStart + i]) {
            return 0;
        }
    }
    return 1;
}

int HOPNameEqLiteral(HOPStrView src, uint32_t start, uint32_t end, const char* lit) {
    uint32_t i = 0;
    uint32_t len;
    if (end < start) {
        return 0;
    }
    len = end - start;
    while (i < len) {
        if (lit[i] == '\0' || src.ptr[start + i] != lit[i]) {
            return 0;
        }
        i++;
    }
    return lit[i] == '\0';
}

int HOPNameHasPrefix(HOPStrView src, uint32_t start, uint32_t end, const char* prefix) {
    uint32_t i = 0;
    if (end < start) {
        return 0;
    }
    while (prefix[i] != '\0') {
        if (start + i >= end || src.ptr[start + i] != prefix[i]) {
            return 0;
        }
        i++;
    }
    return 1;
}

int HOPNameHasSuffix(HOPStrView src, uint32_t start, uint32_t end, const char* suffix) {
    uint32_t suffixLen = 0;
    uint32_t nameLen;
    while (suffix[suffixLen] != '\0') {
        suffixLen++;
    }
    if (end < start) {
        return 0;
    }
    nameLen = end - start;
    if (suffixLen > nameLen) {
        return 0;
    }
    return memcmp(src.ptr + end - suffixLen, suffix, suffixLen) == 0;
}

int32_t HOPTCFindMemAllocatorType(HOPTypeCheckCtx* c) {
    return c->typeMemAllocator;
}

int32_t HOPTCGetStrRefType(HOPTypeCheckCtx* c, uint32_t start, uint32_t end) {
    if (c->typeStr < 0) {
        return -1;
    }
    return HOPTCInternRefType(c, c->typeStr, 0, start, end);
}

int32_t HOPTCGetStrPtrType(HOPTypeCheckCtx* c, uint32_t start, uint32_t end) {
    if (c->typeStr < 0) {
        return -1;
    }
    return HOPTCInternPtrType(c, c->typeStr, start, end);
}

int32_t HOPTCAddType(HOPTypeCheckCtx* c, const HOPTCType* t, uint32_t errStart, uint32_t errEnd) {
    int32_t idx;
    if (c->typeLen >= c->typeCap) {
        HOPTCSetDiag(c->diag, HOPDiag_ARENA_OOM, errStart, errEnd);
        return -1;
    }
    idx = (int32_t)c->typeLen++;
    c->types[idx] = *t;
    return idx;
}

int32_t HOPTCAddBuiltinType(HOPTypeCheckCtx* c, const char* name, HOPBuiltinKind builtinKind) {
    HOPTCType t;
    uint32_t  i = 0;
    while (name[i] != '\0') {
        i++;
    }
    t.kind = HOPTCType_BUILTIN;
    t.builtin = builtinKind;
    t.baseType = -1;
    t.declNode = -1;
    t.funcIndex = -1;
    t.arrayLen = 0;
    t.nameStart = 0;
    t.nameEnd = i;
    t.fieldStart = 0;
    t.fieldCount = 0;
    t.flags = 0;
    return HOPTCAddType(c, &t, 0, 0);
}

int HOPTCEnsureInitialized(HOPTypeCheckCtx* c) {
    HOPTCType t;
    int32_t   u32Type;

    c->typeVoid = -1;
    c->typeBool = -1;
    c->typeStr = -1;
    c->typeType = -1;
    c->typeRune = -1;
    c->typeMemAllocator = -1;
    c->typeUsize = -1;
    c->typeRawptr = -1;
    c->typeSourceLocation = -1;
    c->typeFmtValue = -1;
    c->typeUntypedInt = -1;
    c->typeUntypedFloat = -1;
    c->typeNull = -1;
    c->typeAnytype = -1;

    c->typeVoid = HOPTCAddBuiltinType(c, "void", HOPBuiltin_VOID);
    c->typeBool = HOPTCAddBuiltinType(c, "bool", HOPBuiltin_BOOL);
    c->typeType = HOPTCAddBuiltinType(c, "type", HOPBuiltin_TYPE);
    if (c->typeVoid < 0 || c->typeBool < 0 || c->typeType < 0) {
        return -1;
    }

    if (HOPTCAddBuiltinType(c, "u8", HOPBuiltin_U8) < 0
        || HOPTCAddBuiltinType(c, "u16", HOPBuiltin_U16) < 0
        || HOPTCAddBuiltinType(c, "u32", HOPBuiltin_U32) < 0
        || HOPTCAddBuiltinType(c, "u64", HOPBuiltin_U64) < 0
        || HOPTCAddBuiltinType(c, "i8", HOPBuiltin_I8) < 0
        || HOPTCAddBuiltinType(c, "i16", HOPBuiltin_I16) < 0
        || HOPTCAddBuiltinType(c, "i32", HOPBuiltin_I32) < 0
        || HOPTCAddBuiltinType(c, "i64", HOPBuiltin_I64) < 0
        || HOPTCAddBuiltinType(c, "uint", HOPBuiltin_USIZE) < 0
        || HOPTCAddBuiltinType(c, "int", HOPBuiltin_ISIZE) < 0
        || HOPTCAddBuiltinType(c, "rawptr", HOPBuiltin_RAWPTR) < 0
        || HOPTCAddBuiltinType(c, "f32", HOPBuiltin_F32) < 0
        || HOPTCAddBuiltinType(c, "f64", HOPBuiltin_F64) < 0)
    {
        return -1;
    }
    c->typeRawptr = HOPTCFindBuiltinByKind(c, HOPBuiltin_RAWPTR);
    if (c->typeRawptr < 0) {
        return -1;
    }
    c->typeStr = HOPTCAddBuiltinType(c, "str", HOPBuiltin_INVALID);
    if (c->typeStr < 0) {
        return -1;
    }
    u32Type = HOPTCFindBuiltinByKind(c, HOPBuiltin_U32);
    if (u32Type < 0) {
        return -1;
    }
    t.kind = HOPTCType_ALIAS;
    t.builtin = HOPBuiltin_INVALID;
    t.baseType = u32Type;
    t.declNode = -1;
    t.funcIndex = -1;
    t.arrayLen = 0;
    t.nameStart = 0;
    t.nameEnd = 0;
    t.fieldStart = 0;
    t.fieldCount = 0;
    t.flags = HOPTCTypeFlag_ALIAS_RESOLVED;
    c->typeRune = HOPTCAddType(c, &t, 0, 0);
    if (c->typeRune < 0) {
        return -1;
    }

    t.kind = HOPTCType_ANYTYPE;
    t.builtin = HOPBuiltin_INVALID;
    t.baseType = -1;
    t.declNode = -1;
    t.funcIndex = -1;
    t.arrayLen = 0;
    t.nameStart = 0;
    t.nameEnd = 0;
    t.fieldStart = 0;
    t.fieldCount = 0;
    t.flags = 0;
    c->typeAnytype = HOPTCAddType(c, &t, 0, 0);
    if (c->typeAnytype < 0) {
        return -1;
    }

    t.kind = HOPTCType_UNTYPED_INT;
    t.builtin = HOPBuiltin_INVALID;
    t.baseType = -1;
    t.declNode = -1;
    t.funcIndex = -1;
    t.arrayLen = 0;
    t.nameStart = 0;
    t.nameEnd = 0;
    t.fieldStart = 0;
    t.fieldCount = 0;
    t.flags = 0;
    c->typeUntypedInt = HOPTCAddType(c, &t, 0, 0);
    if (c->typeUntypedInt < 0) {
        return -1;
    }

    t.kind = HOPTCType_UNTYPED_FLOAT;
    c->typeUntypedFloat = HOPTCAddType(c, &t, 0, 0);
    if (c->typeUntypedFloat < 0) {
        return -1;
    }

    t.kind = HOPTCType_NULL;
    c->typeNull = HOPTCAddType(c, &t, 0, 0);
    return c->typeNull < 0 ? -1 : 0;
}

int32_t HOPTCFindBuiltinType(HOPTypeCheckCtx* c, uint32_t start, uint32_t end) {
    uint32_t i;
    if (HOPNameEqLiteral(c->src, start, end, "const_int")) {
        return c->typeUntypedInt;
    }
    if (HOPNameEqLiteral(c->src, start, end, "const_float")) {
        return c->typeUntypedFloat;
    }
    if (HOPNameEqLiteral(c->src, start, end, "str")
        || HOPNameEqLiteral(c->src, start, end, "builtin__str"))
    {
        return c->typeStr;
    }
    if ((HOPNameEqLiteral(c->src, start, end, "rune")
         || HOPNameEqLiteral(c->src, start, end, "builtin__rune"))
        && c->typeRune >= 0)
    {
        return c->typeRune;
    }
    if (HOPNameEqLiteral(c->src, start, end, "rawptr")
        || HOPNameEqLiteral(c->src, start, end, "builtin__rawptr"))
    {
        return c->typeRawptr;
    }
    for (i = 0; i < c->typeLen; i++) {
        const HOPTCType* t = &c->types[i];
        if (t->kind != HOPTCType_BUILTIN) {
            continue;
        }
        if (HOPNameEqLiteral(c->src, start, end, "bool") && t->builtin == HOPBuiltin_BOOL) {
            return (int32_t)i;
        }
        if (HOPNameEqLiteral(c->src, start, end, "type") && t->builtin == HOPBuiltin_TYPE) {
            return (int32_t)i;
        }
        if (HOPNameEqLiteral(c->src, start, end, "u8") && t->builtin == HOPBuiltin_U8) {
            return (int32_t)i;
        }
        if (HOPNameEqLiteral(c->src, start, end, "u16") && t->builtin == HOPBuiltin_U16) {
            return (int32_t)i;
        }
        if (HOPNameEqLiteral(c->src, start, end, "u32") && t->builtin == HOPBuiltin_U32) {
            return (int32_t)i;
        }
        if (HOPNameEqLiteral(c->src, start, end, "u64") && t->builtin == HOPBuiltin_U64) {
            return (int32_t)i;
        }
        if (HOPNameEqLiteral(c->src, start, end, "i8") && t->builtin == HOPBuiltin_I8) {
            return (int32_t)i;
        }
        if (HOPNameEqLiteral(c->src, start, end, "i16") && t->builtin == HOPBuiltin_I16) {
            return (int32_t)i;
        }
        if (HOPNameEqLiteral(c->src, start, end, "i32") && t->builtin == HOPBuiltin_I32) {
            return (int32_t)i;
        }
        if (HOPNameEqLiteral(c->src, start, end, "i64") && t->builtin == HOPBuiltin_I64) {
            return (int32_t)i;
        }
        if (HOPNameEqLiteral(c->src, start, end, "uint") && t->builtin == HOPBuiltin_USIZE) {
            return (int32_t)i;
        }
        if (HOPNameEqLiteral(c->src, start, end, "int") && t->builtin == HOPBuiltin_ISIZE) {
            return (int32_t)i;
        }
        if (HOPNameEqLiteral(c->src, start, end, "f32") && t->builtin == HOPBuiltin_F32) {
            return (int32_t)i;
        }
        if (HOPNameEqLiteral(c->src, start, end, "f64") && t->builtin == HOPBuiltin_F64) {
            return (int32_t)i;
        }
    }
    return -1;
}

int32_t HOPTCFindBuiltinByKind(HOPTypeCheckCtx* c, HOPBuiltinKind builtinKind) {
    uint32_t i;
    for (i = 0; i < c->typeLen; i++) {
        if (c->types[i].kind == HOPTCType_BUILTIN && c->types[i].builtin == builtinKind) {
            return (int32_t)i;
        }
    }
    return -1;
}

uint8_t HOPTCTypeTagKindOf(HOPTypeCheckCtx* c, int32_t typeId) {
    const HOPTCType* t;
    int32_t          declNode;
    if (c == NULL || typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return HOPTCTypeTagKind_INVALID;
    }
    t = &c->types[typeId];
    switch (t->kind) {
        case HOPTCType_ALIAS:       return HOPTCTypeTagKind_ALIAS;
        case HOPTCType_PTR:         return HOPTCTypeTagKind_POINTER;
        case HOPTCType_REF:         return HOPTCTypeTagKind_REFERENCE;
        case HOPTCType_ARRAY:       return HOPTCTypeTagKind_ARRAY;
        case HOPTCType_SLICE:       return HOPTCTypeTagKind_SLICE;
        case HOPTCType_OPTIONAL:    return HOPTCTypeTagKind_OPTIONAL;
        case HOPTCType_FUNCTION:    return HOPTCTypeTagKind_FUNCTION;
        case HOPTCType_TUPLE:       return HOPTCTypeTagKind_TUPLE;
        case HOPTCType_ANON_STRUCT: return HOPTCTypeTagKind_STRUCT;
        case HOPTCType_ANON_UNION:  return HOPTCTypeTagKind_UNION;
        case HOPTCType_NAMED:
            declNode = t->declNode;
            if (declNode >= 0 && (uint32_t)declNode < c->ast->len) {
                switch (c->ast->nodes[declNode].kind) {
                    case HOPAst_STRUCT: return HOPTCTypeTagKind_STRUCT;
                    case HOPAst_UNION:  return HOPTCTypeTagKind_UNION;
                    case HOPAst_ENUM:   return HOPTCTypeTagKind_ENUM;
                    default:            break;
                }
            }
            return HOPTCTypeTagKind_PRIMITIVE;
        case HOPTCType_BUILTIN:
        case HOPTCType_UNTYPED_INT:
        case HOPTCType_UNTYPED_FLOAT:
        case HOPTCType_NULL:          return HOPTCTypeTagKind_PRIMITIVE;
        default:                      return HOPTCTypeTagKind_INVALID;
    }
}

uint64_t HOPTCEncodeTypeTag(HOPTypeCheckCtx* c, int32_t typeId) {
    uint64_t kind = (uint64_t)HOPTCTypeTagKindOf(c, typeId);
    uint64_t id = typeId >= 0 ? ((uint64_t)(uint32_t)typeId + 1u) : 0u;
    return (kind << 56u) | (id & 0x00ffffffffffffffULL);
}

int HOPTCDecodeTypeTag(HOPTypeCheckCtx* c, uint64_t typeTag, int32_t* outTypeId) {
    uint64_t id;
    int32_t  typeId;
    if (c == NULL || outTypeId == NULL) {
        return -1;
    }
    id = typeTag & 0x00ffffffffffffffULL;
    if (id == 0u) {
        return -1;
    }
    if (id - 1u > (uint64_t)INT32_MAX) {
        return -1;
    }
    typeId = (int32_t)(id - 1u);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return -1;
    }
    *outTypeId = typeId;
    return 0;
}

static int32_t HOPTCFindNamedTypeIndexByTypeId(HOPTypeCheckCtx* c, int32_t typeId) {
    uint32_t i;
    for (i = 0; i < c->namedTypeLen; i++) {
        if (c->namedTypes[i].typeId == typeId) {
            return (int32_t)i;
        }
    }
    return -1;
}

static int32_t HOPTCParentOwnerTypeId(HOPTypeCheckCtx* c, int32_t typeId) {
    int32_t idx = HOPTCFindNamedTypeIndexByTypeId(c, typeId);
    if (idx < 0) {
        return -1;
    }
    return c->namedTypes[(uint32_t)idx].ownerTypeId;
}

int32_t HOPTCFindNamedTypeIndexOwned(
    HOPTypeCheckCtx* c, int32_t ownerTypeId, uint32_t start, uint32_t end) {
    uint32_t i;
    for (i = 0; i < c->namedTypeLen; i++) {
        if (c->namedTypes[i].ownerTypeId == ownerTypeId
            && HOPNameEqSlice(
                c->src, c->namedTypes[i].nameStart, c->namedTypes[i].nameEnd, start, end))
        {
            return (int32_t)i;
        }
    }
    return -1;
}

static int32_t HOPTCFindNamedTypeIndexInOwnerScope(
    HOPTypeCheckCtx* c, int32_t ownerTypeId, uint32_t start, uint32_t end) {
    int32_t owner = ownerTypeId;
    while (owner >= 0) {
        int32_t idx = HOPTCFindNamedTypeIndexOwned(c, owner, start, end);
        if (idx >= 0) {
            return idx;
        }
        owner = HOPTCParentOwnerTypeId(c, owner);
    }
    return HOPTCFindNamedTypeIndexOwned(c, -1, start, end);
}

int32_t HOPTCResolveTypeNamePath(
    HOPTypeCheckCtx* c, uint32_t start, uint32_t end, int32_t ownerTypeId) {
    uint32_t segStart = start;
    uint32_t pos = start;
    int32_t  currentTypeId = -1;
    if (end <= start) {
        return -1;
    }
    while (pos <= end) {
        if (pos == end || c->src.ptr[pos] == '.') {
            uint32_t segEnd = pos;
            int32_t  idx;
            if (segEnd <= segStart) {
                return -1;
            }
            if (currentTypeId < 0) {
                idx = HOPTCFindNamedTypeIndexInOwnerScope(c, ownerTypeId, segStart, segEnd);
            } else {
                idx = HOPTCFindNamedTypeIndexOwned(c, currentTypeId, segStart, segEnd);
            }
            if (idx < 0) {
                return -1;
            }
            currentTypeId = c->namedTypes[(uint32_t)idx].typeId;
            segStart = pos + 1u;
        }
        if (pos == end) {
            break;
        }
        pos++;
    }
    return currentTypeId;
}

int32_t HOPTCResolveTypeValueName(HOPTypeCheckCtx* c, uint32_t start, uint32_t end) {
    int32_t builtinType = HOPTCFindBuiltinType(c, start, end);
    int32_t typeId;
    if (c != NULL && c->activeGenericDeclNode >= 0) {
        int32_t idx = HOPTCDeclTypeParamIndex(c, c->activeGenericDeclNode, start, end);
        if (idx >= 0 && (uint32_t)idx < c->activeGenericArgCount) {
            return c->genericArgTypes[c->activeGenericArgStart + (uint32_t)idx];
        }
    }
    if (builtinType >= 0) {
        return builtinType;
    }
    typeId = HOPTCResolveTypeNamePath(c, start, end, c->currentTypeOwnerTypeId);
    if (typeId >= 0) {
        int32_t namedIndex = HOPTCFindNamedTypeIndexByTypeId(c, typeId);
        if (namedIndex >= 0 && c->namedTypes[(uint32_t)namedIndex].templateArgCount > 0
            && c->namedTypes[(uint32_t)namedIndex].templateRootNamedIndex < 0)
        {
            return -1;
        }
        return typeId;
    }
    typeId = HOPTCFindBuiltinQualifiedNamedType(c, start, end);
    if (typeId >= 0) {
        int32_t namedIndex = HOPTCFindNamedTypeIndexByTypeId(c, typeId);
        if (namedIndex >= 0 && c->namedTypes[(uint32_t)namedIndex].templateArgCount > 0
            && c->namedTypes[(uint32_t)namedIndex].templateRootNamedIndex < 0)
        {
            return -1;
        }
        return typeId;
    }
    return -1;
}

int32_t HOPTCFindNamedTypeIndex(HOPTypeCheckCtx* c, uint32_t start, uint32_t end) {
    return HOPTCFindNamedTypeIndexOwned(c, -1, start, end);
}

int32_t HOPTCFindNamedTypeByLiteral(HOPTypeCheckCtx* c, const char* name) {
    uint32_t i;
    for (i = 0; i < c->typeLen; i++) {
        const HOPTCType* t = &c->types[i];
        if (t->kind != HOPTCType_NAMED) {
            continue;
        }
        if (HOPNameEqLiteral(c->src, t->nameStart, t->nameEnd, name)) {
            return (int32_t)i;
        }
    }
    return -1;
}

int32_t HOPTCFindBuiltinQualifiedNamedType(HOPTypeCheckCtx* c, uint32_t start, uint32_t end) {
    uint32_t i;
    size_t   nameLen;
    if (c == NULL || end <= start || end > c->src.len) {
        return -1;
    }
    nameLen = (size_t)(end - start);
    for (i = 0; i < c->typeLen; i++) {
        const HOPTCType* t = &c->types[i];
        uint32_t         candLen;
        if (t->kind != HOPTCType_NAMED || t->nameEnd <= t->nameStart) {
            continue;
        }
        candLen = t->nameEnd - t->nameStart;
        if (candLen != 9u + (uint32_t)nameLen) {
            continue;
        }
        if (memcmp(c->src.ptr + t->nameStart, "builtin__", 9u) != 0) {
            continue;
        }
        if (memcmp(c->src.ptr + t->nameStart + 9u, c->src.ptr + start, nameLen) == 0) {
            return (int32_t)i;
        }
    }
    return -1;
}

int32_t HOPTCFindBuiltinNamedTypeBySuffix(HOPTypeCheckCtx* c, const char* suffix) {
    uint32_t i;
    for (i = 0; i < c->typeLen; i++) {
        const HOPTCType* t = &c->types[i];
        if (t->kind != HOPTCType_NAMED) {
            continue;
        }
        if (HOPNameHasPrefix(c->src, t->nameStart, t->nameEnd, "builtin")
            && HOPNameHasSuffix(c->src, t->nameStart, t->nameEnd, suffix))
        {
            return (int32_t)i;
        }
    }
    return -1;
}

int32_t HOPTCFindNamedTypeBySuffix(HOPTypeCheckCtx* c, const char* suffix) {
    uint32_t i;
    for (i = 0; i < c->typeLen; i++) {
        const HOPTCType* t = &c->types[i];
        if (t->kind != HOPTCType_NAMED) {
            continue;
        }
        if (HOPNameHasSuffix(c->src, t->nameStart, t->nameEnd, suffix)) {
            return (int32_t)i;
        }
    }
    return -1;
}

int32_t HOPTCFindReflectKindType(HOPTypeCheckCtx* c) {
    uint32_t i;
    int32_t  direct = HOPTCFindNamedTypeByLiteral(c, "reflect__Kind");
    if (direct >= 0) {
        return direct;
    }
    for (i = 0; i < c->typeLen; i++) {
        const HOPTCType* t = &c->types[i];
        if (t->kind != HOPTCType_NAMED) {
            continue;
        }
        if (!HOPNameHasPrefix(c->src, t->nameStart, t->nameEnd, "reflect")
            || !HOPNameHasSuffix(c->src, t->nameStart, t->nameEnd, "__Kind"))
        {
            continue;
        }
        if (t->declNode >= 0 && (uint32_t)t->declNode < c->ast->len
            && c->ast->nodes[t->declNode].kind == HOPAst_ENUM)
        {
            return (int32_t)i;
        }
    }
    return -1;
}

int HOPTCNameEqLiteralOrPkgBuiltin(
    HOPTypeCheckCtx* c, uint32_t start, uint32_t end, const char* name, const char* pkgPrefix) {
    uint32_t i = 0;
    uint32_t j = 0;
    uint32_t prefixLen = 0;
    uint32_t nameLen = 0;
    if (HOPNameEqLiteral(c->src, start, end, name)) {
        return 1;
    }
    while (pkgPrefix[prefixLen] != '\0') {
        prefixLen++;
    }
    while (name[nameLen] != '\0') {
        nameLen++;
    }
    if (end < start || end - start != prefixLen + 2u + nameLen) {
        return 0;
    }
    for (i = 0; i < prefixLen; i++) {
        if (c->src.ptr[start + i] != pkgPrefix[i]) {
            return 0;
        }
    }
    if (c->src.ptr[start + prefixLen] != '_' || c->src.ptr[start + prefixLen + 1u] != '_') {
        return 0;
    }
    for (j = 0; j < nameLen; j++) {
        if (c->src.ptr[start + prefixLen + 2u + j] != name[j]) {
            return 0;
        }
    }
    return 1;
}

HOPTCCompilerDiagOp HOPTCCompilerDiagOpFromName(HOPTypeCheckCtx* c, uint32_t start, uint32_t end) {
    if (HOPTCNameEqLiteralOrPkgBuiltin(c, start, end, "error", "compiler")) {
        return HOPTCCompilerDiagOp_ERROR;
    }
    if (HOPTCNameEqLiteralOrPkgBuiltin(c, start, end, "error_at", "compiler")) {
        return HOPTCCompilerDiagOp_ERROR_AT;
    }
    if (HOPTCNameEqLiteralOrPkgBuiltin(c, start, end, "warn", "compiler")) {
        return HOPTCCompilerDiagOp_WARN;
    }
    if (HOPTCNameEqLiteralOrPkgBuiltin(c, start, end, "warn_at", "compiler")) {
        return HOPTCCompilerDiagOp_WARN_AT;
    }
    return HOPTCCompilerDiagOp_NONE;
}

int HOPTCIsSourceLocationOfName(HOPTypeCheckCtx* c, uint32_t start, uint32_t end) {
    return HOPTCNameEqLiteralOrPkgBuiltin(c, start, end, "source_location_of", "builtin");
}

int32_t HOPTCFindSourceLocationType(HOPTypeCheckCtx* c) {
    uint32_t i;
    int32_t  direct = HOPTCFindNamedTypeByLiteral(c, "builtin__SourceLocation");
    if (direct >= 0) {
        return direct;
    }
    for (i = 0; i < c->typeLen; i++) {
        const HOPTCType* t = &c->types[i];
        if (t->kind != HOPTCType_NAMED) {
            continue;
        }
        if (!HOPNameHasPrefix(c->src, t->nameStart, t->nameEnd, "builtin")
            || !HOPNameHasSuffix(c->src, t->nameStart, t->nameEnd, "__SourceLocation"))
        {
            continue;
        }
        if (t->declNode >= 0 && (uint32_t)t->declNode < c->ast->len
            && c->ast->nodes[t->declNode].kind == HOPAst_STRUCT)
        {
            return (int32_t)i;
        }
    }
    return -1;
}

int32_t HOPTCFindFmtValueType(HOPTypeCheckCtx* c) {
    uint32_t i;
    int32_t  direct = HOPTCFindNamedTypeByLiteral(c, "builtin__FmtValue");
    if (direct >= 0) {
        return direct;
    }
    for (i = 0; i < c->typeLen; i++) {
        const HOPTCType* t = &c->types[i];
        if (t->kind != HOPTCType_NAMED) {
            continue;
        }
        if (!HOPNameHasPrefix(c->src, t->nameStart, t->nameEnd, "builtin")
            || !HOPNameHasSuffix(c->src, t->nameStart, t->nameEnd, "__FmtValue"))
        {
            continue;
        }
        if (t->declNode >= 0 && (uint32_t)t->declNode < c->ast->len
            && c->ast->nodes[t->declNode].kind == HOPAst_STRUCT)
        {
            return (int32_t)i;
        }
    }
    return -1;
}

int HOPTCTypeIsSourceLocation(HOPTypeCheckCtx* c, int32_t typeId) {
    int32_t locationType;
    if (c == NULL || typeId < 0) {
        return 0;
    }
    if (c->typeSourceLocation < 0) {
        c->typeSourceLocation = HOPTCFindSourceLocationType(c);
    }
    locationType = c->typeSourceLocation;
    if (locationType < 0) {
        return 0;
    }
    typeId = HOPTCResolveAliasBaseType(c, typeId);
    return typeId == locationType;
}

int HOPTCTypeIsFmtValue(HOPTypeCheckCtx* c, int32_t typeId) {
    const HOPTCType* t;
    if (c == NULL || typeId < 0) {
        return 0;
    }
    typeId = HOPTCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return 0;
    }
    t = &c->types[typeId];
    if (t->kind != HOPTCType_NAMED) {
        return 0;
    }
    if (!HOPNameHasSuffix(c->src, t->nameStart, t->nameEnd, "__FmtValue")) {
        return 0;
    }
    return t->declNode >= 0 && (uint32_t)t->declNode < c->ast->len
        && c->ast->nodes[t->declNode].kind == HOPAst_STRUCT;
}

int32_t HOPTCFindFunctionIndex(HOPTypeCheckCtx* c, uint32_t start, uint32_t end) {
    uint32_t i;
    for (i = 0; i < c->funcLen; i++) {
        if (HOPNameEqSlice(c->src, c->funcs[i].nameStart, c->funcs[i].nameEnd, start, end)) {
            return (int32_t)i;
        }
    }
    return -1;
}

int32_t HOPTCFindBuiltinQualifiedFunctionIndex(HOPTypeCheckCtx* c, uint32_t start, uint32_t end) {
    uint32_t i;
    size_t   nameLen;
    if (c == NULL || end <= start || end > c->src.len) {
        return -1;
    }
    nameLen = (size_t)(end - start);
    for (i = 0; i < c->funcLen; i++) {
        const HOPTCFunction* fn = &c->funcs[i];
        uint32_t             candLen;
        if (fn->nameEnd <= fn->nameStart) {
            continue;
        }
        candLen = fn->nameEnd - fn->nameStart;
        if (candLen != 9u + (uint32_t)nameLen) {
            continue;
        }
        if (memcmp(c->src.ptr + fn->nameStart, "builtin__", 9u) != 0) {
            continue;
        }
        if (memcmp(c->src.ptr + fn->nameStart + 9u, c->src.ptr + start, nameLen) == 0) {
            return (int32_t)i;
        }
    }
    return -1;
}

static int HOPTCFunctionSupportsFunctionValue(const HOPTypeCheckCtx* c, const HOPTCFunction* fn) {
    if (c == NULL || fn == NULL) {
        return 0;
    }
    if (fn->contextType >= 0 || (fn->flags & HOPTCFunctionFlag_VARIADIC) != 0) {
        return 0;
    }
    if (fn->defNode >= 0) {
        return 1;
    }
    return fn->declNode >= 0 && HOPTCHasForeignImportDirective(c->ast, c->src, fn->declNode);
}

int32_t HOPTCFindPlainFunctionValueIndex(HOPTypeCheckCtx* c, uint32_t start, uint32_t end) {
    uint32_t i;
    int32_t  found = -1;
    for (i = 0; i < c->funcLen; i++) {
        const HOPTCFunction* fn = &c->funcs[i];
        if (!HOPNameEqSlice(c->src, fn->nameStart, fn->nameEnd, start, end)) {
            continue;
        }
        if (!HOPTCFunctionSupportsFunctionValue(c, fn)) {
            continue;
        }
        if (found >= 0) {
            return -1;
        }
        found = (int32_t)i;
    }
    if (found < 0) {
        int32_t builtinFn = HOPTCFindBuiltinQualifiedFunctionIndex(c, start, end);
        if (builtinFn >= 0 && HOPTCFunctionSupportsFunctionValue(c, &c->funcs[(uint32_t)builtinFn]))
        {
            found = builtinFn;
        }
    }
    return found;
}

int32_t HOPTCFindPkgQualifiedFunctionValueIndex(
    HOPTypeCheckCtx* c, uint32_t pkgStart, uint32_t pkgEnd, uint32_t nameStart, uint32_t nameEnd) {
    int32_t  candidates[HOPTC_MAX_CALL_CANDIDATES];
    uint32_t candidateCount = 0;
    int      nameFound = 0;
    uint32_t i;
    int32_t  found = -1;
    HOPTCGatherCallCandidatesByPkgMethod(
        c, pkgStart, pkgEnd, nameStart, nameEnd, candidates, &candidateCount, &nameFound);
    if (!nameFound) {
        return -1;
    }
    for (i = 0; i < candidateCount; i++) {
        const HOPTCFunction* fn;
        int32_t              fnIndex = candidates[i];
        if (fnIndex < 0 || (uint32_t)fnIndex >= c->funcLen) {
            continue;
        }
        fn = &c->funcs[(uint32_t)fnIndex];
        if (!HOPTCFunctionSupportsFunctionValue(c, fn)) {
            continue;
        }
        if (found >= 0) {
            return -1;
        }
        found = fnIndex;
    }
    return found;
}

int HOPTCFunctionNameEq(
    const HOPTypeCheckCtx* c, uint32_t funcIndex, uint32_t start, uint32_t end) {
    return HOPNameEqSlice(
        c->src, c->funcs[funcIndex].nameStart, c->funcs[funcIndex].nameEnd, start, end);
}

int HOPTCNameEqPkgPrefixedMethod(
    HOPTypeCheckCtx* c,
    uint32_t         candidateStart,
    uint32_t         candidateEnd,
    uint32_t         pkgStart,
    uint32_t         pkgEnd,
    uint32_t         methodStart,
    uint32_t         methodEnd) {
    uint32_t pkgLen;
    uint32_t methodLen;
    if (candidateEnd < candidateStart || pkgEnd < pkgStart || methodEnd < methodStart) {
        return 0;
    }
    pkgLen = pkgEnd - pkgStart;
    methodLen = methodEnd - methodStart;
    if (candidateEnd - candidateStart != pkgLen + 2u + methodLen) {
        return 0;
    }
    if (!HOPNameEqSlice(c->src, candidateStart, candidateStart + pkgLen, pkgStart, pkgEnd)) {
        return 0;
    }
    if (c->src.ptr[candidateStart + pkgLen] != '_'
        || c->src.ptr[candidateStart + pkgLen + 1u] != '_')
    {
        return 0;
    }
    return HOPNameEqSlice(
        c->src, candidateStart + pkgLen + 2u, candidateEnd, methodStart, methodEnd);
}

int HOPTCExtractPkgPrefixFromTypeName(
    HOPTypeCheckCtx* c,
    uint32_t         typeNameStart,
    uint32_t         typeNameEnd,
    uint32_t*        outPkgStart,
    uint32_t*        outPkgEnd) {
    uint32_t i;
    if (typeNameEnd <= typeNameStart + 2u) {
        return 0;
    }
    for (i = typeNameStart; i + 1u < typeNameEnd; i++) {
        if (c->src.ptr[i] == '_' && c->src.ptr[i + 1u] == '_') {
            if (i == typeNameStart) {
                return 0;
            }
            *outPkgStart = typeNameStart;
            *outPkgEnd = i;
            return 1;
        }
    }
    return 0;
}

int HOPTCImportDefaultAliasEq(
    HOPStrView src, uint32_t pathStart, uint32_t pathEnd, uint32_t aliasStart, uint32_t aliasEnd) {
    uint32_t start;
    uint32_t end;
    uint32_t i;
    if (pathEnd <= pathStart + 2u || aliasEnd <= aliasStart || pathEnd > src.len
        || aliasEnd > src.len)
    {
        return 0;
    }
    if (src.ptr[pathStart] != '"' || src.ptr[pathEnd - 1u] != '"') {
        return 0;
    }
    start = pathStart + 1u;
    end = pathEnd - 1u;
    for (i = start; i < end; i++) {
        if (src.ptr[i] == '/') {
            start = i + 1u;
        }
    }
    return HOPNameEqSlice(src, start, end, aliasStart, aliasEnd);
}

int HOPTCHasImportAlias(HOPTypeCheckCtx* c, uint32_t aliasStart, uint32_t aliasEnd) {
    uint32_t nodeId;
    if (c == NULL || aliasEnd <= aliasStart || aliasEnd > c->src.len) {
        return 0;
    }
    for (nodeId = 0; nodeId < c->ast->len; nodeId++) {
        const HOPAstNode* importNode;
        int32_t           child;
        if (c->ast->nodes[nodeId].kind != HOPAst_IMPORT) {
            continue;
        }
        importNode = &c->ast->nodes[nodeId];
        child = importNode->firstChild;
        while (child >= 0) {
            const HOPAstNode* ch = &c->ast->nodes[child];
            if (ch->kind == HOPAst_IDENT) {
                if (HOPNameEqSlice(c->src, ch->dataStart, ch->dataEnd, aliasStart, aliasEnd)) {
                    return 1;
                }
                break;
            }
            child = ch->nextSibling;
        }
        if (HOPTCImportDefaultAliasEq(
                c->src, importNode->dataStart, importNode->dataEnd, aliasStart, aliasEnd))
        {
            return 1;
        }
    }
    return 0;
}

int HOPTCResolveReceiverPkgPrefix(
    HOPTypeCheckCtx* c, int32_t typeId, uint32_t* outPkgStart, uint32_t* outPkgEnd) {
    uint32_t depth = 0;
    while (typeId >= 0 && (uint32_t)typeId < c->typeLen && depth++ <= c->typeLen) {
        const HOPTCType* t = &c->types[typeId];
        switch (t->kind) {
            case HOPTCType_PTR:
            case HOPTCType_REF:
            case HOPTCType_OPTIONAL: typeId = t->baseType; continue;
            case HOPTCType_ALIAS:
                if (HOPTCResolveAliasTypeId(c, typeId) != 0) {
                    return -1;
                }
                if (HOPTCExtractPkgPrefixFromTypeName(
                        c, t->nameStart, t->nameEnd, outPkgStart, outPkgEnd))
                {
                    return 1;
                }
                typeId = t->baseType;
                continue;
            case HOPTCType_NAMED:
                return HOPTCExtractPkgPrefixFromTypeName(
                    c, t->nameStart, t->nameEnd, outPkgStart, outPkgEnd);
            default: return 0;
        }
    }
    return 0;
}

static int HOPTCResolveTypePathExprTypeForEnumMember(
    HOPTypeCheckCtx* c, int32_t exprNode, int32_t ownerTypeId, int32_t* outTypeId) {
    const HOPAstNode* n;
    if (exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return 0;
    }
    n = &c->ast->nodes[exprNode];
    if (n->kind == HOPAst_IDENT) {
        int32_t typeId;
        if (HOPTCLocalFind(c, n->dataStart, n->dataEnd) >= 0
            || HOPTCFindFunctionIndex(c, n->dataStart, n->dataEnd) >= 0)
        {
            return 0;
        }
        typeId = HOPTCResolveTypeNamePath(c, n->dataStart, n->dataEnd, ownerTypeId);
        if (typeId < 0) {
            return 0;
        }
        *outTypeId = typeId;
        return 1;
    }
    if (n->kind == HOPAst_FIELD_EXPR) {
        int32_t recvExpr = HOPAstFirstChild(c->ast, exprNode);
        int32_t recvTypeId;
        int32_t idx;
        if (recvExpr < 0
            || !HOPTCResolveTypePathExprTypeForEnumMember(c, recvExpr, ownerTypeId, &recvTypeId))
        {
            return 0;
        }
        idx = HOPTCFindNamedTypeIndexOwned(c, recvTypeId, n->dataStart, n->dataEnd);
        if (idx < 0) {
            return 0;
        }
        *outTypeId = c->namedTypes[(uint32_t)idx].typeId;
        return 1;
    }
    return 0;
}

int HOPTCResolveEnumMemberType(
    HOPTypeCheckCtx* c,
    int32_t          recvNode,
    uint32_t         memberStart,
    uint32_t         memberEnd,
    int32_t*         outType) {
    int32_t enumTypeId;
    int32_t enumFieldType;

    if (recvNode < 0 || (uint32_t)recvNode >= c->ast->len) {
        return 0;
    }
    if (!HOPTCResolveTypePathExprTypeForEnumMember(
            c, recvNode, c->currentTypeOwnerTypeId, &enumTypeId))
    {
        return 0;
    }
    enumTypeId = HOPTCResolveAliasBaseType(c, enumTypeId);
    if (!HOPTCIsNamedDeclKind(c, enumTypeId, HOPAst_ENUM)) {
        return 0;
    }

    if (HOPTCFieldLookup(c, enumTypeId, memberStart, memberEnd, &enumFieldType, NULL) != 0) {
        return 0;
    }
    *outType = enumFieldType;
    return 1;
}

int32_t HOPTCInternPtrType(
    HOPTypeCheckCtx* c, int32_t baseType, uint32_t errStart, uint32_t errEnd) {
    uint32_t  i;
    HOPTCType t;
    for (i = 0; i < c->typeLen; i++) {
        if (c->types[i].kind == HOPTCType_PTR && c->types[i].baseType == baseType) {
            return (int32_t)i;
        }
    }
    t.kind = HOPTCType_PTR;
    t.builtin = HOPBuiltin_INVALID;
    t.baseType = baseType;
    t.declNode = -1;
    t.funcIndex = -1;
    t.arrayLen = 0;
    t.nameStart = 0;
    t.nameEnd = 0;
    t.fieldStart = 0;
    t.fieldCount = 0;
    t.flags = 0;
    return HOPTCAddType(c, &t, errStart, errEnd);
}

int HOPTCTypeIsMutable(const HOPTCType* t) {
    if (t->kind == HOPTCType_PTR) {
        return 1;
    }
    return (t->flags & HOPTCTypeFlag_MUTABLE) != 0;
}

int HOPTCIsMutableRefType(HOPTypeCheckCtx* c, int32_t typeId) {
    return typeId >= 0 && (uint32_t)typeId < c->typeLen
        && (c->types[typeId].kind == HOPTCType_PTR
            || (c->types[typeId].kind == HOPTCType_REF && HOPTCTypeIsMutable(&c->types[typeId])));
}

int32_t HOPTCInternRefType(
    HOPTypeCheckCtx* c, int32_t baseType, int isMutable, uint32_t errStart, uint32_t errEnd) {
    uint32_t  i;
    HOPTCType t;
    for (i = 0; i < c->typeLen; i++) {
        if (c->types[i].kind == HOPTCType_REF && c->types[i].baseType == baseType
            && HOPTCTypeIsMutable(&c->types[i]) == isMutable)
        {
            return (int32_t)i;
        }
    }
    t.kind = HOPTCType_REF;
    t.builtin = HOPBuiltin_INVALID;
    t.baseType = baseType;
    t.declNode = -1;
    t.funcIndex = -1;
    t.arrayLen = 0;
    t.nameStart = 0;
    t.nameEnd = 0;
    t.fieldStart = 0;
    t.fieldCount = 0;
    t.flags = isMutable ? HOPTCTypeFlag_MUTABLE : 0;
    return HOPTCAddType(c, &t, errStart, errEnd);
}

int32_t HOPTCInternArrayType(
    HOPTypeCheckCtx* c, int32_t baseType, uint32_t arrayLen, uint32_t errStart, uint32_t errEnd) {
    uint32_t  i;
    HOPTCType t;
    for (i = 0; i < c->typeLen; i++) {
        if (c->types[i].kind == HOPTCType_ARRAY && c->types[i].baseType == baseType
            && c->types[i].arrayLen == arrayLen)
        {
            return (int32_t)i;
        }
    }
    t.kind = HOPTCType_ARRAY;
    t.builtin = HOPBuiltin_INVALID;
    t.baseType = baseType;
    t.declNode = -1;
    t.funcIndex = -1;
    t.arrayLen = arrayLen;
    t.nameStart = 0;
    t.nameEnd = 0;
    t.fieldStart = 0;
    t.fieldCount = 0;
    t.flags = 0;
    return HOPTCAddType(c, &t, errStart, errEnd);
}

int32_t HOPTCInternSliceType(
    HOPTypeCheckCtx* c, int32_t baseType, int isMutable, uint32_t errStart, uint32_t errEnd) {
    uint32_t  i;
    HOPTCType t;
    for (i = 0; i < c->typeLen; i++) {
        if (c->types[i].kind == HOPTCType_SLICE && c->types[i].baseType == baseType
            && HOPTCTypeIsMutable(&c->types[i]) == isMutable)
        {
            return (int32_t)i;
        }
    }
    t.kind = HOPTCType_SLICE;
    t.builtin = HOPBuiltin_INVALID;
    t.baseType = baseType;
    t.declNode = -1;
    t.funcIndex = -1;
    t.arrayLen = 0;
    t.nameStart = 0;
    t.nameEnd = 0;
    t.fieldStart = 0;
    t.fieldCount = 0;
    t.flags = isMutable ? HOPTCTypeFlag_MUTABLE : 0;
    return HOPTCAddType(c, &t, errStart, errEnd);
}

int32_t HOPTCInternOptionalType(
    HOPTypeCheckCtx* c, int32_t baseType, uint32_t errStart, uint32_t errEnd) {
    uint32_t  i;
    HOPTCType t;
    for (i = 0; i < c->typeLen; i++) {
        if (c->types[i].kind == HOPTCType_OPTIONAL && c->types[i].baseType == baseType) {
            return (int32_t)i;
        }
    }
    t.kind = HOPTCType_OPTIONAL;
    t.builtin = HOPBuiltin_INVALID;
    t.baseType = baseType;
    t.declNode = -1;
    t.funcIndex = -1;
    t.arrayLen = 0;
    t.nameStart = 0;
    t.nameEnd = 0;
    t.fieldStart = 0;
    t.fieldCount = 0;
    t.flags = 0;
    return HOPTCAddType(c, &t, errStart, errEnd);
}

int32_t HOPTCInternAnonAggregateType(
    HOPTypeCheckCtx*         c,
    int                      isUnion,
    const HOPTCAnonFieldSig* fields,
    uint32_t                 fieldCount,
    int32_t                  declNode,
    uint32_t                 errStart,
    uint32_t                 errEnd) {
    uint32_t  i;
    HOPTCType t;

    if (fieldCount > UINT16_MAX) {
        return HOPTCFailSpan(c, HOPDiag_ARENA_OOM, errStart, errEnd);
    }

    for (i = 0; i < c->typeLen; i++) {
        const HOPTCType* ct = &c->types[i];
        uint32_t         j;
        int              same = 1;
        if ((isUnion ? HOPTCType_ANON_UNION : HOPTCType_ANON_STRUCT) != ct->kind
            || ct->fieldCount != fieldCount)
        {
            continue;
        }
        for (j = 0; j < fieldCount; j++) {
            const HOPTCField* f = &c->fields[ct->fieldStart + j];
            if (f->typeId != fields[j].typeId
                || !HOPNameEqSlice(
                    c->src, f->nameStart, f->nameEnd, fields[j].nameStart, fields[j].nameEnd))
            {
                same = 0;
                break;
            }
        }
        if (same) {
            return (int32_t)i;
        }
    }

    if (c->fieldLen + fieldCount > c->fieldCap) {
        return HOPTCFailSpan(c, HOPDiag_ARENA_OOM, errStart, errEnd);
    }

    t.kind = isUnion ? HOPTCType_ANON_UNION : HOPTCType_ANON_STRUCT;
    t.builtin = HOPBuiltin_INVALID;
    t.baseType = -1;
    t.declNode = declNode;
    t.funcIndex = -1;
    t.arrayLen = 0;
    t.nameStart = 0;
    t.nameEnd = 0;
    t.fieldStart = c->fieldLen;
    t.fieldCount = (uint16_t)fieldCount;
    t.flags = 0;
    {
        int32_t  typeId = HOPTCAddType(c, &t, errStart, errEnd);
        uint32_t j;
        if (typeId < 0) {
            return -1;
        }
        for (j = 0; j < fieldCount; j++) {
            c->fields[c->fieldLen].nameStart = fields[j].nameStart;
            c->fields[c->fieldLen].nameEnd = fields[j].nameEnd;
            c->fields[c->fieldLen].typeId = fields[j].typeId;
            c->fields[c->fieldLen].lenNameStart = 0;
            c->fields[c->fieldLen].lenNameEnd = 0;
            c->fields[c->fieldLen].flags = 0;
            c->fieldLen++;
        }
        return typeId;
    }
}

int HOPTCFunctionTypeMatchesSignature(
    HOPTypeCheckCtx* c,
    const HOPTCType* t,
    int32_t          returnType,
    const int32_t*   paramTypes,
    const uint8_t*   paramFlags,
    uint32_t         paramCount,
    int              isVariadic) {
    uint32_t i;
    if (t->kind != HOPTCType_FUNCTION || t->baseType != returnType || t->fieldCount != paramCount) {
        return 0;
    }
    if (((t->flags & HOPTCTypeFlag_FUNCTION_VARIADIC) != 0) != (isVariadic != 0)) {
        return 0;
    }
    for (i = 0; i < paramCount; i++) {
        if (c->funcParamTypes[t->fieldStart + i] != paramTypes[i]) {
            return 0;
        }
        if ((c->funcParamFlags[t->fieldStart + i] & HOPTCFuncParamFlag_CONST)
            != ((paramFlags != NULL ? paramFlags[i] : 0u) & HOPTCFuncParamFlag_CONST))
        {
            return 0;
        }
    }
    return 1;
}

int32_t HOPTCInternFunctionType(
    HOPTypeCheckCtx* c,
    int32_t          returnType,
    const int32_t*   paramTypes,
    const uint8_t*   paramFlags,
    uint32_t         paramCount,
    int              isVariadic,
    int32_t          funcIndex,
    uint32_t         errStart,
    uint32_t         errEnd) {
    uint32_t  i;
    HOPTCType t;
    if (paramCount > UINT16_MAX) {
        return HOPTCFailSpan(c, HOPDiag_ARENA_OOM, errStart, errEnd);
    }
    for (i = 0; i < c->typeLen; i++) {
        if (HOPTCFunctionTypeMatchesSignature(
                c, &c->types[i], returnType, paramTypes, paramFlags, paramCount, isVariadic))
        {
            if (funcIndex >= 0 && c->types[i].funcIndex < 0) {
                c->types[i].funcIndex = funcIndex;
            }
            return (int32_t)i;
        }
    }
    if (c->funcParamLen + paramCount > c->funcParamCap) {
        return HOPTCFailSpan(c, HOPDiag_ARENA_OOM, errStart, errEnd);
    }
    t.kind = HOPTCType_FUNCTION;
    t.builtin = HOPBuiltin_INVALID;
    t.baseType = returnType;
    t.declNode = -1;
    t.funcIndex = funcIndex;
    t.arrayLen = 0;
    t.nameStart = 0;
    t.nameEnd = 0;
    t.fieldStart = c->funcParamLen;
    t.fieldCount = (uint16_t)paramCount;
    t.flags = (uint16_t)(isVariadic ? HOPTCTypeFlag_FUNCTION_VARIADIC : 0u);
    for (i = 0; i < paramCount; i++) {
        c->funcParamTypes[c->funcParamLen++] = paramTypes[i];
        c->funcParamNameStarts[c->funcParamLen - 1u] = 0;
        c->funcParamNameEnds[c->funcParamLen - 1u] = 0;
        c->funcParamFlags[c->funcParamLen - 1u] =
            paramFlags != NULL ? (paramFlags[i] & HOPTCFuncParamFlag_CONST) : 0u;
    }
    return HOPTCAddType(c, &t, errStart, errEnd);
}

int HOPTCTupleTypeMatchesSignature(
    HOPTypeCheckCtx* c, const HOPTCType* t, const int32_t* elemTypes, uint32_t elemCount) {
    uint32_t i;
    if (t->kind != HOPTCType_TUPLE || t->fieldCount != elemCount) {
        return 0;
    }
    for (i = 0; i < elemCount; i++) {
        if (c->funcParamTypes[t->fieldStart + i] != elemTypes[i]) {
            return 0;
        }
    }
    return 1;
}

int32_t HOPTCInternTupleType(
    HOPTypeCheckCtx* c,
    const int32_t*   elemTypes,
    uint32_t         elemCount,
    uint32_t         errStart,
    uint32_t         errEnd) {
    uint32_t  i;
    HOPTCType t;
    if (elemCount < 2u || elemCount > UINT16_MAX) {
        return HOPTCFailSpan(c, HOPDiag_EXPECTED_TYPE, errStart, errEnd);
    }
    for (i = 0; i < c->typeLen; i++) {
        if (HOPTCTupleTypeMatchesSignature(c, &c->types[i], elemTypes, elemCount)) {
            return (int32_t)i;
        }
    }
    if (c->funcParamLen + elemCount > c->funcParamCap) {
        return HOPTCFailSpan(c, HOPDiag_ARENA_OOM, errStart, errEnd);
    }
    t.kind = HOPTCType_TUPLE;
    t.builtin = HOPBuiltin_INVALID;
    t.baseType = -1;
    t.declNode = -1;
    t.funcIndex = -1;
    t.arrayLen = 0;
    t.nameStart = 0;
    t.nameEnd = 0;
    t.fieldStart = c->funcParamLen;
    t.fieldCount = (uint16_t)elemCount;
    t.flags = 0;
    for (i = 0; i < elemCount; i++) {
        c->funcParamTypes[c->funcParamLen] = elemTypes[i];
        c->funcParamNameStarts[c->funcParamLen] = 0;
        c->funcParamNameEnds[c->funcParamLen] = 0;
        c->funcParamFlags[c->funcParamLen] = 0;
        c->funcParamLen++;
    }
    return HOPTCAddType(c, &t, errStart, errEnd);
}

int32_t HOPTCInternPackType(
    HOPTypeCheckCtx* c,
    const int32_t*   elemTypes,
    uint32_t         elemCount,
    uint32_t         errStart,
    uint32_t         errEnd) {
    uint32_t  i;
    uint32_t  j;
    HOPTCType t;
    for (i = 0; i < c->typeLen; i++) {
        if (c->types[i].kind == HOPTCType_PACK && c->types[i].fieldCount == elemCount) {
            int same = 1;
            for (j = 0; j < elemCount; j++) {
                if (c->funcParamTypes[c->types[i].fieldStart + j] != elemTypes[j]) {
                    same = 0;
                    break;
                }
            }
            if (same) {
                return (int32_t)i;
            }
        }
    }
    if (elemCount > UINT16_MAX) {
        return HOPTCFailSpan(c, HOPDiag_ARENA_OOM, errStart, errEnd);
    }
    if (c->funcParamLen + elemCount > c->funcParamCap) {
        return HOPTCFailSpan(c, HOPDiag_ARENA_OOM, errStart, errEnd);
    }
    t.kind = HOPTCType_PACK;
    t.builtin = HOPBuiltin_INVALID;
    t.baseType = -1;
    t.declNode = -1;
    t.funcIndex = -1;
    t.arrayLen = 0;
    t.nameStart = 0;
    t.nameEnd = 0;
    t.fieldStart = c->funcParamLen;
    t.fieldCount = (uint16_t)elemCount;
    t.flags = 0;
    for (i = 0; i < elemCount; i++) {
        c->funcParamTypes[c->funcParamLen] = elemTypes[i];
        c->funcParamNameStarts[c->funcParamLen] = 0;
        c->funcParamNameEnds[c->funcParamLen] = 0;
        c->funcParamFlags[c->funcParamLen] = 0;
        c->funcParamLen++;
    }
    return HOPTCAddType(c, &t, errStart, errEnd);
}

int HOPTCParseArrayLen(HOPTypeCheckCtx* c, const HOPAstNode* node, uint32_t* outLen) {
    uint32_t i;
    uint32_t v = 0;
    if (node->dataEnd <= node->dataStart) {
        return -1;
    }
    for (i = node->dataStart; i < node->dataEnd; i++) {
        unsigned char ch = (unsigned char)c->src.ptr[i];
        if (ch < (unsigned char)'0' || ch > (unsigned char)'9') {
            return -1;
        }
        v = v * 10u + (uint32_t)(ch - (unsigned char)'0');
    }
    *outLen = v;
    return 0;
}

int HOPTCResolveIndexBaseInfo(HOPTypeCheckCtx* c, int32_t baseType, HOPTCIndexBaseInfo* out) {
    const HOPTCType* t;
    int32_t          resolvedBaseType;
    out->elemType = -1;
    out->indexable = 0;
    out->sliceable = 0;
    out->sliceMutable = 0;
    out->isStringLike = 0;
    out->hasKnownLen = 0;
    out->knownLen = 0;

    if (baseType < 0 || (uint32_t)baseType >= c->typeLen) {
        return -1;
    }
    resolvedBaseType = HOPTCResolveAliasBaseType(c, baseType);
    if (resolvedBaseType == c->typeStr) {
        int32_t u8Type = HOPTCFindBuiltinByKind(c, HOPBuiltin_U8);
        if (u8Type < 0) {
            return -1;
        }
        out->elemType = u8Type;
        out->indexable = 1;
        out->sliceable = 1;
        out->isStringLike = 1;
        return 0;
    }
    t = &c->types[baseType];
    switch (t->kind) {
        case HOPTCType_ARRAY:
            out->elemType = t->baseType;
            out->indexable = 1;
            out->sliceable = 1;
            out->sliceMutable = 1;
            out->hasKnownLen = 1;
            out->knownLen = t->arrayLen;
            return 0;
        case HOPTCType_SLICE:
            out->elemType = t->baseType;
            out->indexable = 1;
            out->sliceable = 1;
            out->sliceMutable = HOPTCTypeIsMutable(t);
            return 0;
        case HOPTCType_PTR: {
            int32_t pointee = t->baseType;
            int32_t resolvedPointee;
            if (pointee < 0 || (uint32_t)pointee >= c->typeLen) {
                return -1;
            }
            resolvedPointee = HOPTCResolveAliasBaseType(c, pointee);
            if (resolvedPointee == c->typeStr) {
                int32_t u8Type = HOPTCFindBuiltinByKind(c, HOPBuiltin_U8);
                if (u8Type < 0) {
                    return -1;
                }
                out->elemType = u8Type;
                out->indexable = 1;
                out->sliceable = 1;
                out->sliceMutable = 1;
                out->isStringLike = 1;
                return 0;
            }
            if (c->types[pointee].kind == HOPTCType_ARRAY) {
                out->elemType = c->types[pointee].baseType;
                out->indexable = 1;
                out->sliceable = 1;
                out->sliceMutable = 1;
                out->hasKnownLen = 1;
                out->knownLen = c->types[pointee].arrayLen;
                return 0;
            }
            if (c->types[pointee].kind == HOPTCType_SLICE) {
                out->elemType = c->types[pointee].baseType;
                out->indexable = 1;
                out->sliceable = 1;
                out->sliceMutable = HOPTCTypeIsMutable(&c->types[pointee]);
                return 0;
            }
            out->elemType = pointee;
            out->indexable = 1;
            out->sliceMutable = 1;
            return 0;
        }
        case HOPTCType_REF: {
            int32_t refBase = t->baseType;
            int32_t resolvedRefBase;
            if (refBase < 0 || (uint32_t)refBase >= c->typeLen) {
                return -1;
            }
            resolvedRefBase = HOPTCResolveAliasBaseType(c, refBase);
            if (resolvedRefBase == c->typeStr) {
                int32_t u8Type = HOPTCFindBuiltinByKind(c, HOPBuiltin_U8);
                if (u8Type < 0) {
                    return -1;
                }
                out->elemType = u8Type;
                out->indexable = 1;
                out->sliceable = 1;
                out->sliceMutable = HOPTCTypeIsMutable(t);
                out->isStringLike = 1;
                return 0;
            }
            if (c->types[refBase].kind == HOPTCType_ARRAY) {
                out->elemType = c->types[refBase].baseType;
                out->indexable = 1;
                out->sliceable = 1;
                out->sliceMutable = HOPTCTypeIsMutable(t);
                out->hasKnownLen = 1;
                out->knownLen = c->types[refBase].arrayLen;
                return 0;
            }
            if (c->types[refBase].kind == HOPTCType_SLICE) {
                out->elemType = c->types[refBase].baseType;
                out->indexable = 1;
                out->sliceable = 1;
                out->sliceMutable = HOPTCTypeIsMutable(t) && HOPTCTypeIsMutable(&c->types[refBase]);
                return 0;
            }
            out->elemType = refBase;
            out->indexable = 1;
            out->sliceMutable = HOPTCTypeIsMutable(t);
            return 0;
        }
        default: return 0;
    }
}

int32_t HOPTCListItemAt(const HOPAst* ast, int32_t listNode, uint32_t index) {
    int32_t  child;
    uint32_t i = 0;
    if (listNode < 0 || (uint32_t)listNode >= ast->len) {
        return -1;
    }
    child = ast->nodes[listNode].firstChild;
    while (child >= 0) {
        if (i == index) {
            return child;
        }
        child = ast->nodes[child].nextSibling;
        i++;
    }
    return -1;
}

uint32_t HOPTCListCount(const HOPAst* ast, int32_t listNode) {
    uint32_t count = 0;
    int32_t  child;
    if (listNode < 0 || (uint32_t)listNode >= ast->len) {
        return 0;
    }
    child = ast->nodes[listNode].firstChild;
    while (child >= 0) {
        count++;
        child = ast->nodes[child].nextSibling;
    }
    return count;
}

int HOPTCHasForeignImportDirective(const HOPAst* ast, HOPStrView src, int32_t nodeId) {
    int32_t child;
    int32_t first = -1;
    if (ast == NULL || nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return 0;
    }
    child = HOPAstFirstChild(ast, ast->root);
    while (child >= 0) {
        const HOPAstNode* n = &ast->nodes[child];
        if (n->kind == HOPAst_DIRECTIVE) {
            if (first < 0) {
                first = child;
            }
        } else {
            if (child == nodeId && first >= 0) {
                int32_t dir = first;
                while (dir >= 0 && ast->nodes[dir].kind == HOPAst_DIRECTIVE) {
                    const HOPAstNode* dn = &ast->nodes[dir];
                    uint32_t          len = dn->dataEnd - dn->dataStart;
                    if ((len == 8u && memcmp(src.ptr + dn->dataStart, "c_import", 8u) == 0)
                        || (len == 11u && memcmp(src.ptr + dn->dataStart, "wasm_import", 11u) == 0))
                    {
                        return 1;
                    }
                    dir = HOPAstNextSibling(ast, dir);
                }
                return 0;
            }
            first = -1;
        }
        child = HOPAstNextSibling(ast, child);
    }
    return 0;
}

int HOPTCVarLikeGetParts(HOPTypeCheckCtx* c, int32_t nodeId, HOPTCVarLikeParts* out) {
    int32_t           firstChild;
    const HOPAstNode* firstNode;
    out->nameListNode = -1;
    out->typeNode = -1;
    out->initNode = -1;
    out->nameCount = 0;
    out->grouped = 0;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return -1;
    }
    firstChild = HOPAstFirstChild(c->ast, nodeId);
    if (firstChild < 0) {
        return 0;
    }
    firstNode = &c->ast->nodes[firstChild];
    if (firstNode->kind == HOPAst_NAME_LIST) {
        int32_t afterNames = HOPAstNextSibling(c->ast, firstChild);
        out->grouped = 1;
        out->nameListNode = firstChild;
        out->nameCount = HOPTCListCount(c->ast, firstChild);
        if (afterNames >= 0 && HOPTCIsTypeNodeKind(c->ast->nodes[afterNames].kind)) {
            out->typeNode = afterNames;
            out->initNode = HOPAstNextSibling(c->ast, afterNames);
        } else {
            out->initNode = afterNames;
        }
        return 0;
    }
    out->grouped = 0;
    out->nameCount = 1;
    if (HOPTCIsTypeNodeKind(firstNode->kind)) {
        out->typeNode = firstChild;
        out->initNode = HOPAstNextSibling(c->ast, firstChild);
    } else {
        out->initNode = firstChild;
    }
    return 0;
}

int32_t HOPTCVarLikeNameIndexBySlice(
    HOPTypeCheckCtx* c, int32_t nodeId, uint32_t start, uint32_t end) {
    HOPTCVarLikeParts parts;
    const HOPAstNode* n;
    uint32_t          i;
    if (HOPTCVarLikeGetParts(c, nodeId, &parts) != 0 || parts.nameCount == 0) {
        return -1;
    }
    n = &c->ast->nodes[nodeId];
    if (!parts.grouped) {
        return HOPNameEqSlice(c->src, n->dataStart, n->dataEnd, start, end) ? 0 : -1;
    }
    for (i = 0; i < parts.nameCount; i++) {
        int32_t item = HOPTCListItemAt(c->ast, parts.nameListNode, i);
        if (item >= 0
            && HOPNameEqSlice(
                c->src, c->ast->nodes[item].dataStart, c->ast->nodes[item].dataEnd, start, end))
        {
            return (int32_t)i;
        }
    }
    return -1;
}

int32_t HOPTCVarLikeInitExprNodeAt(HOPTypeCheckCtx* c, int32_t nodeId, int32_t nameIndex) {
    HOPTCVarLikeParts parts;
    uint32_t          initCount;
    int32_t           onlyInit;
    if (HOPTCVarLikeGetParts(c, nodeId, &parts) != 0) {
        return -1;
    }
    if (!parts.grouped) {
        return (nameIndex == 0) ? parts.initNode : -1;
    }
    if (parts.initNode < 0 || c->ast->nodes[parts.initNode].kind != HOPAst_EXPR_LIST
        || nameIndex < 0)
    {
        return -1;
    }
    initCount = HOPTCListCount(c->ast, parts.initNode);
    if (initCount == parts.nameCount) {
        return HOPTCListItemAt(c->ast, parts.initNode, (uint32_t)nameIndex);
    }
    if (initCount != 1u) {
        return -1;
    }
    onlyInit = HOPTCListItemAt(c->ast, parts.initNode, 0);
    if (onlyInit < 0 || (uint32_t)onlyInit >= c->ast->len
        || c->ast->nodes[onlyInit].kind != HOPAst_TUPLE_EXPR)
    {
        return -1;
    }
    return HOPTCListItemAt(c->ast, onlyInit, (uint32_t)nameIndex);
}

int32_t HOPTCVarLikeInitExprNode(HOPTypeCheckCtx* c, int32_t nodeId) {
    return HOPTCVarLikeInitExprNodeAt(c, nodeId, 0);
}

HOP_API_END
