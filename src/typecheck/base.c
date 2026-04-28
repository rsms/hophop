#include "internal.h"

H2_API_BEGIN

static int32_t H2TCFindNamedTypeIndexByTypeId(H2TypeCheckCtx* c, int32_t typeId);

void H2TCSetDiag(H2Diag* diag, H2DiagCode code, uint32_t start, uint32_t end) {
    if (diag == NULL) {
        return;
    }
    H2DiagReset(diag, code);
    diag->start = start;
    diag->end = end;
    diag->phase = H2DiagPhase_TYPECHECK;
    diag->groupId = 0;
    diag->isPrimary = 1;
    diag->_reserved[0] = 0;
    diag->_reserved[1] = 0;
    diag->_reserved[2] = 0;
    diag->notes = NULL;
    diag->notesLen = 0;
    diag->fixIts = NULL;
    diag->fixItsLen = 0;
    diag->expectations = NULL;
    diag->expectationsLen = 0;
}

void H2TCSetDiagWithArg(
    H2Diag*    diag,
    H2DiagCode code,
    uint32_t   start,
    uint32_t   end,
    uint32_t   argStart,
    uint32_t   argEnd) {
    if (diag == NULL) {
        return;
    }
    H2DiagReset(diag, code);
    diag->start = start;
    diag->end = end;
    diag->argStart = argStart;
    diag->argEnd = argEnd;
    diag->phase = H2DiagPhase_TYPECHECK;
    diag->groupId = 0;
    diag->isPrimary = 1;
    diag->_reserved[0] = 0;
    diag->_reserved[1] = 0;
    diag->_reserved[2] = 0;
    diag->notes = NULL;
    diag->notesLen = 0;
    diag->fixIts = NULL;
    diag->fixItsLen = 0;
    diag->expectations = NULL;
    diag->expectationsLen = 0;
}

void H2TCSetDiagWith2Args(
    H2Diag*    diag,
    H2DiagCode code,
    uint32_t   start,
    uint32_t   end,
    uint32_t   argStart,
    uint32_t   argEnd,
    uint32_t   arg2Start,
    uint32_t   arg2End) {
    H2TCSetDiagWithArg(diag, code, start, end, argStart, argEnd);
    if (diag == NULL) {
        return;
    }
    diag->arg2Start = arg2Start;
    diag->arg2End = arg2End;
}

int H2TCFailSpan(H2TypeCheckCtx* c, H2DiagCode code, uint32_t start, uint32_t end) {
    H2TCSetDiag(c->diag, code, start, end);
    return -1;
}

int H2TCFailDuplicateDefinition(
    H2TypeCheckCtx* c,
    uint32_t        nameStart,
    uint32_t        nameEnd,
    uint32_t        otherStart,
    uint32_t        otherEnd) {
    H2TCSetDiagWithArg(c->diag, H2Diag_DUPLICATE_SYMBOL, nameStart, nameEnd, nameStart, nameEnd);
    if (c->diag == NULL) {
        return -1;
    }
    c->diag->relatedStart = otherStart;
    c->diag->relatedEnd = otherEnd;
    (void)H2DiagAddNote(
        c->arena,
        c->diag,
        H2DiagNoteKind_PREVIOUS_DEFINITION,
        otherStart,
        otherEnd,
        "previous definition is here");
    return -1;
}

int H2TCFailNode(H2TypeCheckCtx* c, int32_t nodeId, H2DiagCode code) {
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return H2TCFailSpan(c, code, 0, 0);
    }
    return H2TCFailSpan(c, code, c->ast->nodes[nodeId].start, c->ast->nodes[nodeId].end);
}

const char* _Nullable H2TCAllocCStringBytes(H2TypeCheckCtx* c, const uint8_t* bytes, uint32_t len) {
    char* s;
    if (c == NULL) {
        return NULL;
    }
    s = (char*)H2ArenaAlloc(c->arena, len + 1u, 1u);
    if (s == NULL) {
        return NULL;
    }
    if (len > 0u && bytes != NULL) {
        memcpy(s, bytes, len);
    }
    s[len] = '\0';
    return s;
}

int H2TCStrEqNullable(const char* _Nullable a, const char* _Nullable b) {
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

int H2TCEmitWarningDiag(H2TypeCheckCtx* c, const H2Diag* diag) {
    uint32_t i;
    if (c == NULL || diag == NULL || diag->type != H2DiagType_WARNING) {
        return 0;
    }
    for (i = 0; i < c->warningDedupLen; i++) {
        const H2TCWarningDedup* seen = &c->warningDedup[i];
        if (seen->start == diag->start && seen->end == diag->end
            && H2TCStrEqNullable(seen->message, diag->detail))
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

int H2TCRecordConstDiagUse(H2TypeCheckCtx* c, int32_t nodeId) {
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
        return H2TCFailNode(c, nodeId, H2Diag_ARENA_OOM);
    }
    c->constDiagUses[c->constDiagUseLen].nodeId = nodeId;
    c->constDiagUses[c->constDiagUseLen].ownerFnIndex = c->currentFunctionIndex;
    c->constDiagUses[c->constDiagUseLen].executed = 0;
    c->constDiagUseLen++;
    return 0;
}

void H2TCMarkConstDiagUseExecuted(H2TypeCheckCtx* c, int32_t nodeId) {
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

void H2TCMarkConstDiagFnInvoked(H2TypeCheckCtx* c, int32_t fnIndex) {
    if (c == NULL || fnIndex < 0 || (uint32_t)fnIndex >= c->constDiagFnInvokedCap
        || c->constDiagFnInvoked == NULL)
    {
        return;
    }
    c->constDiagFnInvoked[fnIndex] = 1;
}

int H2TCValidateConstDiagUses(H2TypeCheckCtx* c) {
    uint32_t i;
    if (c == NULL) {
        return -1;
    }
    for (i = 0; i < c->constDiagUseLen; i++) {
        const H2TCConstDiagUse* use = &c->constDiagUses[i];
        if (use->ownerFnIndex >= 0) {
            if ((uint32_t)use->ownerFnIndex < c->constDiagFnInvokedCap
                && c->constDiagFnInvoked != NULL && c->constDiagFnInvoked[use->ownerFnIndex])
            {
                continue;
            }
        } else if (use->executed) {
            continue;
        }
        return H2TCFailNode(c, use->nodeId, H2Diag_CONSTEVAL_DIAG_NON_CONST_CONTEXT);
    }
    return 0;
}

int H2TCRecordCallTarget(H2TypeCheckCtx* c, int32_t callNode, int32_t targetFnIndex) {
    uint32_t i;
    if (c == NULL || callNode < 0 || targetFnIndex < 0 || (uint32_t)callNode >= c->ast->len
        || (uint32_t)targetFnIndex >= c->funcLen)
    {
        return -1;
    }
    for (i = 0; i < c->callTargetLen; i++) {
        H2TCCallTarget* target = &c->callTargets[i];
        if (target->callNode == callNode && target->ownerFnIndex == c->currentFunctionIndex) {
            target->targetFnIndex = targetFnIndex;
            return 0;
        }
    }
    if (c->callTargetLen >= c->callTargetCap || c->callTargets == NULL) {
        return H2TCFailNode(c, callNode, H2Diag_ARENA_OOM);
    }
    c->callTargets[c->callTargetLen++] = (H2TCCallTarget){
        .callNode = callNode,
        .ownerFnIndex = c->currentFunctionIndex,
        .targetFnIndex = targetFnIndex,
    };
    return 0;
}

int H2TCFindCallTarget(
    const H2TypeCheckCtx* c, int32_t ownerFnIndex, int32_t callNode, int32_t* outTargetFnIndex) {
    uint32_t i;
    if (outTargetFnIndex != NULL) {
        *outTargetFnIndex = -1;
    }
    if (c == NULL || callNode < 0 || outTargetFnIndex == NULL) {
        return 0;
    }
    for (i = 0; i < c->callTargetLen; i++) {
        const H2TCCallTarget* target = &c->callTargets[i];
        if (target->callNode == callNode && target->ownerFnIndex == ownerFnIndex) {
            *outTargetFnIndex = target->targetFnIndex;
            return 1;
        }
    }
    return 0;
}

static H2TCLocalUse* _Nullable H2TCGetLocalUseByIndex(H2TypeCheckCtx* c, int32_t localIdx) {
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

void H2TCMarkFunctionUsed(H2TypeCheckCtx* c, int32_t fnIndex) {
    const H2TCFunction* fn;
    uint32_t            i;
    if (c == NULL || fnIndex < 0 || (uint32_t)fnIndex >= c->funcUsedCap || c->funcUsed == NULL) {
        return;
    }
    c->funcUsed[fnIndex] = 1;
    if ((uint32_t)fnIndex >= c->funcLen) {
        return;
    }
    fn = &c->funcs[fnIndex];
    if ((fn->flags & H2TCFunctionFlag_TEMPLATE_INSTANCE) == 0) {
        return;
    }
    for (i = 0; i < c->funcLen; i++) {
        const H2TCFunction* cand = &c->funcs[i];
        if (cand->declNode == fn->declNode
            && (cand->flags & H2TCFunctionFlag_TEMPLATE_INSTANCE) == 0)
        {
            c->funcUsed[i] = 1;
        }
    }
}

void H2TCMarkLocalRead(H2TypeCheckCtx* c, int32_t localIdx) {
    H2TCLocalUse* use = H2TCGetLocalUseByIndex(c, localIdx);
    if (use == NULL) {
        return;
    }
    if (use->readCount < UINT32_MAX) {
        use->readCount++;
    }
}

void H2TCMarkLocalWrite(H2TypeCheckCtx* c, int32_t localIdx) {
    H2TCLocalUse* use = H2TCGetLocalUseByIndex(c, localIdx);
    if (use == NULL) {
        return;
    }
    if (use->writeCount < UINT32_MAX) {
        use->writeCount++;
    }
}

int H2TCTypeIsTrackedPtrRef(H2TypeCheckCtx* c, int32_t typeId) {
    int32_t         resolvedType;
    int32_t         baseType;
    const H2TCType* type;
    if (c == NULL || typeId < 0) {
        return 0;
    }
    resolvedType = H2TCResolveAliasBaseType(c, typeId);
    if (resolvedType < 0 || (uint32_t)resolvedType >= c->typeLen) {
        return 0;
    }
    type = &c->types[resolvedType];
    if (type->kind == H2TCType_REF && !H2TCTypeIsMutable(type)) {
        baseType = H2TCResolveAliasBaseType(c, type->baseType);
        if (baseType == c->typeStr) {
            return 0;
        }
        if (baseType >= 0 && (uint32_t)baseType < c->typeLen
            && c->types[baseType].kind == H2TCType_SLICE && !H2TCTypeIsMutable(&c->types[baseType]))
        {
            return 0;
        }
    }
    return type->kind == H2TCType_PTR || type->kind == H2TCType_REF;
}

void H2TCMarkLocalInitialized(H2TypeCheckCtx* c, int32_t localIdx) {
    if (c == NULL || localIdx < 0 || (uint32_t)localIdx >= c->localLen || c->locals == NULL) {
        return;
    }
    if (c->locals[localIdx].initState != H2TCLocalInit_UNTRACKED) {
        c->locals[localIdx].initState = H2TCLocalInit_INIT;
    }
}

int H2TCCheckLocalInitialized(H2TypeCheckCtx* c, int32_t localIdx, uint32_t start, uint32_t end) {
    H2TCLocal* local;
    H2DiagCode code;
    if (c == NULL || localIdx < 0 || (uint32_t)localIdx >= c->localLen || c->locals == NULL) {
        return 0;
    }
    local = &c->locals[localIdx];
    if (local->initState == H2TCLocalInit_UNTRACKED || local->initState == H2TCLocalInit_INIT) {
        return 0;
    }
    code = local->initState == H2TCLocalInit_MAYBE
             ? H2Diag_LOCAL_PTR_REF_MAYBE_UNINIT
             : H2Diag_LOCAL_PTR_REF_UNINIT;
    H2TCSetDiagWithArg(c->diag, code, start, end, local->nameStart, local->nameEnd);
    return -1;
}

int H2TCFailTopLevelPtrRefMissingInitializer(
    H2TypeCheckCtx* c, uint32_t start, uint32_t end, uint32_t nameStart, uint32_t nameEnd) {
    H2TCSetDiagWithArg(
        c->diag, H2Diag_TOPLEVEL_PTR_REF_INIT_REQUIRED, start, end, nameStart, nameEnd);
    return -1;
}

void H2TCUnmarkLocalRead(H2TypeCheckCtx* c, int32_t localIdx) {
    H2TCLocalUse* use = H2TCGetLocalUseByIndex(c, localIdx);
    if (use == NULL || use->readCount == 0) {
        return;
    }
    use->readCount--;
}

void H2TCSetLocalUsageKind(H2TypeCheckCtx* c, int32_t localIdx, uint8_t kind) {
    H2TCLocalUse* use = H2TCGetLocalUseByIndex(c, localIdx);
    if (use == NULL) {
        return;
    }
    use->kind = kind;
}

void H2TCSetLocalUsageSuppress(H2TypeCheckCtx* c, int32_t localIdx, int suppress) {
    H2TCLocalUse* use = H2TCGetLocalUseByIndex(c, localIdx);
    if (use == NULL) {
        return;
    }
    use->suppressWarning = suppress ? 1u : 0u;
}

static int H2TCFunctionNameHasPrefix(
    H2TypeCheckCtx* c, const H2TCFunction* fn, const char* prefix) {
    uint32_t prefixLen = 0;
    if (c == NULL || fn == NULL || prefix == NULL || fn->nameEnd < fn->nameStart) {
        return 0;
    }
    while (prefix[prefixLen] != '\0') {
        prefixLen++;
    }
    if (fn->nameEnd - fn->nameStart < prefixLen) {
        return 0;
    }
    return memcmp(c->src.ptr + fn->nameStart, prefix, prefixLen) == 0;
}

int H2TCEmitUnusedSymbolWarnings(H2TypeCheckCtx* c) {
    uint32_t i;
    if (c == NULL) {
        return -1;
    }
    for (i = 0; i < c->localUseLen; i++) {
        const H2TCLocalUse* use = &c->localUses[i];
        H2DiagCode          code;
        H2Diag              warning;
        if (use->ownerFnIndex < 0 || use->suppressWarning || use->readCount > 0) {
            continue;
        }
        if (use->nameStart >= use->nameEnd) {
            continue;
        }
        if (use->ownerFnIndex >= 0 && (uint32_t)use->ownerFnIndex < c->funcLen
            && H2TCFunctionNameHasPrefix(c, &c->funcs[(uint32_t)use->ownerFnIndex], "builtin__"))
        {
            continue;
        }
        if (use->ownerFnIndex >= 0 && (uint32_t)use->ownerFnIndex < c->funcLen
            && H2TCFunctionNameHasPrefix(c, &c->funcs[(uint32_t)use->ownerFnIndex], "str__"))
        {
            continue;
        }
        if (use->kind == H2TCLocalUseKind_PARAM) {
            code =
                use->writeCount > 0 ? H2Diag_UNUSED_PARAMETER_NEVER_READ : H2Diag_UNUSED_PARAMETER;
        } else {
            code = use->writeCount > 0 ? H2Diag_UNUSED_VARIABLE_NEVER_READ : H2Diag_UNUSED_VARIABLE;
        }
        warning = (H2Diag){ 0 };
        H2TCSetDiagWithArg(
            &warning, code, use->nameStart, use->nameEnd, use->nameStart, use->nameEnd);
        warning.type = H2DiagType_WARNING;
        if (H2TCEmitWarningDiag(c, &warning) != 0) {
            return -1;
        }
    }
    for (i = 0; i < c->funcLen; i++) {
        const H2TCFunction* fn = &c->funcs[i];
        int32_t             fnNode;
        H2Diag              warning;
        if (fn->defNode < 0) {
            continue;
        }
        if ((fn->flags & H2TCFunctionFlag_TEMPLATE_INSTANCE) != 0) {
            continue;
        }
        if (c->funcUsed != NULL && i < c->funcUsedCap && c->funcUsed[i]) {
            continue;
        }
        if (H2TCIsMainFunction(c, fn)) {
            continue;
        }
        fnNode = fn->defNode >= 0 ? fn->defNode : fn->declNode;
        if (fnNode < 0 || (uint32_t)fnNode >= c->ast->len) {
            continue;
        }
        if ((c->ast->nodes[fnNode].flags & H2AstFlag_PUB) != 0) {
            continue;
        }
        if (fn->nameStart >= fn->nameEnd) {
            continue;
        }
        if (H2TCFunctionNameHasPrefix(c, fn, "builtin__")) {
            continue;
        }
        if (H2TCFunctionNameHasPrefix(c, fn, "str__")) {
            continue;
        }
        warning = (H2Diag){ 0 };
        H2TCSetDiagWithArg(
            &warning,
            H2Diag_UNUSED_FUNCTION,
            fn->nameStart,
            fn->nameEnd,
            fn->nameStart,
            fn->nameEnd);
        warning.type = H2DiagType_WARNING;
        if (H2TCEmitWarningDiag(c, &warning) != 0) {
            return -1;
        }
    }
    return 0;
}

void H2TCOffsetToLineCol(
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

int H2TCLineColToOffset(
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

void H2TCTextBufInit(H2TCTextBuf* b, char* ptr, uint32_t cap) {
    b->ptr = ptr;
    b->cap = cap;
    b->len = 0;
    if (cap > 0) {
        ptr[0] = '\0';
    }
}

void H2TCTextBufAppendChar(H2TCTextBuf* b, char ch) {
    if (b->cap == 0 || b->ptr == NULL) {
        return;
    }
    if (b->len + 1u < b->cap) {
        b->ptr[b->len++] = ch;
        b->ptr[b->len] = '\0';
    }
}

void H2TCTextBufAppendCStr(H2TCTextBuf* b, const char* s) {
    uint32_t i = 0;
    if (s == NULL) {
        return;
    }
    while (s[i] != '\0') {
        H2TCTextBufAppendChar(b, s[i]);
        i++;
    }
}

void H2TCTextBufAppendSlice(H2TCTextBuf* b, H2StrView src, uint32_t start, uint32_t end) {
    uint32_t i;
    if (end < start || end > src.len) {
        return;
    }
    for (i = start; i < end; i++) {
        H2TCTextBufAppendChar(b, src.ptr[i]);
    }
}

void H2TCTextBufAppendU32(H2TCTextBuf* b, uint32_t v) {
    char     tmp[16];
    uint32_t n = 0;
    if (v == 0) {
        H2TCTextBufAppendChar(b, '0');
        return;
    }
    while (v > 0 && n < (uint32_t)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0) {
        n--;
        H2TCTextBufAppendChar(b, tmp[n]);
    }
}

void H2TCTextBufAppendHexU64(H2TCTextBuf* b, uint64_t v) {
    char     tmp[16];
    uint32_t n = 0;
    if (v == 0u) {
        H2TCTextBufAppendChar(b, '0');
        return;
    }
    while (v > 0u && n < (uint32_t)sizeof(tmp)) {
        uint32_t digit = (uint32_t)(v & 0xFu);
        tmp[n++] = (char)(digit < 10u ? ('0' + digit) : ('a' + (digit - 10u)));
        v >>= 4u;
    }
    while (n > 0u) {
        n--;
        H2TCTextBufAppendChar(b, tmp[n]);
    }
}

const char* H2TCBuiltinName(H2TypeCheckCtx* c, int32_t typeId, H2BuiltinKind kind) {
    switch (kind) {
        case H2Builtin_VOID:   return "void";
        case H2Builtin_BOOL:   return "bool";
        case H2Builtin_TYPE:   return "type";
        case H2Builtin_U8:     return "u8";
        case H2Builtin_U16:    return "u16";
        case H2Builtin_U32:    return "u32";
        case H2Builtin_U64:    return "u64";
        case H2Builtin_I8:     return "i8";
        case H2Builtin_I16:    return "i16";
        case H2Builtin_I32:    return "i32";
        case H2Builtin_I64:    return "i64";
        case H2Builtin_USIZE:  return "uint";
        case H2Builtin_ISIZE:  return "int";
        case H2Builtin_RAWPTR: return "rawptr";
        case H2Builtin_F32:    return "f32";
        case H2Builtin_F64:    return "f64";
        case H2Builtin_STR:    return "str";
        case H2Builtin_INVALID:
            if (typeId >= 0 && typeId == c->typeStr) {
                return "str";
            }
            return "<builtin>";
    }
    return "<builtin>";
}

void H2TCFormatTypeRec(H2TypeCheckCtx* c, int32_t typeId, H2TCTextBuf* b, uint32_t depth) {
    const H2TCType* t;
    if (depth > 32u) {
        H2TCTextBufAppendCStr(b, "...");
        return;
    }
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        H2TCTextBufAppendCStr(b, "<invalid>");
        return;
    }
    t = &c->types[typeId];
    switch (t->kind) {
        case H2TCType_BUILTIN:
            H2TCTextBufAppendCStr(b, H2TCBuiltinName(c, typeId, t->builtin));
            return;
        case H2TCType_NAMED:
        case H2TCType_ALIAS:
            if (typeId == c->typeRune && (t->nameEnd <= t->nameStart || t->nameEnd > c->src.len)) {
                H2TCTextBufAppendCStr(b, "rune");
                return;
            }
            if (t->nameEnd > t->nameStart && t->nameEnd <= c->src.len) {
                uint32_t nameStart = t->nameStart;
                if (t->nameEnd - t->nameStart > 9u
                    && memcmp(c->src.ptr + t->nameStart, "builtin__", 9u) == 0)
                {
                    nameStart += 9u;
                }
                H2TCTextBufAppendSlice(b, c->src, nameStart, t->nameEnd);
            } else {
                H2TCTextBufAppendCStr(b, "<unnamed>");
            }
            {
                int32_t namedIndex = H2TCFindNamedTypeIndexByTypeId(c, typeId);
                if (namedIndex >= 0 && c->namedTypes[(uint32_t)namedIndex].templateArgCount > 0) {
                    uint16_t i;
                    H2TCTextBufAppendChar(b, '[');
                    for (i = 0; i < c->namedTypes[(uint32_t)namedIndex].templateArgCount; i++) {
                        if (i > 0) {
                            H2TCTextBufAppendCStr(b, ", ");
                        }
                        H2TCFormatTypeRec(
                            c,
                            c->genericArgTypes
                                [c->namedTypes[(uint32_t)namedIndex].templateArgStart + i],
                            b,
                            depth + 1u);
                    }
                    H2TCTextBufAppendChar(b, ']');
                }
            }
            return;
        case H2TCType_PTR:
            H2TCTextBufAppendChar(b, '*');
            H2TCFormatTypeRec(c, t->baseType, b, depth + 1u);
            return;
        case H2TCType_REF:
            H2TCTextBufAppendChar(b, (t->flags & H2TCTypeFlag_MUTABLE) != 0 ? '*' : '&');
            H2TCFormatTypeRec(c, t->baseType, b, depth + 1u);
            return;
        case H2TCType_ARRAY:
            H2TCTextBufAppendChar(b, '[');
            H2TCFormatTypeRec(c, t->baseType, b, depth + 1u);
            H2TCTextBufAppendChar(b, ' ');
            H2TCTextBufAppendU32(b, t->arrayLen);
            H2TCTextBufAppendChar(b, ']');
            return;
        case H2TCType_SLICE:
            H2TCTextBufAppendChar(b, '[');
            H2TCFormatTypeRec(c, t->baseType, b, depth + 1u);
            H2TCTextBufAppendChar(b, ']');
            return;
        case H2TCType_OPTIONAL:
            H2TCTextBufAppendChar(b, '?');
            H2TCFormatTypeRec(c, t->baseType, b, depth + 1u);
            return;
        case H2TCType_TUPLE: {
            uint32_t i;
            H2TCTextBufAppendChar(b, '(');
            for (i = 0; i < t->fieldCount; i++) {
                if (i > 0) {
                    H2TCTextBufAppendCStr(b, ", ");
                }
                H2TCFormatTypeRec(c, c->funcParamTypes[t->fieldStart + i], b, depth + 1u);
            }
            H2TCTextBufAppendChar(b, ')');
            return;
        }
        case H2TCType_PACK: {
            uint32_t i;
            H2TCTextBufAppendCStr(b, "pack(");
            for (i = 0; i < t->fieldCount; i++) {
                if (i > 0) {
                    H2TCTextBufAppendCStr(b, ", ");
                }
                H2TCFormatTypeRec(c, c->funcParamTypes[t->fieldStart + i], b, depth + 1u);
            }
            H2TCTextBufAppendChar(b, ')');
            return;
        }
        case H2TCType_TYPE_PARAM:
            if (t->nameEnd > t->nameStart && t->nameEnd <= c->src.len) {
                H2TCTextBufAppendSlice(b, c->src, t->nameStart, t->nameEnd);
            } else {
                H2TCTextBufAppendCStr(b, "<type-param>");
            }
            return;
        case H2TCType_ANYTYPE:       H2TCTextBufAppendCStr(b, "anytype"); return;
        case H2TCType_UNTYPED_INT:   H2TCTextBufAppendCStr(b, "const_int"); return;
        case H2TCType_UNTYPED_FLOAT: H2TCTextBufAppendCStr(b, "const_float"); return;
        case H2TCType_NULL:          H2TCTextBufAppendCStr(b, "null"); return;
        case H2TCType_FUNCTION:      H2TCTextBufAppendCStr(b, "fn(...)"); return;
        case H2TCType_ANON_STRUCT:   H2TCTextBufAppendCStr(b, "struct{...}"); return;
        case H2TCType_ANON_UNION:    H2TCTextBufAppendCStr(b, "union{...}"); return;
        case H2TCType_INVALID:
        default:                     H2TCTextBufAppendCStr(b, "<type>"); return;
    }
}

int H2TCExprIsStringConstant(H2TypeCheckCtx* c, int32_t nodeId) {
    const H2AstNode* n;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return 0;
    }
    n = &c->ast->nodes[nodeId];
    if (n->kind == H2Ast_STRING) {
        return 1;
    }
    if (n->kind == H2Ast_BINARY && (H2TokenKind)n->op == H2Tok_ADD
        && H2IsStringLiteralConcatChain(c->ast, nodeId))
    {
        return 1;
    }
    return 0;
}

void H2TCFormatExprSubject(H2TypeCheckCtx* c, int32_t nodeId, H2TCTextBuf* b) {
    const H2AstNode* n;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        H2TCTextBufAppendCStr(b, "expression");
        return;
    }
    n = &c->ast->nodes[nodeId];
    if (n->kind == H2Ast_IDENT && n->dataEnd > n->dataStart && n->dataEnd <= c->src.len) {
        H2TCTextBufAppendChar(b, '\'');
        H2TCTextBufAppendSlice(b, c->src, n->dataStart, n->dataEnd);
        H2TCTextBufAppendChar(b, '\'');
        return;
    }
    if (H2TCExprIsStringConstant(c, nodeId)) {
        H2TCTextBufAppendCStr(b, "string constant");
        return;
    }
    H2TCTextBufAppendCStr(b, "expression");
}

char* _Nullable H2TCAllocDiagText(H2TypeCheckCtx* c, const char* text) {
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
    p = (char*)H2ArenaAlloc(c->arena, len + 1u, 1u);
    if (p == NULL) {
        return NULL;
    }
    memcpy(p, text, len + 1u);
    return p;
}

int H2TCFailTypeMismatchDetail(
    H2TypeCheckCtx* c, int32_t failNode, int32_t exprNode, int32_t srcType, int32_t dstType) {
    uint32_t    start = 0;
    uint32_t    end = 0;
    char        subjectBuf[H2TC_DIAG_TEXT_CAP];
    char        srcTypeBuf[H2TC_DIAG_TEXT_CAP];
    char        dstTypeBuf[H2TC_DIAG_TEXT_CAP];
    char        detailBuf[384];
    H2TCTextBuf subject;
    H2TCTextBuf srcTypeText;
    H2TCTextBuf dstTypeText;
    H2TCTextBuf detailText;
    char*       detail;
    int         sourceIsStringLike = 0;

    if (failNode >= 0 && (uint32_t)failNode < c->ast->len) {
        start = c->ast->nodes[failNode].start;
        end = c->ast->nodes[failNode].end;
    }
    H2TCSetDiag(c->diag, H2Diag_TYPE_MISMATCH, start, end);
    if (c->diag == NULL) {
        return -1;
    }

    H2TCTextBufInit(&subject, subjectBuf, (uint32_t)sizeof(subjectBuf));
    H2TCTextBufInit(&srcTypeText, srcTypeBuf, (uint32_t)sizeof(srcTypeBuf));
    H2TCTextBufInit(&dstTypeText, dstTypeBuf, (uint32_t)sizeof(dstTypeBuf));
    H2TCFormatExprSubject(c, exprNode, &subject);
    H2TCFormatTypeRec(c, srcType, &srcTypeText, 0);
    H2TCFormatTypeRec(c, dstType, &dstTypeText, 0);

    H2TCTextBufInit(&detailText, detailBuf, (uint32_t)sizeof(detailBuf));
    H2TCTextBufAppendCStr(&detailText, "cannot use ");
    H2TCTextBufAppendCStr(&detailText, subjectBuf);
    H2TCTextBufAppendCStr(&detailText, " of type ");
    H2TCTextBufAppendCStr(&detailText, srcTypeBuf);
    H2TCTextBufAppendCStr(&detailText, " as ");
    H2TCTextBufAppendCStr(&detailText, dstTypeBuf);
    detail = H2TCAllocDiagText(c, detailBuf);
    if (detail != NULL) {
        c->diag->detail = detail;
    }

    if (srcType >= 0 && (uint32_t)srcType < c->typeLen) {
        const H2TCType* src = &c->types[srcType];
        sourceIsStringLike =
            srcType == c->typeStr
            || ((src->kind == H2TCType_PTR || src->kind == H2TCType_REF)
                && src->baseType == c->typeStr);
    }

    if (dstType == c->typeStr && H2TCExprIsStringConstant(c, exprNode) && sourceIsStringLike) {
        c->diag->hintOverride = H2TCAllocDiagText(c, "change type to &str or *str");
    }
    return -1;
}

int H2TCFailDiagText(H2TypeCheckCtx* c, int32_t nodeId, H2DiagCode code, const char* detail) {
    int rc = H2TCFailNode(c, nodeId, code);
    if (rc != -1 || c == NULL || c->diag == NULL || detail == NULL) {
        return rc;
    }
    c->diag->detail = H2TCAllocDiagText(c, detail);
    return rc;
}

int H2TCFailTypeMismatchText(H2TypeCheckCtx* c, int32_t nodeId, const char* detail) {
    return H2TCFailDiagText(c, nodeId, H2Diag_TYPE_MISMATCH, detail);
}

int H2TCFailVarSizeByValue(
    H2TypeCheckCtx* c, int32_t nodeId, int32_t typeId, const char* position) {
    char        typeBuf[H2TC_DIAG_TEXT_CAP];
    char        detailBuf[256];
    H2TCTextBuf typeText;
    H2TCTextBuf detailText;
    const char* where = position != NULL && position[0] != 0 ? position : "here";

    H2TCTextBufInit(&typeText, typeBuf, (uint32_t)sizeof(typeBuf));
    H2TCFormatTypeRec(c, typeId, &typeText, 0);

    H2TCTextBufInit(&detailText, detailBuf, (uint32_t)sizeof(detailBuf));
    H2TCTextBufAppendCStr(&detailText, "type ");
    H2TCTextBufAppendCStr(&detailText, typeBuf);
    H2TCTextBufAppendCStr(&detailText, " is not allowed by value in ");
    H2TCTextBufAppendCStr(&detailText, where);
    H2TCTextBufAppendCStr(&detailText, "; use &");
    H2TCTextBufAppendCStr(&detailText, typeBuf);
    H2TCTextBufAppendCStr(&detailText, " or *");
    H2TCTextBufAppendCStr(&detailText, typeBuf);
    return H2TCFailDiagText(c, nodeId, H2Diag_VAR_SIZE_BYVALUE_FORBIDDEN, detailBuf);
}

int H2TCFailAssignToConst(H2TypeCheckCtx* c, int32_t lhsNode) {
    int rc = H2TCFailNode(c, lhsNode, H2Diag_TYPE_MISMATCH);
    c->diag->detail = H2TCAllocDiagText(c, "assignment target is const");
    return rc;
}

int H2TCFailAssignTargetNotAssignable(H2TypeCheckCtx* c, int32_t lhsNode) {
    int rc = H2TCFailNode(c, lhsNode, H2Diag_TYPE_MISMATCH);
    if (rc != -1 || c == NULL || c->diag == NULL) {
        return rc;
    }
    c->diag->detail = H2TCAllocDiagText(c, "assignment target is not assignable");
    return rc;
}

int H2TCFailSwitchMissingCases(
    H2TypeCheckCtx* c,
    int32_t         failNode,
    int32_t         subjectType,
    int32_t         subjectEnumType,
    uint32_t        enumVariantCount,
    const uint32_t* _Nullable enumVariantStarts,
    const uint32_t* _Nullable enumVariantEnds,
    const uint8_t* _Nullable enumCovered,
    int boolCoveredTrue,
    int boolCoveredFalse) {
    uint32_t          start = 0;
    uint32_t          end = 0;
    char              typeBuf[H2TC_DIAG_TEXT_CAP];
    H2TCTextBuf       typeText;
    uint32_t          missingCount = 0;
    uint32_t          missingNameLen = 0;
    uint32_t          detailLen;
    uint32_t          i;
    char*             detail;
    H2TCTextBuf       detailText;
    static const char prefix[] = "of type ";

    if (failNode >= 0 && (uint32_t)failNode < c->ast->len) {
        start = c->ast->nodes[failNode].start;
        end = c->ast->nodes[failNode].end;
    }
    H2TCSetDiag(c->diag, H2Diag_SWITCH_MISSING_CASES, start, end);
    if (c->diag == NULL) {
        return -1;
    }

    H2TCTextBufInit(&typeText, typeBuf, (uint32_t)sizeof(typeBuf));
    H2TCFormatTypeRec(c, subjectType, &typeText, 0);

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

    detail = (char*)H2ArenaAlloc(c->arena, detailLen, 1u);
    if (detail == NULL) {
        return -1;
    }

    H2TCTextBufInit(&detailText, detail, detailLen);
    H2TCTextBufAppendCStr(&detailText, prefix);
    H2TCTextBufAppendCStr(&detailText, typeBuf);
    H2TCTextBufAppendCStr(&detailText, ": ");

    if (subjectEnumType >= 0) {
        int first = 1;
        for (i = 0; i < enumVariantCount; i++) {
            if (enumCovered[i]) {
                continue;
            }
            if (!first) {
                H2TCTextBufAppendCStr(&detailText, ", ");
            }
            first = 0;
            H2TCTextBufAppendSlice(&detailText, c->src, enumVariantStarts[i], enumVariantEnds[i]);
        }
    } else {
        int first = 1;
        if (!boolCoveredTrue) {
            H2TCTextBufAppendCStr(&detailText, "true");
            first = 0;
        }
        if (!boolCoveredFalse) {
            if (!first) {
                H2TCTextBufAppendCStr(&detailText, ", ");
            }
            H2TCTextBufAppendCStr(&detailText, "false");
        }
    }

    c->diag->detail = detail;
    return -1;
}

int32_t H2AstFirstChild(const H2Ast* ast, int32_t nodeId) {
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    return ast->nodes[nodeId].firstChild;
}

int32_t H2AstNextSibling(const H2Ast* ast, int32_t nodeId) {
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    return ast->nodes[nodeId].nextSibling;
}

int H2NameEqSlice(H2StrView src, uint32_t aStart, uint32_t aEnd, uint32_t bStart, uint32_t bEnd) {
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

int H2NameEqLiteral(H2StrView src, uint32_t start, uint32_t end, const char* lit) {
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

int H2NameHasPrefix(H2StrView src, uint32_t start, uint32_t end, const char* prefix) {
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

int H2NameHasSuffix(H2StrView src, uint32_t start, uint32_t end, const char* suffix) {
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

int32_t H2TCFindMemAllocatorType(H2TypeCheckCtx* c) {
    return c->typeMemAllocator;
}

int32_t H2TCGetStrRefType(H2TypeCheckCtx* c, uint32_t start, uint32_t end) {
    if (c->typeStr < 0) {
        return -1;
    }
    return H2TCInternRefType(c, c->typeStr, 0, start, end);
}

int32_t H2TCGetStrPtrType(H2TypeCheckCtx* c, uint32_t start, uint32_t end) {
    if (c->typeStr < 0) {
        return -1;
    }
    return H2TCInternPtrType(c, c->typeStr, start, end);
}

int32_t H2TCAddType(H2TypeCheckCtx* c, const H2TCType* t, uint32_t errStart, uint32_t errEnd) {
    int32_t idx;
    if (c->typeLen >= c->typeCap) {
        H2TCSetDiag(c->diag, H2Diag_ARENA_OOM, errStart, errEnd);
        return -1;
    }
    idx = (int32_t)c->typeLen++;
    c->types[idx] = *t;
    return idx;
}

int32_t H2TCAddBuiltinType(H2TypeCheckCtx* c, const char* name, H2BuiltinKind builtinKind) {
    H2TCType t;
    uint32_t i = 0;
    while (name[i] != '\0') {
        i++;
    }
    t.kind = H2TCType_BUILTIN;
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
    return H2TCAddType(c, &t, 0, 0);
}

int H2TCEnsureInitialized(H2TypeCheckCtx* c) {
    H2TCType t;
    int32_t  u32Type;

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

    c->typeVoid = H2TCAddBuiltinType(c, "void", H2Builtin_VOID);
    c->typeBool = H2TCAddBuiltinType(c, "bool", H2Builtin_BOOL);
    c->typeType = H2TCAddBuiltinType(c, "type", H2Builtin_TYPE);
    if (c->typeVoid < 0 || c->typeBool < 0 || c->typeType < 0) {
        return -1;
    }

    if (H2TCAddBuiltinType(c, "u8", H2Builtin_U8) < 0
        || H2TCAddBuiltinType(c, "u16", H2Builtin_U16) < 0
        || H2TCAddBuiltinType(c, "u32", H2Builtin_U32) < 0
        || H2TCAddBuiltinType(c, "u64", H2Builtin_U64) < 0
        || H2TCAddBuiltinType(c, "i8", H2Builtin_I8) < 0
        || H2TCAddBuiltinType(c, "i16", H2Builtin_I16) < 0
        || H2TCAddBuiltinType(c, "i32", H2Builtin_I32) < 0
        || H2TCAddBuiltinType(c, "i64", H2Builtin_I64) < 0
        || H2TCAddBuiltinType(c, "uint", H2Builtin_USIZE) < 0
        || H2TCAddBuiltinType(c, "int", H2Builtin_ISIZE) < 0
        || H2TCAddBuiltinType(c, "rawptr", H2Builtin_RAWPTR) < 0
        || H2TCAddBuiltinType(c, "f32", H2Builtin_F32) < 0
        || H2TCAddBuiltinType(c, "f64", H2Builtin_F64) < 0)
    {
        return -1;
    }
    c->typeRawptr = H2TCFindBuiltinByKind(c, H2Builtin_RAWPTR);
    if (c->typeRawptr < 0) {
        return -1;
    }
    c->typeStr = H2TCAddBuiltinType(c, "str", H2Builtin_INVALID);
    if (c->typeStr < 0) {
        return -1;
    }
    u32Type = H2TCFindBuiltinByKind(c, H2Builtin_U32);
    if (u32Type < 0) {
        return -1;
    }
    t.kind = H2TCType_ALIAS;
    t.builtin = H2Builtin_INVALID;
    t.baseType = u32Type;
    t.declNode = -1;
    t.funcIndex = -1;
    t.arrayLen = 0;
    t.nameStart = 0;
    t.nameEnd = 0;
    t.fieldStart = 0;
    t.fieldCount = 0;
    t.flags = H2TCTypeFlag_ALIAS_RESOLVED;
    c->typeRune = H2TCAddType(c, &t, 0, 0);
    if (c->typeRune < 0) {
        return -1;
    }

    t.kind = H2TCType_ANYTYPE;
    t.builtin = H2Builtin_INVALID;
    t.baseType = -1;
    t.declNode = -1;
    t.funcIndex = -1;
    t.arrayLen = 0;
    t.nameStart = 0;
    t.nameEnd = 0;
    t.fieldStart = 0;
    t.fieldCount = 0;
    t.flags = 0;
    c->typeAnytype = H2TCAddType(c, &t, 0, 0);
    if (c->typeAnytype < 0) {
        return -1;
    }

    t.kind = H2TCType_UNTYPED_INT;
    t.builtin = H2Builtin_INVALID;
    t.baseType = -1;
    t.declNode = -1;
    t.funcIndex = -1;
    t.arrayLen = 0;
    t.nameStart = 0;
    t.nameEnd = 0;
    t.fieldStart = 0;
    t.fieldCount = 0;
    t.flags = 0;
    c->typeUntypedInt = H2TCAddType(c, &t, 0, 0);
    if (c->typeUntypedInt < 0) {
        return -1;
    }

    t.kind = H2TCType_UNTYPED_FLOAT;
    c->typeUntypedFloat = H2TCAddType(c, &t, 0, 0);
    if (c->typeUntypedFloat < 0) {
        return -1;
    }

    t.kind = H2TCType_NULL;
    c->typeNull = H2TCAddType(c, &t, 0, 0);
    return c->typeNull < 0 ? -1 : 0;
}

int32_t H2TCFindBuiltinType(H2TypeCheckCtx* c, uint32_t start, uint32_t end) {
    uint32_t i;
    if (H2NameEqLiteral(c->src, start, end, "const_int")) {
        return c->typeUntypedInt;
    }
    if (H2NameEqLiteral(c->src, start, end, "const_float")) {
        return c->typeUntypedFloat;
    }
    if (H2NameEqLiteral(c->src, start, end, "str")
        || H2NameEqLiteral(c->src, start, end, "builtin__str"))
    {
        return c->typeStr;
    }
    if ((H2NameEqLiteral(c->src, start, end, "rune")
         || H2NameEqLiteral(c->src, start, end, "builtin__rune"))
        && c->typeRune >= 0)
    {
        return c->typeRune;
    }
    if (H2NameEqLiteral(c->src, start, end, "rawptr")
        || H2NameEqLiteral(c->src, start, end, "builtin__rawptr"))
    {
        return c->typeRawptr;
    }
    for (i = 0; i < c->typeLen; i++) {
        const H2TCType* t = &c->types[i];
        if (t->kind != H2TCType_BUILTIN) {
            continue;
        }
        if (H2NameEqLiteral(c->src, start, end, "bool") && t->builtin == H2Builtin_BOOL) {
            return (int32_t)i;
        }
        if (H2NameEqLiteral(c->src, start, end, "type") && t->builtin == H2Builtin_TYPE) {
            return (int32_t)i;
        }
        if (H2NameEqLiteral(c->src, start, end, "u8") && t->builtin == H2Builtin_U8) {
            return (int32_t)i;
        }
        if (H2NameEqLiteral(c->src, start, end, "u16") && t->builtin == H2Builtin_U16) {
            return (int32_t)i;
        }
        if (H2NameEqLiteral(c->src, start, end, "u32") && t->builtin == H2Builtin_U32) {
            return (int32_t)i;
        }
        if (H2NameEqLiteral(c->src, start, end, "u64") && t->builtin == H2Builtin_U64) {
            return (int32_t)i;
        }
        if (H2NameEqLiteral(c->src, start, end, "i8") && t->builtin == H2Builtin_I8) {
            return (int32_t)i;
        }
        if (H2NameEqLiteral(c->src, start, end, "i16") && t->builtin == H2Builtin_I16) {
            return (int32_t)i;
        }
        if (H2NameEqLiteral(c->src, start, end, "i32") && t->builtin == H2Builtin_I32) {
            return (int32_t)i;
        }
        if (H2NameEqLiteral(c->src, start, end, "i64") && t->builtin == H2Builtin_I64) {
            return (int32_t)i;
        }
        if (H2NameEqLiteral(c->src, start, end, "uint") && t->builtin == H2Builtin_USIZE) {
            return (int32_t)i;
        }
        if (H2NameEqLiteral(c->src, start, end, "int") && t->builtin == H2Builtin_ISIZE) {
            return (int32_t)i;
        }
        if (H2NameEqLiteral(c->src, start, end, "f32") && t->builtin == H2Builtin_F32) {
            return (int32_t)i;
        }
        if (H2NameEqLiteral(c->src, start, end, "f64") && t->builtin == H2Builtin_F64) {
            return (int32_t)i;
        }
    }
    return -1;
}

int32_t H2TCFindBuiltinByKind(H2TypeCheckCtx* c, H2BuiltinKind builtinKind) {
    uint32_t i;
    for (i = 0; i < c->typeLen; i++) {
        if (c->types[i].kind == H2TCType_BUILTIN && c->types[i].builtin == builtinKind) {
            return (int32_t)i;
        }
    }
    return -1;
}

uint8_t H2TCTypeTagKindOf(H2TypeCheckCtx* c, int32_t typeId) {
    const H2TCType* t;
    int32_t         declNode;
    if (c == NULL || typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return H2TCTypeTagKind_INVALID;
    }
    t = &c->types[typeId];
    switch (t->kind) {
        case H2TCType_ALIAS:       return H2TCTypeTagKind_ALIAS;
        case H2TCType_PTR:         return H2TCTypeTagKind_POINTER;
        case H2TCType_REF:         return H2TCTypeTagKind_REFERENCE;
        case H2TCType_ARRAY:       return H2TCTypeTagKind_ARRAY;
        case H2TCType_SLICE:       return H2TCTypeTagKind_SLICE;
        case H2TCType_OPTIONAL:    return H2TCTypeTagKind_OPTIONAL;
        case H2TCType_FUNCTION:    return H2TCTypeTagKind_FUNCTION;
        case H2TCType_TUPLE:       return H2TCTypeTagKind_TUPLE;
        case H2TCType_ANON_STRUCT: return H2TCTypeTagKind_STRUCT;
        case H2TCType_ANON_UNION:  return H2TCTypeTagKind_UNION;
        case H2TCType_NAMED:
            declNode = t->declNode;
            if (declNode >= 0 && (uint32_t)declNode < c->ast->len) {
                switch (c->ast->nodes[declNode].kind) {
                    case H2Ast_STRUCT: return H2TCTypeTagKind_STRUCT;
                    case H2Ast_UNION:  return H2TCTypeTagKind_UNION;
                    case H2Ast_ENUM:   return H2TCTypeTagKind_ENUM;
                    default:           break;
                }
            }
            return H2TCTypeTagKind_PRIMITIVE;
        case H2TCType_BUILTIN:
        case H2TCType_UNTYPED_INT:
        case H2TCType_UNTYPED_FLOAT:
        case H2TCType_NULL:          return H2TCTypeTagKind_PRIMITIVE;
        default:                     return H2TCTypeTagKind_INVALID;
    }
}

uint64_t H2TCEncodeTypeTag(H2TypeCheckCtx* c, int32_t typeId) {
    uint64_t kind = (uint64_t)H2TCTypeTagKindOf(c, typeId);
    uint64_t id = typeId >= 0 ? ((uint64_t)(uint32_t)typeId + 1u) : 0u;
    return (kind << 56u) | (id & 0x00ffffffffffffffULL);
}

int H2TCDecodeTypeTag(H2TypeCheckCtx* c, uint64_t typeTag, int32_t* outTypeId) {
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

static int32_t H2TCFindNamedTypeIndexByTypeId(H2TypeCheckCtx* c, int32_t typeId) {
    uint32_t i;
    for (i = 0; i < c->namedTypeLen; i++) {
        if (c->namedTypes[i].typeId == typeId) {
            return (int32_t)i;
        }
    }
    return -1;
}

static int32_t H2TCParentOwnerTypeId(H2TypeCheckCtx* c, int32_t typeId) {
    int32_t idx = H2TCFindNamedTypeIndexByTypeId(c, typeId);
    if (idx < 0) {
        return -1;
    }
    return c->namedTypes[(uint32_t)idx].ownerTypeId;
}

int32_t H2TCFindNamedTypeIndexOwned(
    H2TypeCheckCtx* c, int32_t ownerTypeId, uint32_t start, uint32_t end) {
    uint32_t i;
    for (i = 0; i < c->namedTypeLen; i++) {
        if (c->namedTypes[i].ownerTypeId == ownerTypeId
            && H2NameEqSlice(
                c->src, c->namedTypes[i].nameStart, c->namedTypes[i].nameEnd, start, end))
        {
            return (int32_t)i;
        }
    }
    return -1;
}

static int32_t H2TCFindNamedTypeIndexInOwnerScope(
    H2TypeCheckCtx* c, int32_t ownerTypeId, uint32_t start, uint32_t end) {
    int32_t owner = ownerTypeId;
    while (owner >= 0) {
        int32_t idx = H2TCFindNamedTypeIndexOwned(c, owner, start, end);
        if (idx >= 0) {
            return idx;
        }
        owner = H2TCParentOwnerTypeId(c, owner);
    }
    return H2TCFindNamedTypeIndexOwned(c, -1, start, end);
}

int32_t H2TCResolveTypeNamePath(
    H2TypeCheckCtx* c, uint32_t start, uint32_t end, int32_t ownerTypeId) {
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
                idx = H2TCFindNamedTypeIndexInOwnerScope(c, ownerTypeId, segStart, segEnd);
            } else {
                idx = H2TCFindNamedTypeIndexOwned(c, currentTypeId, segStart, segEnd);
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

int32_t H2TCResolveTypeValueName(H2TypeCheckCtx* c, uint32_t start, uint32_t end) {
    int32_t builtinType = H2TCFindBuiltinType(c, start, end);
    int32_t typeId;
    if (c != NULL && c->activeGenericDeclNode >= 0) {
        int32_t idx = H2TCDeclTypeParamIndex(c, c->activeGenericDeclNode, start, end);
        if (idx >= 0 && (uint32_t)idx < c->activeGenericArgCount) {
            return c->genericArgTypes[c->activeGenericArgStart + (uint32_t)idx];
        }
    }
    if (builtinType >= 0) {
        return builtinType;
    }
    typeId = H2TCResolveTypeNamePath(c, start, end, c->currentTypeOwnerTypeId);
    if (typeId >= 0) {
        int32_t namedIndex = H2TCFindNamedTypeIndexByTypeId(c, typeId);
        if (namedIndex >= 0 && c->namedTypes[(uint32_t)namedIndex].templateArgCount > 0
            && c->namedTypes[(uint32_t)namedIndex].templateRootNamedIndex < 0)
        {
            return -1;
        }
        return typeId;
    }
    typeId = H2TCFindBuiltinQualifiedNamedType(c, start, end);
    if (typeId >= 0) {
        int32_t namedIndex = H2TCFindNamedTypeIndexByTypeId(c, typeId);
        if (namedIndex >= 0 && c->namedTypes[(uint32_t)namedIndex].templateArgCount > 0
            && c->namedTypes[(uint32_t)namedIndex].templateRootNamedIndex < 0)
        {
            return -1;
        }
        return typeId;
    }
    return -1;
}

int32_t H2TCFindNamedTypeIndex(H2TypeCheckCtx* c, uint32_t start, uint32_t end) {
    return H2TCFindNamedTypeIndexOwned(c, -1, start, end);
}

int32_t H2TCFindNamedTypeByLiteral(H2TypeCheckCtx* c, const char* name) {
    uint32_t i;
    for (i = 0; i < c->typeLen; i++) {
        const H2TCType* t = &c->types[i];
        if (t->kind != H2TCType_NAMED) {
            continue;
        }
        if (H2NameEqLiteral(c->src, t->nameStart, t->nameEnd, name)) {
            return (int32_t)i;
        }
    }
    return -1;
}

int32_t H2TCFindBuiltinQualifiedNamedType(H2TypeCheckCtx* c, uint32_t start, uint32_t end) {
    uint32_t i;
    size_t   nameLen;
    if (c == NULL || end <= start || end > c->src.len) {
        return -1;
    }
    nameLen = (size_t)(end - start);
    for (i = 0; i < c->typeLen; i++) {
        const H2TCType* t = &c->types[i];
        uint32_t        candLen;
        if (t->kind != H2TCType_NAMED || t->nameEnd <= t->nameStart) {
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

int32_t H2TCFindBuiltinNamedTypeBySuffix(H2TypeCheckCtx* c, const char* suffix) {
    uint32_t i;
    for (i = 0; i < c->typeLen; i++) {
        const H2TCType* t = &c->types[i];
        if (t->kind != H2TCType_NAMED) {
            continue;
        }
        if (H2NameHasPrefix(c->src, t->nameStart, t->nameEnd, "builtin")
            && H2NameHasSuffix(c->src, t->nameStart, t->nameEnd, suffix))
        {
            return (int32_t)i;
        }
    }
    return -1;
}

int32_t H2TCFindNamedTypeBySuffix(H2TypeCheckCtx* c, const char* suffix) {
    uint32_t i;
    for (i = 0; i < c->typeLen; i++) {
        const H2TCType* t = &c->types[i];
        if (t->kind != H2TCType_NAMED) {
            continue;
        }
        if (H2NameHasSuffix(c->src, t->nameStart, t->nameEnd, suffix)) {
            return (int32_t)i;
        }
    }
    return -1;
}

int32_t H2TCFindReflectKindType(H2TypeCheckCtx* c) {
    uint32_t i;
    int32_t  direct = H2TCFindNamedTypeByLiteral(c, "reflect__Kind");
    if (direct >= 0) {
        return direct;
    }
    for (i = 0; i < c->typeLen; i++) {
        const H2TCType* t = &c->types[i];
        if (t->kind != H2TCType_NAMED) {
            continue;
        }
        if (!H2NameHasPrefix(c->src, t->nameStart, t->nameEnd, "reflect")
            || !H2NameHasSuffix(c->src, t->nameStart, t->nameEnd, "__Kind"))
        {
            continue;
        }
        if (t->declNode >= 0 && (uint32_t)t->declNode < c->ast->len
            && c->ast->nodes[t->declNode].kind == H2Ast_ENUM)
        {
            return (int32_t)i;
        }
    }
    return -1;
}

int H2TCNameEqLiteralOrPkgBuiltin(
    H2TypeCheckCtx* c, uint32_t start, uint32_t end, const char* name, const char* pkgPrefix) {
    uint32_t i = 0;
    uint32_t j = 0;
    uint32_t prefixLen = 0;
    uint32_t nameLen = 0;
    if (H2NameEqLiteral(c->src, start, end, name)) {
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

H2TCCompilerDiagOp H2TCCompilerDiagOpFromName(H2TypeCheckCtx* c, uint32_t start, uint32_t end) {
    if (H2TCNameEqLiteralOrPkgBuiltin(c, start, end, "error", "compiler")) {
        return H2TCCompilerDiagOp_ERROR;
    }
    if (H2TCNameEqLiteralOrPkgBuiltin(c, start, end, "error_at", "compiler")) {
        return H2TCCompilerDiagOp_ERROR_AT;
    }
    if (H2TCNameEqLiteralOrPkgBuiltin(c, start, end, "warn", "compiler")) {
        return H2TCCompilerDiagOp_WARN;
    }
    if (H2TCNameEqLiteralOrPkgBuiltin(c, start, end, "warn_at", "compiler")) {
        return H2TCCompilerDiagOp_WARN_AT;
    }
    return H2TCCompilerDiagOp_NONE;
}

int H2TCIsSourceLocationOfName(H2TypeCheckCtx* c, uint32_t start, uint32_t end) {
    return H2TCNameEqLiteralOrPkgBuiltin(c, start, end, "source_location_of", "builtin");
}

int32_t H2TCFindSourceLocationType(H2TypeCheckCtx* c) {
    uint32_t i;
    int32_t  direct = H2TCFindNamedTypeByLiteral(c, "builtin__SourceLocation");
    if (direct >= 0) {
        return direct;
    }
    for (i = 0; i < c->typeLen; i++) {
        const H2TCType* t = &c->types[i];
        if (t->kind != H2TCType_NAMED) {
            continue;
        }
        if (!H2NameHasPrefix(c->src, t->nameStart, t->nameEnd, "builtin")
            || !H2NameHasSuffix(c->src, t->nameStart, t->nameEnd, "__SourceLocation"))
        {
            continue;
        }
        if (t->declNode >= 0 && (uint32_t)t->declNode < c->ast->len
            && c->ast->nodes[t->declNode].kind == H2Ast_STRUCT)
        {
            return (int32_t)i;
        }
    }
    return -1;
}

int32_t H2TCFindFmtValueType(H2TypeCheckCtx* c) {
    uint32_t i;
    int32_t  direct = H2TCFindNamedTypeByLiteral(c, "builtin__FmtValue");
    if (direct >= 0) {
        return direct;
    }
    for (i = 0; i < c->typeLen; i++) {
        const H2TCType* t = &c->types[i];
        if (t->kind != H2TCType_NAMED) {
            continue;
        }
        if (!H2NameHasPrefix(c->src, t->nameStart, t->nameEnd, "builtin")
            || !H2NameHasSuffix(c->src, t->nameStart, t->nameEnd, "__FmtValue"))
        {
            continue;
        }
        if (t->declNode >= 0 && (uint32_t)t->declNode < c->ast->len
            && c->ast->nodes[t->declNode].kind == H2Ast_STRUCT)
        {
            return (int32_t)i;
        }
    }
    return -1;
}

int H2TCTypeIsSourceLocation(H2TypeCheckCtx* c, int32_t typeId) {
    int32_t locationType;
    if (c == NULL || typeId < 0) {
        return 0;
    }
    if (c->typeSourceLocation < 0) {
        c->typeSourceLocation = H2TCFindSourceLocationType(c);
    }
    locationType = c->typeSourceLocation;
    if (locationType < 0) {
        return 0;
    }
    typeId = H2TCResolveAliasBaseType(c, typeId);
    return typeId == locationType;
}

int H2TCTypeIsFmtValue(H2TypeCheckCtx* c, int32_t typeId) {
    const H2TCType* t;
    if (c == NULL || typeId < 0) {
        return 0;
    }
    typeId = H2TCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return 0;
    }
    t = &c->types[typeId];
    if (t->kind != H2TCType_NAMED) {
        return 0;
    }
    if (!H2NameHasSuffix(c->src, t->nameStart, t->nameEnd, "__FmtValue")) {
        return 0;
    }
    return t->declNode >= 0 && (uint32_t)t->declNode < c->ast->len
        && c->ast->nodes[t->declNode].kind == H2Ast_STRUCT;
}

int32_t H2TCFindFunctionIndex(H2TypeCheckCtx* c, uint32_t start, uint32_t end) {
    uint32_t i;
    for (i = 0; i < c->funcLen; i++) {
        if (H2NameEqSlice(c->src, c->funcs[i].nameStart, c->funcs[i].nameEnd, start, end)) {
            return (int32_t)i;
        }
    }
    return -1;
}

int32_t H2TCFindBuiltinQualifiedFunctionIndex(H2TypeCheckCtx* c, uint32_t start, uint32_t end) {
    uint32_t i;
    size_t   nameLen;
    if (c == NULL || end <= start || end > c->src.len) {
        return -1;
    }
    nameLen = (size_t)(end - start);
    for (i = 0; i < c->funcLen; i++) {
        const H2TCFunction* fn = &c->funcs[i];
        uint32_t            candLen;
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

static int H2TCFunctionSupportsFunctionValue(const H2TypeCheckCtx* c, const H2TCFunction* fn) {
    if (c == NULL || fn == NULL) {
        return 0;
    }
    if (fn->contextType >= 0 || (fn->flags & H2TCFunctionFlag_VARIADIC) != 0) {
        return 0;
    }
    if (fn->defNode >= 0) {
        return 1;
    }
    return fn->declNode >= 0 && H2TCHasForeignImportDirective(c->ast, c->src, fn->declNode);
}

int32_t H2TCFindPlainFunctionValueIndex(H2TypeCheckCtx* c, uint32_t start, uint32_t end) {
    uint32_t i;
    int32_t  found = -1;
    for (i = 0; i < c->funcLen; i++) {
        const H2TCFunction* fn = &c->funcs[i];
        if (!H2NameEqSlice(c->src, fn->nameStart, fn->nameEnd, start, end)) {
            continue;
        }
        if (!H2TCFunctionSupportsFunctionValue(c, fn)) {
            continue;
        }
        if (found >= 0) {
            return -1;
        }
        found = (int32_t)i;
    }
    if (found < 0) {
        int32_t builtinFn = H2TCFindBuiltinQualifiedFunctionIndex(c, start, end);
        if (builtinFn >= 0 && H2TCFunctionSupportsFunctionValue(c, &c->funcs[(uint32_t)builtinFn]))
        {
            found = builtinFn;
        }
    }
    return found;
}

int32_t H2TCFindPkgQualifiedFunctionValueIndex(
    H2TypeCheckCtx* c, uint32_t pkgStart, uint32_t pkgEnd, uint32_t nameStart, uint32_t nameEnd) {
    int32_t  candidates[H2TC_MAX_CALL_CANDIDATES];
    uint32_t candidateCount = 0;
    int      nameFound = 0;
    uint32_t i;
    int32_t  found = -1;
    H2TCGatherCallCandidatesByPkgMethod(
        c, pkgStart, pkgEnd, nameStart, nameEnd, candidates, &candidateCount, &nameFound);
    if (!nameFound) {
        return -1;
    }
    for (i = 0; i < candidateCount; i++) {
        const H2TCFunction* fn;
        int32_t             fnIndex = candidates[i];
        if (fnIndex < 0 || (uint32_t)fnIndex >= c->funcLen) {
            continue;
        }
        fn = &c->funcs[(uint32_t)fnIndex];
        if (!H2TCFunctionSupportsFunctionValue(c, fn)) {
            continue;
        }
        if (found >= 0) {
            return -1;
        }
        found = fnIndex;
    }
    return found;
}

int H2TCFunctionNameEq(const H2TypeCheckCtx* c, uint32_t funcIndex, uint32_t start, uint32_t end) {
    return H2NameEqSlice(
        c->src, c->funcs[funcIndex].nameStart, c->funcs[funcIndex].nameEnd, start, end);
}

int H2TCNameEqPkgPrefixedMethod(
    H2TypeCheckCtx* c,
    uint32_t        candidateStart,
    uint32_t        candidateEnd,
    uint32_t        pkgStart,
    uint32_t        pkgEnd,
    uint32_t        methodStart,
    uint32_t        methodEnd) {
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
    if (!H2NameEqSlice(c->src, candidateStart, candidateStart + pkgLen, pkgStart, pkgEnd)) {
        return 0;
    }
    if (c->src.ptr[candidateStart + pkgLen] != '_'
        || c->src.ptr[candidateStart + pkgLen + 1u] != '_')
    {
        return 0;
    }
    return H2NameEqSlice(
        c->src, candidateStart + pkgLen + 2u, candidateEnd, methodStart, methodEnd);
}

int H2TCExtractPkgPrefixFromTypeName(
    H2TypeCheckCtx* c,
    uint32_t        typeNameStart,
    uint32_t        typeNameEnd,
    uint32_t*       outPkgStart,
    uint32_t*       outPkgEnd) {
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

int H2TCImportDefaultAliasEq(
    H2StrView src, uint32_t pathStart, uint32_t pathEnd, uint32_t aliasStart, uint32_t aliasEnd) {
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
    return H2NameEqSlice(src, start, end, aliasStart, aliasEnd);
}

int H2TCHasImportAlias(H2TypeCheckCtx* c, uint32_t aliasStart, uint32_t aliasEnd) {
    uint32_t nodeId;
    if (c == NULL || aliasEnd <= aliasStart || aliasEnd > c->src.len) {
        return 0;
    }
    for (nodeId = 0; nodeId < c->ast->len; nodeId++) {
        const H2AstNode* importNode;
        int32_t          child;
        if (c->ast->nodes[nodeId].kind != H2Ast_IMPORT) {
            continue;
        }
        importNode = &c->ast->nodes[nodeId];
        child = importNode->firstChild;
        while (child >= 0) {
            const H2AstNode* ch = &c->ast->nodes[child];
            if (ch->kind == H2Ast_IDENT) {
                if (H2NameEqSlice(c->src, ch->dataStart, ch->dataEnd, aliasStart, aliasEnd)) {
                    return 1;
                }
                break;
            }
            child = ch->nextSibling;
        }
        if (H2TCImportDefaultAliasEq(
                c->src, importNode->dataStart, importNode->dataEnd, aliasStart, aliasEnd))
        {
            return 1;
        }
    }
    return 0;
}

int H2TCResolveReceiverPkgPrefix(
    H2TypeCheckCtx* c, int32_t typeId, uint32_t* outPkgStart, uint32_t* outPkgEnd) {
    uint32_t depth = 0;
    while (typeId >= 0 && (uint32_t)typeId < c->typeLen && depth++ <= c->typeLen) {
        const H2TCType* t = &c->types[typeId];
        switch (t->kind) {
            case H2TCType_PTR:
            case H2TCType_REF:
            case H2TCType_OPTIONAL: typeId = t->baseType; continue;
            case H2TCType_ALIAS:
                if (H2TCResolveAliasTypeId(c, typeId) != 0) {
                    return -1;
                }
                if (H2TCExtractPkgPrefixFromTypeName(
                        c, t->nameStart, t->nameEnd, outPkgStart, outPkgEnd))
                {
                    return 1;
                }
                typeId = t->baseType;
                continue;
            case H2TCType_NAMED:
                return H2TCExtractPkgPrefixFromTypeName(
                    c, t->nameStart, t->nameEnd, outPkgStart, outPkgEnd);
            default: return 0;
        }
    }
    return 0;
}

static int H2TCResolveTypePathExprTypeForEnumMember(
    H2TypeCheckCtx* c, int32_t exprNode, int32_t ownerTypeId, int32_t* outTypeId) {
    const H2AstNode* n;
    if (exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return 0;
    }
    n = &c->ast->nodes[exprNode];
    if (n->kind == H2Ast_IDENT) {
        int32_t typeId;
        if (H2TCLocalFind(c, n->dataStart, n->dataEnd) >= 0
            || H2TCFindFunctionIndex(c, n->dataStart, n->dataEnd) >= 0)
        {
            return 0;
        }
        typeId = H2TCResolveTypeNamePath(c, n->dataStart, n->dataEnd, ownerTypeId);
        if (typeId < 0) {
            return 0;
        }
        *outTypeId = typeId;
        return 1;
    }
    if (n->kind == H2Ast_FIELD_EXPR) {
        int32_t recvExpr = H2AstFirstChild(c->ast, exprNode);
        int32_t recvTypeId;
        int32_t idx;
        if (recvExpr < 0
            || !H2TCResolveTypePathExprTypeForEnumMember(c, recvExpr, ownerTypeId, &recvTypeId))
        {
            return 0;
        }
        idx = H2TCFindNamedTypeIndexOwned(c, recvTypeId, n->dataStart, n->dataEnd);
        if (idx < 0) {
            return 0;
        }
        *outTypeId = c->namedTypes[(uint32_t)idx].typeId;
        return 1;
    }
    return 0;
}

int H2TCResolveEnumMemberType(
    H2TypeCheckCtx* c,
    int32_t         recvNode,
    uint32_t        memberStart,
    uint32_t        memberEnd,
    int32_t*        outType) {
    int32_t enumTypeId;
    int32_t enumFieldType;

    if (recvNode < 0 || (uint32_t)recvNode >= c->ast->len) {
        return 0;
    }
    if (!H2TCResolveTypePathExprTypeForEnumMember(
            c, recvNode, c->currentTypeOwnerTypeId, &enumTypeId))
    {
        return 0;
    }
    enumTypeId = H2TCResolveAliasBaseType(c, enumTypeId);
    if (!H2TCIsNamedDeclKind(c, enumTypeId, H2Ast_ENUM)) {
        return 0;
    }

    if (H2TCFieldLookup(c, enumTypeId, memberStart, memberEnd, &enumFieldType, NULL) != 0) {
        return 0;
    }
    *outType = enumFieldType;
    return 1;
}

int32_t H2TCInternPtrType(H2TypeCheckCtx* c, int32_t baseType, uint32_t errStart, uint32_t errEnd) {
    uint32_t i;
    H2TCType t;
    for (i = 0; i < c->typeLen; i++) {
        if (c->types[i].kind == H2TCType_PTR && c->types[i].baseType == baseType) {
            return (int32_t)i;
        }
    }
    t.kind = H2TCType_PTR;
    t.builtin = H2Builtin_INVALID;
    t.baseType = baseType;
    t.declNode = -1;
    t.funcIndex = -1;
    t.arrayLen = 0;
    t.nameStart = 0;
    t.nameEnd = 0;
    t.fieldStart = 0;
    t.fieldCount = 0;
    t.flags = 0;
    return H2TCAddType(c, &t, errStart, errEnd);
}

int H2TCTypeIsMutable(const H2TCType* t) {
    if (t->kind == H2TCType_PTR) {
        return 1;
    }
    return (t->flags & H2TCTypeFlag_MUTABLE) != 0;
}

int H2TCIsMutableRefType(H2TypeCheckCtx* c, int32_t typeId) {
    return typeId >= 0 && (uint32_t)typeId < c->typeLen
        && (c->types[typeId].kind == H2TCType_PTR
            || (c->types[typeId].kind == H2TCType_REF && H2TCTypeIsMutable(&c->types[typeId])));
}

int32_t H2TCInternRefType(
    H2TypeCheckCtx* c, int32_t baseType, int isMutable, uint32_t errStart, uint32_t errEnd) {
    uint32_t i;
    H2TCType t;
    for (i = 0; i < c->typeLen; i++) {
        if (c->types[i].kind == H2TCType_REF && c->types[i].baseType == baseType
            && H2TCTypeIsMutable(&c->types[i]) == isMutable)
        {
            return (int32_t)i;
        }
    }
    t.kind = H2TCType_REF;
    t.builtin = H2Builtin_INVALID;
    t.baseType = baseType;
    t.declNode = -1;
    t.funcIndex = -1;
    t.arrayLen = 0;
    t.nameStart = 0;
    t.nameEnd = 0;
    t.fieldStart = 0;
    t.fieldCount = 0;
    t.flags = isMutable ? H2TCTypeFlag_MUTABLE : 0;
    return H2TCAddType(c, &t, errStart, errEnd);
}

int32_t H2TCInternArrayType(
    H2TypeCheckCtx* c, int32_t baseType, uint32_t arrayLen, uint32_t errStart, uint32_t errEnd) {
    uint32_t i;
    H2TCType t;
    for (i = 0; i < c->typeLen; i++) {
        if (c->types[i].kind == H2TCType_ARRAY && c->types[i].baseType == baseType
            && c->types[i].arrayLen == arrayLen)
        {
            return (int32_t)i;
        }
    }
    t.kind = H2TCType_ARRAY;
    t.builtin = H2Builtin_INVALID;
    t.baseType = baseType;
    t.declNode = -1;
    t.funcIndex = -1;
    t.arrayLen = arrayLen;
    t.nameStart = 0;
    t.nameEnd = 0;
    t.fieldStart = 0;
    t.fieldCount = 0;
    t.flags = 0;
    return H2TCAddType(c, &t, errStart, errEnd);
}

int32_t H2TCInternSliceType(
    H2TypeCheckCtx* c, int32_t baseType, int isMutable, uint32_t errStart, uint32_t errEnd) {
    uint32_t i;
    H2TCType t;
    for (i = 0; i < c->typeLen; i++) {
        if (c->types[i].kind == H2TCType_SLICE && c->types[i].baseType == baseType
            && H2TCTypeIsMutable(&c->types[i]) == isMutable)
        {
            return (int32_t)i;
        }
    }
    t.kind = H2TCType_SLICE;
    t.builtin = H2Builtin_INVALID;
    t.baseType = baseType;
    t.declNode = -1;
    t.funcIndex = -1;
    t.arrayLen = 0;
    t.nameStart = 0;
    t.nameEnd = 0;
    t.fieldStart = 0;
    t.fieldCount = 0;
    t.flags = isMutable ? H2TCTypeFlag_MUTABLE : 0;
    return H2TCAddType(c, &t, errStart, errEnd);
}

int32_t H2TCInternOptionalType(
    H2TypeCheckCtx* c, int32_t baseType, uint32_t errStart, uint32_t errEnd) {
    uint32_t i;
    H2TCType t;
    for (i = 0; i < c->typeLen; i++) {
        if (c->types[i].kind == H2TCType_OPTIONAL && c->types[i].baseType == baseType) {
            return (int32_t)i;
        }
    }
    t.kind = H2TCType_OPTIONAL;
    t.builtin = H2Builtin_INVALID;
    t.baseType = baseType;
    t.declNode = -1;
    t.funcIndex = -1;
    t.arrayLen = 0;
    t.nameStart = 0;
    t.nameEnd = 0;
    t.fieldStart = 0;
    t.fieldCount = 0;
    t.flags = 0;
    return H2TCAddType(c, &t, errStart, errEnd);
}

int32_t H2TCInternAnonAggregateType(
    H2TypeCheckCtx*         c,
    int                     isUnion,
    const H2TCAnonFieldSig* fields,
    uint32_t                fieldCount,
    int32_t                 declNode,
    uint32_t                errStart,
    uint32_t                errEnd) {
    uint32_t i;
    H2TCType t;

    if (fieldCount > UINT16_MAX) {
        return H2TCFailSpan(c, H2Diag_ARENA_OOM, errStart, errEnd);
    }

    for (i = 0; i < c->typeLen; i++) {
        const H2TCType* ct = &c->types[i];
        uint32_t        j;
        int             same = 1;
        if ((isUnion ? H2TCType_ANON_UNION : H2TCType_ANON_STRUCT) != ct->kind
            || ct->fieldCount != fieldCount)
        {
            continue;
        }
        for (j = 0; j < fieldCount; j++) {
            const H2TCField* f = &c->fields[ct->fieldStart + j];
            if (f->typeId != fields[j].typeId
                || !H2NameEqSlice(
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
        return H2TCFailSpan(c, H2Diag_ARENA_OOM, errStart, errEnd);
    }

    t.kind = isUnion ? H2TCType_ANON_UNION : H2TCType_ANON_STRUCT;
    t.builtin = H2Builtin_INVALID;
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
        int32_t  typeId = H2TCAddType(c, &t, errStart, errEnd);
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

int H2TCFunctionTypeMatchesSignature(
    H2TypeCheckCtx* c,
    const H2TCType* t,
    int32_t         returnType,
    const int32_t*  paramTypes,
    const uint8_t*  paramFlags,
    uint32_t        paramCount,
    int             isVariadic) {
    uint32_t i;
    if (t->kind != H2TCType_FUNCTION || t->baseType != returnType || t->fieldCount != paramCount) {
        return 0;
    }
    if (((t->flags & H2TCTypeFlag_FUNCTION_VARIADIC) != 0) != (isVariadic != 0)) {
        return 0;
    }
    for (i = 0; i < paramCount; i++) {
        if (c->funcParamTypes[t->fieldStart + i] != paramTypes[i]) {
            return 0;
        }
        if ((c->funcParamFlags[t->fieldStart + i] & H2TCFuncParamFlag_CONST)
            != ((paramFlags != NULL ? paramFlags[i] : 0u) & H2TCFuncParamFlag_CONST))
        {
            return 0;
        }
    }
    return 1;
}

int32_t H2TCInternFunctionType(
    H2TypeCheckCtx* c,
    int32_t         returnType,
    const int32_t*  paramTypes,
    const uint8_t*  paramFlags,
    uint32_t        paramCount,
    int             isVariadic,
    int32_t         funcIndex,
    uint32_t        errStart,
    uint32_t        errEnd) {
    uint32_t i;
    H2TCType t;
    if (paramCount > UINT16_MAX) {
        return H2TCFailSpan(c, H2Diag_ARENA_OOM, errStart, errEnd);
    }
    for (i = 0; i < c->typeLen; i++) {
        if (H2TCFunctionTypeMatchesSignature(
                c, &c->types[i], returnType, paramTypes, paramFlags, paramCount, isVariadic))
        {
            if (funcIndex >= 0 && c->types[i].funcIndex < 0) {
                c->types[i].funcIndex = funcIndex;
            }
            return (int32_t)i;
        }
    }
    if (c->funcParamLen + paramCount > c->funcParamCap) {
        return H2TCFailSpan(c, H2Diag_ARENA_OOM, errStart, errEnd);
    }
    t.kind = H2TCType_FUNCTION;
    t.builtin = H2Builtin_INVALID;
    t.baseType = returnType;
    t.declNode = -1;
    t.funcIndex = funcIndex;
    t.arrayLen = 0;
    t.nameStart = 0;
    t.nameEnd = 0;
    t.fieldStart = c->funcParamLen;
    t.fieldCount = (uint16_t)paramCount;
    t.flags = (uint16_t)(isVariadic ? H2TCTypeFlag_FUNCTION_VARIADIC : 0u);
    for (i = 0; i < paramCount; i++) {
        c->funcParamTypes[c->funcParamLen++] = paramTypes[i];
        c->funcParamNameStarts[c->funcParamLen - 1u] = 0;
        c->funcParamNameEnds[c->funcParamLen - 1u] = 0;
        c->funcParamFlags[c->funcParamLen - 1u] =
            paramFlags != NULL ? (paramFlags[i] & H2TCFuncParamFlag_CONST) : 0u;
    }
    return H2TCAddType(c, &t, errStart, errEnd);
}

int H2TCTupleTypeMatchesSignature(
    H2TypeCheckCtx* c, const H2TCType* t, const int32_t* elemTypes, uint32_t elemCount) {
    uint32_t i;
    if (t->kind != H2TCType_TUPLE || t->fieldCount != elemCount) {
        return 0;
    }
    for (i = 0; i < elemCount; i++) {
        if (c->funcParamTypes[t->fieldStart + i] != elemTypes[i]) {
            return 0;
        }
    }
    return 1;
}

int32_t H2TCInternTupleType(
    H2TypeCheckCtx* c,
    const int32_t*  elemTypes,
    uint32_t        elemCount,
    uint32_t        errStart,
    uint32_t        errEnd) {
    uint32_t i;
    H2TCType t;
    if (elemCount < 2u || elemCount > UINT16_MAX) {
        return H2TCFailSpan(c, H2Diag_EXPECTED_TYPE, errStart, errEnd);
    }
    for (i = 0; i < c->typeLen; i++) {
        if (H2TCTupleTypeMatchesSignature(c, &c->types[i], elemTypes, elemCount)) {
            return (int32_t)i;
        }
    }
    if (c->funcParamLen + elemCount > c->funcParamCap) {
        return H2TCFailSpan(c, H2Diag_ARENA_OOM, errStart, errEnd);
    }
    t.kind = H2TCType_TUPLE;
    t.builtin = H2Builtin_INVALID;
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
    return H2TCAddType(c, &t, errStart, errEnd);
}

int32_t H2TCInternPackType(
    H2TypeCheckCtx* c,
    const int32_t*  elemTypes,
    uint32_t        elemCount,
    uint32_t        errStart,
    uint32_t        errEnd) {
    uint32_t i;
    uint32_t j;
    H2TCType t;
    for (i = 0; i < c->typeLen; i++) {
        if (c->types[i].kind == H2TCType_PACK && c->types[i].fieldCount == elemCount) {
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
        return H2TCFailSpan(c, H2Diag_ARENA_OOM, errStart, errEnd);
    }
    if (c->funcParamLen + elemCount > c->funcParamCap) {
        return H2TCFailSpan(c, H2Diag_ARENA_OOM, errStart, errEnd);
    }
    t.kind = H2TCType_PACK;
    t.builtin = H2Builtin_INVALID;
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
    return H2TCAddType(c, &t, errStart, errEnd);
}

int H2TCParseArrayLen(H2TypeCheckCtx* c, const H2AstNode* node, uint32_t* outLen) {
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

int H2TCResolveIndexBaseInfo(H2TypeCheckCtx* c, int32_t baseType, H2TCIndexBaseInfo* out) {
    const H2TCType* t;
    int32_t         resolvedBaseType;
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
    resolvedBaseType = H2TCResolveAliasBaseType(c, baseType);
    if (resolvedBaseType == c->typeStr) {
        int32_t u8Type = H2TCFindBuiltinByKind(c, H2Builtin_U8);
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
        case H2TCType_ARRAY:
            out->elemType = t->baseType;
            out->indexable = 1;
            out->sliceable = 1;
            out->sliceMutable = 1;
            out->hasKnownLen = 1;
            out->knownLen = t->arrayLen;
            return 0;
        case H2TCType_SLICE:
            out->elemType = t->baseType;
            out->indexable = 1;
            out->sliceable = 1;
            out->sliceMutable = H2TCTypeIsMutable(t);
            return 0;
        case H2TCType_PTR: {
            int32_t pointee = t->baseType;
            int32_t resolvedPointee;
            if (pointee < 0 || (uint32_t)pointee >= c->typeLen) {
                return -1;
            }
            resolvedPointee = H2TCResolveAliasBaseType(c, pointee);
            if (resolvedPointee == c->typeStr) {
                int32_t u8Type = H2TCFindBuiltinByKind(c, H2Builtin_U8);
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
            if (c->types[pointee].kind == H2TCType_ARRAY) {
                out->elemType = c->types[pointee].baseType;
                out->indexable = 1;
                out->sliceable = 1;
                out->sliceMutable = 1;
                out->hasKnownLen = 1;
                out->knownLen = c->types[pointee].arrayLen;
                return 0;
            }
            if (c->types[pointee].kind == H2TCType_SLICE) {
                out->elemType = c->types[pointee].baseType;
                out->indexable = 1;
                out->sliceable = 1;
                out->sliceMutable = H2TCTypeIsMutable(&c->types[pointee]);
                return 0;
            }
            out->elemType = pointee;
            out->indexable = 1;
            out->sliceMutable = 1;
            return 0;
        }
        case H2TCType_REF: {
            int32_t refBase = t->baseType;
            int32_t resolvedRefBase;
            if (refBase < 0 || (uint32_t)refBase >= c->typeLen) {
                return -1;
            }
            resolvedRefBase = H2TCResolveAliasBaseType(c, refBase);
            if (resolvedRefBase == c->typeStr) {
                int32_t u8Type = H2TCFindBuiltinByKind(c, H2Builtin_U8);
                if (u8Type < 0) {
                    return -1;
                }
                out->elemType = u8Type;
                out->indexable = 1;
                out->sliceable = 1;
                out->sliceMutable = H2TCTypeIsMutable(t);
                out->isStringLike = 1;
                return 0;
            }
            if (c->types[refBase].kind == H2TCType_ARRAY) {
                out->elemType = c->types[refBase].baseType;
                out->indexable = 1;
                out->sliceable = 1;
                out->sliceMutable = H2TCTypeIsMutable(t);
                out->hasKnownLen = 1;
                out->knownLen = c->types[refBase].arrayLen;
                return 0;
            }
            if (c->types[refBase].kind == H2TCType_SLICE) {
                out->elemType = c->types[refBase].baseType;
                out->indexable = 1;
                out->sliceable = 1;
                out->sliceMutable = H2TCTypeIsMutable(t) && H2TCTypeIsMutable(&c->types[refBase]);
                return 0;
            }
            out->elemType = refBase;
            out->indexable = 1;
            out->sliceMutable = H2TCTypeIsMutable(t);
            return 0;
        }
        default: return 0;
    }
}

int32_t H2TCListItemAt(const H2Ast* ast, int32_t listNode, uint32_t index) {
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

uint32_t H2TCListCount(const H2Ast* ast, int32_t listNode) {
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

int H2TCHasForeignImportDirective(const H2Ast* ast, H2StrView src, int32_t nodeId) {
    int32_t child;
    int32_t first = -1;
    if (ast == NULL || nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return 0;
    }
    child = H2AstFirstChild(ast, ast->root);
    while (child >= 0) {
        const H2AstNode* n = &ast->nodes[child];
        if (n->kind == H2Ast_DIRECTIVE) {
            if (first < 0) {
                first = child;
            }
        } else {
            if (child == nodeId && first >= 0) {
                int32_t dir = first;
                while (dir >= 0 && ast->nodes[dir].kind == H2Ast_DIRECTIVE) {
                    const H2AstNode* dn = &ast->nodes[dir];
                    uint32_t         len = dn->dataEnd - dn->dataStart;
                    if ((len == 8u && memcmp(src.ptr + dn->dataStart, "c_import", 8u) == 0)
                        || (len == 11u && memcmp(src.ptr + dn->dataStart, "wasm_import", 11u) == 0))
                    {
                        return 1;
                    }
                    dir = H2AstNextSibling(ast, dir);
                }
                return 0;
            }
            first = -1;
        }
        child = H2AstNextSibling(ast, child);
    }
    return 0;
}

int H2TCVarLikeGetParts(H2TypeCheckCtx* c, int32_t nodeId, H2TCVarLikeParts* out) {
    int32_t          firstChild;
    const H2AstNode* firstNode;
    out->nameListNode = -1;
    out->typeNode = -1;
    out->initNode = -1;
    out->nameCount = 0;
    out->grouped = 0;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return -1;
    }
    firstChild = H2AstFirstChild(c->ast, nodeId);
    if (firstChild < 0) {
        return 0;
    }
    firstNode = &c->ast->nodes[firstChild];
    if (firstNode->kind == H2Ast_NAME_LIST) {
        int32_t afterNames = H2AstNextSibling(c->ast, firstChild);
        out->grouped = 1;
        out->nameListNode = firstChild;
        out->nameCount = H2TCListCount(c->ast, firstChild);
        if (afterNames >= 0 && H2TCIsTypeNodeKind(c->ast->nodes[afterNames].kind)) {
            out->typeNode = afterNames;
            out->initNode = H2AstNextSibling(c->ast, afterNames);
        } else {
            out->initNode = afterNames;
        }
        return 0;
    }
    out->grouped = 0;
    out->nameCount = 1;
    if (H2TCIsTypeNodeKind(firstNode->kind)) {
        out->typeNode = firstChild;
        out->initNode = H2AstNextSibling(c->ast, firstChild);
    } else {
        out->initNode = firstChild;
    }
    return 0;
}

int32_t H2TCVarLikeNameIndexBySlice(
    H2TypeCheckCtx* c, int32_t nodeId, uint32_t start, uint32_t end) {
    H2TCVarLikeParts parts;
    const H2AstNode* n;
    uint32_t         i;
    if (H2TCVarLikeGetParts(c, nodeId, &parts) != 0 || parts.nameCount == 0) {
        return -1;
    }
    n = &c->ast->nodes[nodeId];
    if (!parts.grouped) {
        return H2NameEqSlice(c->src, n->dataStart, n->dataEnd, start, end) ? 0 : -1;
    }
    for (i = 0; i < parts.nameCount; i++) {
        int32_t item = H2TCListItemAt(c->ast, parts.nameListNode, i);
        if (item >= 0
            && H2NameEqSlice(
                c->src, c->ast->nodes[item].dataStart, c->ast->nodes[item].dataEnd, start, end))
        {
            return (int32_t)i;
        }
    }
    return -1;
}

int32_t H2TCVarLikeInitExprNodeAt(H2TypeCheckCtx* c, int32_t nodeId, int32_t nameIndex) {
    H2TCVarLikeParts parts;
    uint32_t         initCount;
    int32_t          onlyInit;
    if (H2TCVarLikeGetParts(c, nodeId, &parts) != 0) {
        return -1;
    }
    if (!parts.grouped) {
        return (nameIndex == 0) ? parts.initNode : -1;
    }
    if (parts.initNode < 0 || c->ast->nodes[parts.initNode].kind != H2Ast_EXPR_LIST
        || nameIndex < 0)
    {
        return -1;
    }
    initCount = H2TCListCount(c->ast, parts.initNode);
    if (initCount == parts.nameCount) {
        return H2TCListItemAt(c->ast, parts.initNode, (uint32_t)nameIndex);
    }
    if (initCount != 1u) {
        return -1;
    }
    onlyInit = H2TCListItemAt(c->ast, parts.initNode, 0);
    if (onlyInit < 0 || (uint32_t)onlyInit >= c->ast->len
        || c->ast->nodes[onlyInit].kind != H2Ast_TUPLE_EXPR)
    {
        return -1;
    }
    return H2TCListItemAt(c->ast, onlyInit, (uint32_t)nameIndex);
}

int32_t H2TCVarLikeInitExprNode(H2TypeCheckCtx* c, int32_t nodeId) {
    return H2TCVarLikeInitExprNodeAt(c, nodeId, 0);
}

H2_API_END
