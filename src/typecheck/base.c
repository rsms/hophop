#include "internal.h"

SL_API_BEGIN

static int32_t SLTCFindNamedTypeIndexByTypeId(SLTypeCheckCtx* c, int32_t typeId);

void SLTCSetDiag(SLDiag* diag, SLDiagCode code, uint32_t start, uint32_t end) {
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

void SLTCSetDiagWithArg(
    SLDiag*    diag,
    SLDiagCode code,
    uint32_t   start,
    uint32_t   end,
    uint32_t   argStart,
    uint32_t   argEnd) {
    if (diag == NULL) {
        return;
    }
    diag->code = code;
    diag->type = SLDiagTypeOfCode(code);
    diag->start = start;
    diag->end = end;
    diag->argStart = argStart;
    diag->argEnd = argEnd;
    diag->detail = NULL;
    diag->hintOverride = NULL;
}

int SLTCFailSpan(SLTypeCheckCtx* c, SLDiagCode code, uint32_t start, uint32_t end) {
    SLTCSetDiag(c->diag, code, start, end);
    return -1;
}

int SLTCFailNode(SLTypeCheckCtx* c, int32_t nodeId, SLDiagCode code) {
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return SLTCFailSpan(c, code, 0, 0);
    }
    return SLTCFailSpan(c, code, c->ast->nodes[nodeId].start, c->ast->nodes[nodeId].end);
}

const char* _Nullable SLTCAllocCStringBytes(SLTypeCheckCtx* c, const uint8_t* bytes, uint32_t len) {
    char* s;
    if (c == NULL) {
        return NULL;
    }
    s = (char*)SLArenaAlloc(c->arena, len + 1u, 1u);
    if (s == NULL) {
        return NULL;
    }
    if (len > 0u && bytes != NULL) {
        memcpy(s, bytes, len);
    }
    s[len] = '\0';
    return s;
}

int SLTCStrEqNullable(const char* _Nullable a, const char* _Nullable b) {
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

int SLTCEmitWarningDiag(SLTypeCheckCtx* c, const SLDiag* diag) {
    uint32_t i;
    if (c == NULL || diag == NULL || diag->type != SLDiagType_WARNING) {
        return 0;
    }
    for (i = 0; i < c->warningDedupLen; i++) {
        const SLTCWarningDedup* seen = &c->warningDedup[i];
        if (seen->start == diag->start && seen->end == diag->end
            && SLTCStrEqNullable(seen->message, diag->detail))
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

int SLTCRecordConstDiagUse(SLTypeCheckCtx* c, int32_t nodeId) {
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
        return SLTCFailNode(c, nodeId, SLDiag_ARENA_OOM);
    }
    c->constDiagUses[c->constDiagUseLen].nodeId = nodeId;
    c->constDiagUses[c->constDiagUseLen].ownerFnIndex = c->currentFunctionIndex;
    c->constDiagUses[c->constDiagUseLen].executed = 0;
    c->constDiagUseLen++;
    return 0;
}

void SLTCMarkConstDiagUseExecuted(SLTypeCheckCtx* c, int32_t nodeId) {
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

void SLTCMarkConstDiagFnInvoked(SLTypeCheckCtx* c, int32_t fnIndex) {
    if (c == NULL || fnIndex < 0 || (uint32_t)fnIndex >= c->constDiagFnInvokedCap
        || c->constDiagFnInvoked == NULL)
    {
        return;
    }
    c->constDiagFnInvoked[fnIndex] = 1;
}

int SLTCValidateConstDiagUses(SLTypeCheckCtx* c) {
    uint32_t i;
    if (c == NULL) {
        return -1;
    }
    for (i = 0; i < c->constDiagUseLen; i++) {
        const SLTCConstDiagUse* use = &c->constDiagUses[i];
        if (use->ownerFnIndex >= 0) {
            if ((uint32_t)use->ownerFnIndex < c->constDiagFnInvokedCap
                && c->constDiagFnInvoked != NULL && c->constDiagFnInvoked[use->ownerFnIndex])
            {
                continue;
            }
        } else if (use->executed) {
            continue;
        }
        return SLTCFailNode(c, use->nodeId, SLDiag_CONSTEVAL_DIAG_NON_CONST_CONTEXT);
    }
    return 0;
}

static SLTCLocalUse* _Nullable SLTCGetLocalUseByIndex(SLTypeCheckCtx* c, int32_t localIdx) {
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

void SLTCMarkFunctionUsed(SLTypeCheckCtx* c, int32_t fnIndex) {
    const SLTCFunction* fn;
    uint32_t            i;
    if (c == NULL || fnIndex < 0 || (uint32_t)fnIndex >= c->funcUsedCap || c->funcUsed == NULL) {
        return;
    }
    c->funcUsed[fnIndex] = 1;
    if ((uint32_t)fnIndex >= c->funcLen) {
        return;
    }
    fn = &c->funcs[fnIndex];
    if ((fn->flags & SLTCFunctionFlag_TEMPLATE_INSTANCE) == 0) {
        return;
    }
    for (i = 0; i < c->funcLen; i++) {
        const SLTCFunction* cand = &c->funcs[i];
        if (cand->declNode == fn->declNode
            && (cand->flags & SLTCFunctionFlag_TEMPLATE_INSTANCE) == 0)
        {
            c->funcUsed[i] = 1;
        }
    }
}

void SLTCMarkLocalRead(SLTypeCheckCtx* c, int32_t localIdx) {
    SLTCLocalUse* use = SLTCGetLocalUseByIndex(c, localIdx);
    if (use == NULL) {
        return;
    }
    if (use->readCount < UINT32_MAX) {
        use->readCount++;
    }
}

void SLTCMarkLocalWrite(SLTypeCheckCtx* c, int32_t localIdx) {
    SLTCLocalUse* use = SLTCGetLocalUseByIndex(c, localIdx);
    if (use == NULL) {
        return;
    }
    if (use->writeCount < UINT32_MAX) {
        use->writeCount++;
    }
}

void SLTCUnmarkLocalRead(SLTypeCheckCtx* c, int32_t localIdx) {
    SLTCLocalUse* use = SLTCGetLocalUseByIndex(c, localIdx);
    if (use == NULL || use->readCount == 0) {
        return;
    }
    use->readCount--;
}

void SLTCSetLocalUsageKind(SLTypeCheckCtx* c, int32_t localIdx, uint8_t kind) {
    SLTCLocalUse* use = SLTCGetLocalUseByIndex(c, localIdx);
    if (use == NULL) {
        return;
    }
    use->kind = kind;
}

void SLTCSetLocalUsageSuppress(SLTypeCheckCtx* c, int32_t localIdx, int suppress) {
    SLTCLocalUse* use = SLTCGetLocalUseByIndex(c, localIdx);
    if (use == NULL) {
        return;
    }
    use->suppressWarning = suppress ? 1u : 0u;
}

int SLTCEmitUnusedSymbolWarnings(SLTypeCheckCtx* c) {
    uint32_t i;
    if (c == NULL) {
        return -1;
    }
    for (i = 0; i < c->localUseLen; i++) {
        const SLTCLocalUse* use = &c->localUses[i];
        SLDiagCode          code;
        SLDiag              warning;
        if (use->ownerFnIndex < 0 || use->suppressWarning || use->readCount > 0) {
            continue;
        }
        if (use->kind == SLTCLocalUseKind_PARAM) {
            code =
                use->writeCount > 0 ? SLDiag_UNUSED_PARAMETER_NEVER_READ : SLDiag_UNUSED_PARAMETER;
        } else {
            code = use->writeCount > 0 ? SLDiag_UNUSED_VARIABLE_NEVER_READ : SLDiag_UNUSED_VARIABLE;
        }
        warning = (SLDiag){ 0 };
        SLTCSetDiagWithArg(
            &warning, code, use->nameStart, use->nameEnd, use->nameStart, use->nameEnd);
        warning.type = SLDiagType_WARNING;
        if (SLTCEmitWarningDiag(c, &warning) != 0) {
            return -1;
        }
    }
    for (i = 0; i < c->funcLen; i++) {
        const SLTCFunction* fn = &c->funcs[i];
        int32_t             fnNode;
        SLDiag              warning;
        if (fn->defNode < 0) {
            continue;
        }
        if ((fn->flags & SLTCFunctionFlag_TEMPLATE_INSTANCE) != 0) {
            continue;
        }
        if (c->funcUsed != NULL && i < c->funcUsedCap && c->funcUsed[i]) {
            continue;
        }
        if (SLTCIsMainFunction(c, fn)) {
            continue;
        }
        fnNode = fn->defNode >= 0 ? fn->defNode : fn->declNode;
        if (fnNode < 0 || (uint32_t)fnNode >= c->ast->len) {
            continue;
        }
        if ((c->ast->nodes[fnNode].flags & SLAstFlag_PUB) != 0) {
            continue;
        }
        warning = (SLDiag){ 0 };
        SLTCSetDiagWithArg(
            &warning,
            SLDiag_UNUSED_FUNCTION,
            fn->nameStart,
            fn->nameEnd,
            fn->nameStart,
            fn->nameEnd);
        warning.type = SLDiagType_WARNING;
        if (SLTCEmitWarningDiag(c, &warning) != 0) {
            return -1;
        }
    }
    return 0;
}

void SLTCOffsetToLineCol(
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

int SLTCLineColToOffset(
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

void SLTCTextBufInit(SLTCTextBuf* b, char* ptr, uint32_t cap) {
    b->ptr = ptr;
    b->cap = cap;
    b->len = 0;
    if (cap > 0) {
        ptr[0] = '\0';
    }
}

void SLTCTextBufAppendChar(SLTCTextBuf* b, char ch) {
    if (b->cap == 0 || b->ptr == NULL) {
        return;
    }
    if (b->len + 1u < b->cap) {
        b->ptr[b->len++] = ch;
        b->ptr[b->len] = '\0';
    }
}

void SLTCTextBufAppendCStr(SLTCTextBuf* b, const char* s) {
    uint32_t i = 0;
    if (s == NULL) {
        return;
    }
    while (s[i] != '\0') {
        SLTCTextBufAppendChar(b, s[i]);
        i++;
    }
}

void SLTCTextBufAppendSlice(SLTCTextBuf* b, SLStrView src, uint32_t start, uint32_t end) {
    uint32_t i;
    if (end < start || end > src.len) {
        return;
    }
    for (i = start; i < end; i++) {
        SLTCTextBufAppendChar(b, src.ptr[i]);
    }
}

void SLTCTextBufAppendU32(SLTCTextBuf* b, uint32_t v) {
    char     tmp[16];
    uint32_t n = 0;
    if (v == 0) {
        SLTCTextBufAppendChar(b, '0');
        return;
    }
    while (v > 0 && n < (uint32_t)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0) {
        n--;
        SLTCTextBufAppendChar(b, tmp[n]);
    }
}

void SLTCTextBufAppendHexU64(SLTCTextBuf* b, uint64_t v) {
    char     tmp[16];
    uint32_t n = 0;
    if (v == 0u) {
        SLTCTextBufAppendChar(b, '0');
        return;
    }
    while (v > 0u && n < (uint32_t)sizeof(tmp)) {
        uint32_t digit = (uint32_t)(v & 0xFu);
        tmp[n++] = (char)(digit < 10u ? ('0' + digit) : ('a' + (digit - 10u)));
        v >>= 4u;
    }
    while (n > 0u) {
        n--;
        SLTCTextBufAppendChar(b, tmp[n]);
    }
}

const char* SLTCBuiltinName(SLTypeCheckCtx* c, int32_t typeId, SLBuiltinKind kind) {
    switch (kind) {
        case SLBuiltin_VOID:   return "void";
        case SLBuiltin_BOOL:   return "bool";
        case SLBuiltin_TYPE:   return "type";
        case SLBuiltin_U8:     return "u8";
        case SLBuiltin_U16:    return "u16";
        case SLBuiltin_U32:    return "u32";
        case SLBuiltin_U64:    return "u64";
        case SLBuiltin_I8:     return "i8";
        case SLBuiltin_I16:    return "i16";
        case SLBuiltin_I32:    return "i32";
        case SLBuiltin_I64:    return "i64";
        case SLBuiltin_USIZE:  return "uint";
        case SLBuiltin_ISIZE:  return "int";
        case SLBuiltin_RAWPTR: return "rawptr";
        case SLBuiltin_F32:    return "f32";
        case SLBuiltin_F64:    return "f64";
        case SLBuiltin_STR:    return "str";
        case SLBuiltin_INVALID:
            if (typeId >= 0 && typeId == c->typeStr) {
                return "str";
            }
            return "<builtin>";
    }
    return "<builtin>";
}

void SLTCFormatTypeRec(SLTypeCheckCtx* c, int32_t typeId, SLTCTextBuf* b, uint32_t depth) {
    const SLTCType* t;
    if (depth > 32u) {
        SLTCTextBufAppendCStr(b, "...");
        return;
    }
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        SLTCTextBufAppendCStr(b, "<invalid>");
        return;
    }
    t = &c->types[typeId];
    switch (t->kind) {
        case SLTCType_BUILTIN:
            SLTCTextBufAppendCStr(b, SLTCBuiltinName(c, typeId, t->builtin));
            return;
        case SLTCType_NAMED:
        case SLTCType_ALIAS:
            if (typeId == c->typeRune && (t->nameEnd <= t->nameStart || t->nameEnd > c->src.len)) {
                SLTCTextBufAppendCStr(b, "rune");
                return;
            }
            if (t->nameEnd > t->nameStart && t->nameEnd <= c->src.len) {
                SLTCTextBufAppendSlice(b, c->src, t->nameStart, t->nameEnd);
            } else {
                SLTCTextBufAppendCStr(b, "<unnamed>");
            }
            {
                int32_t namedIndex = SLTCFindNamedTypeIndexByTypeId(c, typeId);
                if (namedIndex >= 0 && c->namedTypes[(uint32_t)namedIndex].templateArgCount > 0) {
                    uint16_t i;
                    SLTCTextBufAppendChar(b, '[');
                    for (i = 0; i < c->namedTypes[(uint32_t)namedIndex].templateArgCount; i++) {
                        if (i > 0) {
                            SLTCTextBufAppendCStr(b, ", ");
                        }
                        SLTCFormatTypeRec(
                            c,
                            c->genericArgTypes
                                [c->namedTypes[(uint32_t)namedIndex].templateArgStart + i],
                            b,
                            depth + 1u);
                    }
                    SLTCTextBufAppendChar(b, ']');
                }
            }
            return;
        case SLTCType_PTR:
            SLTCTextBufAppendChar(b, '*');
            SLTCFormatTypeRec(c, t->baseType, b, depth + 1u);
            return;
        case SLTCType_REF:
            SLTCTextBufAppendChar(b, (t->flags & SLTCTypeFlag_MUTABLE) != 0 ? '*' : '&');
            SLTCFormatTypeRec(c, t->baseType, b, depth + 1u);
            return;
        case SLTCType_ARRAY:
            SLTCTextBufAppendChar(b, '[');
            SLTCFormatTypeRec(c, t->baseType, b, depth + 1u);
            SLTCTextBufAppendChar(b, ' ');
            SLTCTextBufAppendU32(b, t->arrayLen);
            SLTCTextBufAppendChar(b, ']');
            return;
        case SLTCType_SLICE:
            SLTCTextBufAppendChar(b, '[');
            SLTCFormatTypeRec(c, t->baseType, b, depth + 1u);
            SLTCTextBufAppendChar(b, ']');
            return;
        case SLTCType_OPTIONAL:
            SLTCTextBufAppendChar(b, '?');
            SLTCFormatTypeRec(c, t->baseType, b, depth + 1u);
            return;
        case SLTCType_TUPLE: {
            uint32_t i;
            SLTCTextBufAppendChar(b, '(');
            for (i = 0; i < t->fieldCount; i++) {
                if (i > 0) {
                    SLTCTextBufAppendCStr(b, ", ");
                }
                SLTCFormatTypeRec(c, c->funcParamTypes[t->fieldStart + i], b, depth + 1u);
            }
            SLTCTextBufAppendChar(b, ')');
            return;
        }
        case SLTCType_PACK: {
            uint32_t i;
            SLTCTextBufAppendCStr(b, "pack(");
            for (i = 0; i < t->fieldCount; i++) {
                if (i > 0) {
                    SLTCTextBufAppendCStr(b, ", ");
                }
                SLTCFormatTypeRec(c, c->funcParamTypes[t->fieldStart + i], b, depth + 1u);
            }
            SLTCTextBufAppendChar(b, ')');
            return;
        }
        case SLTCType_TYPE_PARAM:
            if (t->nameEnd > t->nameStart && t->nameEnd <= c->src.len) {
                SLTCTextBufAppendSlice(b, c->src, t->nameStart, t->nameEnd);
            } else {
                SLTCTextBufAppendCStr(b, "<type-param>");
            }
            return;
        case SLTCType_ANYTYPE:       SLTCTextBufAppendCStr(b, "anytype"); return;
        case SLTCType_UNTYPED_INT:   SLTCTextBufAppendCStr(b, "const_int"); return;
        case SLTCType_UNTYPED_FLOAT: SLTCTextBufAppendCStr(b, "const_float"); return;
        case SLTCType_NULL:          SLTCTextBufAppendCStr(b, "null"); return;
        case SLTCType_FUNCTION:      SLTCTextBufAppendCStr(b, "fn(...)"); return;
        case SLTCType_ANON_STRUCT:   SLTCTextBufAppendCStr(b, "struct{...}"); return;
        case SLTCType_ANON_UNION:    SLTCTextBufAppendCStr(b, "union{...}"); return;
        case SLTCType_INVALID:
        default:                     SLTCTextBufAppendCStr(b, "<type>"); return;
    }
}

int SLTCExprIsStringConstant(SLTypeCheckCtx* c, int32_t nodeId) {
    const SLAstNode* n;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return 0;
    }
    n = &c->ast->nodes[nodeId];
    if (n->kind == SLAst_STRING) {
        return 1;
    }
    if (n->kind == SLAst_BINARY && (SLTokenKind)n->op == SLTok_ADD
        && SLIsStringLiteralConcatChain(c->ast, nodeId))
    {
        return 1;
    }
    return 0;
}

void SLTCFormatExprSubject(SLTypeCheckCtx* c, int32_t nodeId, SLTCTextBuf* b) {
    const SLAstNode* n;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        SLTCTextBufAppendCStr(b, "expression");
        return;
    }
    n = &c->ast->nodes[nodeId];
    if (n->kind == SLAst_IDENT && n->dataEnd > n->dataStart && n->dataEnd <= c->src.len) {
        SLTCTextBufAppendChar(b, '\'');
        SLTCTextBufAppendSlice(b, c->src, n->dataStart, n->dataEnd);
        SLTCTextBufAppendChar(b, '\'');
        return;
    }
    if (SLTCExprIsStringConstant(c, nodeId)) {
        SLTCTextBufAppendCStr(b, "string constant");
        return;
    }
    SLTCTextBufAppendCStr(b, "expression");
}

char* _Nullable SLTCAllocDiagText(SLTypeCheckCtx* c, const char* text) {
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
    p = (char*)SLArenaAlloc(c->arena, len + 1u, 1u);
    if (p == NULL) {
        return NULL;
    }
    memcpy(p, text, len + 1u);
    return p;
}

int SLTCFailTypeMismatchDetail(
    SLTypeCheckCtx* c, int32_t failNode, int32_t exprNode, int32_t srcType, int32_t dstType) {
    uint32_t    start = 0;
    uint32_t    end = 0;
    char        subjectBuf[SLTC_DIAG_TEXT_CAP];
    char        srcTypeBuf[SLTC_DIAG_TEXT_CAP];
    char        dstTypeBuf[SLTC_DIAG_TEXT_CAP];
    char        detailBuf[384];
    SLTCTextBuf subject;
    SLTCTextBuf srcTypeText;
    SLTCTextBuf dstTypeText;
    SLTCTextBuf detailText;
    char*       detail;
    int         sourceIsStringLike = 0;

    if (failNode >= 0 && (uint32_t)failNode < c->ast->len) {
        start = c->ast->nodes[failNode].start;
        end = c->ast->nodes[failNode].end;
    }
    SLTCSetDiag(c->diag, SLDiag_TYPE_MISMATCH, start, end);
    if (c->diag == NULL) {
        return -1;
    }

    SLTCTextBufInit(&subject, subjectBuf, (uint32_t)sizeof(subjectBuf));
    SLTCTextBufInit(&srcTypeText, srcTypeBuf, (uint32_t)sizeof(srcTypeBuf));
    SLTCTextBufInit(&dstTypeText, dstTypeBuf, (uint32_t)sizeof(dstTypeBuf));
    SLTCFormatExprSubject(c, exprNode, &subject);
    SLTCFormatTypeRec(c, srcType, &srcTypeText, 0);
    SLTCFormatTypeRec(c, dstType, &dstTypeText, 0);

    SLTCTextBufInit(&detailText, detailBuf, (uint32_t)sizeof(detailBuf));
    SLTCTextBufAppendCStr(&detailText, "cannot use ");
    SLTCTextBufAppendCStr(&detailText, subjectBuf);
    SLTCTextBufAppendCStr(&detailText, " of type ");
    SLTCTextBufAppendCStr(&detailText, srcTypeBuf);
    SLTCTextBufAppendCStr(&detailText, " as ");
    SLTCTextBufAppendCStr(&detailText, dstTypeBuf);
    detail = SLTCAllocDiagText(c, detailBuf);
    if (detail != NULL) {
        c->diag->detail = detail;
    }

    if (srcType >= 0 && (uint32_t)srcType < c->typeLen) {
        const SLTCType* src = &c->types[srcType];
        sourceIsStringLike =
            srcType == c->typeStr
            || ((src->kind == SLTCType_PTR || src->kind == SLTCType_REF)
                && src->baseType == c->typeStr);
    }

    if (dstType == c->typeStr && SLTCExprIsStringConstant(c, exprNode) && sourceIsStringLike) {
        c->diag->hintOverride = SLTCAllocDiagText(c, "change type to &str or *str");
    }
    return -1;
}

int SLTCFailAssignToConst(SLTypeCheckCtx* c, int32_t lhsNode) {
    int rc = SLTCFailNode(c, lhsNode, SLDiag_TYPE_MISMATCH);
    c->diag->detail = SLTCAllocDiagText(c, "assignment target is const");
    return rc;
}

int SLTCFailSwitchMissingCases(
    SLTypeCheckCtx* c,
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
    char              typeBuf[SLTC_DIAG_TEXT_CAP];
    SLTCTextBuf       typeText;
    uint32_t          missingCount = 0;
    uint32_t          missingNameLen = 0;
    uint32_t          detailLen;
    uint32_t          i;
    char*             detail;
    SLTCTextBuf       detailText;
    static const char prefix[] = "of type ";

    if (failNode >= 0 && (uint32_t)failNode < c->ast->len) {
        start = c->ast->nodes[failNode].start;
        end = c->ast->nodes[failNode].end;
    }
    SLTCSetDiag(c->diag, SLDiag_SWITCH_MISSING_CASES, start, end);
    if (c->diag == NULL) {
        return -1;
    }

    SLTCTextBufInit(&typeText, typeBuf, (uint32_t)sizeof(typeBuf));
    SLTCFormatTypeRec(c, subjectType, &typeText, 0);

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

    detail = (char*)SLArenaAlloc(c->arena, detailLen, 1u);
    if (detail == NULL) {
        return -1;
    }

    SLTCTextBufInit(&detailText, detail, detailLen);
    SLTCTextBufAppendCStr(&detailText, prefix);
    SLTCTextBufAppendCStr(&detailText, typeBuf);
    SLTCTextBufAppendCStr(&detailText, ": ");

    if (subjectEnumType >= 0) {
        int first = 1;
        for (i = 0; i < enumVariantCount; i++) {
            if (enumCovered[i]) {
                continue;
            }
            if (!first) {
                SLTCTextBufAppendCStr(&detailText, ", ");
            }
            first = 0;
            SLTCTextBufAppendSlice(&detailText, c->src, enumVariantStarts[i], enumVariantEnds[i]);
        }
    } else {
        int first = 1;
        if (!boolCoveredTrue) {
            SLTCTextBufAppendCStr(&detailText, "true");
            first = 0;
        }
        if (!boolCoveredFalse) {
            if (!first) {
                SLTCTextBufAppendCStr(&detailText, ", ");
            }
            SLTCTextBufAppendCStr(&detailText, "false");
        }
    }

    c->diag->detail = detail;
    return -1;
}

int32_t SLAstFirstChild(const SLAst* ast, int32_t nodeId) {
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    return ast->nodes[nodeId].firstChild;
}

int32_t SLAstNextSibling(const SLAst* ast, int32_t nodeId) {
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    return ast->nodes[nodeId].nextSibling;
}

int SLNameEqSlice(SLStrView src, uint32_t aStart, uint32_t aEnd, uint32_t bStart, uint32_t bEnd) {
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

int SLNameEqLiteral(SLStrView src, uint32_t start, uint32_t end, const char* lit) {
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

int SLNameHasPrefix(SLStrView src, uint32_t start, uint32_t end, const char* prefix) {
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

int SLNameHasSuffix(SLStrView src, uint32_t start, uint32_t end, const char* suffix) {
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

int32_t SLTCFindMemAllocatorType(SLTypeCheckCtx* c) {
    return c->typeMemAllocator;
}

int32_t SLTCGetStrRefType(SLTypeCheckCtx* c, uint32_t start, uint32_t end) {
    if (c->typeStr < 0) {
        return -1;
    }
    return SLTCInternRefType(c, c->typeStr, 0, start, end);
}

int32_t SLTCGetStrPtrType(SLTypeCheckCtx* c, uint32_t start, uint32_t end) {
    if (c->typeStr < 0) {
        return -1;
    }
    return SLTCInternPtrType(c, c->typeStr, start, end);
}

int32_t SLTCAddType(SLTypeCheckCtx* c, const SLTCType* t, uint32_t errStart, uint32_t errEnd) {
    int32_t idx;
    if (c->typeLen >= c->typeCap) {
        SLTCSetDiag(c->diag, SLDiag_ARENA_OOM, errStart, errEnd);
        return -1;
    }
    idx = (int32_t)c->typeLen++;
    c->types[idx] = *t;
    return idx;
}

int32_t SLTCAddBuiltinType(SLTypeCheckCtx* c, const char* name, SLBuiltinKind builtinKind) {
    SLTCType t;
    uint32_t i = 0;
    while (name[i] != '\0') {
        i++;
    }
    t.kind = SLTCType_BUILTIN;
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
    return SLTCAddType(c, &t, 0, 0);
}

int SLTCEnsureInitialized(SLTypeCheckCtx* c) {
    SLTCType t;
    int32_t  u32Type;

    c->typeVoid = -1;
    c->typeBool = -1;
    c->typeStr = -1;
    c->typeType = -1;
    c->typeRune = -1;
    c->typeMemAllocator = -1;
    c->typeUsize = -1;
    c->typeRawptr = -1;
    c->typeReflectSpan = -1;
    c->typeFmtValue = -1;
    c->typeUntypedInt = -1;
    c->typeUntypedFloat = -1;
    c->typeNull = -1;
    c->typeAnytype = -1;

    c->typeVoid = SLTCAddBuiltinType(c, "void", SLBuiltin_VOID);
    c->typeBool = SLTCAddBuiltinType(c, "bool", SLBuiltin_BOOL);
    c->typeType = SLTCAddBuiltinType(c, "type", SLBuiltin_TYPE);
    if (c->typeVoid < 0 || c->typeBool < 0 || c->typeType < 0) {
        return -1;
    }

    if (SLTCAddBuiltinType(c, "u8", SLBuiltin_U8) < 0
        || SLTCAddBuiltinType(c, "u16", SLBuiltin_U16) < 0
        || SLTCAddBuiltinType(c, "u32", SLBuiltin_U32) < 0
        || SLTCAddBuiltinType(c, "u64", SLBuiltin_U64) < 0
        || SLTCAddBuiltinType(c, "i8", SLBuiltin_I8) < 0
        || SLTCAddBuiltinType(c, "i16", SLBuiltin_I16) < 0
        || SLTCAddBuiltinType(c, "i32", SLBuiltin_I32) < 0
        || SLTCAddBuiltinType(c, "i64", SLBuiltin_I64) < 0
        || SLTCAddBuiltinType(c, "uint", SLBuiltin_USIZE) < 0
        || SLTCAddBuiltinType(c, "int", SLBuiltin_ISIZE) < 0
        || SLTCAddBuiltinType(c, "rawptr", SLBuiltin_RAWPTR) < 0
        || SLTCAddBuiltinType(c, "f32", SLBuiltin_F32) < 0
        || SLTCAddBuiltinType(c, "f64", SLBuiltin_F64) < 0)
    {
        return -1;
    }
    c->typeRawptr = SLTCFindBuiltinByKind(c, SLBuiltin_RAWPTR);
    if (c->typeRawptr < 0) {
        return -1;
    }
    c->typeStr = SLTCAddBuiltinType(c, "str", SLBuiltin_INVALID);
    if (c->typeStr < 0) {
        return -1;
    }
    u32Type = SLTCFindBuiltinByKind(c, SLBuiltin_U32);
    if (u32Type < 0) {
        return -1;
    }
    t.kind = SLTCType_ALIAS;
    t.builtin = SLBuiltin_INVALID;
    t.baseType = u32Type;
    t.declNode = -1;
    t.funcIndex = -1;
    t.arrayLen = 0;
    t.nameStart = 0;
    t.nameEnd = 0;
    t.fieldStart = 0;
    t.fieldCount = 0;
    t.flags = SLTCTypeFlag_ALIAS_RESOLVED;
    c->typeRune = SLTCAddType(c, &t, 0, 0);
    if (c->typeRune < 0) {
        return -1;
    }

    t.kind = SLTCType_ANYTYPE;
    t.builtin = SLBuiltin_INVALID;
    t.baseType = -1;
    t.declNode = -1;
    t.funcIndex = -1;
    t.arrayLen = 0;
    t.nameStart = 0;
    t.nameEnd = 0;
    t.fieldStart = 0;
    t.fieldCount = 0;
    t.flags = 0;
    c->typeAnytype = SLTCAddType(c, &t, 0, 0);
    if (c->typeAnytype < 0) {
        return -1;
    }

    t.kind = SLTCType_UNTYPED_INT;
    t.builtin = SLBuiltin_INVALID;
    t.baseType = -1;
    t.declNode = -1;
    t.funcIndex = -1;
    t.arrayLen = 0;
    t.nameStart = 0;
    t.nameEnd = 0;
    t.fieldStart = 0;
    t.fieldCount = 0;
    t.flags = 0;
    c->typeUntypedInt = SLTCAddType(c, &t, 0, 0);
    if (c->typeUntypedInt < 0) {
        return -1;
    }

    t.kind = SLTCType_UNTYPED_FLOAT;
    c->typeUntypedFloat = SLTCAddType(c, &t, 0, 0);
    if (c->typeUntypedFloat < 0) {
        return -1;
    }

    t.kind = SLTCType_NULL;
    c->typeNull = SLTCAddType(c, &t, 0, 0);
    return c->typeNull < 0 ? -1 : 0;
}

int32_t SLTCFindBuiltinType(SLTypeCheckCtx* c, uint32_t start, uint32_t end) {
    uint32_t i;
    if (SLNameEqLiteral(c->src, start, end, "const_int")) {
        return c->typeUntypedInt;
    }
    if (SLNameEqLiteral(c->src, start, end, "const_float")) {
        return c->typeUntypedFloat;
    }
    if (SLNameEqLiteral(c->src, start, end, "str")
        || SLNameEqLiteral(c->src, start, end, "builtin__str"))
    {
        return c->typeStr;
    }
    if ((SLNameEqLiteral(c->src, start, end, "rune")
         || SLNameEqLiteral(c->src, start, end, "builtin__rune"))
        && c->typeRune >= 0)
    {
        return c->typeRune;
    }
    if (SLNameEqLiteral(c->src, start, end, "rawptr")
        || SLNameEqLiteral(c->src, start, end, "builtin__rawptr"))
    {
        return c->typeRawptr;
    }
    for (i = 0; i < c->typeLen; i++) {
        const SLTCType* t = &c->types[i];
        if (t->kind != SLTCType_BUILTIN) {
            continue;
        }
        if (SLNameEqLiteral(c->src, start, end, "bool") && t->builtin == SLBuiltin_BOOL) {
            return (int32_t)i;
        }
        if (SLNameEqLiteral(c->src, start, end, "type") && t->builtin == SLBuiltin_TYPE) {
            return (int32_t)i;
        }
        if (SLNameEqLiteral(c->src, start, end, "u8") && t->builtin == SLBuiltin_U8) {
            return (int32_t)i;
        }
        if (SLNameEqLiteral(c->src, start, end, "u16") && t->builtin == SLBuiltin_U16) {
            return (int32_t)i;
        }
        if (SLNameEqLiteral(c->src, start, end, "u32") && t->builtin == SLBuiltin_U32) {
            return (int32_t)i;
        }
        if (SLNameEqLiteral(c->src, start, end, "u64") && t->builtin == SLBuiltin_U64) {
            return (int32_t)i;
        }
        if (SLNameEqLiteral(c->src, start, end, "i8") && t->builtin == SLBuiltin_I8) {
            return (int32_t)i;
        }
        if (SLNameEqLiteral(c->src, start, end, "i16") && t->builtin == SLBuiltin_I16) {
            return (int32_t)i;
        }
        if (SLNameEqLiteral(c->src, start, end, "i32") && t->builtin == SLBuiltin_I32) {
            return (int32_t)i;
        }
        if (SLNameEqLiteral(c->src, start, end, "i64") && t->builtin == SLBuiltin_I64) {
            return (int32_t)i;
        }
        if (SLNameEqLiteral(c->src, start, end, "uint") && t->builtin == SLBuiltin_USIZE) {
            return (int32_t)i;
        }
        if (SLNameEqLiteral(c->src, start, end, "int") && t->builtin == SLBuiltin_ISIZE) {
            return (int32_t)i;
        }
        if (SLNameEqLiteral(c->src, start, end, "f32") && t->builtin == SLBuiltin_F32) {
            return (int32_t)i;
        }
        if (SLNameEqLiteral(c->src, start, end, "f64") && t->builtin == SLBuiltin_F64) {
            return (int32_t)i;
        }
    }
    return -1;
}

int32_t SLTCFindBuiltinByKind(SLTypeCheckCtx* c, SLBuiltinKind builtinKind) {
    uint32_t i;
    for (i = 0; i < c->typeLen; i++) {
        if (c->types[i].kind == SLTCType_BUILTIN && c->types[i].builtin == builtinKind) {
            return (int32_t)i;
        }
    }
    return -1;
}

uint8_t SLTCTypeTagKindOf(SLTypeCheckCtx* c, int32_t typeId) {
    const SLTCType* t;
    int32_t         declNode;
    if (c == NULL || typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return SLTCTypeTagKind_INVALID;
    }
    t = &c->types[typeId];
    switch (t->kind) {
        case SLTCType_ALIAS:       return SLTCTypeTagKind_ALIAS;
        case SLTCType_PTR:         return SLTCTypeTagKind_POINTER;
        case SLTCType_REF:         return SLTCTypeTagKind_REFERENCE;
        case SLTCType_ARRAY:       return SLTCTypeTagKind_ARRAY;
        case SLTCType_SLICE:       return SLTCTypeTagKind_SLICE;
        case SLTCType_OPTIONAL:    return SLTCTypeTagKind_OPTIONAL;
        case SLTCType_FUNCTION:    return SLTCTypeTagKind_FUNCTION;
        case SLTCType_TUPLE:       return SLTCTypeTagKind_TUPLE;
        case SLTCType_ANON_STRUCT: return SLTCTypeTagKind_STRUCT;
        case SLTCType_ANON_UNION:  return SLTCTypeTagKind_UNION;
        case SLTCType_NAMED:
            declNode = t->declNode;
            if (declNode >= 0 && (uint32_t)declNode < c->ast->len) {
                switch (c->ast->nodes[declNode].kind) {
                    case SLAst_STRUCT: return SLTCTypeTagKind_STRUCT;
                    case SLAst_UNION:  return SLTCTypeTagKind_UNION;
                    case SLAst_ENUM:   return SLTCTypeTagKind_ENUM;
                    default:           break;
                }
            }
            return SLTCTypeTagKind_PRIMITIVE;
        case SLTCType_BUILTIN:
        case SLTCType_UNTYPED_INT:
        case SLTCType_UNTYPED_FLOAT:
        case SLTCType_NULL:          return SLTCTypeTagKind_PRIMITIVE;
        default:                     return SLTCTypeTagKind_INVALID;
    }
}

uint64_t SLTCEncodeTypeTag(SLTypeCheckCtx* c, int32_t typeId) {
    uint64_t kind = (uint64_t)SLTCTypeTagKindOf(c, typeId);
    uint64_t id = typeId >= 0 ? ((uint64_t)(uint32_t)typeId + 1u) : 0u;
    return (kind << 56u) | (id & 0x00ffffffffffffffULL);
}

int SLTCDecodeTypeTag(SLTypeCheckCtx* c, uint64_t typeTag, int32_t* outTypeId) {
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

static int32_t SLTCFindNamedTypeIndexByTypeId(SLTypeCheckCtx* c, int32_t typeId) {
    uint32_t i;
    for (i = 0; i < c->namedTypeLen; i++) {
        if (c->namedTypes[i].typeId == typeId) {
            return (int32_t)i;
        }
    }
    return -1;
}

static int32_t SLTCParentOwnerTypeId(SLTypeCheckCtx* c, int32_t typeId) {
    int32_t idx = SLTCFindNamedTypeIndexByTypeId(c, typeId);
    if (idx < 0) {
        return -1;
    }
    return c->namedTypes[(uint32_t)idx].ownerTypeId;
}

int32_t SLTCFindNamedTypeIndexOwned(
    SLTypeCheckCtx* c, int32_t ownerTypeId, uint32_t start, uint32_t end) {
    uint32_t i;
    for (i = 0; i < c->namedTypeLen; i++) {
        if (c->namedTypes[i].ownerTypeId == ownerTypeId
            && SLNameEqSlice(
                c->src, c->namedTypes[i].nameStart, c->namedTypes[i].nameEnd, start, end))
        {
            return (int32_t)i;
        }
    }
    return -1;
}

static int32_t SLTCFindNamedTypeIndexInOwnerScope(
    SLTypeCheckCtx* c, int32_t ownerTypeId, uint32_t start, uint32_t end) {
    int32_t owner = ownerTypeId;
    while (owner >= 0) {
        int32_t idx = SLTCFindNamedTypeIndexOwned(c, owner, start, end);
        if (idx >= 0) {
            return idx;
        }
        owner = SLTCParentOwnerTypeId(c, owner);
    }
    return SLTCFindNamedTypeIndexOwned(c, -1, start, end);
}

int32_t SLTCResolveTypeNamePath(
    SLTypeCheckCtx* c, uint32_t start, uint32_t end, int32_t ownerTypeId) {
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
                idx = SLTCFindNamedTypeIndexInOwnerScope(c, ownerTypeId, segStart, segEnd);
            } else {
                idx = SLTCFindNamedTypeIndexOwned(c, currentTypeId, segStart, segEnd);
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

int32_t SLTCResolveTypeValueName(SLTypeCheckCtx* c, uint32_t start, uint32_t end) {
    int32_t builtinType = SLTCFindBuiltinType(c, start, end);
    int32_t typeId;
    if (c != NULL && c->activeGenericDeclNode >= 0) {
        int32_t idx = SLTCDeclTypeParamIndex(c, c->activeGenericDeclNode, start, end);
        if (idx >= 0 && (uint32_t)idx < c->activeGenericArgCount) {
            return c->genericArgTypes[c->activeGenericArgStart + (uint32_t)idx];
        }
    }
    if (builtinType >= 0) {
        return builtinType;
    }
    typeId = SLTCResolveTypeNamePath(c, start, end, c->currentTypeOwnerTypeId);
    if (typeId >= 0) {
        int32_t namedIndex = SLTCFindNamedTypeIndexByTypeId(c, typeId);
        if (namedIndex >= 0 && c->namedTypes[(uint32_t)namedIndex].templateArgCount > 0
            && c->namedTypes[(uint32_t)namedIndex].templateRootNamedIndex < 0)
        {
            return -1;
        }
        return typeId;
    }
    return -1;
}

int32_t SLTCFindNamedTypeIndex(SLTypeCheckCtx* c, uint32_t start, uint32_t end) {
    return SLTCFindNamedTypeIndexOwned(c, -1, start, end);
}

int32_t SLTCFindNamedTypeByLiteral(SLTypeCheckCtx* c, const char* name) {
    uint32_t i;
    for (i = 0; i < c->typeLen; i++) {
        const SLTCType* t = &c->types[i];
        if (t->kind != SLTCType_NAMED) {
            continue;
        }
        if (SLNameEqLiteral(c->src, t->nameStart, t->nameEnd, name)) {
            return (int32_t)i;
        }
    }
    return -1;
}

int32_t SLTCFindBuiltinNamedTypeBySuffix(SLTypeCheckCtx* c, const char* suffix) {
    uint32_t i;
    for (i = 0; i < c->typeLen; i++) {
        const SLTCType* t = &c->types[i];
        if (t->kind != SLTCType_NAMED) {
            continue;
        }
        if (SLNameHasPrefix(c->src, t->nameStart, t->nameEnd, "builtin")
            && SLNameHasSuffix(c->src, t->nameStart, t->nameEnd, suffix))
        {
            return (int32_t)i;
        }
    }
    return -1;
}

int32_t SLTCFindNamedTypeBySuffix(SLTypeCheckCtx* c, const char* suffix) {
    uint32_t i;
    for (i = 0; i < c->typeLen; i++) {
        const SLTCType* t = &c->types[i];
        if (t->kind != SLTCType_NAMED) {
            continue;
        }
        if (SLNameHasSuffix(c->src, t->nameStart, t->nameEnd, suffix)) {
            return (int32_t)i;
        }
    }
    return -1;
}

int32_t SLTCFindReflectKindType(SLTypeCheckCtx* c) {
    uint32_t i;
    int32_t  direct = SLTCFindNamedTypeByLiteral(c, "reflect__Kind");
    if (direct >= 0) {
        return direct;
    }
    for (i = 0; i < c->typeLen; i++) {
        const SLTCType* t = &c->types[i];
        if (t->kind != SLTCType_NAMED) {
            continue;
        }
        if (!SLNameHasPrefix(c->src, t->nameStart, t->nameEnd, "reflect")
            || !SLNameHasSuffix(c->src, t->nameStart, t->nameEnd, "__Kind"))
        {
            continue;
        }
        if (t->declNode >= 0 && (uint32_t)t->declNode < c->ast->len
            && c->ast->nodes[t->declNode].kind == SLAst_ENUM)
        {
            return (int32_t)i;
        }
    }
    return -1;
}

int SLTCNameEqLiteralOrPkgBuiltin(
    SLTypeCheckCtx* c, uint32_t start, uint32_t end, const char* name, const char* pkgPrefix) {
    uint32_t i = 0;
    uint32_t j = 0;
    uint32_t prefixLen = 0;
    uint32_t nameLen = 0;
    if (SLNameEqLiteral(c->src, start, end, name)) {
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

SLTCCompilerDiagOp SLTCCompilerDiagOpFromName(SLTypeCheckCtx* c, uint32_t start, uint32_t end) {
    if (SLTCNameEqLiteralOrPkgBuiltin(c, start, end, "error", "compiler")) {
        return SLTCCompilerDiagOp_ERROR;
    }
    if (SLTCNameEqLiteralOrPkgBuiltin(c, start, end, "error_at", "compiler")) {
        return SLTCCompilerDiagOp_ERROR_AT;
    }
    if (SLTCNameEqLiteralOrPkgBuiltin(c, start, end, "warn", "compiler")) {
        return SLTCCompilerDiagOp_WARN;
    }
    if (SLTCNameEqLiteralOrPkgBuiltin(c, start, end, "warn_at", "compiler")) {
        return SLTCCompilerDiagOp_WARN_AT;
    }
    return SLTCCompilerDiagOp_NONE;
}

int SLTCIsSpanOfName(SLTypeCheckCtx* c, uint32_t start, uint32_t end) {
    return SLTCNameEqLiteralOrPkgBuiltin(c, start, end, "span_of", "reflect");
}

int32_t SLTCFindReflectSpanType(SLTypeCheckCtx* c) {
    uint32_t i;
    int32_t  direct = SLTCFindNamedTypeByLiteral(c, "reflect__Span");
    if (direct >= 0) {
        return direct;
    }
    for (i = 0; i < c->typeLen; i++) {
        const SLTCType* t = &c->types[i];
        if (t->kind != SLTCType_NAMED) {
            continue;
        }
        if (!SLNameHasPrefix(c->src, t->nameStart, t->nameEnd, "reflect")
            || !SLNameHasSuffix(c->src, t->nameStart, t->nameEnd, "__Span"))
        {
            continue;
        }
        if (t->declNode >= 0 && (uint32_t)t->declNode < c->ast->len
            && c->ast->nodes[t->declNode].kind == SLAst_STRUCT)
        {
            return (int32_t)i;
        }
    }
    return -1;
}

int32_t SLTCFindFmtValueType(SLTypeCheckCtx* c) {
    uint32_t i;
    int32_t  direct = SLTCFindNamedTypeByLiteral(c, "builtin__FmtValue");
    if (direct >= 0) {
        return direct;
    }
    for (i = 0; i < c->typeLen; i++) {
        const SLTCType* t = &c->types[i];
        if (t->kind != SLTCType_NAMED) {
            continue;
        }
        if (!SLNameHasPrefix(c->src, t->nameStart, t->nameEnd, "builtin")
            || !SLNameHasSuffix(c->src, t->nameStart, t->nameEnd, "__FmtValue"))
        {
            continue;
        }
        if (t->declNode >= 0 && (uint32_t)t->declNode < c->ast->len
            && c->ast->nodes[t->declNode].kind == SLAst_STRUCT)
        {
            return (int32_t)i;
        }
    }
    return -1;
}

int SLTCTypeIsReflectSpan(SLTypeCheckCtx* c, int32_t typeId) {
    int32_t spanType;
    if (c == NULL || typeId < 0) {
        return 0;
    }
    if (c->typeReflectSpan < 0) {
        c->typeReflectSpan = SLTCFindReflectSpanType(c);
    }
    spanType = c->typeReflectSpan;
    if (spanType < 0) {
        return 0;
    }
    typeId = SLTCResolveAliasBaseType(c, typeId);
    return typeId == spanType;
}

int SLTCTypeIsFmtValue(SLTypeCheckCtx* c, int32_t typeId) {
    const SLTCType* t;
    if (c == NULL || typeId < 0) {
        return 0;
    }
    typeId = SLTCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return 0;
    }
    t = &c->types[typeId];
    if (t->kind != SLTCType_NAMED) {
        return 0;
    }
    if (!SLNameHasSuffix(c->src, t->nameStart, t->nameEnd, "__FmtValue")) {
        return 0;
    }
    return t->declNode >= 0 && (uint32_t)t->declNode < c->ast->len
        && c->ast->nodes[t->declNode].kind == SLAst_STRUCT;
}

int32_t SLTCFindFunctionIndex(SLTypeCheckCtx* c, uint32_t start, uint32_t end) {
    uint32_t i;
    for (i = 0; i < c->funcLen; i++) {
        if (SLNameEqSlice(c->src, c->funcs[i].nameStart, c->funcs[i].nameEnd, start, end)) {
            return (int32_t)i;
        }
    }
    return -1;
}

static int SLTCFunctionSupportsFunctionValue(const SLTypeCheckCtx* c, const SLTCFunction* fn) {
    if (c == NULL || fn == NULL) {
        return 0;
    }
    if (fn->contextType >= 0 || (fn->flags & SLTCFunctionFlag_VARIADIC) != 0) {
        return 0;
    }
    if (fn->defNode >= 0) {
        return 1;
    }
    return fn->declNode >= 0 && SLTCHasForeignImportDirective(c->ast, c->src, fn->declNode);
}

int32_t SLTCFindPlainFunctionValueIndex(SLTypeCheckCtx* c, uint32_t start, uint32_t end) {
    uint32_t i;
    int32_t  found = -1;
    for (i = 0; i < c->funcLen; i++) {
        const SLTCFunction* fn = &c->funcs[i];
        if (!SLNameEqSlice(c->src, fn->nameStart, fn->nameEnd, start, end)) {
            continue;
        }
        if (!SLTCFunctionSupportsFunctionValue(c, fn)) {
            continue;
        }
        if (found >= 0) {
            return -1;
        }
        found = (int32_t)i;
    }
    return found;
}

int32_t SLTCFindPkgQualifiedFunctionValueIndex(
    SLTypeCheckCtx* c, uint32_t pkgStart, uint32_t pkgEnd, uint32_t nameStart, uint32_t nameEnd) {
    int32_t  candidates[SLTC_MAX_CALL_CANDIDATES];
    uint32_t candidateCount = 0;
    int      nameFound = 0;
    uint32_t i;
    int32_t  found = -1;
    SLTCGatherCallCandidatesByPkgMethod(
        c, pkgStart, pkgEnd, nameStart, nameEnd, candidates, &candidateCount, &nameFound);
    if (!nameFound) {
        return -1;
    }
    for (i = 0; i < candidateCount; i++) {
        const SLTCFunction* fn;
        int32_t             fnIndex = candidates[i];
        if (fnIndex < 0 || (uint32_t)fnIndex >= c->funcLen) {
            continue;
        }
        fn = &c->funcs[(uint32_t)fnIndex];
        if (!SLTCFunctionSupportsFunctionValue(c, fn)) {
            continue;
        }
        if (found >= 0) {
            return -1;
        }
        found = fnIndex;
    }
    return found;
}

int SLTCFunctionNameEq(const SLTypeCheckCtx* c, uint32_t funcIndex, uint32_t start, uint32_t end) {
    return SLNameEqSlice(
        c->src, c->funcs[funcIndex].nameStart, c->funcs[funcIndex].nameEnd, start, end);
}

int SLTCNameEqPkgPrefixedMethod(
    SLTypeCheckCtx* c,
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
    if (!SLNameEqSlice(c->src, candidateStart, candidateStart + pkgLen, pkgStart, pkgEnd)) {
        return 0;
    }
    if (c->src.ptr[candidateStart + pkgLen] != '_'
        || c->src.ptr[candidateStart + pkgLen + 1u] != '_')
    {
        return 0;
    }
    return SLNameEqSlice(
        c->src, candidateStart + pkgLen + 2u, candidateEnd, methodStart, methodEnd);
}

int SLTCExtractPkgPrefixFromTypeName(
    SLTypeCheckCtx* c,
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

int SLTCImportDefaultAliasEq(
    SLStrView src, uint32_t pathStart, uint32_t pathEnd, uint32_t aliasStart, uint32_t aliasEnd) {
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
    return SLNameEqSlice(src, start, end, aliasStart, aliasEnd);
}

int SLTCHasImportAlias(SLTypeCheckCtx* c, uint32_t aliasStart, uint32_t aliasEnd) {
    uint32_t nodeId;
    if (c == NULL || aliasEnd <= aliasStart || aliasEnd > c->src.len) {
        return 0;
    }
    for (nodeId = 0; nodeId < c->ast->len; nodeId++) {
        const SLAstNode* importNode;
        int32_t          child;
        if (c->ast->nodes[nodeId].kind != SLAst_IMPORT) {
            continue;
        }
        importNode = &c->ast->nodes[nodeId];
        child = importNode->firstChild;
        while (child >= 0) {
            const SLAstNode* ch = &c->ast->nodes[child];
            if (ch->kind == SLAst_IDENT) {
                if (SLNameEqSlice(c->src, ch->dataStart, ch->dataEnd, aliasStart, aliasEnd)) {
                    return 1;
                }
                break;
            }
            child = ch->nextSibling;
        }
        if (SLTCImportDefaultAliasEq(
                c->src, importNode->dataStart, importNode->dataEnd, aliasStart, aliasEnd))
        {
            return 1;
        }
    }
    return 0;
}

int SLTCResolveReceiverPkgPrefix(
    SLTypeCheckCtx* c, int32_t typeId, uint32_t* outPkgStart, uint32_t* outPkgEnd) {
    uint32_t depth = 0;
    while (typeId >= 0 && (uint32_t)typeId < c->typeLen && depth++ <= c->typeLen) {
        const SLTCType* t = &c->types[typeId];
        switch (t->kind) {
            case SLTCType_PTR:
            case SLTCType_REF:
            case SLTCType_OPTIONAL: typeId = t->baseType; continue;
            case SLTCType_ALIAS:
                if (SLTCResolveAliasTypeId(c, typeId) != 0) {
                    return -1;
                }
                if (SLTCExtractPkgPrefixFromTypeName(
                        c, t->nameStart, t->nameEnd, outPkgStart, outPkgEnd))
                {
                    return 1;
                }
                typeId = t->baseType;
                continue;
            case SLTCType_NAMED:
                return SLTCExtractPkgPrefixFromTypeName(
                    c, t->nameStart, t->nameEnd, outPkgStart, outPkgEnd);
            default: return 0;
        }
    }
    return 0;
}

static int SLTCResolveTypePathExprTypeForEnumMember(
    SLTypeCheckCtx* c, int32_t exprNode, int32_t ownerTypeId, int32_t* outTypeId) {
    const SLAstNode* n;
    if (exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return 0;
    }
    n = &c->ast->nodes[exprNode];
    if (n->kind == SLAst_IDENT) {
        int32_t typeId;
        if (SLTCLocalFind(c, n->dataStart, n->dataEnd) >= 0
            || SLTCFindFunctionIndex(c, n->dataStart, n->dataEnd) >= 0)
        {
            return 0;
        }
        typeId = SLTCResolveTypeNamePath(c, n->dataStart, n->dataEnd, ownerTypeId);
        if (typeId < 0) {
            return 0;
        }
        *outTypeId = typeId;
        return 1;
    }
    if (n->kind == SLAst_FIELD_EXPR) {
        int32_t recvExpr = SLAstFirstChild(c->ast, exprNode);
        int32_t recvTypeId;
        int32_t idx;
        if (recvExpr < 0
            || !SLTCResolveTypePathExprTypeForEnumMember(c, recvExpr, ownerTypeId, &recvTypeId))
        {
            return 0;
        }
        idx = SLTCFindNamedTypeIndexOwned(c, recvTypeId, n->dataStart, n->dataEnd);
        if (idx < 0) {
            return 0;
        }
        *outTypeId = c->namedTypes[(uint32_t)idx].typeId;
        return 1;
    }
    return 0;
}

int SLTCResolveEnumMemberType(
    SLTypeCheckCtx* c,
    int32_t         recvNode,
    uint32_t        memberStart,
    uint32_t        memberEnd,
    int32_t*        outType) {
    int32_t enumTypeId;
    int32_t enumFieldType;

    if (recvNode < 0 || (uint32_t)recvNode >= c->ast->len) {
        return 0;
    }
    if (!SLTCResolveTypePathExprTypeForEnumMember(
            c, recvNode, c->currentTypeOwnerTypeId, &enumTypeId))
    {
        return 0;
    }
    enumTypeId = SLTCResolveAliasBaseType(c, enumTypeId);
    if (!SLTCIsNamedDeclKind(c, enumTypeId, SLAst_ENUM)) {
        return 0;
    }

    if (SLTCFieldLookup(c, enumTypeId, memberStart, memberEnd, &enumFieldType, NULL) != 0) {
        return 0;
    }
    *outType = enumFieldType;
    return 1;
}

int32_t SLTCInternPtrType(SLTypeCheckCtx* c, int32_t baseType, uint32_t errStart, uint32_t errEnd) {
    uint32_t i;
    SLTCType t;
    for (i = 0; i < c->typeLen; i++) {
        if (c->types[i].kind == SLTCType_PTR && c->types[i].baseType == baseType) {
            return (int32_t)i;
        }
    }
    t.kind = SLTCType_PTR;
    t.builtin = SLBuiltin_INVALID;
    t.baseType = baseType;
    t.declNode = -1;
    t.funcIndex = -1;
    t.arrayLen = 0;
    t.nameStart = 0;
    t.nameEnd = 0;
    t.fieldStart = 0;
    t.fieldCount = 0;
    t.flags = 0;
    return SLTCAddType(c, &t, errStart, errEnd);
}

int SLTCTypeIsMutable(const SLTCType* t) {
    if (t->kind == SLTCType_PTR) {
        return 1;
    }
    return (t->flags & SLTCTypeFlag_MUTABLE) != 0;
}

int SLTCIsMutableRefType(SLTypeCheckCtx* c, int32_t typeId) {
    return typeId >= 0 && (uint32_t)typeId < c->typeLen
        && (c->types[typeId].kind == SLTCType_PTR
            || (c->types[typeId].kind == SLTCType_REF && SLTCTypeIsMutable(&c->types[typeId])));
}

int32_t SLTCInternRefType(
    SLTypeCheckCtx* c, int32_t baseType, int isMutable, uint32_t errStart, uint32_t errEnd) {
    uint32_t i;
    SLTCType t;
    for (i = 0; i < c->typeLen; i++) {
        if (c->types[i].kind == SLTCType_REF && c->types[i].baseType == baseType
            && SLTCTypeIsMutable(&c->types[i]) == isMutable)
        {
            return (int32_t)i;
        }
    }
    t.kind = SLTCType_REF;
    t.builtin = SLBuiltin_INVALID;
    t.baseType = baseType;
    t.declNode = -1;
    t.funcIndex = -1;
    t.arrayLen = 0;
    t.nameStart = 0;
    t.nameEnd = 0;
    t.fieldStart = 0;
    t.fieldCount = 0;
    t.flags = isMutable ? SLTCTypeFlag_MUTABLE : 0;
    return SLTCAddType(c, &t, errStart, errEnd);
}

int32_t SLTCInternArrayType(
    SLTypeCheckCtx* c, int32_t baseType, uint32_t arrayLen, uint32_t errStart, uint32_t errEnd) {
    uint32_t i;
    SLTCType t;
    for (i = 0; i < c->typeLen; i++) {
        if (c->types[i].kind == SLTCType_ARRAY && c->types[i].baseType == baseType
            && c->types[i].arrayLen == arrayLen)
        {
            return (int32_t)i;
        }
    }
    t.kind = SLTCType_ARRAY;
    t.builtin = SLBuiltin_INVALID;
    t.baseType = baseType;
    t.declNode = -1;
    t.funcIndex = -1;
    t.arrayLen = arrayLen;
    t.nameStart = 0;
    t.nameEnd = 0;
    t.fieldStart = 0;
    t.fieldCount = 0;
    t.flags = 0;
    return SLTCAddType(c, &t, errStart, errEnd);
}

int32_t SLTCInternSliceType(
    SLTypeCheckCtx* c, int32_t baseType, int isMutable, uint32_t errStart, uint32_t errEnd) {
    uint32_t i;
    SLTCType t;
    for (i = 0; i < c->typeLen; i++) {
        if (c->types[i].kind == SLTCType_SLICE && c->types[i].baseType == baseType
            && SLTCTypeIsMutable(&c->types[i]) == isMutable)
        {
            return (int32_t)i;
        }
    }
    t.kind = SLTCType_SLICE;
    t.builtin = SLBuiltin_INVALID;
    t.baseType = baseType;
    t.declNode = -1;
    t.funcIndex = -1;
    t.arrayLen = 0;
    t.nameStart = 0;
    t.nameEnd = 0;
    t.fieldStart = 0;
    t.fieldCount = 0;
    t.flags = isMutable ? SLTCTypeFlag_MUTABLE : 0;
    return SLTCAddType(c, &t, errStart, errEnd);
}

int32_t SLTCInternOptionalType(
    SLTypeCheckCtx* c, int32_t baseType, uint32_t errStart, uint32_t errEnd) {
    uint32_t i;
    SLTCType t;
    for (i = 0; i < c->typeLen; i++) {
        if (c->types[i].kind == SLTCType_OPTIONAL && c->types[i].baseType == baseType) {
            return (int32_t)i;
        }
    }
    t.kind = SLTCType_OPTIONAL;
    t.builtin = SLBuiltin_INVALID;
    t.baseType = baseType;
    t.declNode = -1;
    t.funcIndex = -1;
    t.arrayLen = 0;
    t.nameStart = 0;
    t.nameEnd = 0;
    t.fieldStart = 0;
    t.fieldCount = 0;
    t.flags = 0;
    return SLTCAddType(c, &t, errStart, errEnd);
}

int32_t SLTCInternAnonAggregateType(
    SLTypeCheckCtx*         c,
    int                     isUnion,
    const SLTCAnonFieldSig* fields,
    uint32_t                fieldCount,
    int32_t                 declNode,
    uint32_t                errStart,
    uint32_t                errEnd) {
    uint32_t i;
    SLTCType t;

    if (fieldCount > UINT16_MAX) {
        return SLTCFailSpan(c, SLDiag_ARENA_OOM, errStart, errEnd);
    }

    for (i = 0; i < c->typeLen; i++) {
        const SLTCType* ct = &c->types[i];
        uint32_t        j;
        int             same = 1;
        if ((isUnion ? SLTCType_ANON_UNION : SLTCType_ANON_STRUCT) != ct->kind
            || ct->fieldCount != fieldCount)
        {
            continue;
        }
        for (j = 0; j < fieldCount; j++) {
            const SLTCField* f = &c->fields[ct->fieldStart + j];
            if (f->typeId != fields[j].typeId
                || !SLNameEqSlice(
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
        return SLTCFailSpan(c, SLDiag_ARENA_OOM, errStart, errEnd);
    }

    t.kind = isUnion ? SLTCType_ANON_UNION : SLTCType_ANON_STRUCT;
    t.builtin = SLBuiltin_INVALID;
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
        int32_t  typeId = SLTCAddType(c, &t, errStart, errEnd);
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

int SLTCFunctionTypeMatchesSignature(
    SLTypeCheckCtx* c,
    const SLTCType* t,
    int32_t         returnType,
    const int32_t*  paramTypes,
    const uint8_t*  paramFlags,
    uint32_t        paramCount,
    int             isVariadic) {
    uint32_t i;
    if (t->kind != SLTCType_FUNCTION || t->baseType != returnType || t->fieldCount != paramCount) {
        return 0;
    }
    if (((t->flags & SLTCTypeFlag_FUNCTION_VARIADIC) != 0) != (isVariadic != 0)) {
        return 0;
    }
    for (i = 0; i < paramCount; i++) {
        if (c->funcParamTypes[t->fieldStart + i] != paramTypes[i]) {
            return 0;
        }
        if ((c->funcParamFlags[t->fieldStart + i] & SLTCFuncParamFlag_CONST)
            != ((paramFlags != NULL ? paramFlags[i] : 0u) & SLTCFuncParamFlag_CONST))
        {
            return 0;
        }
    }
    return 1;
}

int32_t SLTCInternFunctionType(
    SLTypeCheckCtx* c,
    int32_t         returnType,
    const int32_t*  paramTypes,
    const uint8_t*  paramFlags,
    uint32_t        paramCount,
    int             isVariadic,
    int32_t         funcIndex,
    uint32_t        errStart,
    uint32_t        errEnd) {
    uint32_t i;
    SLTCType t;
    if (paramCount > UINT16_MAX) {
        return SLTCFailSpan(c, SLDiag_ARENA_OOM, errStart, errEnd);
    }
    for (i = 0; i < c->typeLen; i++) {
        if (SLTCFunctionTypeMatchesSignature(
                c, &c->types[i], returnType, paramTypes, paramFlags, paramCount, isVariadic))
        {
            if (funcIndex >= 0 && c->types[i].funcIndex < 0) {
                c->types[i].funcIndex = funcIndex;
            }
            return (int32_t)i;
        }
    }
    if (c->funcParamLen + paramCount > c->funcParamCap) {
        return SLTCFailSpan(c, SLDiag_ARENA_OOM, errStart, errEnd);
    }
    t.kind = SLTCType_FUNCTION;
    t.builtin = SLBuiltin_INVALID;
    t.baseType = returnType;
    t.declNode = -1;
    t.funcIndex = funcIndex;
    t.arrayLen = 0;
    t.nameStart = 0;
    t.nameEnd = 0;
    t.fieldStart = c->funcParamLen;
    t.fieldCount = (uint16_t)paramCount;
    t.flags = (uint16_t)(isVariadic ? SLTCTypeFlag_FUNCTION_VARIADIC : 0u);
    for (i = 0; i < paramCount; i++) {
        c->funcParamTypes[c->funcParamLen++] = paramTypes[i];
        c->funcParamNameStarts[c->funcParamLen - 1u] = 0;
        c->funcParamNameEnds[c->funcParamLen - 1u] = 0;
        c->funcParamFlags[c->funcParamLen - 1u] =
            paramFlags != NULL ? (paramFlags[i] & SLTCFuncParamFlag_CONST) : 0u;
    }
    return SLTCAddType(c, &t, errStart, errEnd);
}

int SLTCTupleTypeMatchesSignature(
    SLTypeCheckCtx* c, const SLTCType* t, const int32_t* elemTypes, uint32_t elemCount) {
    uint32_t i;
    if (t->kind != SLTCType_TUPLE || t->fieldCount != elemCount) {
        return 0;
    }
    for (i = 0; i < elemCount; i++) {
        if (c->funcParamTypes[t->fieldStart + i] != elemTypes[i]) {
            return 0;
        }
    }
    return 1;
}

int32_t SLTCInternTupleType(
    SLTypeCheckCtx* c,
    const int32_t*  elemTypes,
    uint32_t        elemCount,
    uint32_t        errStart,
    uint32_t        errEnd) {
    uint32_t i;
    SLTCType t;
    if (elemCount < 2u || elemCount > UINT16_MAX) {
        return SLTCFailSpan(c, SLDiag_EXPECTED_TYPE, errStart, errEnd);
    }
    for (i = 0; i < c->typeLen; i++) {
        if (SLTCTupleTypeMatchesSignature(c, &c->types[i], elemTypes, elemCount)) {
            return (int32_t)i;
        }
    }
    if (c->funcParamLen + elemCount > c->funcParamCap) {
        return SLTCFailSpan(c, SLDiag_ARENA_OOM, errStart, errEnd);
    }
    t.kind = SLTCType_TUPLE;
    t.builtin = SLBuiltin_INVALID;
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
    return SLTCAddType(c, &t, errStart, errEnd);
}

int32_t SLTCInternPackType(
    SLTypeCheckCtx* c,
    const int32_t*  elemTypes,
    uint32_t        elemCount,
    uint32_t        errStart,
    uint32_t        errEnd) {
    uint32_t i;
    uint32_t j;
    SLTCType t;
    for (i = 0; i < c->typeLen; i++) {
        if (c->types[i].kind == SLTCType_PACK && c->types[i].fieldCount == elemCount) {
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
        return SLTCFailSpan(c, SLDiag_ARENA_OOM, errStart, errEnd);
    }
    if (c->funcParamLen + elemCount > c->funcParamCap) {
        return SLTCFailSpan(c, SLDiag_ARENA_OOM, errStart, errEnd);
    }
    t.kind = SLTCType_PACK;
    t.builtin = SLBuiltin_INVALID;
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
    return SLTCAddType(c, &t, errStart, errEnd);
}

int SLTCParseArrayLen(SLTypeCheckCtx* c, const SLAstNode* node, uint32_t* outLen) {
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

int SLTCResolveIndexBaseInfo(SLTypeCheckCtx* c, int32_t baseType, SLTCIndexBaseInfo* out) {
    const SLTCType* t;
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
    resolvedBaseType = SLTCResolveAliasBaseType(c, baseType);
    if (resolvedBaseType == c->typeStr) {
        int32_t u8Type = SLTCFindBuiltinByKind(c, SLBuiltin_U8);
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
        case SLTCType_ARRAY:
            out->elemType = t->baseType;
            out->indexable = 1;
            out->sliceable = 1;
            out->sliceMutable = 1;
            out->hasKnownLen = 1;
            out->knownLen = t->arrayLen;
            return 0;
        case SLTCType_SLICE:
            out->elemType = t->baseType;
            out->indexable = 1;
            out->sliceable = 1;
            out->sliceMutable = SLTCTypeIsMutable(t);
            return 0;
        case SLTCType_PTR: {
            int32_t pointee = t->baseType;
            int32_t resolvedPointee;
            if (pointee < 0 || (uint32_t)pointee >= c->typeLen) {
                return -1;
            }
            resolvedPointee = SLTCResolveAliasBaseType(c, pointee);
            if (resolvedPointee == c->typeStr) {
                int32_t u8Type = SLTCFindBuiltinByKind(c, SLBuiltin_U8);
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
            if (c->types[pointee].kind == SLTCType_ARRAY) {
                out->elemType = c->types[pointee].baseType;
                out->indexable = 1;
                out->sliceable = 1;
                out->sliceMutable = 1;
                out->hasKnownLen = 1;
                out->knownLen = c->types[pointee].arrayLen;
                return 0;
            }
            if (c->types[pointee].kind == SLTCType_SLICE) {
                out->elemType = c->types[pointee].baseType;
                out->indexable = 1;
                out->sliceable = 1;
                out->sliceMutable = SLTCTypeIsMutable(&c->types[pointee]);
                return 0;
            }
            out->elemType = pointee;
            out->indexable = 1;
            out->sliceMutable = 1;
            return 0;
        }
        case SLTCType_REF: {
            int32_t refBase = t->baseType;
            int32_t resolvedRefBase;
            if (refBase < 0 || (uint32_t)refBase >= c->typeLen) {
                return -1;
            }
            resolvedRefBase = SLTCResolveAliasBaseType(c, refBase);
            if (resolvedRefBase == c->typeStr) {
                int32_t u8Type = SLTCFindBuiltinByKind(c, SLBuiltin_U8);
                if (u8Type < 0) {
                    return -1;
                }
                out->elemType = u8Type;
                out->indexable = 1;
                out->sliceable = 1;
                out->sliceMutable = SLTCTypeIsMutable(t);
                out->isStringLike = 1;
                return 0;
            }
            if (c->types[refBase].kind == SLTCType_ARRAY) {
                out->elemType = c->types[refBase].baseType;
                out->indexable = 1;
                out->sliceable = 1;
                out->sliceMutable = SLTCTypeIsMutable(t);
                out->hasKnownLen = 1;
                out->knownLen = c->types[refBase].arrayLen;
                return 0;
            }
            if (c->types[refBase].kind == SLTCType_SLICE) {
                out->elemType = c->types[refBase].baseType;
                out->indexable = 1;
                out->sliceable = 1;
                out->sliceMutable = SLTCTypeIsMutable(t) && SLTCTypeIsMutable(&c->types[refBase]);
                return 0;
            }
            out->elemType = refBase;
            out->indexable = 1;
            out->sliceMutable = SLTCTypeIsMutable(t);
            return 0;
        }
        default: return 0;
    }
}

int32_t SLTCListItemAt(const SLAst* ast, int32_t listNode, uint32_t index) {
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

uint32_t SLTCListCount(const SLAst* ast, int32_t listNode) {
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

int SLTCHasForeignImportDirective(const SLAst* ast, SLStrView src, int32_t nodeId) {
    int32_t child;
    int32_t first = -1;
    if (ast == NULL || nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return 0;
    }
    child = SLAstFirstChild(ast, ast->root);
    while (child >= 0) {
        const SLAstNode* n = &ast->nodes[child];
        if (n->kind == SLAst_DIRECTIVE) {
            if (first < 0) {
                first = child;
            }
        } else {
            if (child == nodeId && first >= 0) {
                int32_t dir = first;
                while (dir >= 0 && ast->nodes[dir].kind == SLAst_DIRECTIVE) {
                    const SLAstNode* dn = &ast->nodes[dir];
                    uint32_t         len = dn->dataEnd - dn->dataStart;
                    if ((len == 8u && memcmp(src.ptr + dn->dataStart, "c_import", 8u) == 0)
                        || (len == 11u && memcmp(src.ptr + dn->dataStart, "wasm_import", 11u) == 0))
                    {
                        return 1;
                    }
                    dir = SLAstNextSibling(ast, dir);
                }
                return 0;
            }
            first = -1;
        }
        child = SLAstNextSibling(ast, child);
    }
    return 0;
}

int SLTCVarLikeGetParts(SLTypeCheckCtx* c, int32_t nodeId, SLTCVarLikeParts* out) {
    int32_t          firstChild;
    const SLAstNode* firstNode;
    out->nameListNode = -1;
    out->typeNode = -1;
    out->initNode = -1;
    out->nameCount = 0;
    out->grouped = 0;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return -1;
    }
    firstChild = SLAstFirstChild(c->ast, nodeId);
    if (firstChild < 0) {
        return 0;
    }
    firstNode = &c->ast->nodes[firstChild];
    if (firstNode->kind == SLAst_NAME_LIST) {
        int32_t afterNames = SLAstNextSibling(c->ast, firstChild);
        out->grouped = 1;
        out->nameListNode = firstChild;
        out->nameCount = SLTCListCount(c->ast, firstChild);
        if (afterNames >= 0 && SLTCIsTypeNodeKind(c->ast->nodes[afterNames].kind)) {
            out->typeNode = afterNames;
            out->initNode = SLAstNextSibling(c->ast, afterNames);
        } else {
            out->initNode = afterNames;
        }
        return 0;
    }
    out->grouped = 0;
    out->nameCount = 1;
    if (SLTCIsTypeNodeKind(firstNode->kind)) {
        out->typeNode = firstChild;
        out->initNode = SLAstNextSibling(c->ast, firstChild);
    } else {
        out->initNode = firstChild;
    }
    return 0;
}

int32_t SLTCVarLikeNameIndexBySlice(
    SLTypeCheckCtx* c, int32_t nodeId, uint32_t start, uint32_t end) {
    SLTCVarLikeParts parts;
    const SLAstNode* n;
    uint32_t         i;
    if (SLTCVarLikeGetParts(c, nodeId, &parts) != 0 || parts.nameCount == 0) {
        return -1;
    }
    n = &c->ast->nodes[nodeId];
    if (!parts.grouped) {
        return SLNameEqSlice(c->src, n->dataStart, n->dataEnd, start, end) ? 0 : -1;
    }
    for (i = 0; i < parts.nameCount; i++) {
        int32_t item = SLTCListItemAt(c->ast, parts.nameListNode, i);
        if (item >= 0
            && SLNameEqSlice(
                c->src, c->ast->nodes[item].dataStart, c->ast->nodes[item].dataEnd, start, end))
        {
            return (int32_t)i;
        }
    }
    return -1;
}

int32_t SLTCVarLikeInitExprNodeAt(SLTypeCheckCtx* c, int32_t nodeId, int32_t nameIndex) {
    SLTCVarLikeParts parts;
    uint32_t         initCount;
    int32_t          onlyInit;
    if (SLTCVarLikeGetParts(c, nodeId, &parts) != 0) {
        return -1;
    }
    if (!parts.grouped) {
        return (nameIndex == 0) ? parts.initNode : -1;
    }
    if (parts.initNode < 0 || c->ast->nodes[parts.initNode].kind != SLAst_EXPR_LIST
        || nameIndex < 0)
    {
        return -1;
    }
    initCount = SLTCListCount(c->ast, parts.initNode);
    if (initCount == parts.nameCount) {
        return SLTCListItemAt(c->ast, parts.initNode, (uint32_t)nameIndex);
    }
    if (initCount != 1u) {
        return -1;
    }
    onlyInit = SLTCListItemAt(c->ast, parts.initNode, 0);
    if (onlyInit < 0 || (uint32_t)onlyInit >= c->ast->len
        || c->ast->nodes[onlyInit].kind != SLAst_TUPLE_EXPR)
    {
        return -1;
    }
    return SLTCListItemAt(c->ast, onlyInit, (uint32_t)nameIndex);
}

int32_t SLTCVarLikeInitExprNode(SLTypeCheckCtx* c, int32_t nodeId) {
    return SLTCVarLikeInitExprNodeAt(c, nodeId, 0);
}

SL_API_END
