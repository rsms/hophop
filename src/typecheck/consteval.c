#include "internal.h"
#include "../mir_exec.h"
#include "../mir_lower_pkg.h"
#include "../mir_lower_stmt.h"

H2_API_BEGIN

static const uint64_t H2_TC_MIR_TUPLE_TAG = 0x54434d4952545550ULL;
static const uint64_t H2_TC_MIR_ITER_TAG = 0x54434d4952495445ULL;
static const uint64_t H2_TC_MIR_IMPORT_ALIAS_TAG = 0x54434d49524d504bULL;

static int H2TCConstEvalResolveTrackedAnyPackArgIndex(
    H2TCConstEvalCtx* evalCtx, int32_t exprNode, uint32_t* outCallArgIndex);
static int H2TCConstEvalGetConcreteCallArgType(
    H2TCConstEvalCtx* evalCtx, uint32_t callArgIndex, int32_t* outType);
static int H2TCConstEvalGetConcreteCallArgPackType(
    H2TCConstEvalCtx* evalCtx, uint32_t callArgIndex, int32_t* outPackType);
static int H2TCConstEvalDirectCall(
    H2TCConstEvalCtx* evalCtx, int32_t exprNode, H2CTFEValue* outValue, int* outIsConst);
int H2TCResolveConstCallMir(
    void* _Nullable ctx,
    const H2MirProgram* _Nullable program,
    const H2MirFunction* _Nullable function,
    const H2MirInst* _Nullable inst,
    uint32_t nameStart,
    uint32_t nameEnd,
    const H2CTFEValue* _Nonnull args,
    uint32_t argCount,
    H2CTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);
int H2TCResolveConstCallMirPre(
    void* _Nullable ctx,
    const H2MirProgram* _Nullable program,
    const H2MirFunction* _Nullable function,
    const H2MirInst* _Nullable inst,
    uint32_t     nameStart,
    uint32_t     nameEnd,
    H2CTFEValue* outValue,
    int*         outIsConst,
    H2Diag* _Nullable diag);
static int H2TCConstEvalSetOptionalNoneValue(
    H2TypeCheckCtx* c, int32_t optionalTypeId, H2CTFEValue* outValue);
static int H2TCConstEvalSetOptionalSomeValue(
    H2TypeCheckCtx* c, int32_t optionalTypeId, const H2CTFEValue* payload, H2CTFEValue* outValue);
static int H2TCInvokeConstFunctionByIndex(
    H2TCConstEvalCtx* evalCtx,
    uint32_t          nameStart,
    uint32_t          nameEnd,
    int32_t           fnIndex,
    const H2CTFEValue* _Nullable args,
    uint32_t argCount,
    const H2TCCallArgInfo* _Nullable callArgs,
    uint32_t callArgCount,
    const H2TCCallBinding* _Nullable callBinding,
    uint32_t     callPackParamNameStart,
    uint32_t     callPackParamNameEnd,
    H2CTFEValue* outValue,
    int*         outIsConst);
void H2TCConstSetReason(
    H2TCConstEvalCtx* evalCtx, uint32_t start, uint32_t end, const char* reason);
static int H2TCConstLookupMirLocalValue(
    H2TCConstEvalCtx* evalCtx, uint32_t nameStart, uint32_t nameEnd, H2CTFEValue* outValue);
int H2TCMirConstBindFrame(
    void* _Nullable ctx,
    const H2MirProgram* _Nullable program,
    const H2MirFunction* _Nullable function,
    const H2CTFEValue* _Nullable locals,
    uint32_t localCount,
    H2Diag* _Nullable diag);
void H2TCMirConstUnbindFrame(void* _Nullable ctx);

static int H2TCConstEvalIsTrackedAnyPackName(
    const H2TCConstEvalCtx* evalCtx, uint32_t nameStart, uint32_t nameEnd) {
    H2TypeCheckCtx* c;
    int32_t         localIdx;
    if (evalCtx == NULL) {
        return 0;
    }
    c = evalCtx->tc;
    if (c == NULL) {
        return 0;
    }
    if (evalCtx->callPackParamNameStart < evalCtx->callPackParamNameEnd
        && H2NameEqSlice(
            c->src,
            nameStart,
            nameEnd,
            evalCtx->callPackParamNameStart,
            evalCtx->callPackParamNameEnd))
    {
        return 1;
    }
    if (evalCtx->mirProgram != NULL && evalCtx->mirFunction != NULL
        && (evalCtx->mirFunction->flags & H2MirFunctionFlag_VARIADIC) != 0u
        && evalCtx->mirFunction->paramCount > 0u
        && evalCtx->mirFunction->localStart <= evalCtx->mirProgram->localLen
        && evalCtx->mirFunction->paramCount
               <= evalCtx->mirProgram->localLen - evalCtx->mirFunction->localStart)
    {
        const H2MirLocal* packLocal =
            &evalCtx->mirProgram
                 ->locals[evalCtx->mirFunction->localStart + evalCtx->mirFunction->paramCount - 1u];
        if (H2NameEqSlice(c->src, nameStart, nameEnd, packLocal->nameStart, packLocal->nameEnd)) {
            return 1;
        }
    }
    localIdx = H2TCLocalFind(c, nameStart, nameEnd);
    return localIdx >= 0 && (c->locals[localIdx].flags & H2TCLocalFlag_ANYPACK) != 0;
}

static int H2TCConstEvalNodeHasPackType(H2TCConstEvalCtx* evalCtx, int32_t nodeId) {
    H2TypeCheckCtx* c;
    H2TCConstEvalCtx* _Nullable savedActiveEvalCtx;
    int32_t typeId = -1;
    int32_t baseTypeId;
    if (evalCtx == NULL) {
        return 0;
    }
    c = evalCtx->tc;
    if (c == NULL || nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return 0;
    }
    savedActiveEvalCtx = c->activeConstEvalCtx;
    c->activeConstEvalCtx = evalCtx;
    if (H2TCTypeExpr(c, nodeId, &typeId) != 0) {
        c->activeConstEvalCtx = savedActiveEvalCtx;
        return 0;
    }
    c->activeConstEvalCtx = savedActiveEvalCtx;
    baseTypeId = H2TCResolveAliasBaseType(c, typeId);
    return baseTypeId >= 0 && (uint32_t)baseTypeId < c->typeLen
        && c->types[baseTypeId].kind == H2TCType_PACK;
}

static void H2TCMirConstSetReasonCb(
    void* _Nullable ctx, uint32_t start, uint32_t end, const char* reason) {
    H2TCConstSetReason((H2TCConstEvalCtx*)ctx, start, end, reason);
}

void H2TCConstSetReason(
    H2TCConstEvalCtx* evalCtx, uint32_t start, uint32_t end, const char* reason) {
    uint32_t traceDepth;
    if (evalCtx == NULL || reason == NULL || reason[0] == '\0' || evalCtx->nonConstReason != NULL) {
        return;
    }
    evalCtx->nonConstReason = reason;
    evalCtx->nonConstStart = start;
    evalCtx->nonConstEnd = end;
    traceDepth = evalCtx->fnDepth;
    if (traceDepth > H2TC_CONST_CALL_MAX_DEPTH) {
        traceDepth = H2TC_CONST_CALL_MAX_DEPTH;
    }
    evalCtx->nonConstTraceDepth = traceDepth;
    if (traceDepth > 0) {
        memcpy(evalCtx->nonConstTrace, evalCtx->fnStack, sizeof(int32_t) * traceDepth);
    }
    if (evalCtx->execCtx != NULL) {
        H2CTFEExecSetReason(evalCtx->execCtx, start, end, reason);
    }
}

void H2TCClearLastConstEvalReason(H2TypeCheckCtx* c) {
    if (c == NULL) {
        return;
    }
    c->lastConstEvalReason = NULL;
    c->lastConstEvalReasonStart = 0;
    c->lastConstEvalReasonEnd = 0;
    c->lastConstEvalTraceDepth = 0;
    c->lastConstEvalRootFnIndex = -1;
    c->lastConstEvalRootCallStart = 0;
    memset(c->lastConstEvalTrace, 0, sizeof(c->lastConstEvalTrace));
}

void H2TCStoreLastConstEvalReason(H2TypeCheckCtx* c, const H2TCConstEvalCtx* evalCtx) {
    uint32_t traceDepth;
    if (c == NULL) {
        return;
    }
    H2TCClearLastConstEvalReason(c);
    if (evalCtx == NULL) {
        return;
    }
    c->lastConstEvalReason = evalCtx->nonConstReason;
    c->lastConstEvalReasonStart = evalCtx->nonConstStart;
    c->lastConstEvalReasonEnd = evalCtx->nonConstEnd;
    traceDepth = evalCtx->nonConstTraceDepth;
    if (traceDepth > H2TC_CONST_CALL_MAX_DEPTH) {
        traceDepth = H2TC_CONST_CALL_MAX_DEPTH;
    }
    c->lastConstEvalTraceDepth = traceDepth;
    if (traceDepth > 0) {
        memcpy(c->lastConstEvalTrace, evalCtx->nonConstTrace, sizeof(int32_t) * traceDepth);
    }
    c->lastConstEvalRootFnIndex = evalCtx->rootCallOwnerFnIndex;
    c->lastConstEvalRootCallStart = evalCtx->rootCallStart;
}

void H2TCSetLastConstEvalReason(
    H2TypeCheckCtx* c, const char* reason, uint32_t start, uint32_t end) {
    if (c == NULL) {
        return;
    }
    H2TCClearLastConstEvalReason(c);
    c->lastConstEvalReason = reason;
    c->lastConstEvalReasonStart = start;
    c->lastConstEvalReasonEnd = end;
}

static void H2TCConstEvalRememberRootCall(
    H2TCConstEvalCtx* evalCtx, int32_t ownerFnIndex, uint32_t callStart) {
    if (evalCtx == NULL || evalCtx->fnDepth != 0 || evalCtx->rootCallOwnerFnIndex >= 0
        || ownerFnIndex < 0)
    {
        return;
    }
    evalCtx->rootCallOwnerFnIndex = ownerFnIndex;
    evalCtx->rootCallStart = callStart;
}

static int32_t H2TCConstEvalFindOwnerFunctionForOffset(H2TypeCheckCtx* c, uint32_t offset) {
    uint32_t i;
    int32_t  bestFnIndex = -1;
    uint32_t bestSpan = UINT32_MAX;
    if (c == NULL) {
        return -1;
    }
    if (c->currentFunctionIndex >= 0) {
        int32_t currentDefNode = c->funcs[c->currentFunctionIndex].defNode;
        int32_t currentBodyNode =
            currentDefNode >= 0 ? H2AstFirstChild(c->ast, currentDefNode) : -1;
        while (currentBodyNode >= 0 && (uint32_t)currentBodyNode < c->ast->len
               && c->ast->nodes[currentBodyNode].kind != H2Ast_BLOCK)
        {
            currentBodyNode = H2AstNextSibling(c->ast, currentBodyNode);
        }
        if (currentBodyNode >= 0 && (uint32_t)currentBodyNode < c->ast->len
            && c->ast->nodes[currentBodyNode].start <= offset
            && offset < c->ast->nodes[currentBodyNode].end)
        {
            return c->currentFunctionIndex;
        }
    }
    for (i = 0; i < c->funcLen; i++) {
        int32_t  defNode = c->funcs[i].defNode;
        int32_t  bodyNode;
        uint32_t span;
        if (defNode < 0 || (uint32_t)defNode >= c->ast->len) {
            continue;
        }
        bodyNode = H2AstFirstChild(c->ast, defNode);
        while (bodyNode >= 0 && (uint32_t)bodyNode < c->ast->len
               && c->ast->nodes[bodyNode].kind != H2Ast_BLOCK)
        {
            bodyNode = H2AstNextSibling(c->ast, bodyNode);
        }
        if (bodyNode < 0 || (uint32_t)bodyNode >= c->ast->len) {
            continue;
        }
        if (c->ast->nodes[bodyNode].start > offset || offset >= c->ast->nodes[bodyNode].end) {
            continue;
        }
        span = c->ast->nodes[bodyNode].end - c->ast->nodes[bodyNode].start;
        if (bestFnIndex < 0 || span < bestSpan) {
            bestFnIndex = (int32_t)i;
            bestSpan = span;
        }
    }
    return bestFnIndex;
}

static void H2TCConstEvalRememberRootCallNode(
    H2TCConstEvalCtx* evalCtx, int32_t callNode, int32_t callCalleeNode) {
    H2TypeCheckCtx* c;
    uint32_t        callStart;
    int32_t         ownerFnIndex;
    if (evalCtx == NULL || evalCtx->tc == NULL) {
        return;
    }
    c = evalCtx->tc;
    if (callCalleeNode >= 0 && (uint32_t)callCalleeNode < c->ast->len
        && c->ast->nodes[callCalleeNode].dataEnd > c->ast->nodes[callCalleeNode].dataStart)
    {
        callStart = c->ast->nodes[callCalleeNode].dataStart;
    } else if (callNode >= 0 && (uint32_t)callNode < c->ast->len) {
        callStart = c->ast->nodes[callNode].start;
    } else {
        return;
    }
    ownerFnIndex = H2TCConstEvalFindOwnerFunctionForOffset(c, callStart);
    H2TCConstEvalRememberRootCall(evalCtx, ownerFnIndex, callStart);
}

static int H2TCConstLookupMirLocalValue(
    H2TCConstEvalCtx* evalCtx, uint32_t nameStart, uint32_t nameEnd, H2CTFEValue* outValue) {
    H2TypeCheckCtx* c;
    uint32_t        i;
    if (evalCtx == NULL || outValue == NULL || evalCtx->mirProgram == NULL
        || evalCtx->mirFunction == NULL || evalCtx->mirLocals == NULL)
    {
        return 0;
    }
    c = evalCtx->tc;
    if (c == NULL || evalCtx->mirFunction->localStart > evalCtx->mirProgram->localLen
        || evalCtx->mirFunction->localCount
               > evalCtx->mirProgram->localLen - evalCtx->mirFunction->localStart
        || evalCtx->mirLocalCount < evalCtx->mirFunction->localCount)
    {
        return 0;
    }
    for (i = evalCtx->mirFunction->localCount; i > 0; i--) {
        const H2MirLocal* local =
            &evalCtx->mirProgram->locals[evalCtx->mirFunction->localStart + i - 1u];
        const H2CTFEValue* value = &evalCtx->mirLocals[i - 1u];
        if (local->nameEnd <= local->nameStart
            || !H2NameEqSlice(c->src, local->nameStart, local->nameEnd, nameStart, nameEnd))
        {
            continue;
        }
        if (value->kind == H2CTFEValue_INVALID) {
            continue;
        }
        *outValue = *value;
        return 1;
    }
    return 0;
}

int H2TCMirConstBindFrame(
    void* _Nullable ctx,
    const H2MirProgram* _Nullable program,
    const H2MirFunction* _Nullable function,
    const H2CTFEValue* _Nullable locals,
    uint32_t localCount,
    H2Diag* _Nullable diag) {
    H2TCConstEvalCtx* evalCtx = (H2TCConstEvalCtx*)ctx;
    if (evalCtx == NULL || evalCtx->mirFrameDepth >= H2TC_CONST_CALL_MAX_DEPTH) {
        if (diag != NULL) {
            H2DiagReset(diag, H2Diag_UNEXPECTED_TOKEN);
        }
        return -1;
    }
    evalCtx->mirSavedPrograms[evalCtx->mirFrameDepth] = evalCtx->mirProgram;
    evalCtx->mirSavedFunctions[evalCtx->mirFrameDepth] = evalCtx->mirFunction;
    evalCtx->mirSavedLocals[evalCtx->mirFrameDepth] = evalCtx->mirLocals;
    evalCtx->mirSavedLocalCounts[evalCtx->mirFrameDepth] = evalCtx->mirLocalCount;
    evalCtx->mirFrameDepth++;
    evalCtx->mirProgram = program;
    evalCtx->mirFunction = function;
    evalCtx->mirLocals = locals;
    evalCtx->mirLocalCount = localCount;
    return 0;
}

void H2TCMirConstUnbindFrame(void* _Nullable ctx) {
    H2TCConstEvalCtx* evalCtx = (H2TCConstEvalCtx*)ctx;
    if (evalCtx == NULL || evalCtx->mirFrameDepth == 0) {
        return;
    }
    evalCtx->mirFrameDepth--;
    evalCtx->mirProgram = evalCtx->mirSavedPrograms[evalCtx->mirFrameDepth];
    evalCtx->mirFunction = evalCtx->mirSavedFunctions[evalCtx->mirFrameDepth];
    evalCtx->mirLocals = evalCtx->mirSavedLocals[evalCtx->mirFrameDepth];
    evalCtx->mirLocalCount = evalCtx->mirSavedLocalCounts[evalCtx->mirFrameDepth];
}

void H2TCMirConstAdoptLowerDiagReason(H2TCConstEvalCtx* evalCtx, const H2Diag* _Nullable diag) {
    if (evalCtx == NULL || diag == NULL || diag->detail == NULL || diag->detail[0] == '\0') {
        return;
    }
    H2TCConstSetReason(evalCtx, diag->start, diag->end, diag->detail);
}

int H2TCMirConstLowerConstExpr(
    void* _Nullable ctx, int32_t exprNode, H2MirConst* _Nonnull outValue, H2Diag* _Nullable diag) {
    H2TCConstEvalCtx* evalCtx = (H2TCConstEvalCtx*)ctx;
    H2TCConstEvalCtx  localEvalCtx;
    H2TypeCheckCtx*   c;
    H2CTFEValue       value;
    int32_t           reflectedTypeId = -1;
    int               isConst = 0;
    if (outValue == NULL || evalCtx == NULL || evalCtx->tc == NULL || exprNode < 0) {
        return 0;
    }
    c = evalCtx->tc;
    if ((uint32_t)exprNode >= c->ast->len) {
        return 0;
    }
    localEvalCtx = *evalCtx;
    localEvalCtx.nonConstReason = NULL;
    localEvalCtx.nonConstStart = 0;
    localEvalCtx.nonConstEnd = 0;
    localEvalCtx.nonConstTraceDepth = 0;
    if (c->ast->nodes[exprNode].kind == H2Ast_SIZEOF) {
        if (H2TCConstEvalSizeOf(&localEvalCtx, exprNode, &value, &isConst) != 0) {
            return -1;
        }
        if (!isConst || value.kind != H2CTFEValue_INT) {
            if (diag != NULL && diag->detail == NULL && localEvalCtx.nonConstReason != NULL) {
                diag->start = localEvalCtx.nonConstStart;
                diag->end = localEvalCtx.nonConstEnd;
                diag->detail = localEvalCtx.nonConstReason;
            }
            return 0;
        }
        outValue->kind = H2MirConst_INT;
        outValue->bits = (uint64_t)value.i64;
        return 1;
    }
    if (H2TCResolveReflectedTypeValueExpr(c, exprNode, &reflectedTypeId) < 0) {
        return -1;
    }
    if (reflectedTypeId >= 0) {
        outValue->kind = H2MirConst_TYPE;
        outValue->bits = H2TCEncodeTypeTag(c, reflectedTypeId);
        return 1;
    }
    if (c->ast->nodes[exprNode].kind == H2Ast_CALL) {
        int32_t calleeNode = H2AstFirstChild(c->ast, exprNode);
        if (calleeNode < 0 || (uint32_t)calleeNode >= c->ast->len
            || c->ast->nodes[calleeNode].kind != H2Ast_IDENT
            || !H2NameEqLiteral(
                c->src,
                c->ast->nodes[calleeNode].dataStart,
                c->ast->nodes[calleeNode].dataEnd,
                "typeof"))
        {
            return 0;
        }
        if (H2TCConstEvalTypeOf(&localEvalCtx, exprNode, &value, &isConst) != 0) {
            return -1;
        }
        if (!isConst || value.kind != H2CTFEValue_TYPE) {
            if (diag != NULL && diag->detail == NULL && localEvalCtx.nonConstReason != NULL) {
                diag->start = localEvalCtx.nonConstStart;
                diag->end = localEvalCtx.nonConstEnd;
                diag->detail = localEvalCtx.nonConstReason;
            }
            return 0;
        }
        outValue->kind = H2MirConst_TYPE;
        outValue->bits = value.typeTag;
        return 1;
    }
    return 0;
}

void H2TCConstSetReasonNode(H2TCConstEvalCtx* evalCtx, int32_t nodeId, const char* reason) {
    H2TypeCheckCtx* c;
    if (evalCtx == NULL) {
        return;
    }
    c = evalCtx->tc;
    if (c != NULL && nodeId >= 0 && (uint32_t)nodeId < c->ast->len) {
        H2TCConstSetReason(evalCtx, c->ast->nodes[nodeId].start, c->ast->nodes[nodeId].end, reason);
        return;
    }
    H2TCConstSetReason(evalCtx, 0, 0, reason);
}

static void H2TCConstEvalDiagOffsetToLineCol(
    const char* _Nullable source, uint32_t offset, uint32_t* outLine, uint32_t* outCol) {
    uint32_t line = 1;
    uint32_t col = 1;
    uint32_t i = 0;
    if (outLine != NULL) {
        *outLine = 1;
    }
    if (outCol != NULL) {
        *outCol = 1;
    }
    if (source == NULL) {
        return;
    }
    while (i < offset && source[i] != '\0') {
        if (source[i] == '\n') {
            line++;
            col = 1;
        } else if (source[i] == '\t') {
            col += 4u - ((col - 1u) % 4u);
        } else {
            col++;
        }
        i++;
    }
    if (outLine != NULL) {
        *outLine = line;
    }
    if (outCol != NULL) {
        *outCol = col;
    }
}

static void H2TCConstEvalAppendTraceFrame(
    H2TypeCheckCtx* c, H2TCTextBuf* detailText, int32_t fnIndex, uint32_t offset) {
    uint32_t    line = 1;
    uint32_t    col = 1;
    const char* path = NULL;
    if (c == NULL || detailText == NULL || fnIndex < 0 || (uint32_t)fnIndex >= c->funcLen) {
        return;
    }
    H2TCConstEvalDiagOffsetToLineCol(c->src.ptr, offset, &line, &col);
    path = (c->filePath != NULL && c->filePath[0] != '\0') ? c->filePath : "<input>";
    H2TCTextBufAppendCStr(detailText, "\n  ");
    H2TCTextBufAppendCStr(detailText, path);
    H2TCTextBufAppendChar(detailText, ':');
    H2TCTextBufAppendU32(detailText, line);
    H2TCTextBufAppendChar(detailText, ':');
    H2TCTextBufAppendU32(detailText, col);
    H2TCTextBufAppendCStr(detailText, ": ");
    if (c->funcs[fnIndex].nameEnd > c->funcs[fnIndex].nameStart
        && c->funcs[fnIndex].nameEnd <= c->src.len)
    {
        H2TCTextBufAppendSlice(
            detailText, c->src, c->funcs[fnIndex].nameStart, c->funcs[fnIndex].nameEnd);
    } else {
        H2TCTextBufAppendCStr(detailText, "<unknown>");
    }
}

void H2TCAttachConstEvalReason(H2TypeCheckCtx* c) {
    H2TCTextBuf detailText;
    char        detailBuf[2048];
    uint32_t    i;
    if (c == NULL || c->diag == NULL || c->lastConstEvalReason == NULL
        || c->lastConstEvalReason[0] == '\0')
    {
        return;
    }
    H2TCTextBufInit(&detailText, detailBuf, (uint32_t)sizeof(detailBuf));
    H2TCTextBufAppendCStr(&detailText, c->lastConstEvalReason);
    if (c->lastConstEvalTraceDepth > 0) {
        H2TCTextBufAppendCStr(&detailText, "\nCall trace:");
        for (i = c->lastConstEvalTraceDepth; i > 0; i--) {
            int32_t fnIndex = c->lastConstEvalTrace[i - 1u];
            if (fnIndex < 0 || (uint32_t)fnIndex >= c->funcLen) {
                continue;
            }
            H2TCConstEvalAppendTraceFrame(c, &detailText, fnIndex, c->funcs[fnIndex].nameStart);
        }
        if (c->lastConstEvalRootFnIndex >= 0) {
            H2TCConstEvalAppendTraceFrame(
                c, &detailText, c->lastConstEvalRootFnIndex, c->lastConstEvalRootCallStart);
        }
    }
    c->diag->phase = H2DiagPhase_CONSTEVAL;
    c->diag->detail = H2TCAllocDiagText(c, detailBuf);
    if (c->lastConstEvalReasonStart < c->lastConstEvalReasonEnd) {
        (void)H2DiagAddNote(
            c->arena,
            c->diag,
            H2DiagNoteKind_BECAUSE_OF,
            c->lastConstEvalReasonStart,
            c->lastConstEvalReasonEnd,
            c->lastConstEvalReason);
    }
}

static int32_t H2TCFindPkgQualifiedFunctionValueIndexBySlice(
    H2TypeCheckCtx* c, uint32_t nameStart, uint32_t nameEnd) {
    uint32_t i;
    uint32_t pkgStart = 0;
    uint32_t pkgEnd = 0;
    if (c == NULL || nameEnd <= nameStart + 2u || nameEnd > c->src.len) {
        return -1;
    }
    for (i = nameStart + 1u; i + 1u < nameEnd; i++) {
        if (c->src.ptr[i] == '.') {
            return H2TCFindPkgQualifiedFunctionValueIndex(c, nameStart, i, i + 1u, nameEnd);
        }
    }
    if (H2TCExtractPkgPrefixFromTypeName(c, nameStart, nameEnd, &pkgStart, &pkgEnd)) {
        uint32_t methodStart = pkgEnd + 2u;
        if (methodStart < nameEnd) {
            return H2TCFindPkgQualifiedFunctionValueIndex(
                c, pkgStart, pkgEnd, methodStart, nameEnd);
        }
    }
    return -1;
}

static void H2TCConstEvalValueInvalid(H2CTFEValue* v) {
    if (v == NULL) {
        return;
    }
    v->kind = H2CTFEValue_INVALID;
    v->i64 = 0;
    v->f64 = 0.0;
    v->b = 0;
    v->typeTag = 0;
    v->s.bytes = NULL;
    v->s.len = 0;
    v->span.fileBytes = NULL;
    v->span.fileLen = 0;
    v->span.startLine = 0;
    v->span.startColumn = 0;
    v->span.endLine = 0;
    v->span.endColumn = 0;
}

static int H2TCConstEvalValueToF64(const H2CTFEValue* value, double* out) {
    if (value == NULL || out == NULL) {
        return 0;
    }
    if (value->kind == H2CTFEValue_INT) {
        *out = (double)value->i64;
        return 1;
    }
    if (value->kind == H2CTFEValue_FLOAT) {
        *out = value->f64;
        return 1;
    }
    return 0;
}

static int H2TCConstEvalStringEq(const H2CTFEString* a, const H2CTFEString* b) {
    if (a == NULL || b == NULL) {
        return 0;
    }
    if (a->len != b->len) {
        return 0;
    }
    if (a->len == 0) {
        return 1;
    }
    return memcmp(a->bytes, b->bytes, a->len) == 0;
}

static int H2TCConstEvalStringConcat(
    H2TypeCheckCtx* c, const H2CTFEString* a, const H2CTFEString* b, H2CTFEString* out) {
    uint64_t totalLen64;
    uint32_t totalLen;
    uint8_t* bytes;
    if (c == NULL || a == NULL || b == NULL || out == NULL) {
        return -1;
    }
    totalLen64 = (uint64_t)a->len + (uint64_t)b->len;
    if (totalLen64 > UINT32_MAX) {
        return 0;
    }
    totalLen = (uint32_t)totalLen64;
    if (totalLen == 0) {
        out->bytes = NULL;
        out->len = 0;
        return 1;
    }
    bytes = (uint8_t*)H2ArenaAlloc(c->arena, totalLen, (uint32_t)_Alignof(uint8_t));
    if (bytes == NULL) {
        return -1;
    }
    if (a->len > 0) {
        memcpy(bytes, a->bytes, a->len);
    }
    if (b->len > 0) {
        memcpy(bytes + a->len, b->bytes, b->len);
    }
    out->bytes = bytes;
    out->len = totalLen;
    return 1;
}

static int H2TCConstEvalAddI64(int64_t a, int64_t b, int64_t* out) {
#if __has_builtin(__builtin_add_overflow)
    return __builtin_add_overflow(a, b, out) ? -1 : 0;
#else
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) {
        return -1;
    }
    *out = a + b;
    return 0;
#endif
}

static int H2TCConstEvalSubI64(int64_t a, int64_t b, int64_t* out) {
#if __has_builtin(__builtin_sub_overflow)
    return __builtin_sub_overflow(a, b, out) ? -1 : 0;
#else
    if ((b > 0 && a < INT64_MIN + b) || (b < 0 && a > INT64_MAX + b)) {
        return -1;
    }
    *out = a - b;
    return 0;
#endif
}

static int H2TCConstEvalMulI64(int64_t a, int64_t b, int64_t* out) {
#if __has_builtin(__builtin_mul_overflow)
    return __builtin_mul_overflow(a, b, out) ? -1 : 0;
#else
    if (a == 0 || b == 0) {
        *out = 0;
        return 0;
    }
    if ((a == -1 && b == INT64_MIN) || (b == -1 && a == INT64_MIN)) {
        return -1;
    }
    if (a > 0) {
        if (b > 0) {
            if (a > INT64_MAX / b) {
                return -1;
            }
        } else if (b < INT64_MIN / a) {
            return -1;
        }
    } else if (b > 0) {
        if (a < INT64_MIN / b) {
            return -1;
        }
    } else if (a != 0 && b < INT64_MAX / a) {
        return -1;
    }
    *out = a * b;
    return 0;
#endif
}

static int H2TCConstEvalApplyUnary(
    H2TokenKind op, const H2CTFEValue* inValue, H2CTFEValue* outValue) {
    H2TCConstEvalValueInvalid(outValue);
    if (inValue == NULL) {
        return 0;
    }
    if (op == H2Tok_ADD && inValue->kind == H2CTFEValue_INT) {
        *outValue = *inValue;
        return 1;
    }
    if (op == H2Tok_ADD && inValue->kind == H2CTFEValue_FLOAT) {
        *outValue = *inValue;
        return 1;
    }
    if (op == H2Tok_SUB && inValue->kind == H2CTFEValue_INT) {
        if (inValue->i64 == INT64_MIN) {
            return 0;
        }
        outValue->kind = H2CTFEValue_INT;
        outValue->i64 = -inValue->i64;
        return 1;
    }
    if (op == H2Tok_SUB && inValue->kind == H2CTFEValue_FLOAT) {
        outValue->kind = H2CTFEValue_FLOAT;
        outValue->f64 = -inValue->f64;
        return 1;
    }
    if (op == H2Tok_NOT && inValue->kind == H2CTFEValue_BOOL) {
        outValue->kind = H2CTFEValue_BOOL;
        outValue->b = inValue->b ? 0u : 1u;
        return 1;
    }
    return 0;
}

static int H2TCConstEvalApplyBinary(
    H2TypeCheckCtx*    c,
    H2TokenKind        op,
    const H2CTFEValue* lhs,
    const H2CTFEValue* rhs,
    H2CTFEValue*       outValue) {
    int64_t i;
    double  lhsF64;
    double  rhsF64;
    H2TCConstEvalValueInvalid(outValue);
    if (c == NULL || lhs == NULL || rhs == NULL) {
        return 0;
    }

    if (lhs->kind == H2CTFEValue_INT && rhs->kind == H2CTFEValue_INT) {
        switch (op) {
            case H2Tok_ADD:
                if (H2TCConstEvalAddI64(lhs->i64, rhs->i64, &i) != 0) {
                    return 0;
                }
                outValue->kind = H2CTFEValue_INT;
                outValue->i64 = i;
                return 1;
            case H2Tok_SUB:
                if (H2TCConstEvalSubI64(lhs->i64, rhs->i64, &i) != 0) {
                    return 0;
                }
                outValue->kind = H2CTFEValue_INT;
                outValue->i64 = i;
                return 1;
            case H2Tok_MUL:
                if (H2TCConstEvalMulI64(lhs->i64, rhs->i64, &i) != 0) {
                    return 0;
                }
                outValue->kind = H2CTFEValue_INT;
                outValue->i64 = i;
                return 1;
            case H2Tok_DIV:
                if (rhs->i64 == 0 || (lhs->i64 == INT64_MIN && rhs->i64 == -1)) {
                    return 0;
                }
                outValue->kind = H2CTFEValue_INT;
                outValue->i64 = lhs->i64 / rhs->i64;
                return 1;
            case H2Tok_MOD:
                if (rhs->i64 == 0 || (lhs->i64 == INT64_MIN && rhs->i64 == -1)) {
                    return 0;
                }
                outValue->kind = H2CTFEValue_INT;
                outValue->i64 = lhs->i64 % rhs->i64;
                return 1;
            case H2Tok_AND:
                outValue->kind = H2CTFEValue_INT;
                outValue->i64 = lhs->i64 & rhs->i64;
                return 1;
            case H2Tok_OR:
                outValue->kind = H2CTFEValue_INT;
                outValue->i64 = lhs->i64 | rhs->i64;
                return 1;
            case H2Tok_XOR:
                outValue->kind = H2CTFEValue_INT;
                outValue->i64 = lhs->i64 ^ rhs->i64;
                return 1;
            case H2Tok_LSHIFT:
                if (rhs->i64 < 0 || rhs->i64 > 63 || lhs->i64 < 0) {
                    return 0;
                }
                outValue->kind = H2CTFEValue_INT;
                outValue->i64 = (int64_t)((uint64_t)lhs->i64 << (uint32_t)rhs->i64);
                return 1;
            case H2Tok_RSHIFT:
                if (rhs->i64 < 0 || rhs->i64 > 63 || lhs->i64 < 0) {
                    return 0;
                }
                outValue->kind = H2CTFEValue_INT;
                outValue->i64 = (int64_t)((uint64_t)lhs->i64 >> (uint32_t)rhs->i64);
                return 1;
            case H2Tok_EQ:
                outValue->kind = H2CTFEValue_BOOL;
                outValue->b = lhs->i64 == rhs->i64;
                return 1;
            case H2Tok_NEQ:
                outValue->kind = H2CTFEValue_BOOL;
                outValue->b = lhs->i64 != rhs->i64;
                return 1;
            case H2Tok_LT:
                outValue->kind = H2CTFEValue_BOOL;
                outValue->b = lhs->i64 < rhs->i64;
                return 1;
            case H2Tok_GT:
                outValue->kind = H2CTFEValue_BOOL;
                outValue->b = lhs->i64 > rhs->i64;
                return 1;
            case H2Tok_LTE:
                outValue->kind = H2CTFEValue_BOOL;
                outValue->b = lhs->i64 <= rhs->i64;
                return 1;
            case H2Tok_GTE:
                outValue->kind = H2CTFEValue_BOOL;
                outValue->b = lhs->i64 >= rhs->i64;
                return 1;
            default: return 0;
        }
    }

    if (lhs->kind == H2CTFEValue_BOOL && rhs->kind == H2CTFEValue_BOOL) {
        switch (op) {
            case H2Tok_LOGICAL_AND:
                outValue->kind = H2CTFEValue_BOOL;
                outValue->b = lhs->b && rhs->b;
                return 1;
            case H2Tok_LOGICAL_OR:
                outValue->kind = H2CTFEValue_BOOL;
                outValue->b = lhs->b || rhs->b;
                return 1;
            case H2Tok_EQ:
                outValue->kind = H2CTFEValue_BOOL;
                outValue->b = lhs->b == rhs->b;
                return 1;
            case H2Tok_NEQ:
                outValue->kind = H2CTFEValue_BOOL;
                outValue->b = lhs->b != rhs->b;
                return 1;
            default: return 0;
        }
    }

    if (lhs->kind == H2CTFEValue_STRING && rhs->kind == H2CTFEValue_STRING) {
        switch (op) {
            case H2Tok_ADD:
                outValue->kind = H2CTFEValue_STRING;
                return H2TCConstEvalStringConcat(c, &lhs->s, &rhs->s, &outValue->s);
            case H2Tok_EQ:
                outValue->kind = H2CTFEValue_BOOL;
                outValue->b = H2TCConstEvalStringEq(&lhs->s, &rhs->s);
                return 1;
            case H2Tok_NEQ:
                outValue->kind = H2CTFEValue_BOOL;
                outValue->b = !H2TCConstEvalStringEq(&lhs->s, &rhs->s);
                return 1;
            default: return 0;
        }
    }

    if (lhs->kind == H2CTFEValue_TYPE && rhs->kind == H2CTFEValue_TYPE) {
        switch (op) {
            case H2Tok_EQ:
                outValue->kind = H2CTFEValue_BOOL;
                outValue->b = lhs->typeTag == rhs->typeTag;
                return 1;
            case H2Tok_NEQ:
                outValue->kind = H2CTFEValue_BOOL;
                outValue->b = lhs->typeTag != rhs->typeTag;
                return 1;
            default: return 0;
        }
    }

    if (H2TCConstEvalValueToF64(lhs, &lhsF64) && H2TCConstEvalValueToF64(rhs, &rhsF64)) {
        switch (op) {
            case H2Tok_ADD:
                outValue->kind = H2CTFEValue_FLOAT;
                outValue->f64 = lhsF64 + rhsF64;
                return 1;
            case H2Tok_SUB:
                outValue->kind = H2CTFEValue_FLOAT;
                outValue->f64 = lhsF64 - rhsF64;
                return 1;
            case H2Tok_MUL:
                outValue->kind = H2CTFEValue_FLOAT;
                outValue->f64 = lhsF64 * rhsF64;
                return 1;
            case H2Tok_DIV:
                outValue->kind = H2CTFEValue_FLOAT;
                outValue->f64 = lhsF64 / rhsF64;
                return 1;
            case H2Tok_EQ:
                outValue->kind = H2CTFEValue_BOOL;
                outValue->b = lhsF64 == rhsF64;
                return 1;
            case H2Tok_NEQ:
                outValue->kind = H2CTFEValue_BOOL;
                outValue->b = lhsF64 != rhsF64;
                return 1;
            case H2Tok_LT:
                outValue->kind = H2CTFEValue_BOOL;
                outValue->b = lhsF64 < rhsF64;
                return 1;
            case H2Tok_GT:
                outValue->kind = H2CTFEValue_BOOL;
                outValue->b = lhsF64 > rhsF64;
                return 1;
            case H2Tok_LTE:
                outValue->kind = H2CTFEValue_BOOL;
                outValue->b = lhsF64 <= rhsF64;
                return 1;
            case H2Tok_GTE:
                outValue->kind = H2CTFEValue_BOOL;
                outValue->b = lhsF64 >= rhsF64;
                return 1;
            default: return 0;
        }
    }

    if (lhs->kind == H2CTFEValue_NULL && rhs->kind == H2CTFEValue_NULL) {
        switch (op) {
            case H2Tok_EQ:
                outValue->kind = H2CTFEValue_BOOL;
                outValue->b = 1;
                return 1;
            case H2Tok_NEQ:
                outValue->kind = H2CTFEValue_BOOL;
                outValue->b = 0;
                return 1;
            default: return 0;
        }
    }

    return 0;
}

int H2TCResolveConstIdent(
    void*        ctx,
    uint32_t     nameStart,
    uint32_t     nameEnd,
    H2CTFEValue* outValue,
    int*         outIsConst,
    H2Diag* _Nullable diag) {
    H2TCConstEvalCtx* evalCtx = (H2TCConstEvalCtx*)ctx;
    H2TypeCheckCtx*   c;
    int32_t           nodeId;
    int32_t           nameIndex = -1;
    (void)diag;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL) {
        return -1;
    }
    if (evalCtx->execCtx != NULL
        && H2CTFEExecEnvLookup(evalCtx->execCtx, nameStart, nameEnd, outValue))
    {
        *outIsConst = 1;
        return 0;
    }
    if (H2TCConstLookupMirLocalValue(evalCtx, nameStart, nameEnd, outValue)) {
        *outIsConst = 1;
        return 0;
    }
    {
        int32_t localIdx = H2TCLocalFind(c, nameStart, nameEnd);
        if (localIdx >= 0) {
            H2TCLocal* local = &c->locals[localIdx];
            int32_t    localType = local->typeId;
            int32_t    resolvedLocalType = H2TCResolveAliasBaseType(c, localType);
            if ((local->flags & H2TCLocalFlag_ANYPACK) != 0 && evalCtx->callBinding != NULL
                && H2TCConstEvalIsTrackedAnyPackName(evalCtx, nameStart, nameEnd))
            {
                const H2TCCallBinding* binding = (const H2TCCallBinding*)evalCtx->callBinding;
                int32_t                elemTypes[H2TC_MAX_CALL_ARGS];
                uint32_t               elemCount = 0;
                uint32_t               i;
                int                    haveAllTypes = 1;
                if (binding->isVariadic && binding->spreadArgIndex != UINT32_MAX) {
                    uint32_t spreadArgIndex = binding->spreadArgIndex;
                    int32_t  spreadArgType = -1;
                    if (spreadArgIndex < evalCtx->callArgCount
                        && H2TCConstEvalGetConcreteCallArgType(
                               evalCtx, spreadArgIndex, &spreadArgType)
                               == 0)
                    {
                        int32_t spreadType = H2TCResolveAliasBaseType(c, spreadArgType);
                        if (spreadType >= 0 && (uint32_t)spreadType < c->typeLen
                            && c->types[spreadType].kind == H2TCType_PACK)
                        {
                            localType = spreadArgType;
                            resolvedLocalType = spreadType;
                        }
                    }
                } else if (binding->isVariadic) {
                    for (i = 0; i < evalCtx->callArgCount; i++) {
                        int32_t elemType = -1;
                        if (binding->argParamIndices[i] != (int32_t)binding->fixedCount) {
                            continue;
                        }
                        if (elemCount >= H2TC_MAX_CALL_ARGS
                            || H2TCConstEvalGetConcreteCallArgType(evalCtx, i, &elemType) != 0)
                        {
                            haveAllTypes = 0;
                            break;
                        }
                        elemTypes[elemCount++] = elemType;
                    }
                    if (haveAllTypes) {
                        int32_t packTypeId = H2TCInternPackType(
                            c, elemTypes, elemCount, nameStart, nameEnd);
                        if (packTypeId < 0) {
                            return -1;
                        }
                        localType = packTypeId;
                        resolvedLocalType = H2TCResolveAliasBaseType(c, localType);
                    }
                }
            }
            if (resolvedLocalType >= 0 && (uint32_t)resolvedLocalType < c->typeLen
                && c->types[resolvedLocalType].kind == H2TCType_PACK)
            {
                outValue->kind = H2CTFEValue_TYPE;
                outValue->i64 = 0;
                outValue->f64 = 0.0;
                outValue->b = 0;
                outValue->typeTag = H2TCEncodeTypeTag(c, localType);
                outValue->s.bytes = NULL;
                outValue->s.len = 0;
                outValue->span.fileBytes = NULL;
                outValue->span.fileLen = 0;
                outValue->span.startLine = 0;
                outValue->span.startColumn = 0;
                outValue->span.endLine = 0;
                outValue->span.endColumn = 0;
                *outIsConst = 1;
                return 0;
            }
            if ((local->flags & H2TCLocalFlag_CONST) != 0 && local->initExprNode == -1
                && c->currentFunctionIndex >= 0 && (uint32_t)c->currentFunctionIndex < c->funcLen)
            {
                const H2TCFunction* fn = &c->funcs[(uint32_t)c->currentFunctionIndex];
                uint32_t            p;
                for (p = 0; p < fn->paramCount; p++) {
                    uint32_t paramSlot = fn->paramTypeStart + p;
                    int32_t  callArgExprNode;
                    if (paramSlot >= c->funcParamLen) {
                        continue;
                    }
                    if (!H2NameEqSlice(
                            c->src,
                            c->funcParamNameStarts[paramSlot],
                            c->funcParamNameEnds[paramSlot],
                            local->nameStart,
                            local->nameEnd))
                    {
                        continue;
                    }
                    callArgExprNode = c->funcParamCallArgExprNodes[paramSlot];
                    if (callArgExprNode < 0 || (uint32_t)callArgExprNode >= c->ast->len) {
                        break;
                    }
                    if (c->ast->nodes[callArgExprNode].kind == H2Ast_IDENT
                        && H2NameEqSlice(
                            c->src,
                            c->ast->nodes[callArgExprNode].dataStart,
                            c->ast->nodes[callArgExprNode].dataEnd,
                            local->nameStart,
                            local->nameEnd))
                    {
                        break;
                    }
                    return H2TCEvalConstExprNode(evalCtx, callArgExprNode, outValue, outIsConst);
                }
            }
            if ((local->flags & H2TCLocalFlag_CONST) != 0 && local->initExprNode != -1) {
                int32_t initExprNode = local->initExprNode;
                int     evalIsConst = 0;
                int     rc;
                if (initExprNode == -2) {
                    H2TCConstSetReason(
                        evalCtx, nameStart, nameEnd, "const local initializer is recursive");
                    *outIsConst = 0;
                    return 0;
                }
                local->initExprNode = -2;
                rc = H2TCEvalConstExprNode(evalCtx, initExprNode, outValue, &evalIsConst);
                local->initExprNode = initExprNode;
                if (rc != 0) {
                    return -1;
                }
                *outIsConst = evalIsConst;
                return 0;
            }
        }
    }
    {
        int32_t fnIdx = H2TCFindPlainFunctionValueIndex(c, nameStart, nameEnd);
        if (fnIdx >= 0) {
            H2MirValueSetFunctionRef(outValue, (uint32_t)fnIdx);
            *outIsConst = 1;
            return 0;
        }
    }
    {
        int32_t fnIdx = H2TCFindPkgQualifiedFunctionValueIndexBySlice(c, nameStart, nameEnd);
        if (fnIdx >= 0) {
            H2MirValueSetFunctionRef(outValue, (uint32_t)fnIdx);
            *outIsConst = 1;
            return 0;
        }
    }
    if (H2TCHasImportAlias(c, nameStart, nameEnd)) {
        H2TCConstEvalValueInvalid(outValue);
        outValue->kind = H2CTFEValue_SPAN;
        outValue->typeTag = H2_TC_MIR_IMPORT_ALIAS_TAG;
        outValue->span.fileBytes = (const uint8_t*)c->src.ptr + nameStart;
        outValue->span.fileLen = nameEnd - nameStart;
        outValue->span.startLine = 0;
        outValue->span.startColumn = 0;
        outValue->span.endLine = 0;
        outValue->span.endColumn = 0;
        *outIsConst = 1;
        return 0;
    }
    {
        int32_t typeId = H2TCResolveTypeValueName(c, nameStart, nameEnd);
        if (typeId >= 0) {
            outValue->kind = H2CTFEValue_TYPE;
            outValue->i64 = 0;
            outValue->f64 = 0.0;
            outValue->b = 0;
            outValue->typeTag = H2TCEncodeTypeTag(c, typeId);
            outValue->s.bytes = NULL;
            outValue->s.len = 0;
            outValue->span.fileBytes = NULL;
            outValue->span.fileLen = 0;
            outValue->span.startLine = 0;
            outValue->span.startColumn = 0;
            outValue->span.endLine = 0;
            outValue->span.endColumn = 0;
            *outIsConst = 1;
            return 0;
        }
    }
    nodeId = H2TCFindTopLevelVarLikeNode(c, nameStart, nameEnd, &nameIndex);
    if (nodeId < 0 || c->ast->nodes[nodeId].kind != H2Ast_CONST) {
        H2TCConstSetReason(
            evalCtx, nameStart, nameEnd, "identifier is not a const value in this context");
        *outIsConst = 0;
        return 0;
    }
    return H2TCEvalTopLevelConstNodeAt(c, evalCtx, nodeId, nameIndex, outValue, outIsConst);
}

int H2TCConstLookupExecBindingType(
    H2TCConstEvalCtx* evalCtx, uint32_t nameStart, uint32_t nameEnd, int32_t* outType) {
    const H2CTFEExecEnv* frame;
    if (evalCtx == NULL || evalCtx->execCtx == NULL || outType == NULL) {
        return 0;
    }
    frame = evalCtx->execCtx->env;
    while (frame != NULL) {
        uint32_t i = frame->bindingLen;
        while (i > 0) {
            const H2CTFEExecBinding* b;
            i--;
            b = &frame->bindings[i];
            if (H2NameEqSlice(evalCtx->tc->src, b->nameStart, b->nameEnd, nameStart, nameEnd)) {
                if (b->typeId >= 0) {
                    *outType = b->typeId;
                    return 1;
                }
                if (H2TCEvalConstExecInferValueTypeCb(evalCtx, &b->value, outType) == 0) {
                    return 1;
                }
            }
        }
        frame = frame->parent;
    }
    return 0;
}

int H2TCConstLookupMirLocalType(
    H2TCConstEvalCtx* evalCtx, uint32_t nameStart, uint32_t nameEnd, int32_t* outType) {
    H2TypeCheckCtx* c;
    uint32_t        i;
    uint8_t         savedAllowConstNumericTypeName;
    uint8_t         savedAllowAnytypeParamType;
    if (outType != NULL) {
        *outType = -1;
    }
    if (evalCtx == NULL || outType == NULL || evalCtx->mirProgram == NULL
        || evalCtx->mirFunction == NULL)
    {
        return 0;
    }
    c = evalCtx->tc;
    if (c == NULL || evalCtx->mirFunction->localStart > evalCtx->mirProgram->localLen
        || evalCtx->mirFunction->localCount
               > evalCtx->mirProgram->localLen - evalCtx->mirFunction->localStart)
    {
        return 0;
    }
    savedAllowConstNumericTypeName = c->allowConstNumericTypeName;
    savedAllowAnytypeParamType = c->allowAnytypeParamType;
    c->allowConstNumericTypeName = 1;
    c->allowAnytypeParamType = 1;
    for (i = evalCtx->mirFunction->localCount; i > 0; i--) {
        const H2MirLocal* local =
            &evalCtx->mirProgram->locals[evalCtx->mirFunction->localStart + i - 1u];
        const H2CTFEValue* value =
            i - 1u < evalCtx->mirLocalCount ? &evalCtx->mirLocals[i - 1u] : NULL;
        if (local->nameEnd <= local->nameStart
            || !H2NameEqSlice(c->src, local->nameStart, local->nameEnd, nameStart, nameEnd))
        {
            continue;
        }
        if (local->typeRef < evalCtx->mirProgram->typeLen) {
            const H2MirTypeRef* typeRef = &evalCtx->mirProgram->types[local->typeRef];
            if (typeRef->astNode > INT32_MAX || typeRef->astNode >= c->ast->len
                || H2TCResolveTypeNode(c, (int32_t)typeRef->astNode, outType) != 0)
            {
                c->allowConstNumericTypeName = savedAllowConstNumericTypeName;
                c->allowAnytypeParamType = savedAllowAnytypeParamType;
                return -1;
            }
            c->allowConstNumericTypeName = savedAllowConstNumericTypeName;
            c->allowAnytypeParamType = savedAllowAnytypeParamType;
            return 1;
        }
        if (value != NULL && value->kind != H2CTFEValue_INVALID
            && H2TCEvalConstExecInferValueTypeCb(evalCtx, value, outType) == 0)
        {
            c->allowConstNumericTypeName = savedAllowConstNumericTypeName;
            c->allowAnytypeParamType = savedAllowAnytypeParamType;
            return 1;
        }
    }
    c->allowConstNumericTypeName = savedAllowConstNumericTypeName;
    c->allowAnytypeParamType = savedAllowAnytypeParamType;
    return 0;
}

int H2TCConstBuiltinSizeBytes(H2BuiltinKind b, uint64_t* outBytes) {
    if (outBytes == NULL) {
        return 0;
    }
    switch (b) {
        case H2Builtin_BOOL:
        case H2Builtin_U8:
        case H2Builtin_I8:     *outBytes = 1u; return 1;
        case H2Builtin_TYPE:   *outBytes = 8u; return 1;
        case H2Builtin_U16:
        case H2Builtin_I16:    *outBytes = 2u; return 1;
        case H2Builtin_U32:
        case H2Builtin_I32:
        case H2Builtin_F32:    *outBytes = 4u; return 1;
        case H2Builtin_U64:
        case H2Builtin_I64:
        case H2Builtin_F64:    *outBytes = 8u; return 1;
        case H2Builtin_USIZE:
        case H2Builtin_ISIZE:
        case H2Builtin_RAWPTR: *outBytes = (uint64_t)sizeof(void*); return 1;
        default:               return 0;
    }
}

int H2TCConstBuiltinAlignBytes(H2BuiltinKind b, uint64_t* outAlign) {
    uint64_t size = 0;
    if (outAlign == NULL || !H2TCConstBuiltinSizeBytes(b, &size)) {
        return 0;
    }
    *outAlign = size > (uint64_t)sizeof(void*) ? (uint64_t)sizeof(void*) : size;
    if (*outAlign == 0) {
        *outAlign = 1;
    }
    return 1;
}

uint64_t H2TCConstAlignUpU64(uint64_t v, uint64_t align) {
    if (align == 0) {
        return v;
    }
    return (v + align - 1u) & ~(align - 1u);
}

int H2TCConstTypeLayout(
    H2TypeCheckCtx* c, int32_t typeId, uint64_t* outSize, uint64_t* outAlign, uint32_t depth) {
    const H2TCType* t;
    uint64_t        ptrSize = (uint64_t)sizeof(void*);
    uint64_t        usizeSize = (uint64_t)sizeof(uintptr_t);
    typeId = H2TCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen || outSize == NULL || outAlign == NULL
        || depth > c->typeLen)
    {
        return 0;
    }
    t = &c->types[typeId];
    switch (t->kind) {
        case H2TCType_BUILTIN:
            if (!H2TCConstBuiltinSizeBytes(t->builtin, outSize)
                || !H2TCConstBuiltinAlignBytes(t->builtin, outAlign))
            {
                if (typeId == c->typeStr) {
                    *outSize = H2TCConstAlignUpU64(usizeSize, ptrSize) + ptrSize;
                    *outAlign = ptrSize;
                    return 1;
                }
                return 0;
            }
            return 1;
        case H2TCType_UNTYPED_INT:
            *outSize = (uint64_t)sizeof(ptrdiff_t);
            *outAlign = *outSize;
            return 1;
        case H2TCType_UNTYPED_FLOAT:
            *outSize = 8u;
            *outAlign = 8u;
            return 1;
        case H2TCType_PTR:
        case H2TCType_REF:
        case H2TCType_FUNCTION:
            *outSize = ptrSize;
            *outAlign = ptrSize;
            return 1;
        case H2TCType_ARRAY: {
            uint64_t elemSize = 0;
            uint64_t elemAlign = 0;
            if (!H2TCConstTypeLayout(c, t->baseType, &elemSize, &elemAlign, depth + 1u)) {
                return 0;
            }
            if (t->arrayLen > 0 && elemSize > UINT64_MAX / (uint64_t)t->arrayLen) {
                return 0;
            }
            *outSize = elemSize * (uint64_t)t->arrayLen;
            *outAlign = elemAlign;
            return 1;
        }
        case H2TCType_SLICE:
            *outSize = H2TCConstAlignUpU64(usizeSize, ptrSize) + ptrSize;
            *outAlign = ptrSize;
            return 1;
        case H2TCType_OPTIONAL:
            return H2TCConstTypeLayout(c, t->baseType, outSize, outAlign, depth + 1u);
        case H2TCType_NAMED:
        case H2TCType_ANON_STRUCT: {
            uint64_t offset = 0;
            uint64_t maxAlign = 1;
            uint32_t i;
            for (i = 0; i < t->fieldCount; i++) {
                uint64_t fieldSize = 0;
                uint64_t fieldAlign = 0;
                uint32_t fieldIdx = t->fieldStart + i;
                if (fieldIdx >= c->fieldLen
                    || !H2TCConstTypeLayout(
                        c, c->fields[fieldIdx].typeId, &fieldSize, &fieldAlign, depth + 1u))
                {
                    return 0;
                }
                if (fieldAlign > maxAlign) {
                    maxAlign = fieldAlign;
                }
                offset = H2TCConstAlignUpU64(offset, fieldAlign);
                if (fieldSize > UINT64_MAX - offset) {
                    return 0;
                }
                offset += fieldSize;
            }
            *outSize = H2TCConstAlignUpU64(offset, maxAlign);
            *outAlign = maxAlign;
            return 1;
        }
        case H2TCType_ANON_UNION: {
            uint64_t maxSize = 0;
            uint64_t maxAlign = 1;
            uint32_t i;
            for (i = 0; i < t->fieldCount; i++) {
                uint64_t fieldSize = 0;
                uint64_t fieldAlign = 0;
                uint32_t fieldIdx = t->fieldStart + i;
                if (fieldIdx >= c->fieldLen
                    || !H2TCConstTypeLayout(
                        c, c->fields[fieldIdx].typeId, &fieldSize, &fieldAlign, depth + 1u))
                {
                    return 0;
                }
                if (fieldSize > maxSize) {
                    maxSize = fieldSize;
                }
                if (fieldAlign > maxAlign) {
                    maxAlign = fieldAlign;
                }
            }
            *outSize = H2TCConstAlignUpU64(maxSize, maxAlign);
            *outAlign = maxAlign;
            return 1;
        }
        case H2TCType_NULL:
            *outSize = ptrSize;
            *outAlign = ptrSize;
            return 1;
        default: return 0;
    }
}

int H2TCConstEvalSizeOf(
    H2TCConstEvalCtx* evalCtx, int32_t exprNode, H2CTFEValue* outValue, int* outIsConst) {
    H2TypeCheckCtx*  c;
    const H2AstNode* n;
    int32_t          innerNode;
    int32_t          innerType = -1;
    uint64_t         sizeBytes = 0;
    uint64_t         alignBytes = 0;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return -1;
    }
    n = &c->ast->nodes[exprNode];
    innerNode = H2AstFirstChild(c->ast, exprNode);
    if (innerNode < 0) {
        H2TCConstSetReasonNode(evalCtx, exprNode, "sizeof expression is malformed");
        *outIsConst = 0;
        return 0;
    }

    if (n->flags == 1) {
        if (H2TCResolveTypeNode(c, innerNode, &innerType) != 0) {
            if (c->diag != NULL) {
                *c->diag = (H2Diag){ 0 };
            }
            if (c->ast->nodes[innerNode].kind == H2Ast_TYPE_NAME) {
                int32_t localIdx = H2TCLocalFind(
                    c, c->ast->nodes[innerNode].dataStart, c->ast->nodes[innerNode].dataEnd);
                if (localIdx >= 0) {
                    innerType = c->locals[localIdx].typeId;
                } else {
                    int32_t fnIdx = H2TCFindFunctionIndex(
                        c, c->ast->nodes[innerNode].dataStart, c->ast->nodes[innerNode].dataEnd);
                    if (fnIdx >= 0) {
                        innerType = c->funcs[fnIdx].funcTypeId;
                    } else {
                        int32_t topNameIndex = -1;
                        int32_t topNode = H2TCFindTopLevelVarLikeNode(
                            c,
                            c->ast->nodes[innerNode].dataStart,
                            c->ast->nodes[innerNode].dataEnd,
                            &topNameIndex);
                        if (topNode >= 0) {
                            if (H2TCTypeTopLevelVarLikeNode(c, topNode, topNameIndex, &innerType)
                                != 0)
                            {
                                return -1;
                            }
                        }
                    }
                }
                if (innerType < 0
                    && H2TCConstLookupExecBindingType(
                        evalCtx,
                        c->ast->nodes[innerNode].dataStart,
                        c->ast->nodes[innerNode].dataEnd,
                        &innerType))
                {
                    /* resolved from const-eval execution environment */
                }
            }
        }
    } else {
        if (H2TCTypeExpr(c, innerNode, &innerType) != 0) {
            return -1;
        }
        if (H2TCTypeContainsVarSizeByValue(c, innerType)) {
            H2TCConstSetReasonNode(
                evalCtx, innerNode, "sizeof operand type is not const-evaluable");
            *outIsConst = 0;
            return 0;
        }
    }
    if (innerType < 0) {
        H2TCConstSetReasonNode(evalCtx, innerNode, "sizeof operand type is not const-evaluable");
        *outIsConst = 0;
        return 0;
    }
    if (!H2TCConstTypeLayout(c, innerType, &sizeBytes, &alignBytes, 0)
        || sizeBytes > (uint64_t)INT64_MAX || alignBytes == 0)
    {
        H2TCConstSetReasonNode(evalCtx, innerNode, "sizeof operand type is not const-evaluable");
        *outIsConst = 0;
        return 0;
    }

    outValue->kind = H2CTFEValue_INT;
    outValue->i64 = (int64_t)sizeBytes;
    outValue->f64 = 0.0;
    outValue->b = 0;
    outValue->typeTag = 0;
    outValue->s.bytes = NULL;
    outValue->s.len = 0;
    outValue->span.fileBytes = NULL;
    outValue->span.fileLen = 0;
    outValue->span.startLine = 0;
    outValue->span.startColumn = 0;
    outValue->span.endLine = 0;
    outValue->span.endColumn = 0;
    *outIsConst = 1;
    return 0;
}

int H2TCConstEvalCast(
    H2TCConstEvalCtx* evalCtx, int32_t exprNode, H2CTFEValue* outValue, int* outIsConst) {
    H2TypeCheckCtx* c;
    int32_t         valueNode;
    int32_t         typeNode;
    int32_t         targetType;
    int32_t         baseTarget;
    H2CTFEValue     inValue;
    int             inIsConst = 0;

    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return -1;
    }

    valueNode = H2AstFirstChild(c->ast, exprNode);
    typeNode = valueNode >= 0 ? H2AstNextSibling(c->ast, valueNode) : -1;
    if (valueNode < 0 || typeNode < 0) {
        H2TCConstSetReasonNode(evalCtx, exprNode, "cast expression is malformed");
        *outIsConst = 0;
        return 0;
    }

    if (H2TCResolveTypeNode(c, typeNode, &targetType) != 0) {
        return -1;
    }
    if (H2TCEvalConstExprNode(evalCtx, valueNode, &inValue, &inIsConst) != 0) {
        return -1;
    }
    if (!inIsConst) {
        *outIsConst = 0;
        return 0;
    }

    baseTarget = H2TCResolveAliasBaseType(c, targetType);
    if (baseTarget < 0 || (uint32_t)baseTarget >= c->typeLen) {
        *outIsConst = 0;
        return 0;
    }

    if (H2TCIsIntegerType(c, baseTarget)) {
        int64_t asInt = 0;
        if (inValue.kind == H2CTFEValue_INT) {
            asInt = inValue.i64;
        } else if (inValue.kind == H2CTFEValue_BOOL) {
            asInt = inValue.b ? 1 : 0;
        } else if (inValue.kind == H2CTFEValue_FLOAT) {
            if (inValue.f64 != inValue.f64 || inValue.f64 > (double)INT64_MAX
                || inValue.f64 < (double)INT64_MIN)
            {
                H2TCConstSetReasonNode(
                    evalCtx, valueNode, "cast result is out of range for const integer");
                *outIsConst = 0;
                return 0;
            }
            asInt = (int64_t)inValue.f64;
        } else if (inValue.kind == H2CTFEValue_NULL) {
            asInt = 0;
        } else {
            *outIsConst = 0;
            return 0;
        }
        outValue->kind = H2CTFEValue_INT;
        outValue->i64 = asInt;
        outValue->f64 = 0.0;
        outValue->b = 0;
        outValue->typeTag = 0;
        outValue->s.bytes = NULL;
        outValue->s.len = 0;
        outValue->span.fileBytes = NULL;
        outValue->span.fileLen = 0;
        outValue->span.startLine = 0;
        outValue->span.startColumn = 0;
        outValue->span.endLine = 0;
        outValue->span.endColumn = 0;
        *outIsConst = 1;
        return 0;
    }

    if (H2TCIsFloatType(c, baseTarget)) {
        double asFloat = 0.0;
        if (inValue.kind == H2CTFEValue_FLOAT) {
            asFloat = inValue.f64;
        } else if (inValue.kind == H2CTFEValue_INT) {
            asFloat = (double)inValue.i64;
        } else if (inValue.kind == H2CTFEValue_BOOL) {
            asFloat = inValue.b ? 1.0 : 0.0;
        } else if (inValue.kind == H2CTFEValue_NULL) {
            asFloat = 0.0;
        } else {
            *outIsConst = 0;
            return 0;
        }
        outValue->kind = H2CTFEValue_FLOAT;
        outValue->i64 = 0;
        outValue->f64 = asFloat;
        outValue->b = 0;
        outValue->typeTag = 0;
        outValue->s.bytes = NULL;
        outValue->s.len = 0;
        outValue->span.fileBytes = NULL;
        outValue->span.fileLen = 0;
        outValue->span.startLine = 0;
        outValue->span.startColumn = 0;
        outValue->span.endLine = 0;
        outValue->span.endColumn = 0;
        *outIsConst = 1;
        return 0;
    }

    if (H2TCIsBoolType(c, baseTarget)) {
        uint8_t asBool = 0;
        if (inValue.kind == H2CTFEValue_BOOL) {
            asBool = inValue.b ? 1u : 0u;
        } else if (inValue.kind == H2CTFEValue_INT) {
            asBool = inValue.i64 != 0 ? 1u : 0u;
        } else if (inValue.kind == H2CTFEValue_FLOAT) {
            asBool = inValue.f64 != 0.0 ? 1u : 0u;
        } else if (inValue.kind == H2CTFEValue_STRING) {
            asBool = 1u;
        } else if (inValue.kind == H2CTFEValue_NULL) {
            asBool = 0;
        } else {
            *outIsConst = 0;
            return 0;
        }
        outValue->kind = H2CTFEValue_BOOL;
        outValue->i64 = 0;
        outValue->f64 = 0.0;
        outValue->b = asBool;
        outValue->typeTag = 0;
        outValue->s.bytes = NULL;
        outValue->s.len = 0;
        outValue->span.fileBytes = NULL;
        outValue->span.fileLen = 0;
        outValue->span.startLine = 0;
        outValue->span.startColumn = 0;
        outValue->span.endLine = 0;
        outValue->span.endColumn = 0;
        *outIsConst = 1;
        return 0;
    }

    if (H2TCIsRawptrType(c, baseTarget)) {
        if (inValue.kind == H2CTFEValue_NULL || inValue.kind == H2CTFEValue_REFERENCE
            || inValue.kind == H2CTFEValue_STRING)
        {
            *outValue = inValue;
            outValue->typeTag = (uint64_t)(uint32_t)baseTarget;
            *outIsConst = 1;
            return 0;
        }
        *outIsConst = 0;
        return 0;
    }

    if (c->types[baseTarget].kind == H2TCType_OPTIONAL) {
        if (inValue.kind == H2CTFEValue_OPTIONAL) {
            if (inValue.typeTag > 0 && inValue.typeTag <= (uint64_t)INT32_MAX
                && (uint32_t)inValue.typeTag < c->typeLen && (int32_t)inValue.typeTag == baseTarget)
            {
                *outValue = inValue;
                *outIsConst = 1;
                return 0;
            }
            if (inValue.b == 0u) {
                if (H2TCConstEvalSetOptionalNoneValue(c, baseTarget, outValue) != 0) {
                    return -1;
                }
                *outIsConst = 1;
                return 0;
            }
            if (inValue.s.bytes == NULL) {
                *outIsConst = 0;
                return 0;
            }
            if (H2TCConstEvalSetOptionalSomeValue(
                    c, baseTarget, (const H2CTFEValue*)inValue.s.bytes, outValue)
                != 0)
            {
                return -1;
            }
            *outIsConst = 1;
            return 0;
        }
        if (inValue.kind == H2CTFEValue_NULL) {
            if (H2TCConstEvalSetOptionalNoneValue(c, baseTarget, outValue) != 0) {
                return -1;
            }
            *outIsConst = 1;
            return 0;
        }
        if (H2TCConstEvalSetOptionalSomeValue(c, baseTarget, &inValue, outValue) != 0) {
            return -1;
        }
        *outIsConst = 1;
        return 0;
    }

    if ((c->types[baseTarget].kind == H2TCType_PTR || c->types[baseTarget].kind == H2TCType_REF
         || c->types[baseTarget].kind == H2TCType_FUNCTION)
        && inValue.kind == H2CTFEValue_NULL)
    {
        *outValue = inValue;
        *outIsConst = 1;
        return 0;
    }

    *outValue = inValue;
    *outIsConst = 1;
    return 0;
}

int H2TCConstEvalTypeOf(
    H2TCConstEvalCtx* evalCtx, int32_t exprNode, H2CTFEValue* outValue, int* outIsConst) {
    H2TypeCheckCtx*   c;
    const H2AstNode*  callee;
    int32_t           calleeNode;
    int32_t           argNode;
    int32_t           argExprNode;
    int32_t           extraNode;
    int32_t           argType;
    uint32_t          callArgIndex = 0;
    int               packStatus;
    H2TCConstEvalCtx* savedActiveEvalCtx;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return -1;
    }
    calleeNode = H2AstFirstChild(c->ast, exprNode);
    argNode = calleeNode >= 0 ? H2AstNextSibling(c->ast, calleeNode) : -1;
    extraNode = argNode >= 0 ? H2AstNextSibling(c->ast, argNode) : -1;
    if (calleeNode < 0 || argNode < 0 || extraNode >= 0) {
        H2TCConstSetReasonNode(evalCtx, exprNode, "typeof call has invalid arity");
        *outIsConst = 0;
        return 0;
    }
    callee = &c->ast->nodes[calleeNode];
    if (callee->kind != H2Ast_IDENT
        || !H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "typeof"))
    {
        return -1;
    }
    argExprNode = argNode;
    if (c->ast->nodes[argExprNode].kind == H2Ast_CALL_ARG) {
        int32_t inner = H2AstFirstChild(c->ast, argExprNode);
        if (inner >= 0) {
            argExprNode = inner;
        }
    }
    packStatus = H2TCConstEvalResolveTrackedAnyPackArgIndex(evalCtx, argExprNode, &callArgIndex);
    if (packStatus < 0) {
        return -1;
    }
    if (packStatus == 0) {
        if (H2TCConstEvalGetConcreteCallArgType(evalCtx, callArgIndex, &argType) == 0) {
            goto done;
        }
        {
            const H2TCCallBinding* binding = (const H2TCCallBinding*)evalCtx->callBinding;
            if (binding != NULL && callArgIndex < evalCtx->callArgCount
                && binding->argExpectedTypes[callArgIndex] >= 0)
            {
                argType = binding->argExpectedTypes[callArgIndex];
                goto done;
            }
        }
    }
    if (packStatus == 2 || packStatus == 3) {
        *outIsConst = 0;
        return 0;
    }
    {
        H2CTFEValue argValue;
        int         argIsConst = 0;
        if (H2TCEvalConstExprNode(evalCtx, argExprNode, &argValue, &argIsConst) != 0) {
            return -1;
        }
        if (argIsConst && H2TCEvalConstExecInferValueTypeCb(evalCtx, &argValue, &argType) == 0) {
            goto done;
        }
    }
    savedActiveEvalCtx = c->activeConstEvalCtx;
    c->activeConstEvalCtx = evalCtx;
    if (H2TCTypeExpr(c, argExprNode, &argType) != 0) {
        c->activeConstEvalCtx = savedActiveEvalCtx;
        return -1;
    }
    c->activeConstEvalCtx = savedActiveEvalCtx;
done:
    outValue->kind = H2CTFEValue_TYPE;
    outValue->i64 = 0;
    outValue->f64 = 0.0;
    outValue->b = 0;
    outValue->typeTag = H2TCEncodeTypeTag(c, argType);
    outValue->s.bytes = NULL;
    outValue->s.len = 0;
    outValue->span.fileBytes = NULL;
    outValue->span.fileLen = 0;
    outValue->span.startLine = 0;
    outValue->span.startColumn = 0;
    outValue->span.endLine = 0;
    outValue->span.endColumn = 0;
    *outIsConst = 1;
    return 0;
}

int H2TCConstEvalLenCall(
    H2TCConstEvalCtx* evalCtx, int32_t exprNode, H2CTFEValue* outValue, int* outIsConst) {
    H2TypeCheckCtx*   c;
    const H2AstNode*  callee;
    int32_t           calleeNode;
    int32_t           argNode;
    int32_t           argExprNode;
    int32_t           extraNode;
    int32_t           argType = -1;
    int32_t           resolvedArgType = -1;
    H2CTFEValue       argValue;
    int               argIsConst = 0;
    H2TCConstEvalCtx* savedActiveEvalCtx;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return -1;
    }
    calleeNode = H2AstFirstChild(c->ast, exprNode);
    argNode = calleeNode >= 0 ? H2AstNextSibling(c->ast, calleeNode) : -1;
    extraNode = argNode >= 0 ? H2AstNextSibling(c->ast, argNode) : -1;
    if (calleeNode < 0 || argNode < 0 || extraNode >= 0) {
        return 1;
    }
    callee = &c->ast->nodes[calleeNode];
    if (callee->kind != H2Ast_IDENT
        || !H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "len"))
    {
        return 1;
    }
    argExprNode = argNode;
    if (c->ast->nodes[argExprNode].kind == H2Ast_CALL_ARG) {
        argExprNode = H2AstFirstChild(c->ast, argExprNode);
        if (argExprNode < 0) {
            return 1;
        }
    }

    if (H2TCEvalConstExprNode(evalCtx, argExprNode, &argValue, &argIsConst) != 0) {
        return -1;
    }
    if (argIsConst && argValue.kind == H2CTFEValue_STRING) {
        outValue->kind = H2CTFEValue_INT;
        outValue->i64 = (int64_t)argValue.s.len;
        outValue->f64 = 0.0;
        outValue->b = 0;
        outValue->typeTag = 0;
        outValue->s.bytes = NULL;
        outValue->s.len = 0;
        outValue->span.fileBytes = NULL;
        outValue->span.fileLen = 0;
        outValue->span.startLine = 0;
        outValue->span.startColumn = 0;
        outValue->span.endLine = 0;
        outValue->span.endColumn = 0;
        *outIsConst = 1;
        return 0;
    }

    savedActiveEvalCtx = c->activeConstEvalCtx;
    c->activeConstEvalCtx = evalCtx;
    if (H2TCTypeExpr(c, argExprNode, &argType) != 0) {
        c->activeConstEvalCtx = savedActiveEvalCtx;
        return -1;
    }
    c->activeConstEvalCtx = savedActiveEvalCtx;

    resolvedArgType = H2TCResolveAliasBaseType(c, argType);
    if (resolvedArgType >= 0 && (uint32_t)resolvedArgType < c->typeLen) {
        const H2TCType* t = &c->types[resolvedArgType];
        if (t->kind == H2TCType_PACK) {
            outValue->kind = H2CTFEValue_INT;
            outValue->i64 = (int64_t)t->fieldCount;
            outValue->f64 = 0.0;
            outValue->b = 0;
            outValue->typeTag = 0;
            outValue->s.bytes = NULL;
            outValue->s.len = 0;
            outValue->span.fileBytes = NULL;
            outValue->span.fileLen = 0;
            outValue->span.startLine = 0;
            outValue->span.startColumn = 0;
            outValue->span.endLine = 0;
            outValue->span.endColumn = 0;
            *outIsConst = 1;
            return 0;
        }
        if (t->kind == H2TCType_ARRAY) {
            outValue->kind = H2CTFEValue_INT;
            outValue->i64 = (int64_t)t->arrayLen;
            outValue->f64 = 0.0;
            outValue->b = 0;
            outValue->typeTag = 0;
            outValue->s.bytes = NULL;
            outValue->s.len = 0;
            outValue->span.fileBytes = NULL;
            outValue->span.fileLen = 0;
            outValue->span.startLine = 0;
            outValue->span.startColumn = 0;
            outValue->span.endLine = 0;
            outValue->span.endColumn = 0;
            *outIsConst = 1;
            return 0;
        }
    }

    H2TCConstSetReasonNode(evalCtx, argExprNode, "len() operand is not const-evaluable");
    *outIsConst = 0;
    return 0;
}

static int H2TCConstEvalIndexExpr(
    H2TCConstEvalCtx* evalCtx, int32_t exprNode, H2CTFEValue* outValue, int* outIsConst) {
    H2TypeCheckCtx*  c;
    const H2AstNode* n;
    int32_t          baseNode;
    int32_t          idxNode;
    int32_t          extraNode;
    H2CTFEValue      baseValue;
    H2CTFEValue      idxValue;
    int              baseIsConst = 0;
    int              idxIsConst = 0;
    int64_t          idxInt = 0;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return -1;
    }
    n = &c->ast->nodes[exprNode];
    if (n->kind != H2Ast_INDEX) {
        return 1;
    }
    if ((n->flags & H2AstFlag_INDEX_SLICE) != 0) {
        return 1;
    }
    baseNode = H2AstFirstChild(c->ast, exprNode);
    idxNode = baseNode >= 0 ? H2AstNextSibling(c->ast, baseNode) : -1;
    extraNode = idxNode >= 0 ? H2AstNextSibling(c->ast, idxNode) : -1;
    if (baseNode < 0 || idxNode < 0 || extraNode >= 0) {
        return 1;
    }

    if (H2TCEvalConstExprNode(evalCtx, baseNode, &baseValue, &baseIsConst) != 0) {
        return -1;
    }
    if (!baseIsConst) {
        H2TCConstSetReasonNode(evalCtx, baseNode, "index base is not const-evaluable");
        *outIsConst = 0;
        return 0;
    }
    if (baseValue.kind != H2CTFEValue_STRING) {
        H2TCConstSetReasonNode(evalCtx, baseNode, "index base is not const-evaluable string data");
        *outIsConst = 0;
        return 0;
    }

    if (H2TCEvalConstExprNode(evalCtx, idxNode, &idxValue, &idxIsConst) != 0) {
        return -1;
    }
    if (!idxIsConst) {
        H2TCConstSetReasonNode(evalCtx, idxNode, "index is not const-evaluable");
        *outIsConst = 0;
        return 0;
    }
    if (H2CTFEValueToInt64(&idxValue, &idxInt) != 0) {
        H2TCConstSetReasonNode(evalCtx, idxNode, "index expression did not evaluate to integer");
        *outIsConst = 0;
        return 0;
    }
    if (idxInt < 0) {
        H2TCConstSetReasonNode(evalCtx, idxNode, "index is negative in const evaluation");
        *outIsConst = 0;
        return 0;
    }
    if ((uint64_t)idxInt >= (uint64_t)baseValue.s.len) {
        H2TCConstSetReasonNode(evalCtx, idxNode, "index is out of bounds in const evaluation");
        *outIsConst = 0;
        return 0;
    }

    outValue->kind = H2CTFEValue_INT;
    outValue->i64 = (int64_t)baseValue.s.bytes[(uint32_t)idxInt];
    outValue->f64 = 0.0;
    outValue->b = 0;
    outValue->typeTag = 0;
    outValue->s.bytes = NULL;
    outValue->s.len = 0;
    outValue->span.fileBytes = NULL;
    outValue->span.fileLen = 0;
    outValue->span.startLine = 0;
    outValue->span.startColumn = 0;
    outValue->span.endLine = 0;
    outValue->span.endColumn = 0;
    *outIsConst = 1;
    return 0;
}

int H2TCResolveReflectedTypeValueExpr(H2TypeCheckCtx* c, int32_t exprNode, int32_t* outTypeId) {
    const H2AstNode* n;
    if (c == NULL || outTypeId == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return -1;
    }
    n = &c->ast->nodes[exprNode];
    if (n->kind == H2Ast_CALL_ARG) {
        int32_t inner = H2AstFirstChild(c->ast, exprNode);
        if (inner < 0) {
            return 1;
        }
        return H2TCResolveReflectedTypeValueExpr(c, inner, outTypeId);
    }
    if (n->kind == H2Ast_IDENT) {
        int32_t typeId = H2TCResolveTypeValueName(c, n->dataStart, n->dataEnd);
        if (typeId < 0) {
            return 1;
        }
        *outTypeId = typeId;
        return 0;
    }
    if (n->kind == H2Ast_TYPE_VALUE) {
        int32_t typeNode = H2AstFirstChild(c->ast, exprNode);
        if (typeNode < 0) {
            return 1;
        }
        if (H2TCResolveTypeNode(c, typeNode, outTypeId) != 0) {
            return -1;
        }
        return 0;
    }
    if (H2TCIsTypeNodeKind(n->kind)) {
        if (H2TCResolveTypeNode(c, exprNode, outTypeId) != 0) {
            return -1;
        }
        return 0;
    }
    if (n->kind == H2Ast_CALL) {
        int32_t          calleeNode = H2AstFirstChild(c->ast, exprNode);
        const H2AstNode* callee = calleeNode >= 0 ? &c->ast->nodes[calleeNode] : NULL;
        int32_t          argNode;
        int32_t          extraNode;
        int32_t          elemTypeId;
        if (callee == NULL || callee->kind != H2Ast_IDENT) {
            return 1;
        }

        if (H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "typeof")) {
            argNode = H2AstNextSibling(c->ast, calleeNode);
            extraNode = argNode >= 0 ? H2AstNextSibling(c->ast, argNode) : -1;
            if (argNode < 0 || extraNode >= 0) {
                return 1;
            }
            if (H2TCTypeExpr(c, argNode, outTypeId) != 0) {
                return -1;
            }
            return 0;
        }

        if (H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "ptr")) {
            argNode = H2AstNextSibling(c->ast, calleeNode);
            extraNode = argNode >= 0 ? H2AstNextSibling(c->ast, argNode) : -1;
            if (argNode < 0 || extraNode >= 0) {
                return 1;
            }
            if (H2TCResolveReflectedTypeValueExpr(c, argNode, &elemTypeId) != 0) {
                return 1;
            }
            *outTypeId = H2TCInternPtrType(c, elemTypeId, n->start, n->end);
            return *outTypeId >= 0 ? 0 : -1;
        }

        if (H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "slice")) {
            argNode = H2AstNextSibling(c->ast, calleeNode);
            extraNode = argNode >= 0 ? H2AstNextSibling(c->ast, argNode) : -1;
            if (argNode < 0 || extraNode >= 0) {
                return 1;
            }
            if (H2TCResolveReflectedTypeValueExpr(c, argNode, &elemTypeId) != 0) {
                return 1;
            }
            *outTypeId = H2TCInternSliceType(c, elemTypeId, 0, n->start, n->end);
            return *outTypeId >= 0 ? 0 : -1;
        }

        if (H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "array")) {
            int32_t lenNode;
            int32_t lenType;
            int64_t lenValue = 0;
            int     lenIsConst = 0;
            argNode = H2AstNextSibling(c->ast, calleeNode);
            lenNode = argNode >= 0 ? H2AstNextSibling(c->ast, argNode) : -1;
            extraNode = lenNode >= 0 ? H2AstNextSibling(c->ast, lenNode) : -1;
            if (argNode < 0 || lenNode < 0 || extraNode >= 0) {
                return 1;
            }
            if (H2TCResolveReflectedTypeValueExpr(c, argNode, &elemTypeId) != 0) {
                return 1;
            }
            if (H2TCTypeExpr(c, lenNode, &lenType) != 0) {
                return -1;
            }
            if (!H2TCIsIntegerType(c, lenType)) {
                return 1;
            }
            if (H2TCConstIntExpr(c, lenNode, &lenValue, &lenIsConst) != 0 || !lenIsConst
                || lenValue < 0 || lenValue > (int64_t)UINT32_MAX)
            {
                return 1;
            }
            *outTypeId = H2TCInternArrayType(c, elemTypeId, (uint32_t)lenValue, n->start, n->end);
            return *outTypeId >= 0 ? 0 : -1;
        }
    }
    return 1;
}

int H2TCConstEvalTypeNameValue(
    H2TypeCheckCtx* c, int32_t typeId, H2CTFEValue* outValue, int* outIsConst) {
    char        tmp[256];
    H2TCTextBuf b;
    char*       storage;
    if (c == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    H2TCTextBufInit(&b, tmp, (uint32_t)sizeof(tmp));
    H2TCFormatTypeRec(c, typeId, &b, 0);
    storage = (char*)H2ArenaAlloc(c->arena, b.len + 1u, 1u);
    if (storage == NULL) {
        return -1;
    }
    if (b.len > 0u) {
        memcpy(storage, b.ptr, (size_t)b.len);
    }
    storage[b.len] = '\0';
    outValue->kind = H2CTFEValue_STRING;
    outValue->i64 = 0;
    outValue->f64 = 0.0;
    outValue->b = 0;
    outValue->typeTag = 0;
    outValue->s.bytes = (const uint8_t*)storage;
    outValue->s.len = b.len;
    outValue->span.fileBytes = NULL;
    outValue->span.fileLen = 0;
    outValue->span.startLine = 0;
    outValue->span.startColumn = 0;
    outValue->span.endLine = 0;
    outValue->span.endColumn = 0;
    *outIsConst = 1;
    return 0;
}

static void H2TCConstEvalSetTypeValue(H2TypeCheckCtx* c, int32_t typeId, H2CTFEValue* outValue) {
    if (c == NULL || outValue == NULL) {
        return;
    }
    outValue->kind = H2CTFEValue_TYPE;
    outValue->i64 = 0;
    outValue->f64 = 0.0;
    outValue->b = 0;
    outValue->typeTag = H2TCEncodeTypeTag(c, typeId);
    outValue->s.bytes = NULL;
    outValue->s.len = 0;
    outValue->span.fileBytes = NULL;
    outValue->span.fileLen = 0;
    outValue->span.startLine = 0;
    outValue->span.startColumn = 0;
    outValue->span.endLine = 0;
    outValue->span.endColumn = 0;
}

void H2TCConstEvalSetNullValue(H2CTFEValue* outValue) {
    if (outValue == NULL) {
        return;
    }
    outValue->kind = H2CTFEValue_NULL;
    outValue->i64 = 0;
    outValue->f64 = 0.0;
    outValue->b = 0;
    outValue->typeTag = 0;
    outValue->s.bytes = NULL;
    outValue->s.len = 0;
    outValue->span.fileBytes = NULL;
    outValue->span.fileLen = 0;
    outValue->span.startLine = 0;
    outValue->span.startColumn = 0;
    outValue->span.endLine = 0;
    outValue->span.endColumn = 0;
}

static int H2TCConstEvalSetOptionalNoneValue(
    H2TypeCheckCtx* c, int32_t optionalTypeId, H2CTFEValue* outValue) {
    int32_t baseTypeId;
    if (c == NULL || outValue == NULL) {
        return -1;
    }
    baseTypeId = H2TCResolveAliasBaseType(c, optionalTypeId);
    if (baseTypeId < 0 || (uint32_t)baseTypeId >= c->typeLen
        || c->types[baseTypeId].kind != H2TCType_OPTIONAL)
    {
        return -1;
    }
    H2TCConstEvalSetNullValue(outValue);
    outValue->kind = H2CTFEValue_OPTIONAL;
    outValue->b = 0;
    outValue->typeTag = (uint64_t)(uint32_t)baseTypeId;
    return 0;
}

static int H2TCConstEvalSetOptionalSomeValue(
    H2TypeCheckCtx* c, int32_t optionalTypeId, const H2CTFEValue* payload, H2CTFEValue* outValue) {
    H2CTFEValue* payloadCopy;
    int32_t      baseTypeId;
    if (c == NULL || payload == NULL || outValue == NULL) {
        return -1;
    }
    baseTypeId = H2TCResolveAliasBaseType(c, optionalTypeId);
    if (baseTypeId < 0 || (uint32_t)baseTypeId >= c->typeLen
        || c->types[baseTypeId].kind != H2TCType_OPTIONAL)
    {
        return -1;
    }
    payloadCopy = (H2CTFEValue*)H2ArenaAlloc(
        c->arena, sizeof(H2CTFEValue), (uint32_t)_Alignof(H2CTFEValue));
    if (payloadCopy == NULL) {
        return -1;
    }
    *payloadCopy = *payload;
    H2TCConstEvalSetNullValue(outValue);
    outValue->kind = H2CTFEValue_OPTIONAL;
    outValue->b = 1u;
    outValue->typeTag = (uint64_t)(uint32_t)baseTypeId;
    outValue->s.bytes = (const uint8_t*)payloadCopy;
    outValue->s.len = 0;
    return 0;
}

void H2TCConstEvalSetSourceLocationFromOffsets(
    H2TypeCheckCtx* c, uint32_t startOffset, uint32_t endOffset, H2CTFEValue* outValue) {
    H2TCConstEvalSetNullValue(outValue);
    outValue->kind = H2CTFEValue_SPAN;
    outValue->span.fileBytes = (const uint8_t*)"";
    outValue->span.fileLen = 0;
    H2TCOffsetToLineCol(
        c->src.ptr,
        c->src.len,
        startOffset,
        &outValue->span.startLine,
        &outValue->span.startColumn);
    H2TCOffsetToLineCol(
        c->src.ptr, c->src.len, endOffset, &outValue->span.endLine, &outValue->span.endColumn);
}

/* Returns 0 when resolved, 1 when not a tracked anypack index expression, 2 on non-const index,
 * 3 on out-of-bounds index, and -1 on hard error. */
static int H2TCConstEvalResolveTrackedAnyPackArgIndex(
    H2TCConstEvalCtx* evalCtx, int32_t exprNode, uint32_t* outCallArgIndex) {
    H2TypeCheckCtx*        c;
    const H2TCCallBinding* binding;
    const H2TCCallArgInfo* callArgs;
    int32_t                baseNode;
    int32_t                idxNode;
    int32_t                extraNode;
    int64_t                idxValue = 0;
    uint32_t               paramIndex;
    uint32_t               ordinal = 0;
    uint32_t               i;
    H2CTFEValue            idxConstValue;
    int                    idxIsConst = 0;
    if (evalCtx == NULL || outCallArgIndex == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    binding = (const H2TCCallBinding*)evalCtx->callBinding;
    callArgs = (const H2TCCallArgInfo*)evalCtx->callArgs;
    if (c == NULL || binding == NULL || callArgs == NULL || evalCtx->callArgCount == 0) {
        return 1;
    }
    if (exprNode < 0 || (uint32_t)exprNode >= c->ast->len
        || c->ast->nodes[exprNode].kind != H2Ast_INDEX
        || (c->ast->nodes[exprNode].flags & H2AstFlag_INDEX_SLICE) != 0)
    {
        return 1;
    }
    baseNode = H2AstFirstChild(c->ast, exprNode);
    idxNode = baseNode >= 0 ? H2AstNextSibling(c->ast, baseNode) : -1;
    extraNode = idxNode >= 0 ? H2AstNextSibling(c->ast, idxNode) : -1;
    if (baseNode < 0 || idxNode < 0 || extraNode >= 0) {
        return 1;
    }
    if (c->ast->nodes[baseNode].kind != H2Ast_IDENT
        || !H2TCConstEvalIsTrackedAnyPackName(
            evalCtx, c->ast->nodes[baseNode].dataStart, c->ast->nodes[baseNode].dataEnd))
    {
        return 1;
    }
    if (H2TCEvalConstExprNode(evalCtx, idxNode, &idxConstValue, &idxIsConst) != 0) {
        return -1;
    }
    if (!idxIsConst || H2CTFEValueToInt64(&idxConstValue, &idxValue) != 0) {
        H2TCConstSetReasonNode(
            evalCtx, idxNode, "anytype pack index must be const-evaluable integer");
        return 2;
    }
    if (idxValue < 0) {
        H2TCConstSetReasonNode(evalCtx, idxNode, "anytype pack index is out of bounds");
        return 3;
    }
    if (!binding->isVariadic) {
        H2TCConstSetReasonNode(evalCtx, idxNode, "anytype pack index is out of bounds");
        return 3;
    }
    if (binding->spreadArgIndex != UINT32_MAX) {
        uint32_t spreadArgIndex = binding->spreadArgIndex;
        int32_t  spreadArgType = -1;
        if (spreadArgIndex < evalCtx->callArgCount
            && H2TCConstEvalGetConcreteCallArgType(evalCtx, spreadArgIndex, &spreadArgType) == 0)
        {
            int32_t spreadType = H2TCResolveAliasBaseType(c, spreadArgType);
            if (spreadType >= 0 && (uint32_t)spreadType < c->typeLen
                && c->types[spreadType].kind == H2TCType_PACK)
            {
                if ((uint64_t)idxValue >= (uint64_t)c->types[spreadType].fieldCount) {
                    H2TCConstSetReasonNode(evalCtx, idxNode, "anytype pack index is out of bounds");
                    return 3;
                }
                return 1;
            }
        }
        H2TCConstSetReasonNode(evalCtx, idxNode, "anytype pack index is out of bounds");
        return 3;
    }
    paramIndex = binding->fixedCount;
    for (i = 0; i < evalCtx->callArgCount; i++) {
        if (binding->argParamIndices[i] != (int32_t)paramIndex) {
            continue;
        }
        if ((int64_t)ordinal == idxValue) {
            *outCallArgIndex = i;
            return 0;
        }
        ordinal++;
    }
    H2TCConstSetReasonNode(evalCtx, idxNode, "anytype pack index is out of bounds");
    return 3;
}

static int H2TCConstEvalResolveForwardedAnyPackArgSpan(
    H2TCConstEvalCtx* evalCtx,
    int32_t           spreadExprNode,
    int64_t           idxValue,
    uint32_t*         outStart,
    uint32_t*         outEnd) {
    H2TypeCheckCtx* c;
    uint32_t        depth;
    if (outStart != NULL) {
        *outStart = 0;
    }
    if (outEnd != NULL) {
        *outEnd = 0;
    }
    if (evalCtx == NULL || outStart == NULL || outEnd == NULL || idxValue < 0) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || spreadExprNode < 0 || (uint32_t)spreadExprNode >= c->ast->len) {
        return 1;
    }
    if (c->ast->nodes[spreadExprNode].kind == H2Ast_CALL_ARG) {
        int32_t inner = H2AstFirstChild(c->ast, spreadExprNode);
        if (inner < 0) {
            return 1;
        }
        spreadExprNode = inner;
    }
    if (c->ast->nodes[spreadExprNode].kind != H2Ast_IDENT) {
        return 1;
    }
    depth = evalCtx->callFrameDepth;
    while (depth > 0) {
        const H2TCCallArgInfo* callArgs;
        const H2TCCallBinding* binding;
        uint32_t               frameIndex = depth - 1u;
        uint32_t               callArgCount;
        uint32_t               packNameStart;
        uint32_t               packNameEnd;
        uint32_t               i;
        uint32_t               ordinal = 0;
        uint32_t               paramIndex;

        callArgs = (const H2TCCallArgInfo*)evalCtx->callFrameArgs[frameIndex];
        binding = (const H2TCCallBinding*)evalCtx->callFrameBindings[frameIndex];
        callArgCount = evalCtx->callFrameArgCounts[frameIndex];
        packNameStart = evalCtx->callFramePackParamNameStarts[frameIndex];
        packNameEnd = evalCtx->callFramePackParamNameEnds[frameIndex];

        if (callArgs == NULL || binding == NULL || callArgCount == 0
            || packNameStart >= packNameEnd)
        {
            return 1;
        }
        if (!binding->isVariadic) {
            return 1;
        }
        if (binding->spreadArgIndex != UINT32_MAX) {
            uint32_t spreadArgIndex = binding->spreadArgIndex;
            if (spreadArgIndex >= callArgCount || callArgs[spreadArgIndex].exprNode < 0
                || (uint32_t)callArgs[spreadArgIndex].exprNode >= c->ast->len)
            {
                return 1;
            }
            spreadExprNode = callArgs[spreadArgIndex].exprNode;
            if (c->ast->nodes[spreadExprNode].kind != H2Ast_IDENT) {
                return 1;
            }
            depth = frameIndex;
            continue;
        }

        paramIndex = binding->fixedCount;
        for (i = 0; i < callArgCount; i++) {
            if (binding->argParamIndices[i] != (int32_t)paramIndex) {
                continue;
            }
            if ((int64_t)ordinal == idxValue) {
                *outStart = callArgs[i].start;
                *outEnd = callArgs[i].end;
                return 0;
            }
            ordinal++;
        }
        return 1;
    }
    return 1;
}

static int H2TCConstEvalResolveTrackedAnyPackArgSpan(
    H2TCConstEvalCtx* evalCtx, int32_t exprNode, uint32_t* outStart, uint32_t* outEnd) {
    H2TypeCheckCtx*        c;
    const H2TCCallBinding* binding;
    const H2TCCallArgInfo* callArgs;
    int32_t                baseNode;
    int32_t                idxNode;
    int32_t                extraNode;
    int64_t                idxValue = 0;
    uint32_t               callArgIndex = UINT32_MAX;
    H2CTFEValue            idxConstValue;
    int                    idxIsConst = 0;
    int                    indexStatus;
    if (outStart != NULL) {
        *outStart = 0;
    }
    if (outEnd != NULL) {
        *outEnd = 0;
    }
    if (evalCtx == NULL || outStart == NULL || outEnd == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    binding = (const H2TCCallBinding*)evalCtx->callBinding;
    callArgs = (const H2TCCallArgInfo*)evalCtx->callArgs;

    if (c != NULL && exprNode >= 0 && (uint32_t)exprNode < c->ast->len
        && c->ast->nodes[exprNode].kind == H2Ast_INDEX
        && (c->ast->nodes[exprNode].flags & H2AstFlag_INDEX_SLICE) == 0)
    {
        baseNode = H2AstFirstChild(c->ast, exprNode);
        idxNode = baseNode >= 0 ? H2AstNextSibling(c->ast, baseNode) : -1;
        extraNode = idxNode >= 0 ? H2AstNextSibling(c->ast, idxNode) : -1;
        if (baseNode >= 0 && idxNode >= 0 && extraNode < 0
            && H2TCEvalConstExprNode(evalCtx, idxNode, &idxConstValue, &idxIsConst) != 0)
        {
            return -1;
        }
        if (idxIsConst && H2CTFEValueToInt64(&idxConstValue, &idxValue) != 0) {
            idxIsConst = 0;
        }
    }

    indexStatus = H2TCConstEvalResolveTrackedAnyPackArgIndex(evalCtx, exprNode, &callArgIndex);
    if (indexStatus == 0) {
        if (callArgs == NULL || callArgIndex >= evalCtx->callArgCount) {
            return -1;
        }
        if (callArgs[callArgIndex].spread && idxIsConst && idxValue >= 0) {
            int forwardedStatus = H2TCConstEvalResolveForwardedAnyPackArgSpan(
                evalCtx, callArgs[callArgIndex].exprNode, idxValue, outStart, outEnd);
            if (forwardedStatus <= 0) {
                return forwardedStatus;
            }
        }
        *outStart = callArgs[callArgIndex].start;
        *outEnd = callArgs[callArgIndex].end;
        return 0;
    }
    if (indexStatus < 0 || indexStatus == 2 || indexStatus == 3) {
        return indexStatus;
    }

    if (c == NULL || binding == NULL || callArgs == NULL || evalCtx->callArgCount == 0
        || !binding->isVariadic || exprNode < 0 || (uint32_t)exprNode >= c->ast->len
        || c->ast->nodes[exprNode].kind != H2Ast_INDEX
        || (c->ast->nodes[exprNode].flags & H2AstFlag_INDEX_SLICE) != 0)
    {
        return 1;
    }
    baseNode = H2AstFirstChild(c->ast, exprNode);
    idxNode = baseNode >= 0 ? H2AstNextSibling(c->ast, baseNode) : -1;
    extraNode = idxNode >= 0 ? H2AstNextSibling(c->ast, idxNode) : -1;
    if (baseNode < 0 || idxNode < 0 || extraNode >= 0
        || c->ast->nodes[baseNode].kind != H2Ast_IDENT)
    {
        return 1;
    }
    if (H2TCEvalConstExprNode(evalCtx, idxNode, &idxConstValue, &idxIsConst) != 0) {
        return -1;
    }
    if (!idxIsConst || H2CTFEValueToInt64(&idxConstValue, &idxValue) != 0) {
        return 2;
    }
    if (idxValue < 0) {
        return 3;
    }
    if (!H2TCConstEvalIsTrackedAnyPackName(
            evalCtx, c->ast->nodes[baseNode].dataStart, c->ast->nodes[baseNode].dataEnd)
        && !H2TCConstEvalNodeHasPackType(evalCtx, baseNode))
    {
        return 1;
    }
    if (binding->spreadArgIndex == UINT32_MAX) {
        uint32_t paramIndex = binding->fixedCount;
        uint32_t ordinal = 0;
        uint32_t i;
        for (i = 0; i < evalCtx->callArgCount; i++) {
            if (binding->argParamIndices[i] != (int32_t)paramIndex) {
                continue;
            }
            if ((int64_t)ordinal == idxValue) {
                *outStart = callArgs[i].start;
                *outEnd = callArgs[i].end;
                return 0;
            }
            ordinal++;
        }
        return 3;
    }
    if (binding->spreadArgIndex >= evalCtx->callArgCount) {
        return 1;
    }
    return H2TCConstEvalResolveForwardedAnyPackArgSpan(
        evalCtx, callArgs[binding->spreadArgIndex].exprNode, idxValue, outStart, outEnd);
}

static int H2TCConstEvalGetConcreteCallArgType(
    H2TCConstEvalCtx* evalCtx, uint32_t callArgIndex, int32_t* outType) {
    H2TypeCheckCtx*        c;
    const H2TCCallBinding* binding;
    const H2TCCallArgInfo* callArgs;
    H2TCConstEvalCtx* _Nullable savedActiveEvalCtx;
    int32_t argType = -1;
    if (outType != NULL) {
        *outType = -1;
    }
    if (evalCtx == NULL || outType == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    binding = (const H2TCCallBinding*)evalCtx->callBinding;
    callArgs = (const H2TCCallArgInfo*)evalCtx->callArgs;
    if (c == NULL || binding == NULL || callArgs == NULL || callArgIndex >= evalCtx->callArgCount) {
        return 1;
    }
    if (callArgs[callArgIndex].exprNode >= 0
        && (uint32_t)callArgs[callArgIndex].exprNode < c->ast->len)
    {
        savedActiveEvalCtx = c->activeConstEvalCtx;
        c->activeConstEvalCtx = evalCtx;
        if (H2TCTypeExpr(c, callArgs[callArgIndex].exprNode, &argType) != 0) {
            c->activeConstEvalCtx = savedActiveEvalCtx;
            return -1;
        }
        c->activeConstEvalCtx = savedActiveEvalCtx;
        if ((argType == c->typeUntypedInt || argType == c->typeUntypedFloat)
            && H2TCConcretizeInferredType(c, argType, &argType) != 0)
        {
            return -1;
        }
        if (argType >= 0 && argType != c->typeAnytype) {
            *outType = argType;
            return 0;
        }
    }
    if (H2TCConstEvalGetConcreteCallArgPackType(evalCtx, callArgIndex, &argType) == 0) {
        *outType = argType;
        return 0;
    }
    if (binding->argExpectedTypes[callArgIndex] >= 0) {
        *outType = binding->argExpectedTypes[callArgIndex];
        return 0;
    }
    if (argType >= 0) {
        *outType = argType;
        return 0;
    }
    return 1;
}

static void H2TCConstEvalSetBoolValue(H2CTFEValue* outValue, int value) {
    if (outValue == NULL) {
        return;
    }
    H2TCConstEvalSetNullValue(outValue);
    outValue->kind = H2CTFEValue_BOOL;
    outValue->b = value ? 1u : 0u;
}

static int H2TCConstEvalFindCallArgForParam(
    H2TCConstEvalCtx* evalCtx, int32_t operandNode, uint32_t* outCallArgIndex) {
    H2TypeCheckCtx*        c;
    const H2TCCallBinding* binding;
    const H2TCFunction*    fn;
    int32_t                node;
    uint32_t               paramIndex;
    uint32_t               argIndex;
    int                    packStatus;
    if (outCallArgIndex != NULL) {
        *outCallArgIndex = UINT32_MAX;
    }
    if (evalCtx == NULL || outCallArgIndex == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    binding = (const H2TCCallBinding*)evalCtx->callBinding;
    if (c == NULL || binding == NULL || evalCtx->callArgs == NULL || evalCtx->callArgCount == 0) {
        return 1;
    }

    packStatus = H2TCConstEvalResolveTrackedAnyPackArgIndex(evalCtx, operandNode, outCallArgIndex);
    if (packStatus == 0) {
        return 0;
    }
    if (packStatus < 0) {
        return -1;
    }

    node = operandNode;
    if (node >= 0 && (uint32_t)node < c->ast->len && c->ast->nodes[node].kind == H2Ast_CALL_ARG) {
        int32_t inner = H2AstFirstChild(c->ast, node);
        if (inner >= 0) {
            node = inner;
        }
    }
    if (node < 0 || (uint32_t)node >= c->ast->len || c->ast->nodes[node].kind != H2Ast_IDENT
        || evalCtx->callFnIndex < 0 || (uint32_t)evalCtx->callFnIndex >= c->funcLen)
    {
        return 1;
    }
    fn = &c->funcs[evalCtx->callFnIndex];
    for (paramIndex = 0; paramIndex < fn->paramCount; paramIndex++) {
        if (H2NameEqSlice(
                c->src,
                c->funcParamNameStarts[fn->paramTypeStart + paramIndex],
                c->funcParamNameEnds[fn->paramTypeStart + paramIndex],
                c->ast->nodes[node].dataStart,
                c->ast->nodes[node].dataEnd))
        {
            for (argIndex = 0; argIndex < evalCtx->callArgCount; argIndex++) {
                if (binding->argParamIndices[argIndex] == (int32_t)paramIndex) {
                    *outCallArgIndex = argIndex;
                    return 0;
                }
            }
            return 1;
        }
    }
    return 1;
}

static int H2TCConstEvalReflectIsConstCall(
    H2TCConstEvalCtx* evalCtx, int32_t exprNode, H2CTFEValue* outValue, int* outIsConst) {
    H2TypeCheckCtx*        c;
    const H2TCCallArgInfo* callArgs;
    int32_t                calleeNode;
    const H2AstNode*       callee;
    int32_t                operandNode = -1;
    int32_t                nextNode = -1;
    uint32_t               callArgIndex = UINT32_MAX;
    int                    findStatus;
    H2CTFEValue            ignored;
    int                    operandIsConst = 0;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return -1;
    }
    calleeNode = H2AstFirstChild(c->ast, exprNode);
    callee = calleeNode >= 0 ? &c->ast->nodes[calleeNode] : NULL;
    if (callee == NULL) {
        return 1;
    }
    if (callee->kind == H2Ast_IDENT) {
        if (!H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "is_const")) {
            return 1;
        }
        operandNode = H2AstNextSibling(c->ast, calleeNode);
        nextNode = operandNode >= 0 ? H2AstNextSibling(c->ast, operandNode) : -1;
    } else {
        return 1;
    }
    if (operandNode < 0 || nextNode >= 0) {
        return 1;
    }
    if (c->ast->nodes[operandNode].kind == H2Ast_CALL_ARG) {
        int32_t inner = H2AstFirstChild(c->ast, operandNode);
        if (inner >= 0) {
            operandNode = inner;
        }
    }
    findStatus = H2TCConstEvalFindCallArgForParam(evalCtx, operandNode, &callArgIndex);
    if (findStatus < 0) {
        return -1;
    }
    if (findStatus == 0) {
        callArgs = (const H2TCCallArgInfo*)evalCtx->callArgs;
        if (callArgIndex >= evalCtx->callArgCount) {
            return -1;
        }
        if (H2TCEvalConstExprNode(
                evalCtx, callArgs[callArgIndex].exprNode, &ignored, &operandIsConst)
            != 0)
        {
            return -1;
        }
    } else if (H2TCEvalConstExprNode(evalCtx, operandNode, &ignored, &operandIsConst) != 0) {
        return -1;
    }
    H2TCConstEvalSetBoolValue(outValue, operandIsConst);
    *outIsConst = 1;
    return 0;
}

int H2TCConstEvalSourceLocationOfCall(
    H2TCConstEvalCtx* evalCtx, int32_t exprNode, H2CTFEValue* outValue, int* outIsConst) {
    H2TypeCheckCtx*  c;
    int32_t          calleeNode;
    const H2AstNode* callee;
    int32_t          operandNode = -1;
    int32_t          nextNode = -1;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return -1;
    }
    calleeNode = H2AstFirstChild(c->ast, exprNode);
    callee = calleeNode >= 0 ? &c->ast->nodes[calleeNode] : NULL;
    if (callee == NULL) {
        return 1;
    }
    if (callee->kind == H2Ast_IDENT) {
        if (!H2TCIsSourceLocationOfName(c, callee->dataStart, callee->dataEnd)) {
            return 1;
        }
        operandNode = H2AstNextSibling(c->ast, calleeNode);
        nextNode = operandNode >= 0 ? H2AstNextSibling(c->ast, operandNode) : -1;
        if (operandNode < 0 || nextNode >= 0) {
            return 1;
        }
    } else if (callee->kind == H2Ast_FIELD_EXPR) {
        int32_t recvNode = H2AstFirstChild(c->ast, calleeNode);
        if (recvNode < 0 || c->ast->nodes[recvNode].kind != H2Ast_IDENT
            || !H2NameEqLiteral(
                c->src,
                c->ast->nodes[recvNode].dataStart,
                c->ast->nodes[recvNode].dataEnd,
                "builtin")
            || !H2TCIsSourceLocationOfName(c, callee->dataStart, callee->dataEnd))
        {
            return 1;
        }
        operandNode = H2AstNextSibling(c->ast, calleeNode);
        nextNode = operandNode >= 0 ? H2AstNextSibling(c->ast, operandNode) : -1;
        if (operandNode < 0 || nextNode >= 0) {
            return 1;
        }
    } else {
        return 1;
    }
    if (c->ast->nodes[operandNode].kind == H2Ast_CALL_ARG) {
        int32_t inner = H2AstFirstChild(c->ast, operandNode);
        if (inner < 0) {
            return 1;
        }
        operandNode = inner;
    }
    {
        uint32_t start = 0;
        uint32_t end = 0;
        int      packStatus = H2TCConstEvalResolveTrackedAnyPackArgSpan(
            evalCtx, operandNode, &start, &end);
        if (packStatus < 0) {
            return -1;
        }
        if (packStatus == 0) {
            H2TCConstEvalSetSourceLocationFromOffsets(c, start, end, outValue);
            *outIsConst = 1;
            return 0;
        }
        if (packStatus == 2 || packStatus == 3) {
            *outIsConst = 0;
            return 0;
        }
    }
    if ((c->ast->nodes[operandNode].kind == H2Ast_IDENT
         || c->ast->nodes[operandNode].kind == H2Ast_TYPE_NAME)
        && H2NameHasPrefix(
            c->src,
            c->ast->nodes[operandNode].dataStart,
            c->ast->nodes[operandNode].dataEnd,
            "__hop_"))
    {
        H2TCConstSetReasonNode(
            evalCtx, operandNode, "source_location_of operand cannot reference __hop_ names");
        *outIsConst = 0;
        return 0;
    }
    H2TCConstEvalSetSourceLocationFromOffsets(
        c, c->ast->nodes[operandNode].start, c->ast->nodes[operandNode].end, outValue);
    *outIsConst = 1;
    return 0;
}

int H2TCConstEvalU32Arg(
    H2TCConstEvalCtx* evalCtx, int32_t exprNode, uint32_t* outValue, int* outIsConst) {
    H2CTFEValue v;
    int         isConst = 0;
    if (outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    if (H2TCEvalConstExprNode(evalCtx, exprNode, &v, &isConst) != 0) {
        return -1;
    }
    if (!isConst || v.kind != H2CTFEValue_INT || v.i64 < 0 || v.i64 > (int64_t)UINT32_MAX) {
        *outIsConst = 0;
        return 0;
    }
    *outValue = (uint32_t)v.i64;
    *outIsConst = 1;
    return 0;
}

int H2TCConstEvalSourceLocationCompound(
    H2TCConstEvalCtx* evalCtx,
    int32_t           exprNode,
    int               forceSourceLocation,
    H2CTFEValue*      outValue,
    int*              outIsConst) {
    H2TypeCheckCtx* c;
    int32_t         child;
    int32_t         fieldNode;
    int32_t         resolvedType = -1;
    uint32_t        startLine = 0;
    uint32_t        startColumn = 0;
    uint32_t        endLine = 0;
    uint32_t        endColumn = 0;
    const uint8_t*  fileBytes = (const uint8_t*)"";
    uint32_t        fileLen = 0;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len
        || c->ast->nodes[exprNode].kind != H2Ast_COMPOUND_LIT)
    {
        return 0;
    }
    child = H2AstFirstChild(c->ast, exprNode);
    fieldNode = child;
    if (child >= 0 && H2TCIsTypeNodeKind(c->ast->nodes[child].kind)) {
        if (H2TCResolveTypeNode(c, child, &resolvedType) != 0) {
            return -1;
        }
        if (!H2TCTypeIsSourceLocation(c, resolvedType)) {
            return 0;
        }
        fieldNode = H2AstNextSibling(c->ast, child);
    } else if (!forceSourceLocation) {
        return 0;
    }

    H2TCOffsetToLineCol(
        c->src.ptr, c->src.len, c->ast->nodes[exprNode].start, &startLine, &startColumn);
    H2TCOffsetToLineCol(c->src.ptr, c->src.len, c->ast->nodes[exprNode].end, &endLine, &endColumn);

    while (fieldNode >= 0) {
        const H2AstNode* field = &c->ast->nodes[fieldNode];
        int32_t          valueNode = H2AstFirstChild(c->ast, fieldNode);
        if (field->kind != H2Ast_COMPOUND_FIELD || valueNode < 0) {
            goto non_const;
        }
        if (H2NameEqLiteral(c->src, field->dataStart, field->dataEnd, "file")) {
            H2CTFEValue fileValue;
            int         fileIsConst = 0;
            if (H2TCEvalConstExprNode(evalCtx, valueNode, &fileValue, &fileIsConst) != 0) {
                return -1;
            }
            if (!fileIsConst || fileValue.kind != H2CTFEValue_STRING) {
                goto non_const;
            }
            fileBytes = fileValue.s.bytes;
            fileLen = fileValue.s.len;
        } else if (H2NameEqLiteral(c->src, field->dataStart, field->dataEnd, "start_line")) {
            int isConst = 0;
            if (H2TCConstEvalU32Arg(evalCtx, valueNode, &startLine, &isConst) != 0) {
                return -1;
            }
            if (!isConst) {
                goto non_const;
            }
        } else if (H2NameEqLiteral(c->src, field->dataStart, field->dataEnd, "start_column")) {
            int isConst = 0;
            if (H2TCConstEvalU32Arg(evalCtx, valueNode, &startColumn, &isConst) != 0) {
                return -1;
            }
            if (!isConst) {
                goto non_const;
            }
        } else if (H2NameEqLiteral(c->src, field->dataStart, field->dataEnd, "end_line")) {
            int isConst = 0;
            if (H2TCConstEvalU32Arg(evalCtx, valueNode, &endLine, &isConst) != 0) {
                return -1;
            }
            if (!isConst) {
                goto non_const;
            }
        } else if (H2NameEqLiteral(c->src, field->dataStart, field->dataEnd, "end_column")) {
            int isConst = 0;
            if (H2TCConstEvalU32Arg(evalCtx, valueNode, &endColumn, &isConst) != 0) {
                return -1;
            }
            if (!isConst) {
                goto non_const;
            }
        } else {
            goto non_const;
        }
        fieldNode = H2AstNextSibling(c->ast, fieldNode);
    }

    H2TCConstEvalSetNullValue(outValue);
    outValue->kind = H2CTFEValue_SPAN;
    outValue->span.fileBytes = fileBytes;
    outValue->span.fileLen = fileLen;
    outValue->span.startLine = startLine;
    outValue->span.startColumn = startColumn;
    outValue->span.endLine = endLine;
    outValue->span.endColumn = endColumn;
    *outIsConst = 1;
    return 1;

non_const:
    H2TCConstSetReasonNode(
        evalCtx, exprNode, "builtin.SourceLocation literal is not const-evaluable");
    H2TCConstEvalSetNullValue(outValue);
    *outIsConst = 0;
    return 1;
}

int H2TCConstEvalSourceLocationExpr(
    H2TCConstEvalCtx* evalCtx, int32_t exprNode, H2CTFESpan* outSpan, int* outIsConst) {
    H2TypeCheckCtx* c;
    H2CTFEValue     v;
    int             isConst = 0;
    int             handled;
    if (evalCtx == NULL || outSpan == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL) {
        return -1;
    }
    if (exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        *outIsConst = 0;
        return 0;
    }
    if (c->ast->nodes[exprNode].kind == H2Ast_CALL_ARG) {
        int32_t inner = H2AstFirstChild(c->ast, exprNode);
        if (inner < 0) {
            *outIsConst = 0;
            return 0;
        }
        exprNode = inner;
    }
    if (c->ast->nodes[exprNode].kind == H2Ast_COMPOUND_LIT) {
        handled = H2TCConstEvalSourceLocationCompound(evalCtx, exprNode, 1, &v, &isConst);
        if (handled < 0) {
            return -1;
        }
        if (handled > 0) {
            if (!isConst || v.kind != H2CTFEValue_SPAN) {
                *outIsConst = 0;
                return 0;
            }
            *outSpan = v.span;
            *outIsConst = 1;
            return 0;
        }
    }
    if (H2TCEvalConstExprNode(evalCtx, exprNode, &v, &isConst) != 0) {
        return -1;
    }
    if (!isConst || v.kind != H2CTFEValue_SPAN) {
        *outIsConst = 0;
        return 0;
    }
    *outSpan = v.span;
    *outIsConst = 1;
    return 0;
}

static H2TCCompilerDiagOp H2TCConstEvalCompilerDiagOpFromFieldExpr(
    H2TypeCheckCtx* c, const H2AstNode* fieldExpr) {
    H2TCCompilerDiagOp op;
    uint32_t           segStart;
    uint32_t           i;
    if (c == NULL || fieldExpr == NULL) {
        return H2TCCompilerDiagOp_NONE;
    }
    op = H2TCCompilerDiagOpFromName(c, fieldExpr->dataStart, fieldExpr->dataEnd);
    if (op != H2TCCompilerDiagOp_NONE) {
        return op;
    }
    if (fieldExpr->dataEnd <= fieldExpr->dataStart || fieldExpr->dataEnd > c->src.len) {
        return H2TCCompilerDiagOp_NONE;
    }
    segStart = fieldExpr->dataStart;
    for (i = fieldExpr->dataStart; i < fieldExpr->dataEnd; i++) {
        if (c->src.ptr[i] == '.') {
            segStart = i + 1u;
        }
    }
    if (H2NameEqLiteral(c->src, segStart, fieldExpr->dataEnd, "error")) {
        return H2TCCompilerDiagOp_ERROR;
    }
    if (H2NameEqLiteral(c->src, segStart, fieldExpr->dataEnd, "error_at")) {
        return H2TCCompilerDiagOp_ERROR_AT;
    }
    if (H2NameEqLiteral(c->src, segStart, fieldExpr->dataEnd, "warn")) {
        return H2TCCompilerDiagOp_WARN;
    }
    if (H2NameEqLiteral(c->src, segStart, fieldExpr->dataEnd, "warn_at")) {
        return H2TCCompilerDiagOp_WARN_AT;
    }
    return H2TCCompilerDiagOp_NONE;
}

int H2TCConstEvalCompilerDiagCall(
    H2TCConstEvalCtx* evalCtx, int32_t exprNode, H2CTFEValue* outValue, int* outIsConst) {
    H2TypeCheckCtx*    c;
    int32_t            calleeNode;
    const H2AstNode*   callee;
    H2TCCompilerDiagOp op = H2TCCompilerDiagOp_NONE;
    int32_t            msgNode = -1;
    int32_t            spanNode = -1;
    int32_t            nextNode;
    H2CTFEValue        msgValue;
    int                msgIsConst = 0;
    uint32_t           diagStart = 0;
    uint32_t           diagEnd = 0;
    const char*        detail;
    H2Diag             emitted;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return -1;
    }
    calleeNode = H2AstFirstChild(c->ast, exprNode);
    callee = calleeNode >= 0 ? &c->ast->nodes[calleeNode] : NULL;
    if (callee == NULL) {
        return 1;
    }
    if (callee->kind == H2Ast_IDENT) {
        op = H2TCCompilerDiagOpFromName(c, callee->dataStart, callee->dataEnd);
    } else if (callee->kind == H2Ast_FIELD_EXPR) {
        int32_t recvNode = H2AstFirstChild(c->ast, calleeNode);
        if (recvNode >= 0 && c->ast->nodes[recvNode].kind == H2Ast_IDENT
            && H2NameEqLiteral(
                c->src,
                c->ast->nodes[recvNode].dataStart,
                c->ast->nodes[recvNode].dataEnd,
                "compiler"))
        {
            op = H2TCConstEvalCompilerDiagOpFromFieldExpr(c, callee);
        }
    }
    if (op == H2TCCompilerDiagOp_NONE) {
        return 1;
    }

    if (op == H2TCCompilerDiagOp_ERROR || op == H2TCCompilerDiagOp_WARN) {
        msgNode = H2AstNextSibling(c->ast, calleeNode);
        nextNode = msgNode >= 0 ? H2AstNextSibling(c->ast, msgNode) : -1;
        if (msgNode < 0 || nextNode >= 0) {
            return 1;
        }
        diagStart = c->ast->nodes[exprNode].start;
        diagEnd = c->ast->nodes[exprNode].end;
    } else {
        int        spanIsConst = 0;
        H2CTFESpan span;
        uint32_t   spanStartOffset = 0;
        uint32_t   spanEndOffset = 0;
        spanNode = H2AstNextSibling(c->ast, calleeNode);
        msgNode = spanNode >= 0 ? H2AstNextSibling(c->ast, spanNode) : -1;
        nextNode = msgNode >= 0 ? H2AstNextSibling(c->ast, msgNode) : -1;
        if (spanNode < 0 || msgNode < 0 || nextNode >= 0) {
            return 1;
        }
        if (H2TCConstEvalSourceLocationExpr(evalCtx, spanNode, &span, &spanIsConst) != 0) {
            return -1;
        }
        if (!spanIsConst || span.startLine == 0 || span.startColumn == 0 || span.endLine == 0
            || span.endColumn == 0
            || H2TCLineColToOffset(
                   c->src.ptr, c->src.len, span.startLine, span.startColumn, &spanStartOffset)
                   != 0
            || H2TCLineColToOffset(
                   c->src.ptr, c->src.len, span.endLine, span.endColumn, &spanEndOffset)
                   != 0
            || spanEndOffset < spanStartOffset)
        {
            return H2TCFailNode(c, spanNode, H2Diag_CONSTEVAL_DIAG_INVALID_SPAN);
        }
        diagStart = spanStartOffset;
        diagEnd = spanEndOffset;
    }
    if (msgNode >= 0 && (uint32_t)msgNode < c->ast->len
        && c->ast->nodes[msgNode].kind == H2Ast_CALL_ARG)
    {
        int32_t inner = H2AstFirstChild(c->ast, msgNode);
        if (inner >= 0) {
            msgNode = inner;
        }
    }

    if (H2TCEvalConstExprNode(evalCtx, msgNode, &msgValue, &msgIsConst) != 0) {
        return -1;
    }
    if (!msgIsConst || msgValue.kind != H2CTFEValue_STRING) {
        return H2TCFailNode(c, msgNode, H2Diag_CONSTEVAL_DIAG_MESSAGE_NOT_CONST_STRING);
    }
    detail = H2TCAllocCStringBytes(c, msgValue.s.bytes, msgValue.s.len);
    if (detail == NULL) {
        return H2TCFailNode(c, msgNode, H2Diag_ARENA_OOM);
    }

    emitted = (H2Diag){
        .code = (op == H2TCCompilerDiagOp_WARN || op == H2TCCompilerDiagOp_WARN_AT)
                  ? H2Diag_CONSTEVAL_DIAG_WARNING
                  : H2Diag_CONSTEVAL_DIAG_ERROR,
        .type = (op == H2TCCompilerDiagOp_WARN || op == H2TCCompilerDiagOp_WARN_AT)
                  ? H2DiagType_WARNING
                  : H2DiagType_ERROR,
        .start = diagStart,
        .end = diagEnd,
        .argStart = 0,
        .argEnd = 0,
        .detail = detail,
        .hintOverride = NULL,
    };
    H2TCMarkConstDiagUseExecuted(c, exprNode);
    if (emitted.type == H2DiagType_WARNING) {
        if (H2TCEmitWarningDiag(c, &emitted) != 0) {
            return -1;
        }
        H2TCConstEvalSetNullValue(outValue);
        *outIsConst = 1;
        return 0;
    }

    if (c->diag != NULL) {
        *c->diag = emitted;
    }
    return -1;
}

int H2TCConstEvalTypeReflectionCall(
    H2TCConstEvalCtx* evalCtx, int32_t exprNode, H2CTFEValue* outValue, int* outIsConst) {
    H2TypeCheckCtx*  c;
    int32_t          calleeNode;
    const H2AstNode* callee;
    int32_t          op = 0;
    int32_t          operandNode = -1;
    int32_t          operandNode2 = -1;
    H2CTFEValue      operandValue;
    H2CTFEValue      operandValue2;
    int              operandIsConst = 0;
    int              operandIsConst2 = 0;
    int32_t          reflectedTypeId;
    enum {
        H2TCReflectKind_KIND = 1,
        H2TCReflectKind_BASE = 2,
        H2TCReflectKind_IS_ALIAS = 3,
        H2TCReflectKind_TYPE_NAME = 4,
        H2TCReflectKind_PTR = 5,
        H2TCReflectKind_SLICE = 6,
        H2TCReflectKind_ARRAY = 7,
    };
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return -1;
    }
    calleeNode = H2AstFirstChild(c->ast, exprNode);
    callee = calleeNode >= 0 ? &c->ast->nodes[calleeNode] : NULL;
    if (callee == NULL) {
        return 1;
    }
    if (callee->kind == H2Ast_IDENT) {
        int32_t argNode = H2AstNextSibling(c->ast, calleeNode);
        int32_t nextNode = argNode >= 0 ? H2AstNextSibling(c->ast, argNode) : -1;
        if (H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "kind")) {
            op = H2TCReflectKind_KIND;
        } else if (H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "base")) {
            op = H2TCReflectKind_BASE;
        } else if (H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "is_alias")) {
            op = H2TCReflectKind_IS_ALIAS;
        } else if (H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "type_name")) {
            op = H2TCReflectKind_TYPE_NAME;
        } else if (H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "ptr")) {
            op = H2TCReflectKind_PTR;
        } else if (H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "slice")) {
            op = H2TCReflectKind_SLICE;
        } else if (H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "array")) {
            op = H2TCReflectKind_ARRAY;
        } else {
            return 1;
        }
        if (op == H2TCReflectKind_ARRAY) {
            int32_t extraNode = nextNode >= 0 ? H2AstNextSibling(c->ast, nextNode) : -1;
            if (argNode < 0 || nextNode < 0 || extraNode >= 0) {
                return 1;
            }
            operandNode = argNode;
            operandNode2 = nextNode;
        } else {
            if (argNode < 0 || nextNode >= 0) {
                return 1;
            }
            operandNode = argNode;
        }
    } else if (callee->kind == H2Ast_FIELD_EXPR) {
        int32_t recvNode = H2AstFirstChild(c->ast, calleeNode);
        int32_t nextArgNode = H2AstNextSibling(c->ast, calleeNode);
        if (H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "kind")) {
            op = H2TCReflectKind_KIND;
        } else if (H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "base")) {
            op = H2TCReflectKind_BASE;
        } else if (H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "is_alias")) {
            op = H2TCReflectKind_IS_ALIAS;
        } else if (H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "type_name")) {
            op = H2TCReflectKind_TYPE_NAME;
        } else {
            return 1;
        }
        if (recvNode < 0 || nextArgNode >= 0) {
            return 1;
        }
        operandNode = recvNode;
    } else {
        return 1;
    }

    if (c->ast->nodes[operandNode].kind == H2Ast_CALL_ARG) {
        int32_t innerNode = H2AstFirstChild(c->ast, operandNode);
        if (innerNode < 0) {
            return 1;
        }
        operandNode = innerNode;
    }
    if (operandNode2 >= 0 && c->ast->nodes[operandNode2].kind == H2Ast_CALL_ARG) {
        int32_t innerNode = H2AstFirstChild(c->ast, operandNode2);
        if (innerNode < 0) {
            return 1;
        }
        operandNode2 = innerNode;
    }

    if (H2TCEvalConstExprNode(evalCtx, operandNode, &operandValue, &operandIsConst) != 0) {
        return -1;
    }
    if (!operandIsConst || operandValue.kind != H2CTFEValue_TYPE) {
        return 1;
    }

    if (op == H2TCReflectKind_KIND) {
        outValue->kind = H2CTFEValue_INT;
        outValue->i64 = (int64_t)((operandValue.typeTag >> 56u) & 0xffu);
        outValue->f64 = 0.0;
        outValue->b = 0;
        outValue->typeTag = 0;
        outValue->s.bytes = NULL;
        outValue->s.len = 0;
        outValue->span.fileBytes = NULL;
        outValue->span.fileLen = 0;
        outValue->span.startLine = 0;
        outValue->span.startColumn = 0;
        outValue->span.endLine = 0;
        outValue->span.endColumn = 0;
        *outIsConst = 1;
        return 0;
    }
    if (op == H2TCReflectKind_IS_ALIAS) {
        outValue->kind = H2CTFEValue_BOOL;
        outValue->b = ((operandValue.typeTag >> 56u) & 0xffu) == (uint64_t)H2TCTypeTagKind_ALIAS;
        outValue->i64 = 0;
        outValue->f64 = 0.0;
        outValue->typeTag = 0;
        outValue->s.bytes = NULL;
        outValue->s.len = 0;
        outValue->span.fileBytes = NULL;
        outValue->span.fileLen = 0;
        outValue->span.startLine = 0;
        outValue->span.startColumn = 0;
        outValue->span.endLine = 0;
        outValue->span.endColumn = 0;
        *outIsConst = 1;
        return 0;
    }

    if (H2TCDecodeTypeTag(c, operandValue.typeTag, &reflectedTypeId) != 0) {
        return 1;
    }
    if (op == H2TCReflectKind_PTR || op == H2TCReflectKind_SLICE || op == H2TCReflectKind_ARRAY) {
        int32_t constructedTypeId = -1;
        if (op == H2TCReflectKind_PTR) {
            constructedTypeId = H2TCInternPtrType(c, reflectedTypeId, callee->start, callee->end);
        } else if (op == H2TCReflectKind_SLICE) {
            constructedTypeId = H2TCInternSliceType(
                c, reflectedTypeId, 0, callee->start, callee->end);
        } else {
            int64_t arrayLen = 0;
            if (operandNode2 < 0) {
                return 1;
            }
            if (H2TCEvalConstExprNode(evalCtx, operandNode2, &operandValue2, &operandIsConst2) != 0)
            {
                return -1;
            }
            if (!operandIsConst2 || H2CTFEValueToInt64(&operandValue2, &arrayLen) != 0
                || arrayLen < 0 || arrayLen > (int64_t)UINT32_MAX)
            {
                return 1;
            }
            constructedTypeId = H2TCInternArrayType(
                c, reflectedTypeId, (uint32_t)arrayLen, callee->start, callee->end);
        }
        if (constructedTypeId < 0) {
            return -1;
        }
        H2TCConstEvalSetTypeValue(c, constructedTypeId, outValue);
        *outIsConst = 1;
        return 0;
    }
    if (op == H2TCReflectKind_TYPE_NAME) {
        return H2TCConstEvalTypeNameValue(c, reflectedTypeId, outValue, outIsConst);
    }
    if (reflectedTypeId < 0 || (uint32_t)reflectedTypeId >= c->typeLen
        || c->types[reflectedTypeId].kind != H2TCType_ALIAS)
    {
        H2TCConstSetReasonNode(evalCtx, operandNode, "base() requires an alias type");
        *outIsConst = 0;
        return 0;
    }
    if (H2TCResolveAliasTypeId(c, reflectedTypeId) != 0) {
        return -1;
    }
    reflectedTypeId = c->types[reflectedTypeId].baseType;
    H2TCConstEvalSetTypeValue(c, reflectedTypeId, outValue);
    *outIsConst = 1;
    return 0;
}

static int H2TCConstEvalTypeReflectionByArgs(
    H2TCConstEvalCtx*  evalCtx,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    const H2CTFEValue* args,
    uint32_t           argCount,
    H2CTFEValue*       outValue,
    int*               outIsConst) {
    H2TypeCheckCtx* c;
    int32_t         op = 0;
    int32_t         reflectedTypeId = -1;
    enum {
        H2TCReflectArg_KIND = 1,
        H2TCReflectArg_BASE = 2,
        H2TCReflectArg_IS_ALIAS = 3,
        H2TCReflectArg_TYPE_NAME = 4,
        H2TCReflectArg_PTR = 5,
        H2TCReflectArg_SLICE = 6,
        H2TCReflectArg_ARRAY = 7,
    };
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL) {
        return -1;
    }
    if (H2NameEqLiteral(c->src, nameStart, nameEnd, "kind")) {
        op = H2TCReflectArg_KIND;
    } else if (H2NameEqLiteral(c->src, nameStart, nameEnd, "base")) {
        op = H2TCReflectArg_BASE;
    } else if (H2NameEqLiteral(c->src, nameStart, nameEnd, "is_alias")) {
        op = H2TCReflectArg_IS_ALIAS;
    } else if (H2NameEqLiteral(c->src, nameStart, nameEnd, "type_name")) {
        op = H2TCReflectArg_TYPE_NAME;
    } else if (H2NameEqLiteral(c->src, nameStart, nameEnd, "ptr")) {
        op = H2TCReflectArg_PTR;
    } else if (H2NameEqLiteral(c->src, nameStart, nameEnd, "slice")) {
        op = H2TCReflectArg_SLICE;
    } else if (H2NameEqLiteral(c->src, nameStart, nameEnd, "array")) {
        op = H2TCReflectArg_ARRAY;
    } else {
        return 1;
    }
    if (op == H2TCReflectArg_ARRAY) {
        int64_t arrayLen = 0;
        int32_t arrayTypeId;
        if (argCount != 2u || args == NULL || args[0].kind != H2CTFEValue_TYPE
            || H2CTFEValueToInt64(&args[1], &arrayLen) != 0 || arrayLen < 0
            || arrayLen > (int64_t)UINT32_MAX)
        {
            return 1;
        }
        if (H2TCDecodeTypeTag(c, args[0].typeTag, &reflectedTypeId) != 0) {
            return -1;
        }
        arrayTypeId = H2TCInternArrayType(c, reflectedTypeId, (uint32_t)arrayLen, 0, 0);
        if (arrayTypeId < 0) {
            return -1;
        }
        H2TCConstEvalSetTypeValue(c, arrayTypeId, outValue);
        *outIsConst = 1;
        return 0;
    }
    if (argCount != 1u || args == NULL || args[0].kind != H2CTFEValue_TYPE) {
        return 1;
    }
    if (op == H2TCReflectArg_KIND) {
        outValue->kind = H2CTFEValue_INT;
        outValue->i64 = (int64_t)((args[0].typeTag >> 56u) & 0xffu);
        outValue->f64 = 0.0;
        outValue->b = 0;
        outValue->typeTag = 0;
        outValue->s.bytes = NULL;
        outValue->s.len = 0;
        outValue->span.fileBytes = NULL;
        outValue->span.fileLen = 0;
        outValue->span.startLine = 0;
        outValue->span.startColumn = 0;
        outValue->span.endLine = 0;
        outValue->span.endColumn = 0;
        *outIsConst = 1;
        return 0;
    }
    if (op == H2TCReflectArg_IS_ALIAS) {
        outValue->kind = H2CTFEValue_BOOL;
        outValue->b = ((args[0].typeTag >> 56u) & 0xffu) == (uint64_t)H2TCTypeTagKind_ALIAS;
        outValue->i64 = 0;
        outValue->f64 = 0.0;
        outValue->typeTag = 0;
        outValue->s.bytes = NULL;
        outValue->s.len = 0;
        outValue->span.fileBytes = NULL;
        outValue->span.fileLen = 0;
        outValue->span.startLine = 0;
        outValue->span.startColumn = 0;
        outValue->span.endLine = 0;
        outValue->span.endColumn = 0;
        *outIsConst = 1;
        return 0;
    }
    if (H2TCDecodeTypeTag(c, args[0].typeTag, &reflectedTypeId) != 0) {
        return -1;
    }
    if (op == H2TCReflectArg_PTR) {
        int32_t ptrTypeId = H2TCInternPtrType(c, reflectedTypeId, 0, 0);
        if (ptrTypeId < 0) {
            return -1;
        }
        H2TCConstEvalSetTypeValue(c, ptrTypeId, outValue);
        *outIsConst = 1;
        return 0;
    }
    if (op == H2TCReflectArg_SLICE) {
        int32_t sliceTypeId = H2TCInternSliceType(c, reflectedTypeId, 0, 0, 0);
        if (sliceTypeId < 0) {
            return -1;
        }
        H2TCConstEvalSetTypeValue(c, sliceTypeId, outValue);
        *outIsConst = 1;
        return 0;
    }
    if (op == H2TCReflectArg_TYPE_NAME) {
        return H2TCConstEvalTypeNameValue(c, reflectedTypeId, outValue, outIsConst);
    }
    if (reflectedTypeId < 0 || (uint32_t)reflectedTypeId >= c->typeLen
        || c->types[reflectedTypeId].kind != H2TCType_ALIAS)
    {
        H2TCConstSetReason(evalCtx, nameStart, nameEnd, "base() requires an alias type");
        *outIsConst = 0;
        return 0;
    }
    if (H2TCResolveAliasTypeId(c, reflectedTypeId) != 0) {
        return -1;
    }
    H2TCConstEvalSetTypeValue(c, c->types[reflectedTypeId].baseType, outValue);
    *outIsConst = 1;
    return 0;
}

static int H2TCConstEvalPkgFunctionValueExpr(
    H2TCConstEvalCtx* evalCtx, int32_t exprNode, H2CTFEValue* outValue, int* outIsConst) {
    H2TypeCheckCtx*  c;
    const H2AstNode* n;
    int32_t          recvNode;
    const H2AstNode* recv;
    int32_t          fnIndex;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return -1;
    }
    n = &c->ast->nodes[exprNode];
    if (n->kind != H2Ast_FIELD_EXPR) {
        return 1;
    }
    recvNode = H2AstFirstChild(c->ast, exprNode);
    if (recvNode < 0 || (uint32_t)recvNode >= c->ast->len) {
        return 1;
    }
    recv = &c->ast->nodes[recvNode];
    if (recv->kind != H2Ast_IDENT) {
        return 1;
    }
    if (evalCtx->execCtx != NULL) {
        int32_t execType = -1;
        if (H2TCConstLookupExecBindingType(evalCtx, recv->dataStart, recv->dataEnd, &execType)) {
            return 1;
        }
    }
    if (H2TCLocalFind(c, recv->dataStart, recv->dataEnd) >= 0
        || H2TCFindFunctionIndex(c, recv->dataStart, recv->dataEnd) >= 0)
    {
        return 1;
    }
    fnIndex = H2TCFindPkgQualifiedFunctionValueIndex(
        c, recv->dataStart, recv->dataEnd, n->dataStart, n->dataEnd);
    if (fnIndex < 0) {
        return 1;
    }
    H2MirValueSetFunctionRef(outValue, (uint32_t)fnIndex);
    *outIsConst = 1;
    return 0;
}

int H2TCEvalConstExprNode(
    H2TCConstEvalCtx* evalCtx, int32_t exprNode, H2CTFEValue* outValue, int* outIsConst) {
    H2TypeCheckCtx* c;
    H2AstKind       kind;
    int             rc;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL) {
        return -1;
    }
    if (exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return -1;
    }
    kind = c->ast->nodes[exprNode].kind;
    if (kind == H2Ast_BINARY) {
        const H2AstNode* n = &c->ast->nodes[exprNode];
        if ((H2TokenKind)n->op == H2Tok_EQ || (H2TokenKind)n->op == H2Tok_NEQ) {
            int32_t lhsNode = H2AstFirstChild(c->ast, exprNode);
            int32_t rhsNode = lhsNode >= 0 ? H2AstNextSibling(c->ast, lhsNode) : -1;
            int32_t extraNode = rhsNode >= 0 ? H2AstNextSibling(c->ast, rhsNode) : -1;
            int32_t lhsTypeId = -1;
            int32_t rhsTypeId = -1;
            int     lhsStatus;
            int     rhsStatus;
            if (lhsNode >= 0 && rhsNode >= 0 && extraNode < 0) {
                lhsStatus = H2TCResolveReflectedTypeValueExpr(c, lhsNode, &lhsTypeId);
                if (lhsStatus < 0) {
                    return -1;
                }
                rhsStatus = H2TCResolveReflectedTypeValueExpr(c, rhsNode, &rhsTypeId);
                if (rhsStatus < 0) {
                    return -1;
                }
                if (lhsStatus == 0 && rhsStatus == 0) {
                    outValue->kind = H2CTFEValue_BOOL;
                    outValue->i64 = 0;
                    outValue->f64 = 0.0;
                    outValue->b =
                        (((H2TokenKind)n->op == H2Tok_EQ)
                             ? (lhsTypeId == rhsTypeId)
                             : (lhsTypeId != rhsTypeId))
                            ? 1
                            : 0;
                    outValue->typeTag = 0;
                    outValue->s.bytes = NULL;
                    outValue->s.len = 0;
                    outValue->span.fileBytes = NULL;
                    outValue->span.fileLen = 0;
                    outValue->span.startLine = 0;
                    outValue->span.startColumn = 0;
                    outValue->span.endLine = 0;
                    outValue->span.endColumn = 0;
                    *outIsConst = 1;
                    return 0;
                }
            }
        }
        {
            int32_t     lhsNode = H2AstFirstChild(c->ast, exprNode);
            int32_t     rhsNode = lhsNode >= 0 ? H2AstNextSibling(c->ast, lhsNode) : -1;
            int32_t     extraNode = rhsNode >= 0 ? H2AstNextSibling(c->ast, rhsNode) : -1;
            H2TokenKind op = (H2TokenKind)n->op;
            H2CTFEValue lhsValue;
            H2CTFEValue rhsValue;
            int         lhsIsConst = 0;
            int         rhsIsConst = 0;
            if (lhsNode < 0 || rhsNode < 0 || extraNode >= 0) {
                return -1;
            }
            if (H2TCEvalConstExprNode(evalCtx, lhsNode, &lhsValue, &lhsIsConst) != 0) {
                return -1;
            }
            if (!lhsIsConst) {
                *outIsConst = 0;
                return 0;
            }
            if (op == H2Tok_LOGICAL_AND || op == H2Tok_LOGICAL_OR) {
                if (lhsValue.kind != H2CTFEValue_BOOL) {
                    H2TCConstSetReasonNode(evalCtx, lhsNode, "expression is not const-evaluable");
                    *outIsConst = 0;
                    return 0;
                }
                if ((op == H2Tok_LOGICAL_AND && lhsValue.b == 0)
                    || (op == H2Tok_LOGICAL_OR && lhsValue.b != 0))
                {
                    *outValue = lhsValue;
                    *outIsConst = 1;
                    return 0;
                }
            }
            if (H2TCEvalConstExprNode(evalCtx, rhsNode, &rhsValue, &rhsIsConst) != 0) {
                return -1;
            }
            if (!rhsIsConst) {
                *outIsConst = 0;
                return 0;
            }
            if (!H2TCConstEvalApplyBinary(c, op, &lhsValue, &rhsValue, outValue)) {
                H2TCConstSetReasonNode(evalCtx, exprNode, "expression is not const-evaluable");
                *outIsConst = 0;
                return 0;
            }
            *outIsConst = 1;
            return 0;
        }
    }
    if (kind == H2Ast_UNARY) {
        const H2AstNode* n = &c->ast->nodes[exprNode];
        int32_t          operandNode = H2AstFirstChild(c->ast, exprNode);
        int32_t          extraNode = operandNode >= 0 ? H2AstNextSibling(c->ast, operandNode) : -1;
        H2CTFEValue      operandValue;
        int              operandIsConst = 0;
        if (operandNode < 0 || extraNode >= 0) {
            return -1;
        }
        if (H2TCEvalConstExprNode(evalCtx, operandNode, &operandValue, &operandIsConst) != 0) {
            return -1;
        }
        if (!operandIsConst) {
            *outIsConst = 0;
            return 0;
        }
        if (!H2TCConstEvalApplyUnary((H2TokenKind)n->op, &operandValue, outValue)) {
            H2TCConstSetReasonNode(evalCtx, exprNode, "expression is not const-evaluable");
            *outIsConst = 0;
            return 0;
        }
        *outIsConst = 1;
        return 0;
    }
    if (kind == H2Ast_SIZEOF) {
        return H2TCConstEvalSizeOf(evalCtx, exprNode, outValue, outIsConst);
    }
    if (kind == H2Ast_COMPOUND_LIT) {
        int locationStatus = H2TCConstEvalSourceLocationCompound(
            evalCtx, exprNode, 0, outValue, outIsConst);
        if (locationStatus < 0) {
            return -1;
        }
        if (locationStatus > 0) {
            return 0;
        }
    }
    if (kind == H2Ast_INDEX) {
        int indexStatus = H2TCConstEvalIndexExpr(evalCtx, exprNode, outValue, outIsConst);
        if (indexStatus == 0) {
            return 0;
        }
        if (indexStatus < 0) {
            return -1;
        }
    }
    if (kind == H2Ast_FIELD_EXPR) {
        int pkgFnStatus = H2TCConstEvalPkgFunctionValueExpr(
            evalCtx, exprNode, outValue, outIsConst);
        if (pkgFnStatus == 0) {
            return 0;
        }
        if (pkgFnStatus < 0) {
            return -1;
        }
    }
    if (kind == H2Ast_CALL) {
        int32_t          calleeNode = H2AstFirstChild(c->ast, exprNode);
        const H2AstNode* callee = calleeNode >= 0 ? &c->ast->nodes[calleeNode] : NULL;
        int              compilerDiagStatus;
        int              directCallStatus;
        int              lenStatus;
        int              sourceLocationStatus;
        int              isConstStatus;
        int              reflectStatus;
        lenStatus = H2TCConstEvalLenCall(evalCtx, exprNode, outValue, outIsConst);
        if (lenStatus == 0) {
            return 0;
        }
        if (lenStatus < 0) {
            return -1;
        }
        compilerDiagStatus = H2TCConstEvalCompilerDiagCall(evalCtx, exprNode, outValue, outIsConst);
        if (compilerDiagStatus == 0) {
            return 0;
        }
        if (compilerDiagStatus < 0) {
            return -1;
        }
        sourceLocationStatus = H2TCConstEvalSourceLocationOfCall(
            evalCtx, exprNode, outValue, outIsConst);
        if (sourceLocationStatus == 0) {
            return 0;
        }
        if (sourceLocationStatus < 0) {
            return -1;
        }
        isConstStatus = H2TCConstEvalReflectIsConstCall(evalCtx, exprNode, outValue, outIsConst);
        if (isConstStatus == 0) {
            return 0;
        }
        if (isConstStatus < 0) {
            return -1;
        }
        if (callee != NULL && callee->kind == H2Ast_IDENT
            && H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "typeof"))
        {
            return H2TCConstEvalTypeOf(evalCtx, exprNode, outValue, outIsConst);
        }
        reflectStatus = H2TCConstEvalTypeReflectionCall(evalCtx, exprNode, outValue, outIsConst);
        if (reflectStatus == 0) {
            return 0;
        }
        if (reflectStatus < 0) {
            return -1;
        }
        directCallStatus = H2TCConstEvalDirectCall(evalCtx, exprNode, outValue, outIsConst);
        if (directCallStatus == 0) {
            return 0;
        }
        if (directCallStatus < 0) {
            return -1;
        }
    }
    if (kind == H2Ast_CAST) {
        return H2TCConstEvalCast(evalCtx, exprNode, outValue, outIsConst);
    }
    rc = H2CTFEEvalExprEx(
        c->arena,
        c->ast,
        c->src,
        exprNode,
        H2TCResolveConstIdent,
        H2TCResolveConstCall,
        evalCtx,
        H2TCMirConstMakeTuple,
        evalCtx,
        H2TCMirConstIndexValue,
        evalCtx,
        H2TCMirConstAggGetField,
        evalCtx,
        H2TCMirConstAggAddrField,
        evalCtx,
        outValue,
        outIsConst,
        c->diag);
    if (rc == 0 && !*outIsConst) {
        H2TCConstSetReasonNode(evalCtx, exprNode, "expression is not const-evaluable");
    }
    return rc;
}

int H2TCEvalConstExecExprCb(void* ctx, int32_t exprNode, H2CTFEValue* outValue, int* outIsConst) {
    return H2TCEvalConstExprNode((H2TCConstEvalCtx*)ctx, exprNode, outValue, outIsConst);
}

int H2TCEvalConstExecResolveTypeCb(void* ctx, int32_t typeNode, int32_t* outTypeId) {
    H2TCConstEvalCtx* evalCtx = (H2TCConstEvalCtx*)ctx;
    uint8_t           savedAllowConstNumericTypeName;
    if (evalCtx == NULL || evalCtx->tc == NULL || outTypeId == NULL) {
        return -1;
    }
    savedAllowConstNumericTypeName = evalCtx->tc->allowConstNumericTypeName;
    evalCtx->tc->allowConstNumericTypeName = 1;
    if (H2TCResolveTypeNode(evalCtx->tc, typeNode, outTypeId) != 0) {
        evalCtx->tc->allowConstNumericTypeName = savedAllowConstNumericTypeName;
        return -1;
    }
    evalCtx->tc->allowConstNumericTypeName = savedAllowConstNumericTypeName;
    return 0;
}

int H2TCEvalConstExecInferValueTypeCb(void* ctx, const H2CTFEValue* value, int32_t* outTypeId) {
    H2TCConstEvalCtx* evalCtx = (H2TCConstEvalCtx*)ctx;
    H2TypeCheckCtx*   c;
    if (evalCtx == NULL || value == NULL || outTypeId == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL) {
        return -1;
    }
    if (value->kind != H2CTFEValue_TYPE && value->typeTag > 0
        && value->typeTag <= (uint64_t)INT32_MAX && (uint32_t)value->typeTag < c->typeLen)
    {
        *outTypeId = (int32_t)value->typeTag;
        return 0;
    }
    switch (value->kind) {
        case H2CTFEValue_INT:   *outTypeId = c->typeUntypedInt; return *outTypeId >= 0 ? 0 : -1;
        case H2CTFEValue_FLOAT: *outTypeId = c->typeUntypedFloat; return *outTypeId >= 0 ? 0 : -1;
        case H2CTFEValue_BOOL:  *outTypeId = c->typeBool; return *outTypeId >= 0 ? 0 : -1;
        case H2CTFEValue_STRING:
            *outTypeId = H2TCGetStrRefType(c, 0, 0);
            return *outTypeId >= 0 ? 0 : -1;
        case H2CTFEValue_TYPE: *outTypeId = c->typeType; return *outTypeId >= 0 ? 0 : -1;
        case H2CTFEValue_SPAN:
            if (c->typeSourceLocation < 0) {
                c->typeSourceLocation = H2TCFindSourceLocationType(c);
            }
            *outTypeId = c->typeSourceLocation;
            return *outTypeId >= 0 ? 0 : -1;
        case H2CTFEValue_NULL: *outTypeId = c->typeNull; return *outTypeId >= 0 ? 0 : -1;
        case H2CTFEValue_OPTIONAL:
            if (value->typeTag > 0 && value->typeTag <= (uint64_t)INT32_MAX
                && (uint32_t)value->typeTag < c->typeLen)
            {
                *outTypeId = (int32_t)value->typeTag;
                return 0;
            }
            return -1;
        default: return -1;
    }
}

int H2TCEvalConstExecInferExprTypeCb(void* ctx, int32_t exprNode, int32_t* outTypeId) {
    H2TCConstEvalCtx* evalCtx = (H2TCConstEvalCtx*)ctx;
    if (evalCtx == NULL || evalCtx->tc == NULL || outTypeId == NULL) {
        return -1;
    }
    return H2TCTypeExpr(evalCtx->tc, exprNode, outTypeId);
}

int H2TCEvalConstExecIsOptionalTypeCb(
    void* ctx, int32_t typeId, int32_t* outPayloadTypeId, int* outIsOptional) {
    H2TCConstEvalCtx* evalCtx = (H2TCConstEvalCtx*)ctx;
    H2TypeCheckCtx*   c;
    int32_t           baseTypeId;
    if (evalCtx == NULL || evalCtx->tc == NULL || outIsOptional == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    baseTypeId = H2TCResolveAliasBaseType(c, typeId);
    if (baseTypeId < 0 || (uint32_t)baseTypeId >= c->typeLen) {
        return -1;
    }
    *outIsOptional = c->types[baseTypeId].kind == H2TCType_OPTIONAL;
    if (outPayloadTypeId != NULL) {
        *outPayloadTypeId = *outIsOptional ? c->types[baseTypeId].baseType : -1;
    }
    return 0;
}

static int H2TCMirConstResolveTypeRefTypeId(
    H2TCConstEvalCtx* evalCtx, const H2MirTypeRef* typeRef, int32_t* outTypeId) {
    H2TypeCheckCtx* c;
    uint8_t         savedAllowConstNumericTypeName;
    uint8_t         savedAllowAnytypeParamType;
    if (outTypeId != NULL) {
        *outTypeId = -1;
    }
    if (evalCtx == NULL || typeRef == NULL || outTypeId == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || typeRef->astNode > INT32_MAX || typeRef->astNode >= c->ast->len) {
        return -1;
    }
    savedAllowConstNumericTypeName = c->allowConstNumericTypeName;
    savedAllowAnytypeParamType = c->allowAnytypeParamType;
    c->allowConstNumericTypeName = 1;
    c->allowAnytypeParamType = 1;
    if (H2TCResolveTypeNode(c, (int32_t)typeRef->astNode, outTypeId) != 0) {
        c->allowConstNumericTypeName = savedAllowConstNumericTypeName;
        c->allowAnytypeParamType = savedAllowAnytypeParamType;
        return -1;
    }
    c->allowConstNumericTypeName = savedAllowConstNumericTypeName;
    c->allowAnytypeParamType = savedAllowAnytypeParamType;
    return 0;
}

typedef struct {
    uint32_t    len;
    uint32_t    _reserved;
    H2CTFEValue elems[];
} H2TCMirConstTuple;

typedef struct {
    int32_t     typeId;
    uint32_t    fieldCount;
    H2CTFEValue fields[];
} H2TCMirConstAggregate;

enum {
    H2_TC_MIR_CONST_ITER_KIND_SEQUENCE = 1u,
    H2_TC_MIR_CONST_ITER_KIND_PROTOCOL = 2u,
};

typedef struct {
    uint32_t    index;
    uint16_t    flags;
    uint8_t     kind;
    uint8_t     _reserved;
    int32_t     iterFnIndex;
    int32_t     nextFnIndex;
    uint8_t     usePair;
    uint8_t     _reserved2[3];
    H2CTFEValue sourceValue;
    H2CTFEValue iteratorValue;
    H2CTFEValue currentValue;
} H2TCMirConstIter;

static const H2TCMirConstTuple* _Nullable H2TCMirConstTupleFromValue(const H2CTFEValue* value) {
    if (value == NULL || value->kind != H2CTFEValue_ARRAY || value->typeTag != H2_TC_MIR_TUPLE_TAG
        || value->s.bytes == NULL)
    {
        return NULL;
    }
    return (const H2TCMirConstTuple*)value->s.bytes;
}

static int H2TCConstEvalGetConcreteCallArgPackType(
    H2TCConstEvalCtx* evalCtx, uint32_t callArgIndex, int32_t* outPackType) {
    H2TypeCheckCtx*          c;
    const H2TCCallArgInfo*   callArgs;
    H2CTFEValue              argValue;
    const H2CTFEValue*       sourceValue;
    const H2TCMirConstTuple* tuple;
    int32_t                  elemTypes[H2TC_MAX_CALL_ARGS];
    uint32_t                 i;
    int                      argIsConst = 0;
    if (outPackType != NULL) {
        *outPackType = -1;
    }
    if (evalCtx == NULL || outPackType == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    callArgs = (const H2TCCallArgInfo*)evalCtx->callArgs;
    if (c == NULL || callArgs == NULL || callArgIndex >= evalCtx->callArgCount
        || callArgs[callArgIndex].exprNode < 0
        || (uint32_t)callArgs[callArgIndex].exprNode >= c->ast->len)
    {
        return 1;
    }
    if (H2TCEvalConstExprNode(evalCtx, callArgs[callArgIndex].exprNode, &argValue, &argIsConst)
        != 0)
    {
        return -1;
    }
    if (!argIsConst) {
        return 1;
    }
    sourceValue = &argValue;
    if (sourceValue->kind == H2CTFEValue_REFERENCE && sourceValue->s.bytes != NULL) {
        sourceValue = (const H2CTFEValue*)sourceValue->s.bytes;
    }
    tuple = H2TCMirConstTupleFromValue(sourceValue);
    if (tuple == NULL || tuple->len > H2TC_MAX_CALL_ARGS) {
        return 1;
    }
    for (i = 0; i < tuple->len; i++) {
        if (H2TCEvalConstExecInferValueTypeCb(evalCtx, &tuple->elems[i], &elemTypes[i]) != 0) {
            return 1;
        }
        if ((elemTypes[i] == c->typeUntypedInt || elemTypes[i] == c->typeUntypedFloat)
            && H2TCConcretizeInferredType(c, elemTypes[i], &elemTypes[i]) != 0)
        {
            return -1;
        }
    }
    *outPackType = H2TCInternPackType(c, elemTypes, tuple->len, 0, 0);
    return *outPackType >= 0 ? 0 : -1;
}

static H2TCMirConstIter* _Nullable H2TCMirConstIterFromValue(const H2CTFEValue* value) {
    const H2CTFEValue* target = value;
    if (value == NULL) {
        return NULL;
    }
    if (value->kind == H2CTFEValue_REFERENCE && value->s.bytes != NULL) {
        target = (const H2CTFEValue*)value->s.bytes;
    }
    if (target == NULL || target->kind != H2CTFEValue_SPAN || target->typeTag != H2_TC_MIR_ITER_TAG
        || target->s.bytes == NULL)
    {
        return NULL;
    }
    return (H2TCMirConstIter*)target->s.bytes;
}

static const H2TCMirConstAggregate* _Nullable H2TCMirConstAggregateFromValue(
    const H2CTFEValue* value) {
    const H2CTFEValue* target = value;
    if (value == NULL) {
        return NULL;
    }
    if (value->kind == H2CTFEValue_REFERENCE && value->s.bytes != NULL) {
        target = (const H2CTFEValue*)value->s.bytes;
    }
    if (target == NULL || target->kind != H2CTFEValue_AGGREGATE || target->s.bytes == NULL) {
        return NULL;
    }
    return (const H2TCMirConstAggregate*)target->s.bytes;
}

static H2CTFEValue* _Nullable H2TCMirConstAggregateFieldValuePtr(
    const H2TCMirConstAggregate* agg, uint32_t fieldIndex) {
    if (agg == NULL || fieldIndex >= agg->fieldCount) {
        return NULL;
    }
    return (H2CTFEValue*)&agg->fields[fieldIndex];
}

static const H2CTFEValue* H2TCMirConstValueTargetOrSelf(const H2CTFEValue* value) {
    if (value != NULL && value->kind == H2CTFEValue_REFERENCE && value->s.bytes != NULL) {
        return (const H2CTFEValue*)value->s.bytes;
    }
    return value;
}

static void H2TCMirConstSetReference(H2CTFEValue* outValue, H2CTFEValue* target) {
    if (outValue == NULL) {
        return;
    }
    H2TCConstEvalValueInvalid(outValue);
    if (target == NULL) {
        return;
    }
    outValue->kind = H2CTFEValue_REFERENCE;
    outValue->s.bytes = (const uint8_t*)target;
    outValue->s.len = 0;
}

static int H2TCMirConstOptionalPayload(const H2CTFEValue* value, const H2CTFEValue** outPayload) {
    if (outPayload != NULL) {
        *outPayload = NULL;
    }
    if (value == NULL || value->kind != H2CTFEValue_OPTIONAL) {
        return 0;
    }
    if (value->b == 0u || value->s.bytes == NULL) {
        return 1;
    }
    if (outPayload != NULL) {
        *outPayload = (const H2CTFEValue*)value->s.bytes;
    }
    return 1;
}

static void H2TCMirConstAdaptForInValueBinding(
    const H2CTFEValue* inValue, int valueRef, H2CTFEValue* outValue) {
    const H2CTFEValue* target;
    if (outValue == NULL) {
        return;
    }
    H2TCConstEvalValueInvalid(outValue);
    if (inValue == NULL) {
        return;
    }
    if (valueRef) {
        *outValue = *inValue;
        return;
    }
    target = H2TCMirConstValueTargetOrSelf(inValue);
    *outValue = target != NULL ? *target : *inValue;
}

static int H2TCMirConstFindExecBindingTypeId(
    H2TCConstEvalCtx* evalCtx, int32_t sourceNode, int32_t* outTypeId) {
    H2TypeCheckCtx*  c;
    const H2AstNode* node;
    H2CTFEExecCtx*   execCtx;
    H2CTFEExecEnv*   env;
    uint8_t          savedAllowConstNumericTypeName;
    if (outTypeId != NULL) {
        *outTypeId = -1;
    }
    if (evalCtx == NULL || outTypeId == NULL || evalCtx->tc == NULL || evalCtx->execCtx == NULL) {
        return 0;
    }
    c = evalCtx->tc;
    execCtx = evalCtx->execCtx;
    if (sourceNode < 0 || (uint32_t)sourceNode >= c->ast->len) {
        return 0;
    }
    node = &c->ast->nodes[sourceNode];
    if (node->kind != H2Ast_IDENT) {
        return 0;
    }
    for (env = execCtx->env; env != NULL; env = env->parent) {
        uint32_t i;
        for (i = 0; i < env->bindingLen; i++) {
            H2CTFEExecBinding* binding = &env->bindings[i];
            if (binding->nameStart != node->dataStart || binding->nameEnd != node->dataEnd) {
                continue;
            }
            if (binding->typeId >= 0) {
                *outTypeId = binding->typeId;
                return 1;
            }
            if (binding->typeNode < 0) {
                return 0;
            }
            savedAllowConstNumericTypeName = c->allowConstNumericTypeName;
            c->allowConstNumericTypeName = 1;
            if (H2TCResolveTypeNode(c, binding->typeNode, outTypeId) != 0) {
                c->allowConstNumericTypeName = savedAllowConstNumericTypeName;
                return -1;
            }
            c->allowConstNumericTypeName = savedAllowConstNumericTypeName;
            return 1;
        }
    }
    return 0;
}

int H2TCMirConstMakeTuple(
    void* _Nullable ctx,
    const H2CTFEValue* elems,
    uint32_t           elemCount,
    uint32_t           typeNodeHint,
    H2CTFEValue*       outValue,
    int*               outIsConst,
    H2Diag* _Nullable diag) {
    H2TCConstEvalCtx*  evalCtx = (H2TCConstEvalCtx*)ctx;
    H2TypeCheckCtx*    c;
    H2TCMirConstTuple* tuple;
    size_t             bytes;
    (void)typeNodeHint;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (outValue != NULL) {
        H2TCConstEvalValueInvalid(outValue);
    }
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL) {
        return -1;
    }
    bytes = sizeof(*tuple) + sizeof(H2CTFEValue) * (size_t)elemCount;
    tuple = (H2TCMirConstTuple*)H2ArenaAlloc(
        c->arena, (uint32_t)bytes, (uint32_t)_Alignof(H2TCMirConstTuple));
    if (tuple == NULL) {
        return -1;
    }
    tuple->len = elemCount;
    tuple->_reserved = 0u;
    if (elemCount != 0u && elems != NULL) {
        memcpy(tuple->elems, elems, sizeof(H2CTFEValue) * elemCount);
    }
    outValue->kind = H2CTFEValue_ARRAY;
    outValue->i64 = 0;
    outValue->f64 = 0.0;
    outValue->b = 0u;
    outValue->typeTag = H2_TC_MIR_TUPLE_TAG;
    outValue->s.bytes = (const uint8_t*)tuple;
    outValue->s.len = elemCount;
    outValue->span = (H2CTFESpan){ 0 };
    *outIsConst = 1;
    return 0;
}

int H2TCMirConstIndexValue(
    void* _Nullable ctx,
    const H2CTFEValue* base,
    const H2CTFEValue* index,
    H2CTFEValue*       outValue,
    int*               outIsConst,
    H2Diag* _Nullable diag) {
    const H2TCMirConstTuple* tuple;
    int64_t                  indexValue = 0;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (base == NULL || index == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    tuple = H2TCMirConstTupleFromValue(base);
    if (tuple == NULL || H2CTFEValueToInt64(index, &indexValue) != 0 || indexValue < 0
        || (uint64_t)indexValue >= (uint64_t)tuple->len)
    {
        H2TCConstEvalCtx*      evalCtx = (H2TCConstEvalCtx*)ctx;
        H2TypeCheckCtx*        c;
        const H2TCCallBinding* binding;
        const H2TCCallArgInfo* callArgs;
        int32_t                typeId = -1;
        int32_t                baseTypeId;
        uint32_t               paramIndex;
        uint32_t               ordinal = 0;
        uint32_t               callArgIndex = UINT32_MAX;
        uint32_t               i;
        if (evalCtx == NULL || base->kind != H2CTFEValue_TYPE) {
            return 0;
        }
        c = evalCtx->tc;
        binding = (const H2TCCallBinding*)evalCtx->callBinding;
        callArgs = (const H2TCCallArgInfo*)evalCtx->callArgs;
        if (c == NULL || binding == NULL || callArgs == NULL || evalCtx->callArgCount == 0
            || evalCtx->callPackParamNameStart >= evalCtx->callPackParamNameEnd
            || !binding->isVariadic || binding->spreadArgIndex != UINT32_MAX
            || H2TCDecodeTypeTag(c, base->typeTag, &typeId) != 0)
        {
            return 0;
        }
        baseTypeId = H2TCResolveAliasBaseType(c, typeId);
        if (baseTypeId < 0 || (uint32_t)baseTypeId >= c->typeLen
            || c->types[baseTypeId].kind != H2TCType_PACK)
        {
            return 0;
        }
        paramIndex = binding->fixedCount;
        for (i = 0; i < evalCtx->callArgCount; i++) {
            if (binding->argParamIndices[i] != (int32_t)paramIndex) {
                continue;
            }
            if ((int64_t)ordinal == indexValue) {
                callArgIndex = i;
                break;
            }
            ordinal++;
        }
        if (callArgIndex == UINT32_MAX) {
            return 0;
        }
        if (H2TCEvalConstExprNode(evalCtx, callArgs[callArgIndex].exprNode, outValue, outIsConst)
            != 0)
        {
            return -1;
        }
        if (*outIsConst) {
            int32_t elemType = -1;
            if (H2TCConstEvalGetConcreteCallArgType(evalCtx, callArgIndex, &elemType) == 0) {
                switch (outValue->kind) {
                    case H2CTFEValue_INT:
                    case H2CTFEValue_FLOAT:
                    case H2CTFEValue_BOOL:
                    case H2CTFEValue_STRING:
                        outValue->typeTag = (uint64_t)(uint32_t)elemType;
                        break;
                    default: break;
                }
            }
        }
        return 0;
    }
    *outValue = tuple->elems[(uint32_t)indexValue];
    *outIsConst = 1;
    return 0;
}

int H2TCMirConstSequenceLen(
    void* _Nullable ctx,
    const H2CTFEValue* base,
    H2CTFEValue*       outValue,
    int*               outIsConst,
    H2Diag* _Nullable diag) {
    H2TCConstEvalCtx*        evalCtx = (H2TCConstEvalCtx*)ctx;
    H2TypeCheckCtx*          c;
    const H2CTFEValue*       value = base;
    const H2TCMirConstTuple* tuple;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (base == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx != NULL ? evalCtx->tc : NULL;
    if (base->kind == H2CTFEValue_REFERENCE && base->s.bytes != NULL) {
        value = (const H2CTFEValue*)base->s.bytes;
    }
    if (value->kind == H2CTFEValue_STRING) {
        outValue->kind = H2CTFEValue_INT;
        outValue->i64 = (int64_t)value->s.len;
        *outIsConst = 1;
        return 0;
    }
    tuple = H2TCMirConstTupleFromValue(value);
    if (tuple != NULL) {
        outValue->kind = H2CTFEValue_INT;
        outValue->i64 = (int64_t)tuple->len;
        *outIsConst = 1;
        return 0;
    }
    if (c != NULL && value->kind == H2CTFEValue_TYPE) {
        int32_t typeId = -1;
        int32_t baseTypeId;
        if (H2TCDecodeTypeTag(c, value->typeTag, &typeId) == 0) {
            baseTypeId = H2TCResolveAliasBaseType(c, typeId);
            if (baseTypeId >= 0 && (uint32_t)baseTypeId < c->typeLen) {
                const H2TCType* t = &c->types[baseTypeId];
                if (t->kind == H2TCType_PACK) {
                    outValue->kind = H2CTFEValue_INT;
                    outValue->i64 = (int64_t)t->fieldCount;
                    *outIsConst = 1;
                    return 0;
                }
                if (t->kind == H2TCType_ARRAY) {
                    outValue->kind = H2CTFEValue_INT;
                    outValue->i64 = (int64_t)t->arrayLen;
                    *outIsConst = 1;
                    return 0;
                }
            }
        }
    }
    return 0;
}

int H2TCMirConstIterInit(
    void* _Nullable ctx,
    uint32_t           sourceNode,
    const H2CTFEValue* source,
    uint16_t           flags,
    H2CTFEValue*       outIter,
    int*               outIsConst,
    H2Diag* _Nullable diag) {
    H2TCConstEvalCtx*        evalCtx = (H2TCConstEvalCtx*)ctx;
    H2TypeCheckCtx*          c;
    const H2CTFEValue*       sourceValue = source;
    const H2TCMirConstTuple* tuple;
    H2TCMirConstIter*        iter;
    H2CTFEValue*             target;
    int32_t                  sourceType = -1;
    int32_t                  iterType = -1;
    int32_t                  iterPtrType = -1;
    int32_t                  iterFn = -1;
    int32_t                  nextValueFn = -1;
    int32_t                  nextKeyFn = -1;
    int32_t                  nextPairFn = -1;
    int32_t                  valueType = -1;
    int32_t                  keyType = -1;
    const H2AstNode*         sourceAstNode = NULL;
    int                      hasKey;
    int                      valueDiscard;
    int                      valueRef;
    int                      rc;
    int                      iterIsConst = 0;
    H2CTFEValue              iterValue;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (evalCtx == NULL || evalCtx->tc == NULL || source == NULL || outIter == NULL
        || outIsConst == NULL)
    {
        return -1;
    }
    c = evalCtx->tc;
    if ((flags & H2MirIterFlag_KEY_REF) != 0u || (flags & H2MirIterFlag_VALUE_REF) != 0u) {
        if ((flags & H2MirIterFlag_VALUE_REF) == 0u) {
            return 0;
        }
    }
    if (source->kind == H2CTFEValue_REFERENCE && source->s.bytes != NULL) {
        sourceValue = (const H2CTFEValue*)source->s.bytes;
    }
    tuple = H2TCMirConstTupleFromValue(sourceValue);
    iter = (H2TCMirConstIter*)H2ArenaAlloc(
        c->arena, sizeof(*iter), (uint32_t)_Alignof(H2TCMirConstIter));
    target = (H2CTFEValue*)H2ArenaAlloc(c->arena, sizeof(*target), (uint32_t)_Alignof(H2CTFEValue));
    if (iter == NULL || target == NULL) {
        return -1;
    }
    memset(iter, 0, sizeof(*iter));
    iter->flags = flags;
    iter->sourceValue = *source;
    iter->iterFnIndex = -1;
    iter->nextFnIndex = -1;
    hasKey = (flags & H2MirIterFlag_HAS_KEY) != 0u;
    valueDiscard = (flags & H2MirIterFlag_VALUE_DISCARD) != 0u;
    valueRef = (flags & H2MirIterFlag_VALUE_REF) != 0u;
    if (sourceValue->kind == H2CTFEValue_STRING || tuple != NULL) {
        iter->kind = H2_TC_MIR_CONST_ITER_KIND_SEQUENCE;
    } else {
        if (sourceNode < c->ast->len) {
            sourceAstNode = &c->ast->nodes[sourceNode];
        }
        rc = H2TCMirConstFindExecBindingTypeId(evalCtx, (int32_t)sourceNode, &sourceType);
        if (rc < 0) {
            return -1;
        }
        if (rc == 0 && H2TCTypeExpr(c, (int32_t)sourceNode, &sourceType) != 0) {
            return -1;
        }
        rc = H2TCResolveForInIterator(c, (int32_t)sourceNode, sourceType, &iterFn, &iterType);
        if (rc != 0) {
            return 0;
        }
        iterPtrType = H2TCInternPtrType(
            c,
            iterType,
            sourceAstNode != NULL ? sourceAstNode->start : 0u,
            sourceAstNode != NULL ? sourceAstNode->end : 0u);
        if (iterPtrType < 0) {
            return -1;
        }
        if (hasKey && valueDiscard) {
            rc = H2TCResolveForInNextKey(c, iterPtrType, &keyType, &nextKeyFn);
            if (rc == 1 || rc == 2) {
                rc = H2TCResolveForInNextKeyAndValue(
                    c, iterPtrType, H2TCForInValueMode_ANY, &keyType, &valueType, &nextPairFn);
            }
            if (rc != 0) {
                return 0;
            }
            iter->usePair = nextPairFn >= 0 ? 1u : 0u;
            iter->nextFnIndex = nextPairFn >= 0 ? nextPairFn : nextKeyFn;
        } else if (hasKey) {
            rc = H2TCResolveForInNextKeyAndValue(
                c,
                iterPtrType,
                valueDiscard ? H2TCForInValueMode_ANY
                             : (valueRef ? H2TCForInValueMode_REF : H2TCForInValueMode_VALUE),
                &keyType,
                &valueType,
                &nextPairFn);
            if (rc != 0) {
                return 0;
            }
            iter->usePair = 1u;
            iter->nextFnIndex = nextPairFn;
        } else {
            rc = H2TCResolveForInNextValue(
                c,
                iterPtrType,
                valueDiscard ? H2TCForInValueMode_ANY
                             : (valueRef ? H2TCForInValueMode_REF : H2TCForInValueMode_VALUE),
                &valueType,
                &nextValueFn);
            if (rc == 1 || rc == 2) {
                rc = H2TCResolveForInNextKeyAndValue(
                    c,
                    iterPtrType,
                    valueDiscard ? H2TCForInValueMode_ANY
                                 : (valueRef ? H2TCForInValueMode_REF : H2TCForInValueMode_VALUE),
                    &keyType,
                    &valueType,
                    &nextPairFn);
            }
            if (rc != 0) {
                return 0;
            }
            iter->usePair = nextPairFn >= 0 ? 1u : 0u;
            iter->nextFnIndex = nextPairFn >= 0 ? nextPairFn : nextValueFn;
        }
        if (iter->nextFnIndex < 0) {
            return 0;
        }
        H2TCConstEvalValueInvalid(&iterValue);
        if (H2TCInvokeConstFunctionByIndex(
                evalCtx,
                c->funcs[iterFn].nameStart,
                c->funcs[iterFn].nameEnd,
                iterFn,
                source,
                1u,
                NULL,
                0u,
                NULL,
                0u,
                0u,
                &iterValue,
                &iterIsConst)
            != 0)
        {
            return -1;
        }
        if (!iterIsConst) {
            return 0;
        }
        iter->kind = H2_TC_MIR_CONST_ITER_KIND_PROTOCOL;
        iter->iterFnIndex = iterFn;
        iter->iteratorValue = iterValue;
    }
    target->kind = H2CTFEValue_SPAN;
    target->typeTag = H2_TC_MIR_ITER_TAG;
    target->s.bytes = (const uint8_t*)iter;
    target->s.len = 0;
    target->span = (H2CTFESpan){ 0 };
    outIter->kind = H2CTFEValue_REFERENCE;
    outIter->s.bytes = (const uint8_t*)target;
    outIter->s.len = 0;
    outIter->typeTag = 0;
    outIter->span = (H2CTFESpan){ 0 };
    *outIsConst = 1;
    return 0;
}

int H2TCMirConstIterNext(
    void* _Nullable ctx,
    const H2CTFEValue* iterValue,
    uint16_t           flags,
    int*               outHasItem,
    H2CTFEValue*       outKey,
    int*               outKeyIsConst,
    H2CTFEValue*       outValue,
    int*               outValueIsConst,
    H2Diag* _Nullable diag) {
    H2TCConstEvalCtx*        evalCtx = (H2TCConstEvalCtx*)ctx;
    H2TypeCheckCtx*          c;
    H2TCMirConstIter*        iter;
    const H2CTFEValue*       sourceValue;
    const H2TCMirConstTuple* tuple;
    const H2CTFEValue*       payload = NULL;
    H2CTFEValue              callResult;
    H2CTFEValue              iterRef;
    const H2CTFEValue*       pairValue;
    const H2TCMirConstTuple* pairTuple;
    int                      isConst = 0;
    (void)diag;
    if (outHasItem != NULL) {
        *outHasItem = 0;
    }
    if (outKeyIsConst != NULL) {
        *outKeyIsConst = 0;
    }
    if (outValueIsConst != NULL) {
        *outValueIsConst = 0;
    }
    if (evalCtx == NULL || iterValue == NULL || outHasItem == NULL || outKey == NULL
        || outKeyIsConst == NULL || outValue == NULL || outValueIsConst == NULL)
    {
        return -1;
    }
    c = evalCtx->tc;
    if ((flags & H2MirIterFlag_KEY_REF) != 0u) {
        return 0;
    }
    iter = H2TCMirConstIterFromValue(iterValue);
    if (iter == NULL) {
        return 0;
    }
    if (iter->kind == H2_TC_MIR_CONST_ITER_KIND_PROTOCOL) {
        if (c == NULL || iter->nextFnIndex < 0 || (uint32_t)iter->nextFnIndex >= c->funcLen) {
            return 0;
        }
        H2TCConstEvalValueInvalid(&callResult);
        H2TCMirConstSetReference(&iterRef, &iter->iteratorValue);
        if (H2TCInvokeConstFunctionByIndex(
                evalCtx,
                c->funcs[iter->nextFnIndex].nameStart,
                c->funcs[iter->nextFnIndex].nameEnd,
                iter->nextFnIndex,
                &iterRef,
                1u,
                NULL,
                0u,
                NULL,
                0u,
                0u,
                &callResult,
                &isConst)
            != 0)
        {
            return -1;
        }
        if (!isConst) {
            return 0;
        }
        if (callResult.kind == H2CTFEValue_NULL) {
            *outKeyIsConst = 1;
            *outValueIsConst = 1;
            return 0;
        }
        if (callResult.kind == H2CTFEValue_OPTIONAL) {
            if (!H2TCMirConstOptionalPayload(&callResult, &payload)) {
                return 0;
            }
            if (callResult.b == 0u || payload == NULL) {
                *outKeyIsConst = 1;
                *outValueIsConst = 1;
                return 0;
            }
        } else {
            payload = &callResult;
        }
        if (iter->usePair) {
            pairValue = H2TCMirConstValueTargetOrSelf(payload);
            pairTuple = H2TCMirConstTupleFromValue(pairValue);
            if (pairTuple == NULL || pairTuple->len != 2u) {
                return 0;
            }
            if ((flags & H2MirIterFlag_HAS_KEY) != 0u) {
                *outKey = pairTuple->elems[0];
                *outKeyIsConst = 1;
            } else {
                *outKeyIsConst = 1;
            }
            if ((flags & H2MirIterFlag_VALUE_DISCARD) == 0u) {
                H2TCMirConstAdaptForInValueBinding(
                    &pairTuple->elems[1], (flags & H2MirIterFlag_VALUE_REF) != 0u, outValue);
                *outValueIsConst = 1;
            } else {
                *outValueIsConst = 1;
            }
        } else if ((flags & H2MirIterFlag_HAS_KEY) != 0u) {
            *outKey = *payload;
            *outKeyIsConst = 1;
            *outValueIsConst = 1;
        } else {
            H2TCMirConstAdaptForInValueBinding(
                payload, (flags & H2MirIterFlag_VALUE_REF) != 0u, outValue);
            *outKeyIsConst = 1;
            *outValueIsConst = 1;
        }
        *outHasItem = 1;
        return 0;
    }
    sourceValue = &iter->sourceValue;
    if (sourceValue->kind == H2CTFEValue_REFERENCE && sourceValue->s.bytes != NULL) {
        sourceValue = (const H2CTFEValue*)sourceValue->s.bytes;
    }
    tuple = H2TCMirConstTupleFromValue(sourceValue);
    if (sourceValue->kind == H2CTFEValue_STRING) {
        if (iter->index >= sourceValue->s.len) {
            *outKeyIsConst = 1;
            *outValueIsConst = 1;
            return 0;
        }
        if ((flags & H2MirIterFlag_HAS_KEY) != 0u) {
            outKey->kind = H2CTFEValue_INT;
            outKey->i64 = (int64_t)iter->index;
            *outKeyIsConst = 1;
        } else {
            *outKeyIsConst = 1;
        }
        if ((flags & H2MirIterFlag_VALUE_DISCARD) == 0u) {
            iter->currentValue.kind = H2CTFEValue_INT;
            iter->currentValue.i64 = (int64_t)sourceValue->s.bytes[iter->index];
            if ((flags & H2MirIterFlag_VALUE_REF) != 0u) {
                H2TCMirConstSetReference(outValue, &iter->currentValue);
            } else {
                *outValue = iter->currentValue;
            }
            *outValueIsConst = 1;
        } else {
            *outValueIsConst = 1;
        }
        *outHasItem = 1;
        iter->index++;
        return 0;
    }
    if (tuple != NULL) {
        if (iter->index >= tuple->len) {
            *outKeyIsConst = 1;
            *outValueIsConst = 1;
            return 0;
        }
        if ((flags & H2MirIterFlag_HAS_KEY) != 0u) {
            outKey->kind = H2CTFEValue_INT;
            outKey->i64 = (int64_t)iter->index;
            *outKeyIsConst = 1;
        } else {
            *outKeyIsConst = 1;
        }
        if ((flags & H2MirIterFlag_VALUE_DISCARD) == 0u) {
            if ((flags & H2MirIterFlag_VALUE_REF) != 0u) {
                H2TCMirConstSetReference(outValue, (H2CTFEValue*)&tuple->elems[iter->index]);
            } else {
                *outValue = tuple->elems[iter->index];
            }
            *outValueIsConst = 1;
        } else {
            *outValueIsConst = 1;
        }
        *outHasItem = 1;
        iter->index++;
        return 0;
    }
    return 0;
}

int H2TCEvalConstForInIndexCb(
    void* _Nullable ctx,
    H2CTFEExecCtx*     execCtx,
    const H2CTFEValue* sourceValue,
    uint32_t           index,
    int                byRef,
    H2CTFEValue*       outValue,
    int*               outIsConst) {
    const H2CTFEValue*       value = sourceValue;
    const H2TCMirConstTuple* tuple;
    (void)ctx;
    (void)execCtx;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (sourceValue == NULL || outValue == NULL || outIsConst == NULL || byRef) {
        return 0;
    }
    if (sourceValue->kind == H2CTFEValue_REFERENCE && sourceValue->s.bytes != NULL) {
        value = (const H2CTFEValue*)sourceValue->s.bytes;
    }
    tuple = H2TCMirConstTupleFromValue(value);
    if (tuple == NULL || index >= tuple->len) {
        return 0;
    }
    *outValue = tuple->elems[index];
    *outIsConst = 1;
    return 0;
}

static int H2TCMirConstResolveAggregateType(
    H2TypeCheckCtx* c, int32_t typeId, int32_t* outBaseTypeId) {
    int32_t         baseTypeId = -1;
    const H2TCType* t;
    if (outBaseTypeId != NULL) {
        *outBaseTypeId = -1;
    }
    if (c == NULL) {
        return 0;
    }
    baseTypeId = H2TCResolveAliasBaseType(c, typeId);
    if (baseTypeId < 0 || (uint32_t)baseTypeId >= c->typeLen) {
        return 0;
    }
    t = &c->types[baseTypeId];
    if (t->kind == H2TCType_NAMED && t->fieldCount == 0u) {
        int32_t  namedIndex = -1;
        uint32_t i;
        for (i = 0; i < c->namedTypeLen; i++) {
            if (c->namedTypes[i].typeId == baseTypeId) {
                namedIndex = (int32_t)i;
                break;
            }
        }
        if (namedIndex >= 0 && H2TCResolveNamedTypeFields(c, (uint32_t)namedIndex) != 0) {
            return 0;
        }
        t = &c->types[baseTypeId];
    }
    if (t->kind != H2TCType_NAMED && t->kind != H2TCType_ANON_STRUCT) {
        return 0;
    }
    if (outBaseTypeId != NULL) {
        *outBaseTypeId = baseTypeId;
    }
    return 1;
}

static int H2TCMirConstAggregateLookupFieldIndex(
    H2TypeCheckCtx* c,
    int32_t         typeId,
    uint32_t        nameStart,
    uint32_t        nameEnd,
    uint32_t*       outFieldIndex) {
    int32_t  baseTypeId = -1;
    int32_t  fieldType = -1;
    uint32_t absFieldIndex = UINT32_MAX;
    if (outFieldIndex != NULL) {
        *outFieldIndex = UINT32_MAX;
    }
    if (!H2TCMirConstResolveAggregateType(c, typeId, &baseTypeId)) {
        return 0;
    }
    if (H2TCFieldLookup(c, baseTypeId, nameStart, nameEnd, &fieldType, &absFieldIndex) != 0) {
        return 0;
    }
    if (absFieldIndex < c->types[baseTypeId].fieldStart) {
        return 0;
    }
    absFieldIndex -= c->types[baseTypeId].fieldStart;
    if (absFieldIndex >= c->types[baseTypeId].fieldCount) {
        return 0;
    }
    if (outFieldIndex != NULL) {
        *outFieldIndex = absFieldIndex;
    }
    return 1;
}

static int H2TCMirConstZeroInitTypeId(
    H2TCConstEvalCtx* evalCtx, int32_t typeId, H2CTFEValue* outValue, int* outIsConst);

static int H2TCMirConstMakeAggregateValue(
    H2TCConstEvalCtx* evalCtx, int32_t typeId, H2CTFEValue* outValue, int* outIsConst) {
    H2TypeCheckCtx*        c;
    int32_t                baseTypeId = -1;
    uint32_t               fieldCount = 0;
    uint32_t               i;
    size_t                 bytes;
    H2TCMirConstAggregate* agg;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (outValue != NULL) {
        H2TCConstEvalValueInvalid(outValue);
    }
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (!H2TCMirConstResolveAggregateType(c, typeId, &baseTypeId)) {
        return 0;
    }
    fieldCount = c->types[baseTypeId].fieldCount;
    bytes = sizeof(*agg) + sizeof(H2CTFEValue) * (size_t)fieldCount;
    agg = (H2TCMirConstAggregate*)H2ArenaAlloc(
        c->arena, (uint32_t)bytes, (uint32_t)_Alignof(H2TCMirConstAggregate));
    if (agg == NULL) {
        return -1;
    }
    agg->typeId = baseTypeId;
    agg->fieldCount = fieldCount;
    memset(agg->fields, 0, sizeof(H2CTFEValue) * fieldCount);
    for (i = 0; i < fieldCount; i++) {
        uint32_t fieldIndex = c->types[baseTypeId].fieldStart + i;
        if (fieldIndex >= c->fieldLen
            || H2TCMirConstZeroInitTypeId(
                   evalCtx, c->fields[fieldIndex].typeId, &agg->fields[i], outIsConst)
                   != 0)
        {
            return -1;
        }
        if (!*outIsConst) {
            return 0;
        }
    }
    outValue->kind = H2CTFEValue_AGGREGATE;
    outValue->typeTag = (uint64_t)(uint32_t)baseTypeId;
    outValue->s.bytes = (const uint8_t*)agg;
    outValue->s.len = fieldCount;
    *outIsConst = 1;
    return 0;
}

int H2TCMirConstAggGetField(
    void* _Nullable ctx,
    const H2CTFEValue* base,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    H2CTFEValue*       outValue,
    int*               outIsConst,
    H2Diag* _Nullable diag) {
    H2TCConstEvalCtx*            evalCtx = (H2TCConstEvalCtx*)ctx;
    const H2TCMirConstAggregate* agg;
    uint32_t                     fieldIndex = UINT32_MAX;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (outValue != NULL) {
        H2TCConstEvalValueInvalid(outValue);
    }
    if (evalCtx == NULL || evalCtx->tc == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    if (base != NULL && base->kind == H2CTFEValue_SPAN
        && base->typeTag == H2_TC_MIR_IMPORT_ALIAS_TAG)
    {
        H2TypeCheckCtx* c = evalCtx->tc;
        const uint8_t*  srcBytes = c != NULL ? (const uint8_t*)c->src.ptr : NULL;
        const uint8_t*  aliasBytes = base->span.fileBytes;
        uint32_t        aliasLen = base->span.fileLen;
        uint32_t        aliasStart;
        uint32_t        aliasEnd;
        int32_t         fnIndex;
        if (c == NULL || srcBytes == NULL || aliasBytes == NULL || aliasLen == 0u
            || aliasBytes < srcBytes || (uint64_t)(aliasBytes - srcBytes) > UINT32_MAX
            || (uint64_t)(aliasBytes - srcBytes) + aliasLen > c->src.len)
        {
            return 0;
        }
        aliasStart = (uint32_t)(aliasBytes - srcBytes);
        aliasEnd = aliasStart + aliasLen;
        fnIndex = H2TCFindPkgQualifiedFunctionValueIndex(
            c, aliasStart, aliasEnd, nameStart, nameEnd);
        if (fnIndex < 0) {
            return 0;
        }
        H2MirValueSetFunctionRef(outValue, (uint32_t)fnIndex);
        *outIsConst = 1;
        return 0;
    }
    agg = H2TCMirConstAggregateFromValue(base);
    if (agg == NULL
        || !H2TCMirConstAggregateLookupFieldIndex(
            evalCtx->tc, agg->typeId, nameStart, nameEnd, &fieldIndex))
    {
        return 0;
    }
    {
        H2CTFEValue* fieldValue = H2TCMirConstAggregateFieldValuePtr(agg, fieldIndex);
        if (fieldValue == NULL) {
            return 0;
        }
        *outValue = *fieldValue;
    }
    *outIsConst = 1;
    return 0;
}

int H2TCMirConstAggAddrField(
    void* _Nullable ctx,
    const H2CTFEValue* base,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    H2CTFEValue*       outValue,
    int*               outIsConst,
    H2Diag* _Nullable diag) {
    H2TCConstEvalCtx*            evalCtx = (H2TCConstEvalCtx*)ctx;
    const H2TCMirConstAggregate* agg;
    uint32_t                     fieldIndex = UINT32_MAX;
    H2CTFEValue*                 fieldValue;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (outValue != NULL) {
        H2TCConstEvalValueInvalid(outValue);
    }
    if (evalCtx == NULL || evalCtx->tc == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    agg = H2TCMirConstAggregateFromValue(base);
    if (agg == NULL
        || !H2TCMirConstAggregateLookupFieldIndex(
            evalCtx->tc, agg->typeId, nameStart, nameEnd, &fieldIndex))
    {
        return 0;
    }
    fieldValue = H2TCMirConstAggregateFieldValuePtr(agg, fieldIndex);
    if (fieldValue == NULL) {
        return 0;
    }
    H2TCMirConstSetReference(outValue, fieldValue);
    *outIsConst = 1;
    return 0;
}

static int H2TCMirConstZeroInitTypeId(
    H2TCConstEvalCtx* evalCtx, int32_t typeId, H2CTFEValue* outValue, int* outIsConst) {
    H2TypeCheckCtx* c;
    int32_t         baseTypeId;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (outValue != NULL) {
        H2TCConstEvalValueInvalid(outValue);
    }
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL) {
        return -1;
    }
    baseTypeId = H2TCResolveAliasBaseType(c, typeId);
    if (baseTypeId < 0 || (uint32_t)baseTypeId >= c->typeLen) {
        return 0;
    }
    if (H2TCIsIntegerType(c, baseTypeId)) {
        outValue->kind = H2CTFEValue_INT;
        outValue->i64 = 0;
        *outIsConst = 1;
        return 0;
    }
    if (H2TCIsFloatType(c, baseTypeId)) {
        outValue->kind = H2CTFEValue_FLOAT;
        outValue->f64 = 0.0;
        *outIsConst = 1;
        return 0;
    }
    if (H2TCIsBoolType(c, baseTypeId)) {
        outValue->kind = H2CTFEValue_BOOL;
        outValue->b = 0u;
        *outIsConst = 1;
        return 0;
    }
    if (H2TCIsRawptrType(c, baseTypeId)) {
        H2TCConstEvalSetNullValue(outValue);
        outValue->typeTag = (uint64_t)(uint32_t)typeId;
        *outIsConst = 1;
        return 0;
    }
    if (H2TCIsStringLikeType(c, baseTypeId)) {
        outValue->kind = H2CTFEValue_STRING;
        outValue->s.bytes = NULL;
        outValue->s.len = 0;
        outValue->typeTag = (uint64_t)(uint32_t)typeId;
        *outIsConst = 1;
        return 0;
    }
    if (c->types[baseTypeId].kind == H2TCType_OPTIONAL) {
        if (H2TCConstEvalSetOptionalNoneValue(c, baseTypeId, outValue) != 0) {
            return -1;
        }
        *outIsConst = 1;
        return 0;
    }
    if (c->types[baseTypeId].kind == H2TCType_PTR || c->types[baseTypeId].kind == H2TCType_REF
        || c->types[baseTypeId].kind == H2TCType_FUNCTION
        || c->types[baseTypeId].kind == H2TCType_NULL)
    {
        H2TCConstEvalSetNullValue(outValue);
        *outIsConst = 1;
        return 0;
    }
    return H2TCMirConstMakeAggregateValue(evalCtx, baseTypeId, outValue, outIsConst);
}

int H2TCMirConstZeroInitLocal(
    void* _Nullable ctx,
    const H2MirTypeRef* typeRef,
    H2CTFEValue*        outValue,
    int*                outIsConst,
    H2Diag* _Nullable diag) {
    H2TCConstEvalCtx* evalCtx = (H2TCConstEvalCtx*)ctx;
    int32_t           typeId = -1;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (outValue != NULL) {
        H2TCConstEvalValueInvalid(outValue);
    }
    if (evalCtx == NULL || typeRef == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    if (evalCtx->tc == NULL || H2TCMirConstResolveTypeRefTypeId(evalCtx, typeRef, &typeId) != 0) {
        return -1;
    }
    return H2TCMirConstZeroInitTypeId(evalCtx, typeId, outValue, outIsConst);
}

int H2TCMirConstCoerceValueForType(
    void* _Nullable ctx,
    const H2MirTypeRef* typeRef,
    H2CTFEValue*        inOutValue,
    H2Diag* _Nullable diag) {
    H2TCConstEvalCtx* evalCtx = (H2TCConstEvalCtx*)ctx;
    H2TypeCheckCtx*   c;
    int32_t           typeId = -1;
    int32_t           baseTypeId;
    (void)diag;
    if (evalCtx == NULL || typeRef == NULL || inOutValue == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || H2TCMirConstResolveTypeRefTypeId(evalCtx, typeRef, &typeId) != 0) {
        return -1;
    }
    baseTypeId = H2TCResolveAliasBaseType(c, typeId);
    if (baseTypeId < 0 || (uint32_t)baseTypeId >= c->typeLen) {
        return 0;
    }
    if ((c->types[baseTypeId].kind == H2TCType_NAMED
         || c->types[baseTypeId].kind == H2TCType_ANON_STRUCT)
        && inOutValue->kind == H2CTFEValue_AGGREGATE)
    {
        if (inOutValue->typeTag == (uint64_t)(uint32_t)baseTypeId) {
            return 0;
        }
        return 0;
    }
    if (c->types[baseTypeId].kind == H2TCType_OPTIONAL) {
        H2CTFEValue wrapped;
        if (inOutValue->kind == H2CTFEValue_OPTIONAL) {
            if (inOutValue->typeTag > 0 && inOutValue->typeTag <= (uint64_t)INT32_MAX
                && (uint32_t)inOutValue->typeTag < c->typeLen
                && (int32_t)inOutValue->typeTag == baseTypeId)
            {
                return 0;
            }
            if (inOutValue->b == 0u) {
                if (H2TCConstEvalSetOptionalNoneValue(c, baseTypeId, &wrapped) != 0) {
                    return -1;
                }
            } else if (inOutValue->s.bytes == NULL) {
                return 0;
            } else if (
                H2TCConstEvalSetOptionalSomeValue(
                    c, baseTypeId, (const H2CTFEValue*)inOutValue->s.bytes, &wrapped)
                != 0)
            {
                return -1;
            }
            *inOutValue = wrapped;
            return 0;
        }
        if (inOutValue->kind == H2CTFEValue_NULL) {
            if (H2TCConstEvalSetOptionalNoneValue(c, baseTypeId, &wrapped) != 0) {
                return -1;
            }
            *inOutValue = wrapped;
            return 0;
        }
        if (H2TCConstEvalSetOptionalSomeValue(c, baseTypeId, inOutValue, &wrapped) != 0) {
            return -1;
        }
        *inOutValue = wrapped;
        return 0;
    }
    if (H2TCIsIntegerType(c, baseTypeId)) {
        if (inOutValue->kind == H2CTFEValue_BOOL) {
            inOutValue->kind = H2CTFEValue_INT;
            inOutValue->i64 = inOutValue->b ? 1 : 0;
        } else if (inOutValue->kind == H2CTFEValue_FLOAT) {
            inOutValue->kind = H2CTFEValue_INT;
            inOutValue->i64 = (int64_t)inOutValue->f64;
            inOutValue->f64 = 0.0;
        } else if (inOutValue->kind == H2CTFEValue_NULL) {
            inOutValue->kind = H2CTFEValue_INT;
            inOutValue->i64 = 0;
        }
        return 0;
    }
    if (H2TCIsFloatType(c, baseTypeId)) {
        if (inOutValue->kind == H2CTFEValue_INT) {
            inOutValue->kind = H2CTFEValue_FLOAT;
            inOutValue->f64 = (double)inOutValue->i64;
            inOutValue->i64 = 0;
        } else if (inOutValue->kind == H2CTFEValue_BOOL) {
            inOutValue->kind = H2CTFEValue_FLOAT;
            inOutValue->f64 = inOutValue->b ? 1.0 : 0.0;
            inOutValue->i64 = 0;
        } else if (inOutValue->kind == H2CTFEValue_NULL) {
            inOutValue->kind = H2CTFEValue_FLOAT;
            inOutValue->f64 = 0.0;
        }
        return 0;
    }
    if (H2TCIsBoolType(c, baseTypeId)) {
        if (inOutValue->kind == H2CTFEValue_INT) {
            inOutValue->kind = H2CTFEValue_BOOL;
            inOutValue->b = inOutValue->i64 != 0 ? 1u : 0u;
            inOutValue->i64 = 0;
        } else if (inOutValue->kind == H2CTFEValue_FLOAT) {
            inOutValue->kind = H2CTFEValue_BOOL;
            inOutValue->b = inOutValue->f64 != 0.0 ? 1u : 0u;
            inOutValue->f64 = 0.0;
        } else if (inOutValue->kind == H2CTFEValue_NULL) {
            inOutValue->kind = H2CTFEValue_BOOL;
            inOutValue->b = 0u;
        }
        return 0;
    }
    if (H2TCIsStringLikeType(c, typeId)) {
        if (inOutValue->kind == H2CTFEValue_STRING || inOutValue->kind == H2CTFEValue_NULL) {
            inOutValue->typeTag = (uint64_t)(uint32_t)typeId;
        }
        return 0;
    }
    return 0;
}

static const uint32_t H2_TC_MIR_CONST_FN_NONE = UINT32_MAX;

int H2TCMirConstLowerFunction(
    H2TCMirConstLowerCtx* c, int32_t fnIndex, uint32_t* _Nullable outMirFnIndex);

int H2TCMirConstInitLowerCtx(H2TCConstEvalCtx* evalCtx, H2TCMirConstLowerCtx* _Nonnull outCtx) {
    H2TypeCheckCtx* c;
    uint32_t*       tcToMir;
    uint8_t*        loweringFns;
    uint32_t*       topConstToMir;
    uint8_t*        loweringTopConsts;
    uint32_t        i;
    if (outCtx == NULL) {
        return -1;
    }
    memset(outCtx, 0, sizeof(*outCtx));
    if (evalCtx == NULL || evalCtx->tc == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    tcToMir = (uint32_t*)H2ArenaAlloc(
        c->arena, sizeof(uint32_t) * c->funcLen, (uint32_t)_Alignof(uint32_t));
    loweringFns = (uint8_t*)H2ArenaAlloc(
        c->arena, sizeof(uint8_t) * c->funcLen, (uint32_t)_Alignof(uint8_t));
    topConstToMir = (uint32_t*)H2ArenaAlloc(
        c->arena, sizeof(uint32_t) * c->ast->len, (uint32_t)_Alignof(uint32_t));
    loweringTopConsts = (uint8_t*)H2ArenaAlloc(
        c->arena, sizeof(uint8_t) * c->ast->len, (uint32_t)_Alignof(uint8_t));
    if (tcToMir == NULL || loweringFns == NULL || topConstToMir == NULL
        || loweringTopConsts == NULL)
    {
        return -1;
    }
    for (i = 0; i < c->funcLen; i++) {
        tcToMir[i] = H2_TC_MIR_CONST_FN_NONE;
        loweringFns[i] = 0u;
    }
    for (i = 0; i < c->ast->len; i++) {
        topConstToMir[i] = H2_TC_MIR_CONST_FN_NONE;
        loweringTopConsts[i] = 0u;
    }
    H2MirProgramBuilderInit(&outCtx->builder, c->arena);
    outCtx->evalCtx = evalCtx;
    outCtx->tcToMir = tcToMir;
    outCtx->loweringFns = loweringFns;
    outCtx->topConstToMir = topConstToMir;
    outCtx->loweringTopConsts = loweringTopConsts;
    outCtx->diag = c->diag;
    return 0;
}

static int H2TCMirConstGetFunctionBody(
    H2TypeCheckCtx* c, int32_t fnIndex, int32_t* outFnNode, int32_t* outBodyNode) {
    int32_t child;
    int32_t fnNode;
    int32_t bodyNode = -1;
    if (outFnNode != NULL) {
        *outFnNode = -1;
    }
    if (outBodyNode != NULL) {
        *outBodyNode = -1;
    }
    if (c == NULL || outFnNode == NULL || outBodyNode == NULL || fnIndex < 0
        || (uint32_t)fnIndex >= c->funcLen)
    {
        return 0;
    }
    fnNode = c->funcs[(uint32_t)fnIndex].defNode;
    if (fnNode < 0 || (uint32_t)fnNode >= c->ast->len || c->ast->nodes[fnNode].kind != H2Ast_FN) {
        return 0;
    }
    child = H2AstFirstChild(c->ast, fnNode);
    while (child >= 0) {
        if (c->ast->nodes[child].kind == H2Ast_BLOCK) {
            if (bodyNode >= 0) {
                return 0;
            }
            bodyNode = child;
        }
        child = H2AstNextSibling(c->ast, child);
    }
    if (bodyNode < 0) {
        return 0;
    }
    *outFnNode = fnNode;
    *outBodyNode = bodyNode;
    return 1;
}

static int H2TCMirConstMatchPlainCallNode(
    const H2TypeCheckCtx* tc,
    const H2MirSymbolRef* symbol,
    int32_t               rootNode,
    uint32_t              encodedArgCount,
    int32_t* _Nonnull outCallNode,
    int32_t* _Nonnull outCalleeNode) {
    int32_t  stack[256];
    uint32_t stackLen = 0;
    int      found = 0;
    if (outCallNode != NULL) {
        *outCallNode = -1;
    }
    if (outCalleeNode != NULL) {
        *outCalleeNode = -1;
    }
    if (tc == NULL || symbol == NULL || outCallNode == NULL || outCalleeNode == NULL || rootNode < 0
        || (uint32_t)rootNode >= tc->ast->len)
    {
        return 0;
    }
    stack[stackLen++] = rootNode;
    while (stackLen > 0) {
        int32_t          nodeId = stack[--stackLen];
        const H2AstNode* node = &tc->ast->nodes[nodeId];
        int32_t          child;
        if (node->kind == H2Ast_CALL) {
            int32_t          calleeNode = node->firstChild;
            const H2AstNode* callee = calleeNode >= 0 ? &tc->ast->nodes[calleeNode] : NULL;
            if (callee != NULL && callee->kind == H2Ast_IDENT
                && callee->dataStart == symbol->nameStart && callee->dataEnd == symbol->nameEnd
                && H2TCListCount(tc->ast, nodeId)
                       == H2MirCallArgCountFromTok((uint16_t)encodedArgCount))
            {
                if (found) {
                    return 0;
                }
                *outCallNode = nodeId;
                *outCalleeNode = calleeNode;
                found = 1;
            }
        }
        child = node->firstChild;
        while (child >= 0) {
            if (stackLen >= (uint32_t)(sizeof(stack) / sizeof(stack[0]))) {
                return 0;
            }
            stack[stackLen++] = child;
            child = tc->ast->nodes[child].nextSibling;
        }
    }
    return found;
}

static int H2TCMirConstMatchAliasCallNode(
    const H2TypeCheckCtx* tc,
    const H2MirSymbolRef* symbol,
    int32_t               rootNode,
    uint32_t              encodedArgCount,
    int32_t* _Nonnull outCallNode,
    int32_t* _Nonnull outCalleeNode) {
    int32_t  stack[256];
    uint32_t stackLen = 0;
    int      found = 0;
    if (outCallNode != NULL) {
        *outCallNode = -1;
    }
    if (outCalleeNode != NULL) {
        *outCalleeNode = -1;
    }
    if (tc == NULL || symbol == NULL || outCallNode == NULL || outCalleeNode == NULL || rootNode < 0
        || (uint32_t)rootNode >= tc->ast->len)
    {
        return 0;
    }
    stack[stackLen++] = rootNode;
    while (stackLen > 0) {
        int32_t          nodeId = stack[--stackLen];
        const H2AstNode* node = &tc->ast->nodes[nodeId];
        int32_t          child;
        if (node->kind == H2Ast_CALL) {
            int32_t          calleeNode = node->firstChild;
            const H2AstNode* callee = calleeNode >= 0 ? &tc->ast->nodes[calleeNode] : NULL;
            if (callee != NULL && callee->kind == H2Ast_IDENT
                && callee->dataStart == symbol->nameStart && callee->dataEnd == symbol->nameEnd
                && H2TCListCount(tc->ast, nodeId)
                       == H2MirCallArgCountFromTok((uint16_t)encodedArgCount) + 1u)
            {
                if (found) {
                    return 0;
                }
                *outCallNode = nodeId;
                *outCalleeNode = calleeNode;
                found = 1;
            }
        }
        child = node->firstChild;
        while (child >= 0) {
            if (stackLen >= (uint32_t)(sizeof(stack) / sizeof(stack[0]))) {
                return 0;
            }
            stack[stackLen++] = child;
            child = tc->ast->nodes[child].nextSibling;
        }
    }
    return found;
}

static int H2TCMirConstResolveDirectCallTarget(
    const H2TCMirConstLowerCtx* c, int32_t rootNode, const H2MirInst* ins, int32_t* outFnIndex) {
    const H2MirSymbolRef* symbol;
    H2TypeCheckCtx*       tc;
    H2TCCallArgInfo       callArgs[H2TC_MAX_CALL_ARGS];
    int32_t               callNode = -1;
    int32_t               calleeNode = -1;
    int32_t               fnIndex = -1;
    int32_t               mutRefTempArgNode = -1;
    uint32_t              argCount = 0;
    int                   status;
    if (outFnIndex != NULL) {
        *outFnIndex = -1;
    }
    if (c == NULL || c->evalCtx == NULL || ins == NULL || outFnIndex == NULL
        || ins->op != H2MirOp_CALL || c->builder.symbols == NULL
        || ins->aux >= c->builder.symbolLen)
    {
        return 0;
    }
    symbol = &c->builder.symbols[ins->aux];
    if (symbol->kind != H2MirSymbol_CALL || symbol->flags != 0u) {
        return 0;
    }
    tc = c->evalCtx->tc;
    if (tc == NULL
        || !H2TCMirConstMatchPlainCallNode(
            tc, symbol, rootNode, (uint32_t)ins->tok, &callNode, &calleeNode))
    {
        return 0;
    }
    if (H2TCCollectCallArgInfo(tc, callNode, calleeNode, 0, -1, callArgs, NULL, &argCount) != 0) {
        return -1;
    }
    status = H2TCResolveCallByName(
        tc,
        symbol->nameStart,
        symbol->nameEnd,
        callArgs,
        argCount,
        0,
        0,
        &fnIndex,
        &mutRefTempArgNode);
    if (status != 0 || fnIndex < 0 || (uint32_t)fnIndex >= tc->funcLen) {
        return 0;
    }
    *outFnIndex = fnIndex;
    return 1;
}

static int H2TCMirConstHasImportAlias(
    const H2TCMirConstLowerCtx* c, uint32_t aliasStart, uint32_t aliasEnd) {
    H2TypeCheckCtx* tc;
    if (c == NULL || c->evalCtx == NULL) {
        return 0;
    }
    tc = c->evalCtx->tc;
    return tc != NULL && H2TCHasImportAlias(tc, aliasStart, aliasEnd);
}

static int H2TCMirConstMatchQualifiedCallNode(
    const H2TypeCheckCtx* tc,
    const H2MirSymbolRef* symbol,
    int32_t               rootNode,
    uint32_t              encodedArgCount,
    int32_t* _Nonnull outCallNode,
    int32_t* _Nonnull outCalleeNode,
    int32_t* _Nonnull outRecvNode,
    uint32_t* _Nonnull outBaseStart,
    uint32_t* _Nonnull outBaseEnd) {
    int32_t  stack[256];
    uint32_t stackLen = 0;
    int      found = 0;
    if (outCallNode != NULL) {
        *outCallNode = -1;
    }
    if (outCalleeNode != NULL) {
        *outCalleeNode = -1;
    }
    if (outRecvNode != NULL) {
        *outRecvNode = -1;
    }
    if (outBaseStart != NULL) {
        *outBaseStart = 0;
    }
    if (outBaseEnd != NULL) {
        *outBaseEnd = 0;
    }
    if (tc == NULL || symbol == NULL || outCallNode == NULL || outCalleeNode == NULL
        || outRecvNode == NULL || outBaseStart == NULL || outBaseEnd == NULL || rootNode < 0
        || (uint32_t)rootNode >= tc->ast->len)
    {
        return 0;
    }
    stack[stackLen++] = rootNode;
    while (stackLen > 0) {
        int32_t          nodeId = stack[--stackLen];
        const H2AstNode* node = &tc->ast->nodes[nodeId];
        int32_t          child;
        if (node->kind == H2Ast_CALL) {
            int32_t          calleeNode = node->firstChild;
            const H2AstNode* callee = calleeNode >= 0 ? &tc->ast->nodes[calleeNode] : NULL;
            int32_t recvNode = calleeNode >= 0 ? tc->ast->nodes[calleeNode].firstChild : -1;
            if (callee != NULL && callee->kind == H2Ast_FIELD_EXPR && recvNode >= 0
                && (uint32_t)recvNode < tc->ast->len && tc->ast->nodes[recvNode].kind == H2Ast_IDENT
                && callee->dataStart == symbol->nameStart && callee->dataEnd == symbol->nameEnd
                && H2TCListCount(tc->ast, nodeId)
                       == H2MirCallArgCountFromTok((uint16_t)encodedArgCount))
            {
                uint32_t baseStart = tc->ast->nodes[recvNode].dataStart;
                uint32_t baseEnd = tc->ast->nodes[recvNode].dataEnd;
                if (!found) {
                    *outCallNode = nodeId;
                    *outCalleeNode = calleeNode;
                    *outRecvNode = recvNode;
                    *outBaseStart = baseStart;
                    *outBaseEnd = baseEnd;
                    found = 1;
                } else if (*outBaseStart != baseStart || *outBaseEnd != baseEnd) {
                    return 0;
                }
            }
        }
        child = node->firstChild;
        while (child >= 0) {
            if (stackLen >= (uint32_t)(sizeof(stack) / sizeof(stack[0]))) {
                return 0;
            }
            stack[stackLen++] = child;
            child = tc->ast->nodes[child].nextSibling;
        }
    }
    return found;
}

static int H2TCMirConstMatchPkgPrefixedQualifiedCallNode(
    const H2TypeCheckCtx* tc,
    int32_t               rootNode,
    uint32_t              encodedArgCount,
    uint32_t              pkgStart,
    uint32_t              pkgEnd,
    uint32_t              methodStart,
    uint32_t              methodEnd,
    int32_t* _Nonnull outCallNode,
    int32_t* _Nonnull outCalleeNode,
    int32_t* _Nonnull outRecvNode) {
    int32_t  stack[256];
    uint32_t stackLen = 0;
    int      found = 0;
    if (outCallNode != NULL) {
        *outCallNode = -1;
    }
    if (outCalleeNode != NULL) {
        *outCalleeNode = -1;
    }
    if (outRecvNode != NULL) {
        *outRecvNode = -1;
    }
    if (tc == NULL || outCallNode == NULL || outCalleeNode == NULL || outRecvNode == NULL
        || rootNode < 0 || (uint32_t)rootNode >= tc->ast->len)
    {
        return 0;
    }
    stack[stackLen++] = rootNode;
    while (stackLen > 0) {
        int32_t          nodeId = stack[--stackLen];
        const H2AstNode* node = &tc->ast->nodes[nodeId];
        int32_t          child;
        if (node->kind == H2Ast_CALL) {
            int32_t          calleeNode = node->firstChild;
            const H2AstNode* callee = calleeNode >= 0 ? &tc->ast->nodes[calleeNode] : NULL;
            int32_t recvNode = calleeNode >= 0 ? tc->ast->nodes[calleeNode].firstChild : -1;
            if (callee != NULL && callee->kind == H2Ast_FIELD_EXPR && recvNode >= 0
                && (uint32_t)recvNode < tc->ast->len && tc->ast->nodes[recvNode].kind == H2Ast_IDENT
                && H2NameEqSlice(
                    tc->src, callee->dataStart, callee->dataEnd, methodStart, methodEnd)
                && H2NameEqSlice(
                    tc->src,
                    tc->ast->nodes[recvNode].dataStart,
                    tc->ast->nodes[recvNode].dataEnd,
                    pkgStart,
                    pkgEnd)
                && H2TCListCount(tc->ast, nodeId)
                       == H2MirCallArgCountFromTok((uint16_t)encodedArgCount) + 1u)
            {
                if (!found) {
                    *outCallNode = nodeId;
                    *outCalleeNode = calleeNode;
                    *outRecvNode = recvNode;
                    found = 1;
                }
            }
        }
        child = node->firstChild;
        while (child >= 0) {
            if (stackLen >= (uint32_t)(sizeof(stack) / sizeof(stack[0]))) {
                return 0;
            }
            stack[stackLen++] = child;
            child = tc->ast->nodes[child].nextSibling;
        }
    }
    return found;
}

static int H2TCMirConstResolveQualifiedCallTarget(
    const H2TCMirConstLowerCtx* c, int32_t rootNode, const H2MirInst* ins, int32_t* outFnIndex) {
    H2TypeCheckCtx*       tc;
    const H2MirSymbolRef* symbol;
    H2TCCallArgInfo       callArgs[H2TC_MAX_CALL_ARGS];
    int32_t               callNode = -1;
    int32_t               calleeNode = -1;
    int32_t               recvNode = -1;
    int32_t               fnIndex = -1;
    int32_t               mutRefTempArgNode = -1;
    uint32_t              argCount = 0;
    uint32_t              baseStart = 0;
    uint32_t              baseEnd = 0;
    int                   status;
    if (outFnIndex != NULL) {
        *outFnIndex = -1;
    }
    if (c == NULL || c->evalCtx == NULL || ins == NULL || outFnIndex == NULL
        || ins->op != H2MirOp_CALL || c->builder.symbols == NULL
        || ins->aux >= c->builder.symbolLen)
    {
        return 0;
    }
    tc = c->evalCtx->tc;
    if (tc == NULL) {
        return 0;
    }
    symbol = &c->builder.symbols[ins->aux];
    if (symbol->kind != H2MirSymbol_CALL
        || !H2TCMirConstMatchQualifiedCallNode(
            tc,
            symbol,
            rootNode,
            (uint32_t)ins->tok,
            &callNode,
            &calleeNode,
            &recvNode,
            &baseStart,
            &baseEnd))
    {
        return 0;
    }
    if (!H2TCMirConstHasImportAlias(c, baseStart, baseEnd)) {
        return 0;
    }
    if (H2TCCollectCallArgInfo(tc, callNode, calleeNode, 1, recvNode, callArgs, NULL, &argCount)
        != 0)
    {
        return -1;
    }
    status = H2TCResolveCallByPkgMethod(
        tc,
        baseStart,
        baseEnd,
        symbol->nameStart,
        symbol->nameEnd,
        callArgs,
        argCount,
        1,
        0,
        &fnIndex,
        &mutRefTempArgNode);
    if (status != 0 || fnIndex < 0 || (uint32_t)fnIndex >= tc->funcLen) {
        return 0;
    }
    *outFnIndex = fnIndex;
    return 1;
}

static int H2TCMirConstResolveFunctionIdentTarget(
    const H2TCMirConstLowerCtx* c, const H2MirInst* ins, int32_t* outFnIndex) {
    H2TypeCheckCtx* tc;
    if (outFnIndex != NULL) {
        *outFnIndex = -1;
    }
    if (c == NULL || c->evalCtx == NULL || ins == NULL || outFnIndex == NULL
        || ins->op != H2MirOp_LOAD_IDENT)
    {
        return 0;
    }
    tc = c->evalCtx->tc;
    if (tc == NULL) {
        return 0;
    }
    *outFnIndex = H2TCFindPlainFunctionValueIndex(tc, ins->start, ins->end);
    if (*outFnIndex < 0) {
        return 0;
    }
    return 1;
}

static int H2TCMirConstResolveQualifiedFunctionValueTarget(
    const H2TCMirConstLowerCtx* c,
    const H2MirInst*            loadIns,
    const H2MirInst* _Nullable fieldIns,
    int32_t* outFnIndex) {
    H2TypeCheckCtx* tc;
    uint32_t        fieldStart;
    uint32_t        fieldEnd;
    if (outFnIndex != NULL) {
        *outFnIndex = -1;
    }
    if (c == NULL || c->evalCtx == NULL || loadIns == NULL || fieldIns == NULL || outFnIndex == NULL
        || loadIns->op != H2MirOp_LOAD_IDENT || fieldIns->op != H2MirOp_AGG_GET)
    {
        return 0;
    }
    tc = c->evalCtx->tc;
    if (tc == NULL || !H2TCHasImportAlias(tc, loadIns->start, loadIns->end)) {
        return 0;
    }
    if (c->builder.fields != NULL && fieldIns->aux < c->builder.fieldLen) {
        fieldStart = c->builder.fields[fieldIns->aux].nameStart;
        fieldEnd = c->builder.fields[fieldIns->aux].nameEnd;
    } else {
        fieldStart = fieldIns->start;
        fieldEnd = fieldIns->end;
    }
    *outFnIndex = H2TCFindPkgQualifiedFunctionValueIndex(
        tc, loadIns->start, loadIns->end, fieldStart, fieldEnd);
    return *outFnIndex >= 0;
}

static int H2TCMirConstRewriteQualifiedFunctionValueLoad(
    H2TCMirConstLowerCtx* c,
    uint32_t              ownerMirFnIndex,
    uint32_t              loadInstIndex,
    uint32_t              targetMirFnIndex) {
    H2MirInst* loadIns;
    H2MirInst* fieldIns;
    H2MirInst  inserted = { 0 };
    H2MirConst value = { 0 };
    uint32_t   constIndex = UINT32_MAX;
    if (c == NULL || ownerMirFnIndex >= c->builder.funcLen || loadInstIndex >= c->builder.instLen
        || loadInstIndex + 1u >= c->builder.instLen)
    {
        return -1;
    }
    fieldIns = &c->builder.insts[loadInstIndex + 1u];
    value.kind = H2MirConst_FUNCTION;
    value.bits = targetMirFnIndex;
    if (H2MirProgramBuilderAddConst(&c->builder, &value, &constIndex) != 0) {
        return -1;
    }
    inserted.op = H2MirOp_PUSH_CONST;
    inserted.aux = constIndex;
    inserted.start = fieldIns->start;
    inserted.end = fieldIns->end;
    if (H2MirProgramBuilderInsertInst(
            &c->builder,
            ownerMirFnIndex,
            loadInstIndex + 2u - c->builder.funcs[ownerMirFnIndex].instStart,
            &inserted)
        != 0)
    {
        return -1;
    }
    loadIns = &c->builder.insts[loadInstIndex];
    fieldIns = &c->builder.insts[loadInstIndex + 1u];
    loadIns->op = H2MirOp_PUSH_NULL;
    loadIns->tok = 0u;
    loadIns->aux = 0u;
    fieldIns->op = H2MirOp_DROP;
    fieldIns->tok = 0u;
    fieldIns->aux = 0u;
    return 0;
}

static int H2TCMirConstResolveTopConstIdentTarget(
    const H2TCMirConstLowerCtx* c, int32_t rootNode, const H2MirInst* ins, int32_t* outNodeId) {
    H2TypeCheckCtx*  tc;
    int32_t          nodeId = -1;
    int32_t          nameIndex = -1;
    H2TCVarLikeParts parts;
    const H2AstNode* n;
    if (outNodeId != NULL) {
        *outNodeId = -1;
    }
    if (c == NULL || c->evalCtx == NULL || ins == NULL || outNodeId == NULL
        || ins->op != H2MirOp_LOAD_IDENT)
    {
        return 0;
    }
    tc = c->evalCtx->tc;
    if (tc == NULL) {
        return 0;
    }
    nodeId = H2TCFindTopLevelVarLikeNode(tc, ins->start, ins->end, &nameIndex);
    if (nodeId < 0 || (uint32_t)nodeId >= tc->ast->len || nameIndex != 0) {
        return 0;
    }
    n = &tc->ast->nodes[nodeId];
    if (n->kind != H2Ast_CONST || H2TCVarLikeGetParts(tc, nodeId, &parts) != 0 || parts.grouped
        || parts.nameCount != 1u)
    {
        return 0;
    }
    if (rootNode >= 0 && H2TCVarLikeInitExprNodeAt(tc, nodeId, 0) == rootNode) {
        return 0;
    }
    *outNodeId = nodeId;
    return 1;
}

static int H2TCMirConstResolvePkgPrefixedDirectCallTarget(
    const H2TCMirConstLowerCtx* c, int32_t rootNode, const H2MirInst* ins, int32_t* outFnIndex) {
    H2TypeCheckCtx*       tc;
    const H2MirSymbolRef* symbol;
    H2TCCallArgInfo       callArgs[H2TC_MAX_CALL_ARGS];
    int32_t               callNode = -1;
    int32_t               calleeNode = -1;
    int32_t               recvNode = -1;
    int32_t               fnIndex = -1;
    int32_t               mutRefTempArgNode = -1;
    uint32_t              argCount = 0;
    uint32_t              pkgStart = 0;
    uint32_t              pkgEnd = 0;
    uint32_t              methodStart;
    uint32_t              firstPositionalArgIndex = 0;
    int                   collectReceiver = 0;
    int                   status;
    if (outFnIndex != NULL) {
        *outFnIndex = -1;
    }
    if (c == NULL || c->evalCtx == NULL || ins == NULL || outFnIndex == NULL
        || ins->op != H2MirOp_CALL || c->builder.symbols == NULL
        || ins->aux >= c->builder.symbolLen)
    {
        return 0;
    }
    tc = c->evalCtx->tc;
    if (tc == NULL) {
        return 0;
    }
    symbol = &c->builder.symbols[ins->aux];
    if (symbol->kind != H2MirSymbol_CALL || symbol->flags != 0u
        || !H2TCExtractPkgPrefixFromTypeName(
            tc, symbol->nameStart, symbol->nameEnd, &pkgStart, &pkgEnd))
    {
        return 0;
    }
    methodStart = pkgEnd + 2u;
    if (methodStart >= symbol->nameEnd) {
        return 0;
    }
    if (H2TCMirConstMatchPkgPrefixedQualifiedCallNode(
            tc,
            rootNode,
            (uint32_t)ins->tok,
            pkgStart,
            pkgEnd,
            methodStart,
            symbol->nameEnd,
            &callNode,
            &calleeNode,
            &recvNode))
    {
        collectReceiver = 1;
        firstPositionalArgIndex = 1u;
    } else if (!H2TCMirConstMatchPlainCallNode(
                   tc, symbol, rootNode, (uint32_t)ins->tok, &callNode, &calleeNode))
    {
        return 0;
    }
    if (H2TCCollectCallArgInfo(
            tc, callNode, calleeNode, collectReceiver, recvNode, callArgs, NULL, &argCount)
        != 0)
    {
        return -1;
    }
    status = H2TCResolveCallByPkgMethod(
        tc,
        pkgStart,
        pkgEnd,
        methodStart,
        symbol->nameEnd,
        callArgs,
        argCount,
        firstPositionalArgIndex,
        0,
        &fnIndex,
        &mutRefTempArgNode);
    if (status != 0 || fnIndex < 0 || (uint32_t)fnIndex >= tc->funcLen) {
        return 0;
    }
    *outFnIndex = fnIndex;
    return 1;
}

static int H2TCMirConstResolveSimpleTopConstAliasCallTarget(
    const H2TCMirConstLowerCtx* c, int32_t rootNode, const H2MirInst* ins, int32_t* outFnIndex) {
    H2TypeCheckCtx*       tc;
    const H2MirSymbolRef* symbol;
    H2TCVarLikeParts      parts;
    H2TCCallArgInfo       callArgs[H2TC_MAX_CALL_ARGS];
    int32_t               nodeId = -1;
    int32_t               nameIndex = -1;
    int32_t               initNode;
    int32_t               callNode = -1;
    int32_t               calleeNode = -1;
    int32_t               fnIndex = -1;
    int32_t               mutRefTempArgNode = -1;
    uint32_t              argCount = 0;
    int                   status;
    if (outFnIndex != NULL) {
        *outFnIndex = -1;
    }
    if (c == NULL || c->evalCtx == NULL || ins == NULL || outFnIndex == NULL
        || ins->op != H2MirOp_CALL || c->builder.symbols == NULL
        || ins->aux >= c->builder.symbolLen)
    {
        return 0;
    }
    tc = c->evalCtx->tc;
    if (tc == NULL) {
        return 0;
    }
    symbol = &c->builder.symbols[ins->aux];
    if (symbol->kind != H2MirSymbol_CALL || symbol->flags != 0u) {
        return 0;
    }
    nodeId = H2TCFindTopLevelVarLikeNode(tc, symbol->nameStart, symbol->nameEnd, &nameIndex);
    if (nodeId < 0 || nameIndex != 0) {
        return 0;
    }
    if (!H2TCMirConstMatchPlainCallNode(
            tc, symbol, rootNode, (uint32_t)ins->tok, &callNode, &calleeNode)
        && !H2TCMirConstMatchAliasCallNode(
            tc, symbol, rootNode, (uint32_t)ins->tok, &callNode, &calleeNode))
    {
        return 0;
    }
    if (H2TCCollectCallArgInfo(tc, callNode, calleeNode, 0, -1, callArgs, NULL, &argCount) != 0) {
        return -1;
    }
    if (H2TCVarLikeGetParts(tc, nodeId, &parts) != 0 || parts.grouped || parts.nameCount != 1u
        || tc->ast->nodes[nodeId].kind != H2Ast_CONST)
    {
        return 0;
    }
    initNode = H2TCVarLikeInitExprNodeAt(tc, nodeId, 0);
    if (initNode < 0 || (uint32_t)initNode >= tc->ast->len) {
        return 0;
    }
    if (tc->ast->nodes[initNode].kind == H2Ast_IDENT) {
        const H2AstNode* initExpr = &tc->ast->nodes[initNode];
        uint32_t         pkgStart = 0;
        uint32_t         pkgEnd = 0;
        if (H2TCExtractPkgPrefixFromTypeName(
                tc, initExpr->dataStart, initExpr->dataEnd, &pkgStart, &pkgEnd))
        {
            uint32_t methodStart = pkgEnd + 2u;
            if (methodStart >= initExpr->dataEnd) {
                return 0;
            }
            status = H2TCResolveCallByPkgMethod(
                tc,
                pkgStart,
                pkgEnd,
                methodStart,
                initExpr->dataEnd,
                callArgs,
                argCount,
                0,
                0,
                &fnIndex,
                &mutRefTempArgNode);
        } else {
            status = H2TCResolveCallByName(
                tc,
                initExpr->dataStart,
                initExpr->dataEnd,
                callArgs,
                argCount,
                0,
                0,
                &fnIndex,
                &mutRefTempArgNode);
        }
    } else if (tc->ast->nodes[initNode].kind == H2Ast_FIELD_EXPR) {
        const H2AstNode* initExpr = &tc->ast->nodes[initNode];
        int32_t          baseNode = initExpr->firstChild;
        const H2AstNode* baseExpr;
        if (baseNode < 0 || (uint32_t)baseNode >= tc->ast->len) {
            return 0;
        }
        baseExpr = &tc->ast->nodes[baseNode];
        if (baseExpr->kind != H2Ast_IDENT
            || !H2TCHasImportAlias(tc, baseExpr->dataStart, baseExpr->dataEnd))
        {
            return 0;
        }
        status = H2TCResolveCallByPkgMethod(
            tc,
            baseExpr->dataStart,
            baseExpr->dataEnd,
            initExpr->dataStart,
            initExpr->dataEnd,
            callArgs,
            argCount,
            0,
            0,
            &fnIndex,
            &mutRefTempArgNode);
    } else {
        return 0;
    }
    if (status != 0 || fnIndex < 0 || (uint32_t)fnIndex >= tc->funcLen) {
        return 0;
    }
    *outFnIndex = fnIndex;
    return 1;
}

static int H2TCMirConstFinalizeLoweredFunction(
    H2TCMirConstLowerCtx* c, uint32_t* mirMapSlot, uint32_t mirFnIndex, int32_t rootNode);
static int H2TCMirConstRunFunction(
    H2TCConstEvalCtx*     evalCtx,
    H2TCMirConstLowerCtx* lowerCtx,
    const H2MirProgram*   program,
    uint32_t              mirFnIndex,
    const H2CTFEValue* _Nullable args,
    uint32_t     argCount,
    H2CTFEValue* outValue,
    int* _Nullable outDidReturn,
    int* outIsConst);

static int H2TCMirConstLowerTopConstNode(
    H2TCMirConstLowerCtx* c, int32_t nodeId, uint32_t* _Nullable outMirFnIndex) {
    H2TypeCheckCtx*  tc;
    H2TCVarLikeParts parts;
    uint32_t         mirFnIndex = UINT32_MAX;
    int32_t          initNode;
    int              supported = 0;
    int              rewriteRc;
    if (outMirFnIndex != NULL) {
        *outMirFnIndex = UINT32_MAX;
    }
    if (c == NULL || c->evalCtx == NULL || c->topConstToMir == NULL || c->loweringTopConsts == NULL)
    {
        return -1;
    }
    tc = c->evalCtx->tc;
    if (tc == NULL || nodeId < 0 || (uint32_t)nodeId >= tc->ast->len) {
        return 0;
    }
    if (c->topConstToMir[(uint32_t)nodeId] != H2_TC_MIR_CONST_FN_NONE) {
        if (outMirFnIndex != NULL) {
            *outMirFnIndex = c->topConstToMir[(uint32_t)nodeId];
        }
        return 1;
    }
    if (c->loweringTopConsts[(uint32_t)nodeId] != 0u) {
        return 0;
    }
    if (H2TCVarLikeGetParts(tc, nodeId, &parts) != 0 || parts.grouped || parts.nameCount != 1u
        || tc->ast->nodes[nodeId].kind != H2Ast_CONST)
    {
        return 0;
    }
    initNode = H2TCVarLikeInitExprNodeAt(tc, nodeId, 0);
    if (initNode < 0) {
        return 0;
    }
    c->loweringTopConsts[(uint32_t)nodeId] = 1u;
    if (H2MirLowerAppendNamedVarLikeTopInitFunctionBySlice(
            &c->builder,
            tc->arena,
            tc->ast,
            tc->src,
            nodeId,
            tc->ast->nodes[nodeId].dataStart,
            tc->ast->nodes[nodeId].dataEnd,
            &mirFnIndex,
            &supported,
            c->diag)
        != 0)
    {
        c->loweringTopConsts[(uint32_t)nodeId] = 0u;
        return -1;
    }
    if (!supported || mirFnIndex == UINT32_MAX) {
        c->loweringTopConsts[(uint32_t)nodeId] = 0u;
        return 0;
    }
    rewriteRc = H2TCMirConstFinalizeLoweredFunction(
        c, &c->topConstToMir[(uint32_t)nodeId], mirFnIndex, initNode);
    c->loweringTopConsts[(uint32_t)nodeId] = 0u;
    if (rewriteRc <= 0) {
        return rewriteRc;
    }
    if (outMirFnIndex != NULL) {
        *outMirFnIndex = mirFnIndex;
    }
    return 1;
}

static int H2TCMirConstRewriteLoadIdentToFunctionConst(
    H2TCMirConstLowerCtx* c, H2MirInst* ins, uint32_t targetMirFnIndex) {
    H2MirConst value = { 0 };
    uint32_t   constIndex = UINT32_MAX;
    if (c == NULL || ins == NULL) {
        return -1;
    }
    value.kind = H2MirConst_FUNCTION;
    value.bits = targetMirFnIndex;
    if (H2MirProgramBuilderAddConst(&c->builder, &value, &constIndex) != 0) {
        return -1;
    }
    ins->op = H2MirOp_PUSH_CONST;
    ins->tok = 0u;
    ins->aux = constIndex;
    return 0;
}

static int H2TCMirConstFinalizeLoweredFunction(
    H2TCMirConstLowerCtx* c, uint32_t* mirMapSlot, uint32_t mirFnIndex, int32_t rootNode) {
    int rewriteRc;
    if (c == NULL || mirMapSlot == NULL || mirFnIndex == UINT32_MAX) {
        return -1;
    }
    *mirMapSlot = mirFnIndex;
    rewriteRc = H2TCMirConstRewriteDirectCalls(c, mirFnIndex, rootNode);
    if (rewriteRc < 0) {
        return -1;
    }
    if (rewriteRc == 0) {
        *mirMapSlot = H2_TC_MIR_CONST_FN_NONE;
        return 0;
    }
    return 1;
}

static int H2TCMirConstFindTcFunctionIndexForMir(
    const H2TCMirConstLowerCtx* c, uint32_t mirFnIndex, int32_t* outFnIndex) {
    uint32_t i;
    if (outFnIndex != NULL) {
        *outFnIndex = -1;
    }
    if (c == NULL || c->tcToMir == NULL || c->evalCtx == NULL || c->evalCtx->tc == NULL
        || outFnIndex == NULL)
    {
        return 0;
    }
    for (i = 0; i < c->evalCtx->tc->funcLen; i++) {
        if (c->tcToMir[i] == mirFnIndex) {
            *outFnIndex = (int32_t)i;
            return 1;
        }
    }
    return 0;
}

static int H2TCMirConstExportValue(const H2TCMirConstLowerCtx* c, H2CTFEValue* _Nonnull value) {
    uint32_t mirFnIndex = UINT32_MAX;
    int32_t  tcFnIndex = -1;
    if (c == NULL || value == NULL || !H2MirValueAsFunctionRef(value, &mirFnIndex)) {
        return 1;
    }
    if (!H2TCMirConstFindTcFunctionIndexForMir(c, mirFnIndex, &tcFnIndex) || tcFnIndex < 0) {
        return 0;
    }
    H2MirValueSetFunctionRef(value, (uint32_t)tcFnIndex);
    return 1;
}

static int H2TCMirConstCallNodeIsSpecialBuiltin(H2TypeCheckCtx* tc, int32_t callNode) {
    const H2AstNode* call;
    const H2AstNode* callee;
    int32_t          calleeNode;
    int32_t          recvNode;
    if (tc == NULL || callNode < 0 || (uint32_t)callNode >= tc->ast->len) {
        return 0;
    }
    call = &tc->ast->nodes[callNode];
    calleeNode = call->firstChild;
    callee = calleeNode >= 0 ? &tc->ast->nodes[calleeNode] : NULL;
    if (call->kind != H2Ast_CALL || callee == NULL) {
        return 0;
    }
    if (callee->kind == H2Ast_IDENT) {
        return H2NameEqLiteral(tc->src, callee->dataStart, callee->dataEnd, "typeof")
            || H2NameEqLiteral(tc->src, callee->dataStart, callee->dataEnd, "kind")
            || H2NameEqLiteral(tc->src, callee->dataStart, callee->dataEnd, "base")
            || H2NameEqLiteral(tc->src, callee->dataStart, callee->dataEnd, "is_alias")
            || H2NameEqLiteral(tc->src, callee->dataStart, callee->dataEnd, "is_const")
            || H2NameEqLiteral(tc->src, callee->dataStart, callee->dataEnd, "type_name")
            || H2NameEqLiteral(tc->src, callee->dataStart, callee->dataEnd, "ptr")
            || H2NameEqLiteral(tc->src, callee->dataStart, callee->dataEnd, "slice")
            || H2NameEqLiteral(tc->src, callee->dataStart, callee->dataEnd, "array")
            || H2TCIsSourceLocationOfName(tc, callee->dataStart, callee->dataEnd)
            || H2TCCompilerDiagOpFromName(tc, callee->dataStart, callee->dataEnd)
                   != H2TCCompilerDiagOp_NONE;
    }
    if (callee->kind != H2Ast_FIELD_EXPR) {
        return 0;
    }
    recvNode = callee->firstChild;
    if (recvNode < 0 || (uint32_t)recvNode >= tc->ast->len
        || tc->ast->nodes[recvNode].kind != H2Ast_IDENT)
    {
        return 0;
    }
    if (H2NameEqLiteral(
            tc->src,
            tc->ast->nodes[recvNode].dataStart,
            tc->ast->nodes[recvNode].dataEnd,
            "builtin")
        && H2TCIsSourceLocationOfName(tc, callee->dataStart, callee->dataEnd))
    {
        return 1;
    }
    if (H2NameEqLiteral(
            tc->src,
            tc->ast->nodes[recvNode].dataStart,
            tc->ast->nodes[recvNode].dataEnd,
            "compiler")
        && H2TCConstEvalCompilerDiagOpFromFieldExpr(tc, callee) != H2TCCompilerDiagOp_NONE)
    {
        return 1;
    }
    return 0;
}

static int H2TCMirConstResolveCallNode(
    H2TCConstEvalCtx* evalCtx,
    const H2MirProgram* _Nullable program,
    const H2MirInst* _Nullable inst,
    int32_t* outCallNode) {
    const H2MirSymbolRef* symbol;
    if (outCallNode != NULL) {
        *outCallNode = -1;
    }
    if (evalCtx == NULL || evalCtx->tc == NULL || program == NULL || inst == NULL
        || outCallNode == NULL || inst->op != H2MirOp_CALL || inst->aux >= program->symbolLen)
    {
        uint32_t rawNode;
        if (evalCtx != NULL && evalCtx->tc != NULL && inst != NULL && outCallNode != NULL
            && inst->op == H2MirOp_CALL)
        {
            rawNode = H2MirRawCallAuxNode(inst->aux);
            if (rawNode < evalCtx->tc->ast->len
                && evalCtx->tc->ast->nodes[rawNode].kind == H2Ast_CALL)
            {
                *outCallNode = (int32_t)rawNode;
                return 1;
            }
        }
        return 0;
    }
    symbol = &program->symbols[inst->aux];
    if (symbol->kind != H2MirSymbol_CALL || symbol->target >= evalCtx->tc->ast->len) {
        uint32_t rawNode = H2MirRawCallAuxNode(inst->aux);
        if (rawNode < evalCtx->tc->ast->len && evalCtx->tc->ast->nodes[rawNode].kind == H2Ast_CALL)
        {
            *outCallNode = (int32_t)rawNode;
            return 1;
        }
        return 0;
    }
    *outCallNode = (int32_t)symbol->target;
    return 1;
}

static int H2TCMirConstRunFunction(
    H2TCConstEvalCtx*     evalCtx,
    H2TCMirConstLowerCtx* lowerCtx,
    const H2MirProgram*   program,
    uint32_t              mirFnIndex,
    const H2CTFEValue* _Nullable args,
    uint32_t     argCount,
    H2CTFEValue* outValue,
    int* _Nullable outDidReturn,
    int* outIsConst) {
    H2TypeCheckCtx* c;
    H2MirExecEnv    env = { 0 };
    int             mirIsConst = 0;
    if (outDidReturn != NULL) {
        *outDidReturn = 0;
    }
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (evalCtx == NULL || program == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL) {
        return -1;
    }
    env.src = c->src;
    env.resolveIdent = H2TCResolveConstIdent;
    env.resolveCallPre = H2TCResolveConstCallMirPre;
    env.resolveCall = H2TCResolveConstCallMir;
    env.resolveCtx = evalCtx;
    env.zeroInitLocal = H2TCMirConstZeroInitLocal;
    env.zeroInitCtx = evalCtx;
    env.coerceValueForType = H2TCMirConstCoerceValueForType;
    env.coerceValueCtx = evalCtx;
    env.indexValue = H2TCMirConstIndexValue;
    env.indexValueCtx = evalCtx;
    env.sequenceLen = H2TCMirConstSequenceLen;
    env.sequenceLenCtx = evalCtx;
    env.iterInit = H2TCMirConstIterInit;
    env.iterInitCtx = evalCtx;
    env.iterNext = H2TCMirConstIterNext;
    env.iterNextCtx = evalCtx;
    env.aggGetField = H2TCMirConstAggGetField;
    env.aggGetFieldCtx = evalCtx;
    env.aggAddrField = H2TCMirConstAggAddrField;
    env.aggAddrFieldCtx = evalCtx;
    env.makeTuple = H2TCMirConstMakeTuple;
    env.makeTupleCtx = evalCtx;
    env.bindFrame = H2TCMirConstBindFrame;
    env.unbindFrame = H2TCMirConstUnbindFrame;
    env.frameCtx = evalCtx;
    env.setReason = H2TCMirConstSetReasonCb;
    env.setReasonCtx = evalCtx;
    env.backwardJumpLimit = H2TC_CONST_FOR_MAX_ITERS;
    env.diag = c->diag;
    if (!H2MirProgramNeedsDynamicResolution(program)) {
        H2MirExecEnvDisableDynamicResolution(&env);
    }
    if (H2MirEvalFunction(
            c->arena, program, mirFnIndex, args, argCount, &env, outValue, &mirIsConst)
        != 0)
    {
        return -1;
    }
    if (mirIsConst && !H2TCMirConstExportValue(lowerCtx, outValue)) {
        return 0;
    }
    *outIsConst = mirIsConst;
    if (outDidReturn != NULL && mirIsConst) {
        *outDidReturn = outValue->kind != H2CTFEValue_INVALID;
    }
    return 0;
}

int H2TCResolveConstCallMirPre(
    void* _Nullable ctx,
    const H2MirProgram* _Nullable program,
    const H2MirFunction* _Nullable function,
    const H2MirInst* _Nullable inst,
    uint32_t     nameStart,
    uint32_t     nameEnd,
    H2CTFEValue* outValue,
    int*         outIsConst,
    H2Diag* _Nullable diag) {
    H2TCConstEvalCtx* evalCtx = (H2TCConstEvalCtx*)ctx;
    int32_t           callNode = -1;
    int               status;

    (void)function;
    (void)nameStart;
    (void)nameEnd;
    (void)diag;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    if (!H2TCMirConstResolveCallNode(evalCtx, program, inst, &callNode) || callNode < 0) {
        return 0;
    }

    status = H2TCConstEvalCompilerDiagCall(evalCtx, callNode, outValue, outIsConst);
    if (status < 0) {
        return -1;
    }
    if (status == 0) {
        return 1;
    }

    status = H2TCConstEvalSourceLocationOfCall(evalCtx, callNode, outValue, outIsConst);
    if (status < 0) {
        return -1;
    }
    if (status == 0) {
        return 1;
    }

    status = H2TCConstEvalReflectIsConstCall(evalCtx, callNode, outValue, outIsConst);
    if (status < 0) {
        return -1;
    }
    if (status == 0) {
        return 1;
    }

    return 0;
}

int H2TCMirConstRewriteDirectCalls(H2TCMirConstLowerCtx* c, uint32_t mirFnIndex, int32_t rootNode) {
    H2TypeCheckCtx* tc;
    uint32_t        instIndex;
    if (c == NULL || c->evalCtx == NULL || c->tcToMir == NULL || c->loweringFns == NULL) {
        return -1;
    }
    tc = c->evalCtx->tc;
    if (tc == NULL || mirFnIndex >= c->builder.funcLen) {
        return 0;
    }
    for (instIndex = c->builder.funcs[mirFnIndex].instStart;
         instIndex < c->builder.funcs[mirFnIndex].instStart + c->builder.funcs[mirFnIndex].instLen;
         instIndex++)
    {
        H2MirInst* ins = &c->builder.insts[instIndex];
        H2MirInst* nextIns =
            instIndex + 1u < c->builder.instLen ? &c->builder.insts[instIndex + 1u] : NULL;
        int32_t  targetFnIndex = -1;
        int32_t  targetTopConstNode = -1;
        uint32_t targetMirFnIndex = UINT32_MAX;
        int      lowerRc;
        if (ins->op == H2MirOp_CALL && ins->aux < c->builder.symbolLen) {
            const H2MirSymbolRef* symbol = &c->builder.symbols[ins->aux];
            int32_t               callNode = (int32_t)symbol->target;
            if (symbol->kind == H2MirSymbol_CALL && callNode >= 0
                && (uint32_t)callNode < tc->ast->len
                && H2TCMirConstCallNodeIsSpecialBuiltin(tc, callNode))
            {
                continue;
            }
        }
        lowerRc = H2TCMirConstResolveQualifiedFunctionValueTarget(c, ins, nextIns, &targetFnIndex);
        if (lowerRc < 0) {
            return -1;
        }
        if (lowerRc > 0) {
            lowerRc = H2TCMirConstLowerFunction(c, targetFnIndex, &targetMirFnIndex);
            if (lowerRc < 0) {
                return -1;
            }
            if (lowerRc == 0) {
                return 0;
            }
            if (H2TCMirConstRewriteQualifiedFunctionValueLoad(
                    c, mirFnIndex, instIndex, targetMirFnIndex)
                != 0)
            {
                return -1;
            }
            continue;
        }
        if (H2TCMirConstResolveTopConstIdentTarget(c, rootNode, ins, &targetTopConstNode)) {
            lowerRc = H2TCMirConstLowerTopConstNode(c, targetTopConstNode, &targetMirFnIndex);
            if (lowerRc < 0) {
                return -1;
            }
            if (lowerRc == 0) {
                return 0;
            }
            ins = &c->builder.insts[instIndex];
            ins->op = H2MirOp_CALL_FN;
            ins->tok = 0u;
            ins->aux = targetMirFnIndex;
            continue;
        }
        if (H2TCMirConstResolveFunctionIdentTarget(c, ins, &targetFnIndex)) {
            lowerRc = H2TCMirConstLowerFunction(c, targetFnIndex, &targetMirFnIndex);
            if (lowerRc < 0) {
                return -1;
            }
            if (lowerRc == 0) {
                return 0;
            }
            ins = &c->builder.insts[instIndex];
            if (H2TCMirConstRewriteLoadIdentToFunctionConst(c, ins, targetMirFnIndex) != 0) {
                return -1;
            }
            continue;
        }
        lowerRc = H2TCMirConstResolveSimpleTopConstAliasCallTarget(
            c, rootNode, ins, &targetFnIndex);
        if (lowerRc < 0) {
            return -1;
        }
        if (lowerRc > 0) {
            lowerRc = H2TCMirConstLowerFunction(c, targetFnIndex, &targetMirFnIndex);
            if (lowerRc < 0) {
                return -1;
            }
            if (lowerRc == 0) {
                return 0;
            }
            ins = &c->builder.insts[instIndex];
            ins->op = H2MirOp_CALL_FN;
            ins->aux = targetMirFnIndex;
            continue;
        }
        lowerRc = H2TCMirConstResolveQualifiedCallTarget(c, rootNode, ins, &targetFnIndex);
        if (lowerRc < 0) {
            return -1;
        }
        if (lowerRc > 0) {
            lowerRc = H2TCMirConstLowerFunction(c, targetFnIndex, &targetMirFnIndex);
            if (lowerRc < 0) {
                return -1;
            }
            if (lowerRc == 0) {
                return 0;
            }
            ins = &c->builder.insts[instIndex];
            ins->op = H2MirOp_CALL_FN;
            ins->aux = targetMirFnIndex;
            continue;
        }
        lowerRc = H2TCMirConstResolvePkgPrefixedDirectCallTarget(c, rootNode, ins, &targetFnIndex);
        if (lowerRc < 0) {
            return -1;
        }
        if (lowerRc > 0) {
            lowerRc = H2TCMirConstLowerFunction(c, targetFnIndex, &targetMirFnIndex);
            if (lowerRc < 0) {
                return -1;
            }
            if (lowerRc == 0) {
                return 0;
            }
            ins = &c->builder.insts[instIndex];
            ins->op = H2MirOp_CALL_FN;
            ins->aux = targetMirFnIndex;
            continue;
        }
        lowerRc = H2TCMirConstResolveDirectCallTarget(c, rootNode, ins, &targetFnIndex);
        if (lowerRc < 0) {
            return -1;
        }
        if (lowerRc == 0) {
            continue;
        }
        lowerRc = H2TCMirConstLowerFunction(c, targetFnIndex, &targetMirFnIndex);
        if (lowerRc < 0) {
            return -1;
        }
        if (lowerRc == 0) {
            return 0;
        }
        ins = &c->builder.insts[instIndex];
        ins->op = H2MirOp_CALL_FN;
        ins->aux = targetMirFnIndex;
    }
    return 1;
}

int H2TCMirConstLowerFunction(
    H2TCMirConstLowerCtx* c, int32_t fnIndex, uint32_t* _Nullable outMirFnIndex) {
    H2TypeCheckCtx*   tc;
    H2MirLowerOptions options = { 0 };
    uint32_t          mirFnIndex = UINT32_MAX;
    int32_t           fnNode = -1;
    int32_t           bodyNode = -1;
    int               supported = 0;
    if (outMirFnIndex != NULL) {
        *outMirFnIndex = UINT32_MAX;
    }
    if (c == NULL || c->evalCtx == NULL || c->tcToMir == NULL || c->loweringFns == NULL) {
        return -1;
    }
    tc = c->evalCtx->tc;
    if (tc == NULL || fnIndex < 0 || (uint32_t)fnIndex >= tc->funcLen) {
        return 0;
    }
    if (c->loweringFns[(uint32_t)fnIndex] != 0u) {
        return 0;
    }
    if (c->tcToMir[(uint32_t)fnIndex] != H2_TC_MIR_CONST_FN_NONE) {
        if (outMirFnIndex != NULL) {
            *outMirFnIndex = c->tcToMir[(uint32_t)fnIndex];
        }
        return 1;
    }
    if (!H2TCMirConstGetFunctionBody(tc, fnIndex, &fnNode, &bodyNode)) {
        return 0;
    }
    {
        int32_t  stack[256];
        uint32_t stackLen = 0;
        stack[stackLen++] = bodyNode;
        while (stackLen > 0) {
            int32_t          nodeId = stack[--stackLen];
            const H2AstNode* node = &tc->ast->nodes[nodeId];
            int32_t          child;
            child = node->firstChild;
            while (child >= 0) {
                if (stackLen >= (uint32_t)(sizeof(stack) / sizeof(stack[0]))) {
                    return 0;
                }
                stack[stackLen++] = child;
                child = tc->ast->nodes[child].nextSibling;
            }
        }
    }
    c->loweringFns[(uint32_t)fnIndex] = 1u;
    options.lowerConstExpr = H2TCMirConstLowerConstExpr;
    options.lowerConstExprCtx = c->evalCtx;
    if (H2MirLowerAppendSimpleFunctionWithOptions(
            &c->builder,
            tc->arena,
            tc->ast,
            tc->src,
            fnNode,
            bodyNode,
            &options,
            &mirFnIndex,
            &supported,
            c->diag)
        != 0)
    {
        c->loweringFns[(uint32_t)fnIndex] = 0u;
        return -1;
    }
    if (!supported) {
        c->loweringFns[(uint32_t)fnIndex] = 0u;
        return 0;
    }
    {
        int rewriteRc = H2TCMirConstFinalizeLoweredFunction(
            c, &c->tcToMir[(uint32_t)fnIndex], mirFnIndex, bodyNode);
        if (rewriteRc <= 0) {
            c->loweringFns[(uint32_t)fnIndex] = 0u;
            return rewriteRc;
        }
    }
    c->loweringFns[(uint32_t)fnIndex] = 0u;
    if (outMirFnIndex != NULL) {
        *outMirFnIndex = mirFnIndex;
    }
    return 1;
}

static int H2TCTryMirConstCall(
    H2TCConstEvalCtx*  evalCtx,
    int32_t            fnIndex,
    const H2CTFEValue* args,
    uint32_t           argCount,
    H2CTFEValue*       outValue,
    int*               outDidReturn,
    int*               outIsConst,
    int*               outSupported) {
    H2TypeCheckCtx*      c;
    H2MirProgram         program = { 0 };
    H2TCMirConstLowerCtx lowerCtx;
    uint32_t             mirFnIndex = UINT32_MAX;
    int                  lowerRc;
    if (outDidReturn != NULL) {
        *outDidReturn = 0;
    }
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (outSupported != NULL) {
        *outSupported = 0;
    }
    if (evalCtx == NULL || outValue == NULL || outDidReturn == NULL || outIsConst == NULL
        || outSupported == NULL)
    {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || fnIndex < 0 || (uint32_t)fnIndex >= c->funcLen) {
        return -1;
    }
    if (H2TCMirConstInitLowerCtx(evalCtx, &lowerCtx) != 0) {
        return -1;
    }
    lowerRc = H2TCMirConstLowerFunction(&lowerCtx, fnIndex, &mirFnIndex);
    if (lowerRc < 0) {
        return -1;
    }
    if (lowerRc == 0 || mirFnIndex == UINT32_MAX) {
        H2TCMirConstAdoptLowerDiagReason(evalCtx, lowerCtx.diag);
        return 0;
    }
    H2MirProgramBuilderFinish(&lowerCtx.builder, &program);
    if (H2TCMirConstRunFunction(
            evalCtx,
            &lowerCtx,
            &program,
            mirFnIndex,
            args,
            argCount,
            outValue,
            outDidReturn,
            outIsConst)
        != 0)
    {
        return -1;
    }
    *outSupported = 1;
    return 0;
}

static int H2TCConstEvalTryDirectReturnFunction(
    H2TCConstEvalCtx* evalCtx, int32_t bodyNode, H2CTFEValue* outValue, int* outIsConst) {
    H2TypeCheckCtx* c;
    int32_t         stmtNode;
    int32_t         exprNode;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || bodyNode < 0 || (uint32_t)bodyNode >= c->ast->len
        || c->ast->nodes[bodyNode].kind != H2Ast_BLOCK)
    {
        return 1;
    }
    stmtNode = H2AstFirstChild(c->ast, bodyNode);
    if (stmtNode < 0 || H2AstNextSibling(c->ast, stmtNode) >= 0
        || c->ast->nodes[stmtNode].kind != H2Ast_RETURN)
    {
        return 1;
    }
    exprNode = H2AstFirstChild(c->ast, stmtNode);
    if (exprNode < 0 || H2AstNextSibling(c->ast, exprNode) >= 0) {
        return 1;
    }
    return H2TCEvalConstExprNode(evalCtx, exprNode, outValue, outIsConst);
}

static int H2TCTryMirTopLevelConst(
    H2TCConstEvalCtx* evalCtx,
    int32_t           nodeId,
    int32_t           nameIndex,
    H2CTFEValue*      outValue,
    int*              outIsConst,
    int*              outSupported) {
    H2MirProgram         program = { 0 };
    H2TCMirConstLowerCtx lowerCtx;
    uint32_t             mirFnIndex = UINT32_MAX;
    int                  lowerRc;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (outSupported != NULL) {
        *outSupported = 0;
    }
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL || outSupported == NULL) {
        return -1;
    }
    if (evalCtx->tc != NULL) {
        H2TypeCheckCtx*  c = evalCtx->tc;
        H2TCVarLikeParts parts;
        int32_t          initNode = -1;
        int32_t          initType = -1;
        H2Diag           savedDiag = { 0 };
        int              haveSavedDiag = c->diag != NULL;
        if (haveSavedDiag) {
            savedDiag = *c->diag;
        }
        if (H2TCVarLikeGetParts(c, nodeId, &parts) == 0 && !parts.grouped && nameIndex >= 0
            && (uint32_t)nameIndex < parts.nameCount)
        {
            initNode = H2TCVarLikeInitExprNodeAt(c, nodeId, nameIndex);
            if (initNode >= 0 && H2TCTypeExpr(c, initNode, &initType) == 0
                && initType == c->typeType)
            {
                if (haveSavedDiag) {
                    *c->diag = savedDiag;
                }
                return 0;
            }
        }
        if (haveSavedDiag) {
            *c->diag = savedDiag;
        }
    }
    if (H2TCMirConstInitLowerCtx(evalCtx, &lowerCtx) != 0) {
        return -1;
    }
    lowerRc = H2TCMirConstLowerTopConstNode(&lowerCtx, nodeId, &mirFnIndex);
    if (lowerRc < 0) {
        return -1;
    }
    if (lowerRc == 0 || mirFnIndex == UINT32_MAX) {
        H2TCMirConstAdoptLowerDiagReason(evalCtx, lowerCtx.diag);
        return 0;
    }
    H2MirProgramBuilderFinish(&lowerCtx.builder, &program);
    if (H2TCMirConstRunFunction(
            evalCtx, &lowerCtx, &program, mirFnIndex, NULL, 0, outValue, NULL, outIsConst)
        != 0)
    {
        return -1;
    }
    *outSupported = 1;
    return 0;
}

int32_t H2TCFindConstCallableFunction(
    H2TypeCheckCtx* c, uint32_t nameStart, uint32_t nameEnd, uint32_t argCount) {
    uint32_t i;
    int32_t  found = -1;
    for (i = 0; i < c->funcLen; i++) {
        const H2TCFunction* f = &c->funcs[i];
        if (!H2NameEqSlice(c->src, f->nameStart, f->nameEnd, nameStart, nameEnd)) {
            continue;
        }
        if (f->contextType >= 0 || (f->flags & H2TCFunctionFlag_VARIADIC) != 0
            || f->paramCount != argCount || f->defNode < 0)
        {
            continue;
        }
        if (found >= 0) {
            return -2;
        }
        found = (int32_t)i;
    }
    return found;
}

static int32_t H2TCFindPkgConstCallableFunction(
    H2TypeCheckCtx* c,
    uint32_t        pkgStart,
    uint32_t        pkgEnd,
    uint32_t        nameStart,
    uint32_t        nameEnd,
    uint32_t        argCount) {
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
        const H2TCFunction* f;
        int32_t             fnIndex = candidates[i];
        if (fnIndex < 0 || (uint32_t)fnIndex >= c->funcLen) {
            continue;
        }
        f = &c->funcs[(uint32_t)fnIndex];
        if (f->contextType >= 0 || (f->flags & H2TCFunctionFlag_VARIADIC) != 0
            || f->paramCount != argCount || f->defNode < 0)
        {
            continue;
        }
        if (found >= 0) {
            return -2;
        }
        found = fnIndex;
    }
    return found;
}

static int H2TCConstEvalPrepareInvokeCallContext(
    H2TCConstEvalCtx* evalCtx,
    int32_t           callNode,
    int32_t           calleeNode,
    int32_t           fnIndex,
    H2TCCallArgInfo*  outCallArgs,
    uint32_t*         outCallArgCount,
    H2TCCallBinding*  outBinding,
    uint32_t*         outPackParamNameStart,
    uint32_t*         outPackParamNameEnd) {
    H2TypeCheckCtx*     c;
    const H2TCFunction* fn;
    H2TCCallMapError    mapError;
    uint32_t            paramStart;
    uint32_t            argCount = 0;
    if (outCallArgCount != NULL) {
        *outCallArgCount = 0;
    }
    if (outPackParamNameStart != NULL) {
        *outPackParamNameStart = 0;
    }
    if (outPackParamNameEnd != NULL) {
        *outPackParamNameEnd = 0;
    }
    if (evalCtx == NULL || outCallArgs == NULL || outCallArgCount == NULL || outBinding == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || fnIndex < 0 || (uint32_t)fnIndex >= c->funcLen || callNode < 0
        || (uint32_t)callNode >= c->ast->len || calleeNode < 0
        || (uint32_t)calleeNode >= c->ast->len)
    {
        return 1;
    }
    fn = &c->funcs[(uint32_t)fnIndex];
    paramStart = fn->paramTypeStart;
    if (H2TCCollectCallArgInfo(c, callNode, calleeNode, 0, -1, outCallArgs, NULL, &argCount) != 0) {
        return -1;
    }
    H2TCCallMapErrorClear(&mapError);
    if (H2TCPrepareCallBinding(
            c,
            outCallArgs,
            argCount,
            &c->funcParamNameStarts[paramStart],
            &c->funcParamNameEnds[paramStart],
            &c->funcParamTypes[paramStart],
            fn->paramCount,
            (fn->flags & H2TCFunctionFlag_VARIADIC) != 0,
            1,
            0,
            outBinding,
            &mapError)
        != 0)
    {
        return 1;
    }
    *outCallArgCount = argCount;
    if ((fn->flags & H2TCFunctionFlag_VARIADIC) != 0 && fn->paramCount > 0u) {
        if (outPackParamNameStart != NULL) {
            *outPackParamNameStart = c->funcParamNameStarts[paramStart + fn->paramCount - 1u];
        }
        if (outPackParamNameEnd != NULL) {
            *outPackParamNameEnd = c->funcParamNameEnds[paramStart + fn->paramCount - 1u];
        }
    }
    return 0;
}

static int H2TCInvokeConstFunctionByIndex(
    H2TCConstEvalCtx* evalCtx,
    uint32_t          nameStart,
    uint32_t          nameEnd,
    int32_t           fnIndex,
    const H2CTFEValue* _Nullable args,
    uint32_t argCount,
    const H2TCCallArgInfo* _Nullable callArgs,
    uint32_t callArgCount,
    const H2TCCallBinding* _Nullable callBinding,
    uint32_t     callPackParamNameStart,
    uint32_t     callPackParamNameEnd,
    H2CTFEValue* outValue,
    int*         outIsConst) {
    H2TypeCheckCtx*    c;
    int32_t            fnNode;
    int32_t            bodyNode = -1;
    int32_t            child;
    uint32_t           paramCount = 0;
    H2CTFEExecBinding* paramBindings = NULL;
    H2CTFEExecEnv      paramFrame;
    H2CTFEExecCtx      execCtx;
    H2CTFEExecCtx*     savedExecCtx;
    H2TCConstEvalCtx*  savedActiveConstEvalCtx;
    const void*        savedCallArgs;
    uint32_t           savedCallArgCount;
    const void*        savedCallBinding;
    int32_t            savedCallFnIndex;
    uint32_t           savedCallPackParamNameStart;
    uint32_t           savedCallPackParamNameEnd;
    uint32_t           savedCallFrameDepth;
    const H2CTFEValue* invokeArgs = args;
    H2CTFEValue        reorderedArgs[H2TC_MAX_CALL_ARGS];
    uint32_t           savedDepth;
    H2CTFEValue        retValue;
    int                didReturn = 0;
    int                isConst = 0;
    int                mirSupported = 0;
    int                rc;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || fnIndex < 0 || (uint32_t)fnIndex >= c->funcLen) {
        return -1;
    }

    H2TCMarkConstDiagFnInvoked(c, fnIndex);

    for (savedDepth = 0; savedDepth < evalCtx->fnDepth; savedDepth++) {
        if (evalCtx->fnStack[savedDepth] == fnIndex) {
            H2TCConstSetReason(
                evalCtx, nameStart, nameEnd, "recursive const function calls are not supported");
            *outIsConst = 0;
            return 0;
        }
    }
    if (evalCtx->fnDepth >= H2TC_CONST_CALL_MAX_DEPTH) {
        H2TCConstSetReason(evalCtx, nameStart, nameEnd, "const-eval call depth limit exceeded");
        *outIsConst = 0;
        return 0;
    }

    fnNode = c->funcs[fnIndex].defNode;
    if (fnNode < 0 || (uint32_t)fnNode >= c->ast->len || c->ast->nodes[fnNode].kind != H2Ast_FN) {
        H2TCConstSetReason(evalCtx, nameStart, nameEnd, "call target has no const-evaluable body");
        *outIsConst = 0;
        return 0;
    }

    child = H2AstFirstChild(c->ast, fnNode);
    while (child >= 0) {
        const H2AstNode* n = &c->ast->nodes[child];
        if (n->kind == H2Ast_PARAM) {
            if (paramCount >= argCount) {
                H2TCConstSetReasonNode(
                    evalCtx, fnNode, "function signature does not match const-eval call arguments");
                *outIsConst = 0;
                return 0;
            }
            paramCount++;
        } else if (n->kind == H2Ast_BLOCK) {
            if (bodyNode >= 0) {
                H2TCConstSetReasonNode(
                    evalCtx, fnNode, "function body shape is not const-evaluable");
                *outIsConst = 0;
                return 0;
            }
            bodyNode = child;
        }
        child = H2AstNextSibling(c->ast, child);
    }
    if (paramCount != argCount || bodyNode < 0) {
        H2TCConstSetReasonNode(evalCtx, fnNode, "function body shape is not const-evaluable");
        *outIsConst = 0;
        return 0;
    }
    if (argCount > 0 && args == NULL) {
        return -1;
    }
    if (callBinding != NULL && argCount > 0) {
        uint8_t  assigned[H2TC_MAX_CALL_ARGS];
        uint32_t i;
        if (argCount > H2TC_MAX_CALL_ARGS || callBinding->fixedCount != argCount
            || callBinding->fixedInputCount != argCount
            || callBinding->spreadArgIndex != UINT32_MAX)
        {
            return -1;
        }
        memset(assigned, 0, sizeof(assigned));
        for (i = 0; i < argCount; i++) {
            int32_t paramIndex = callBinding->argParamIndices[i];
            if (paramIndex < 0 || (uint32_t)paramIndex >= argCount
                || assigned[(uint32_t)paramIndex])
            {
                return -1;
            }
            reorderedArgs[(uint32_t)paramIndex] = args[i];
            assigned[(uint32_t)paramIndex] = 1;
        }
        invokeArgs = reorderedArgs;
    }

    if (argCount > 0) {
        paramBindings = (H2CTFEExecBinding*)H2ArenaAlloc(
            c->arena, sizeof(H2CTFEExecBinding) * argCount, (uint32_t)_Alignof(H2CTFEExecBinding));
        if (paramBindings == NULL) {
            return H2TCFailNode(c, fnNode, H2Diag_ARENA_OOM);
        }
    }
    paramCount = 0;
    child = H2AstFirstChild(c->ast, fnNode);
    while (child >= 0) {
        const H2AstNode* n = &c->ast->nodes[child];
        if (n->kind == H2Ast_PARAM) {
            if (paramBindings == NULL) {
                return -1;
            }
            int32_t paramTypeNode = H2AstFirstChild(c->ast, child);
            int32_t paramTypeId = -1;
            uint8_t savedAllowConstNumericTypeName = c->allowConstNumericTypeName;
            c->allowConstNumericTypeName = 1;
            if (paramTypeNode < 0 || H2TCResolveTypeNode(c, paramTypeNode, &paramTypeId) != 0) {
                c->allowConstNumericTypeName = savedAllowConstNumericTypeName;
                return -1;
            }
            c->allowConstNumericTypeName = savedAllowConstNumericTypeName;
            paramBindings[paramCount].nameStart = n->dataStart;
            paramBindings[paramCount].nameEnd = n->dataEnd;
            paramBindings[paramCount].typeId = paramTypeId;
            paramBindings[paramCount].mutable = 1;
            paramBindings[paramCount]._reserved[0] = 0;
            paramBindings[paramCount]._reserved[1] = 0;
            paramBindings[paramCount]._reserved[2] = 0;
            if (c->types[paramTypeId].kind == H2TCType_OPTIONAL) {
                H2CTFEValue wrapped;
                if (invokeArgs[paramCount].kind == H2CTFEValue_OPTIONAL) {
                    if (invokeArgs[paramCount].typeTag > 0
                        && invokeArgs[paramCount].typeTag <= (uint64_t)INT32_MAX
                        && (uint32_t)invokeArgs[paramCount].typeTag < c->typeLen
                        && (int32_t)invokeArgs[paramCount].typeTag == paramTypeId)
                    {
                        wrapped = invokeArgs[paramCount];
                    } else if (invokeArgs[paramCount].b == 0u) {
                        if (H2TCConstEvalSetOptionalNoneValue(c, paramTypeId, &wrapped) != 0) {
                            return -1;
                        }
                    } else if (invokeArgs[paramCount].s.bytes == NULL) {
                        return -1;
                    } else if (
                        H2TCConstEvalSetOptionalSomeValue(
                            c,
                            paramTypeId,
                            (const H2CTFEValue*)invokeArgs[paramCount].s.bytes,
                            &wrapped)
                        != 0)
                    {
                        return -1;
                    }
                } else if (invokeArgs[paramCount].kind == H2CTFEValue_NULL) {
                    if (H2TCConstEvalSetOptionalNoneValue(c, paramTypeId, &wrapped) != 0) {
                        return -1;
                    }
                } else if (
                    H2TCConstEvalSetOptionalSomeValue(
                        c, paramTypeId, &invokeArgs[paramCount], &wrapped)
                    != 0)
                {
                    return -1;
                }
                paramBindings[paramCount].value = wrapped;
            } else {
                paramBindings[paramCount].value = invokeArgs[paramCount];
            }
            paramCount++;
        }
        child = H2AstNextSibling(c->ast, child);
    }

    savedExecCtx = evalCtx->execCtx;
    savedActiveConstEvalCtx = c->activeConstEvalCtx;
    savedDepth = evalCtx->fnDepth;
    savedCallArgs = evalCtx->callArgs;
    savedCallArgCount = evalCtx->callArgCount;
    savedCallBinding = evalCtx->callBinding;
    savedCallFnIndex = evalCtx->callFnIndex;
    savedCallPackParamNameStart = evalCtx->callPackParamNameStart;
    savedCallPackParamNameEnd = evalCtx->callPackParamNameEnd;
    savedCallFrameDepth = evalCtx->callFrameDepth;

    paramFrame.parent = NULL;
    paramFrame.bindings = paramBindings;
    paramFrame.bindingLen = argCount;
    memset(&execCtx, 0, sizeof(execCtx));
    execCtx.arena = c->arena;
    execCtx.ast = c->ast;
    execCtx.src = c->src;
    execCtx.diag = c->diag;
    execCtx.env = &paramFrame;
    execCtx.evalExpr = H2TCEvalConstExecExprCb;
    execCtx.evalExprCtx = evalCtx;
    execCtx.resolveType = H2TCEvalConstExecResolveTypeCb;
    execCtx.resolveTypeCtx = evalCtx;
    execCtx.inferValueType = H2TCEvalConstExecInferValueTypeCb;
    execCtx.inferValueTypeCtx = evalCtx;
    execCtx.inferExprType = H2TCEvalConstExecInferExprTypeCb;
    execCtx.inferExprTypeCtx = evalCtx;
    execCtx.isOptionalType = H2TCEvalConstExecIsOptionalTypeCb;
    execCtx.isOptionalTypeCtx = evalCtx;
    execCtx.forInIndex = H2TCEvalConstForInIndexCb;
    execCtx.forInIndexCtx = evalCtx;
    execCtx.pendingReturnExprNode = -1;
    execCtx.forIterLimit = H2TC_CONST_FOR_MAX_ITERS;
    H2CTFEExecResetReason(&execCtx);
    evalCtx->execCtx = &execCtx;
    evalCtx->fnStack[evalCtx->fnDepth++] = fnIndex;
    if (evalCtx->callFrameDepth < H2TC_CONST_CALL_MAX_DEPTH) {
        uint32_t frameIndex = evalCtx->callFrameDepth++;
        evalCtx->callFrameArgs[frameIndex] = savedCallArgs;
        evalCtx->callFrameArgCounts[frameIndex] = savedCallArgCount;
        evalCtx->callFrameBindings[frameIndex] = savedCallBinding;
        evalCtx->callFrameFnIndices[frameIndex] = savedCallFnIndex;
        evalCtx->callFramePackParamNameStarts[frameIndex] = savedCallPackParamNameStart;
        evalCtx->callFramePackParamNameEnds[frameIndex] = savedCallPackParamNameEnd;
    }
    evalCtx->callArgs = callArgs;
    evalCtx->callArgCount = callArgCount;
    evalCtx->callBinding = callBinding;
    evalCtx->callFnIndex = fnIndex;
    evalCtx->callPackParamNameStart = callPackParamNameStart;
    evalCtx->callPackParamNameEnd = callPackParamNameEnd;
    evalCtx->nonConstReason = NULL;
    evalCtx->nonConstStart = 0;
    evalCtx->nonConstEnd = 0;
    evalCtx->nonConstTraceDepth = 0;
    c->activeConstEvalCtx = evalCtx;

    if (c->funcs[fnIndex].returnType == c->typeType) {
        rc = H2TCConstEvalTryDirectReturnFunction(evalCtx, bodyNode, &retValue, &isConst);
        if (rc <= 0) {
            evalCtx->fnDepth = savedDepth;
            evalCtx->execCtx = savedExecCtx;
            evalCtx->callArgs = savedCallArgs;
            evalCtx->callArgCount = savedCallArgCount;
            evalCtx->callBinding = savedCallBinding;
            evalCtx->callFnIndex = savedCallFnIndex;
            evalCtx->callPackParamNameStart = savedCallPackParamNameStart;
            evalCtx->callPackParamNameEnd = savedCallPackParamNameEnd;
            evalCtx->callFrameDepth = savedCallFrameDepth;
            c->activeConstEvalCtx = savedActiveConstEvalCtx;
            if (rc != 0) {
                return -1;
            }
            *outValue = retValue;
            *outIsConst = isConst;
            return 0;
        }
    }

    rc = H2TCTryMirConstCall(
        evalCtx, fnIndex, invokeArgs, argCount, &retValue, &didReturn, &isConst, &mirSupported);
    evalCtx->fnDepth = savedDepth;
    evalCtx->execCtx = savedExecCtx;
    evalCtx->callArgs = savedCallArgs;
    evalCtx->callArgCount = savedCallArgCount;
    evalCtx->callBinding = savedCallBinding;
    evalCtx->callFnIndex = savedCallFnIndex;
    evalCtx->callPackParamNameStart = savedCallPackParamNameStart;
    evalCtx->callPackParamNameEnd = savedCallPackParamNameEnd;
    evalCtx->callFrameDepth = savedCallFrameDepth;
    c->activeConstEvalCtx = savedActiveConstEvalCtx;
    if (rc != 0) {
        return -1;
    }
    if (!mirSupported || !isConst) {
        if (evalCtx->nonConstReason == NULL) {
            H2TCConstSetReasonNode(evalCtx, bodyNode, "function body is not const-evaluable");
        }
        *outIsConst = 0;
        return 0;
    }
    if (!didReturn) {
        if (c->funcs[fnIndex].returnType == c->typeVoid) {
            H2TCConstEvalSetNullValue(outValue);
            *outIsConst = 1;
            return 0;
        }
        if (execCtx.nonConstReason != NULL) {
            evalCtx->nonConstReason = execCtx.nonConstReason;
            evalCtx->nonConstStart = execCtx.nonConstStart;
            evalCtx->nonConstEnd = execCtx.nonConstEnd;
        }
        H2TCConstSetReasonNode(
            evalCtx, bodyNode, "const-evaluable function must produce a const return value");
        *outIsConst = 0;
        return 0;
    }
    if (fnIndex >= 0 && (uint32_t)fnIndex < c->funcLen) {
        int32_t returnTypeId = c->funcs[fnIndex].returnType;
        int32_t returnBaseTypeId = H2TCResolveAliasBaseType(c, returnTypeId);
        if (returnBaseTypeId >= 0 && (uint32_t)returnBaseTypeId < c->typeLen
            && c->types[returnBaseTypeId].kind == H2TCType_OPTIONAL)
        {
            H2CTFEValue wrapped;
            if (retValue.kind == H2CTFEValue_OPTIONAL) {
                if (retValue.typeTag > 0 && retValue.typeTag <= (uint64_t)INT32_MAX
                    && (uint32_t)retValue.typeTag < c->typeLen
                    && (int32_t)retValue.typeTag == returnBaseTypeId)
                {
                    wrapped = retValue;
                } else if (retValue.b == 0u) {
                    if (H2TCConstEvalSetOptionalNoneValue(c, returnBaseTypeId, &wrapped) != 0) {
                        return -1;
                    }
                } else if (retValue.s.bytes == NULL) {
                    return -1;
                } else if (
                    H2TCConstEvalSetOptionalSomeValue(
                        c, returnBaseTypeId, (const H2CTFEValue*)retValue.s.bytes, &wrapped)
                    != 0)
                {
                    return -1;
                }
            } else if (retValue.kind == H2CTFEValue_NULL) {
                if (H2TCConstEvalSetOptionalNoneValue(c, returnBaseTypeId, &wrapped) != 0) {
                    return -1;
                }
            } else if (
                H2TCConstEvalSetOptionalSomeValue(c, returnBaseTypeId, &retValue, &wrapped) != 0)
            {
                return -1;
            }
            retValue = wrapped;
        }
    }
    *outValue = retValue;
    *outIsConst = 1;
    return 0;
}

int H2TCResolveConstCallMir(
    void* _Nullable ctx,
    const H2MirProgram* _Nullable program,
    const H2MirFunction* _Nullable function,
    const H2MirInst* _Nullable inst,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    const H2CTFEValue* args,
    uint32_t           argCount,
    H2CTFEValue*       outValue,
    int*               outIsConst,
    H2Diag* _Nullable diag) {
    H2TCConstEvalCtx* evalCtx = (H2TCConstEvalCtx*)ctx;
    H2TypeCheckCtx*   c;
    int32_t           callNode = -1;
    int32_t           callCalleeNode = -1;
    int32_t           fnIndex;
    H2TCCallArgInfo   invokeCallArgs[H2TC_MAX_CALL_ARGS];
    H2TCCallBinding   invokeBinding;
    uint32_t          invokeCallArgCount = 0;
    uint32_t          invokePackParamNameStart = 0;
    uint32_t          invokePackParamNameEnd = 0;
    int               invokeHasCallContext = 0;

    (void)function;
    (void)diag;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL) {
        return -1;
    }
    if (H2TCMirConstResolveCallNode(evalCtx, program, inst, &callNode)) {
        if (callNode >= 0 && (uint32_t)callNode < c->ast->len) {
            callCalleeNode = H2AstFirstChild(c->ast, callNode);
        }
        int status = H2TCConstEvalLenCall(evalCtx, callNode, outValue, outIsConst);
        if (status == 0) {
            return 0;
        }
        if (status < 0) {
            return -1;
        }
        status = H2TCConstEvalCompilerDiagCall(evalCtx, callNode, outValue, outIsConst);
        if (status == 0) {
            return 0;
        }
        if (status < 0) {
            return -1;
        }
        status = H2TCConstEvalSourceLocationOfCall(evalCtx, callNode, outValue, outIsConst);
        if (status == 0) {
            return 0;
        }
        if (status < 0) {
            return -1;
        }
        status = H2TCConstEvalReflectIsConstCall(evalCtx, callNode, outValue, outIsConst);
        if (status == 0) {
            return 0;
        }
        if (status < 0) {
            return -1;
        }
        status = H2TCConstEvalTypeReflectionCall(evalCtx, callNode, outValue, outIsConst);
        if (status == 0) {
            return 0;
        }
        if (status < 0) {
            return -1;
        }
        if (callNode >= 0) {
            int32_t calleeNode = H2AstFirstChild(c->ast, callNode);
            if (calleeNode >= 0 && c->ast->nodes[calleeNode].kind == H2Ast_IDENT
                && H2NameEqLiteral(
                    c->src,
                    c->ast->nodes[calleeNode].dataStart,
                    c->ast->nodes[calleeNode].dataEnd,
                    "typeof"))
            {
                int32_t argNode = H2AstNextSibling(c->ast, calleeNode);
                int32_t argExprNode = argNode;
                if (argCount == 1u) {
                    int      isCurrentPackIndexExpr = 0;
                    uint32_t trackedAnyPackArgIndex = 0;
                    int32_t  argTypeId = -1;
                    int      packStatus = -1;
                    if (argExprNode >= 0 && (uint32_t)argExprNode < c->ast->len
                        && c->ast->nodes[argExprNode].kind == H2Ast_CALL_ARG)
                    {
                        int32_t innerArgExprNode = H2AstFirstChild(c->ast, argExprNode);
                        if (innerArgExprNode >= 0) {
                            argExprNode = innerArgExprNode;
                        }
                    }
                    if (argExprNode >= 0 && (uint32_t)argExprNode < c->ast->len
                        && c->ast->nodes[argExprNode].kind == H2Ast_INDEX
                        && (c->ast->nodes[argExprNode].flags & H2AstFlag_INDEX_SLICE) == 0u)
                    {
                        int32_t baseNode = H2AstFirstChild(c->ast, argExprNode);
                        if (baseNode >= 0 && (uint32_t)baseNode < c->ast->len
                            && c->ast->nodes[baseNode].kind == H2Ast_IDENT
                            && H2TCConstEvalIsTrackedAnyPackName(
                                evalCtx,
                                c->ast->nodes[baseNode].dataStart,
                                c->ast->nodes[baseNode].dataEnd))
                        {
                            isCurrentPackIndexExpr = 1;
                        }
                    }
                    if (isCurrentPackIndexExpr) {
                        return H2TCConstEvalTypeOf(evalCtx, callNode, outValue, outIsConst);
                    }
                    packStatus = H2TCConstEvalResolveTrackedAnyPackArgIndex(
                        evalCtx, argExprNode, &trackedAnyPackArgIndex);
                    if (packStatus < 0) {
                        return -1;
                    }
                    if (packStatus == 0 || packStatus == 2 || packStatus == 3) {
                        return H2TCConstEvalTypeOf(evalCtx, callNode, outValue, outIsConst);
                    }
                    if (H2TCEvalConstExecInferValueTypeCb(evalCtx, &args[0], &argTypeId) == 0) {
                        outValue->kind = H2CTFEValue_TYPE;
                        outValue->i64 = 0;
                        outValue->f64 = 0.0;
                        outValue->b = 0;
                        outValue->typeTag = H2TCEncodeTypeTag(c, argTypeId);
                        outValue->s.bytes = NULL;
                        outValue->s.len = 0;
                        outValue->span.fileBytes = NULL;
                        outValue->span.fileLen = 0;
                        outValue->span.startLine = 0;
                        outValue->span.startColumn = 0;
                        outValue->span.endLine = 0;
                        outValue->span.endColumn = 0;
                        *outIsConst = 1;
                        return 0;
                    }
                }
                return H2TCConstEvalTypeOf(evalCtx, callNode, outValue, outIsConst);
            }
        }
    }

    if (H2NameEqLiteral(c->src, nameStart, nameEnd, "typeof")) {
        if (argCount == 1u) {
            int32_t argTypeId = -1;
            if (H2TCEvalConstExecInferValueTypeCb(evalCtx, &args[0], &argTypeId) == 0) {
                if ((argTypeId == c->typeUntypedInt || argTypeId == c->typeUntypedFloat)
                    && H2TCConcretizeInferredType(c, argTypeId, &argTypeId) != 0)
                {
                    return -1;
                }
                outValue->kind = H2CTFEValue_TYPE;
                outValue->i64 = 0;
                outValue->f64 = 0.0;
                outValue->b = 0;
                outValue->typeTag = H2TCEncodeTypeTag(c, argTypeId);
                outValue->s.bytes = NULL;
                outValue->s.len = 0;
                outValue->span.fileBytes = NULL;
                outValue->span.fileLen = 0;
                outValue->span.startLine = 0;
                outValue->span.startColumn = 0;
                outValue->span.endLine = 0;
                outValue->span.endColumn = 0;
                *outIsConst = 1;
                return 0;
            }
        }
        H2TCConstSetReason(evalCtx, nameStart, nameEnd, "typeof() operand is not const-evaluable");
        *outIsConst = 0;
        return 0;
    }

    {
        int reflectArgStatus = H2TCConstEvalTypeReflectionByArgs(
            evalCtx, nameStart, nameEnd, args, argCount, outValue, outIsConst);
        if (reflectArgStatus == 0) {
            return 0;
        }
        if (reflectArgStatus < 0) {
            return -1;
        }
    }

    if (H2NameEqLiteral(c->src, nameStart, nameEnd, "is_const")) {
        if (argCount == 1u) {
            H2TCConstEvalSetBoolValue(outValue, 1);
            *outIsConst = 1;
            return 0;
        }
    }

    if (args != NULL && argCount > 0 && args[0].kind == H2CTFEValue_SPAN
        && args[0].typeTag == H2_TC_MIR_IMPORT_ALIAS_TAG && args[0].span.fileBytes != NULL
        && args[0].span.fileLen > 0)
    {
        const uint8_t* srcBytes = (const uint8_t*)c->src.ptr;
        const uint8_t* aliasPtr = args[0].span.fileBytes;
        uint32_t       aliasStart;
        uint32_t       aliasEnd;
        if (aliasPtr >= srcBytes && (uint64_t)(aliasPtr - srcBytes) <= UINT32_MAX) {
            aliasStart = (uint32_t)(aliasPtr - srcBytes);
            aliasEnd = aliasStart + args[0].span.fileLen;
            if (aliasEnd <= c->src.len) {
                fnIndex = H2TCFindPkgConstCallableFunction(
                    c, aliasStart, aliasEnd, nameStart, nameEnd, argCount - 1u);
                if (fnIndex >= 0) {
                    H2TCConstEvalRememberRootCallNode(evalCtx, callNode, callCalleeNode);
                    return H2TCInvokeConstFunctionByIndex(
                        evalCtx,
                        nameStart,
                        nameEnd,
                        fnIndex,
                        args + 1u,
                        argCount - 1u,
                        NULL,
                        0u,
                        NULL,
                        0u,
                        0u,
                        outValue,
                        outIsConst);
                }
            }
        }
    }

    {
        H2CTFEValue calleeValue;
        int         calleeIsConst = 0;
        uint32_t    calleeFnIndex = UINT32_MAX;
        const char* savedReason = evalCtx->nonConstReason;
        uint32_t    savedStart = evalCtx->nonConstStart;
        uint32_t    savedEnd = evalCtx->nonConstEnd;
        if (H2TCResolveConstIdent(
                evalCtx, nameStart, nameEnd, &calleeValue, &calleeIsConst, c->diag)
            != 0)
        {
            return -1;
        }
        if (calleeIsConst && H2MirValueAsFunctionRef(&calleeValue, &calleeFnIndex)
            && calleeFnIndex < c->funcLen)
        {
            const H2TCFunction* fn = &c->funcs[calleeFnIndex];
            if (callNode >= 0 && callCalleeNode >= 0
                && H2TCConstEvalPrepareInvokeCallContext(
                       evalCtx,
                       callNode,
                       callCalleeNode,
                       (int32_t)calleeFnIndex,
                       invokeCallArgs,
                       &invokeCallArgCount,
                       &invokeBinding,
                       &invokePackParamNameStart,
                       &invokePackParamNameEnd)
                       == 0)
            {
                invokeHasCallContext = 1;
            }
            H2TCConstEvalRememberRootCallNode(evalCtx, callNode, callCalleeNode);
            return H2TCInvokeConstFunctionByIndex(
                evalCtx,
                fn->nameStart,
                fn->nameEnd,
                (int32_t)calleeFnIndex,
                args,
                argCount,
                invokeHasCallContext ? invokeCallArgs : NULL,
                invokeHasCallContext ? invokeCallArgCount : 0u,
                invokeHasCallContext ? &invokeBinding : NULL,
                invokeHasCallContext ? invokePackParamNameStart : 0u,
                invokeHasCallContext ? invokePackParamNameEnd : 0u,
                outValue,
                outIsConst);
        }
        evalCtx->nonConstReason = savedReason;
        evalCtx->nonConstStart = savedStart;
        evalCtx->nonConstEnd = savedEnd;
    }

    if (H2NameEqLiteral(c->src, nameStart, nameEnd, "len")) {
        if (argCount == 1u) {
            if (args[0].kind == H2CTFEValue_STRING) {
                outValue->kind = H2CTFEValue_INT;
                outValue->i64 = (int64_t)args[0].s.len;
                outValue->f64 = 0.0;
                outValue->b = 0;
                outValue->typeTag = 0;
                outValue->s.bytes = NULL;
                outValue->s.len = 0;
                outValue->span.fileBytes = NULL;
                outValue->span.fileLen = 0;
                outValue->span.startLine = 0;
                outValue->span.startColumn = 0;
                outValue->span.endLine = 0;
                outValue->span.endColumn = 0;
                *outIsConst = 1;
                return 0;
            }
            if (args[0].kind == H2CTFEValue_TYPE) {
                int32_t typeId = -1;
                int32_t baseType;
                if (H2TCDecodeTypeTag(c, args[0].typeTag, &typeId) == 0) {
                    baseType = H2TCResolveAliasBaseType(c, typeId);
                    if (baseType >= 0 && (uint32_t)baseType < c->typeLen) {
                        const H2TCType* t = &c->types[baseType];
                        if (t->kind == H2TCType_PACK) {
                            outValue->kind = H2CTFEValue_INT;
                            outValue->i64 = (int64_t)t->fieldCount;
                            outValue->f64 = 0.0;
                            outValue->b = 0;
                            outValue->typeTag = 0;
                            outValue->s.bytes = NULL;
                            outValue->s.len = 0;
                            outValue->span.fileBytes = NULL;
                            outValue->span.fileLen = 0;
                            outValue->span.startLine = 0;
                            outValue->span.startColumn = 0;
                            outValue->span.endLine = 0;
                            outValue->span.endColumn = 0;
                            *outIsConst = 1;
                            return 0;
                        }
                        if (t->kind == H2TCType_ARRAY) {
                            outValue->kind = H2CTFEValue_INT;
                            outValue->i64 = (int64_t)t->arrayLen;
                            outValue->f64 = 0.0;
                            outValue->b = 0;
                            outValue->typeTag = 0;
                            outValue->s.bytes = NULL;
                            outValue->s.len = 0;
                            outValue->span.fileBytes = NULL;
                            outValue->span.fileLen = 0;
                            outValue->span.startLine = 0;
                            outValue->span.startColumn = 0;
                            outValue->span.endLine = 0;
                            outValue->span.endColumn = 0;
                            *outIsConst = 1;
                            return 0;
                        }
                    }
                }
            }
        }
        H2TCConstSetReason(evalCtx, nameStart, nameEnd, "len() operand is not const-evaluable");
        *outIsConst = 0;
        return 0;
    }

    fnIndex = H2TCFindConstCallableFunction(c, nameStart, nameEnd, argCount);
    if (fnIndex < 0) {
        H2TCConstSetReason(
            evalCtx,
            nameStart,
            nameEnd,
            "call target is not a const-evaluable function for these arguments");
        *outIsConst = 0;
        return 0;
    }
    if (callNode >= 0 && callCalleeNode >= 0
        && H2TCConstEvalPrepareInvokeCallContext(
               evalCtx,
               callNode,
               callCalleeNode,
               fnIndex,
               invokeCallArgs,
               &invokeCallArgCount,
               &invokeBinding,
               &invokePackParamNameStart,
               &invokePackParamNameEnd)
               == 0)
    {
        invokeHasCallContext = 1;
    }
    H2TCConstEvalRememberRootCallNode(evalCtx, callNode, callCalleeNode);
    return H2TCInvokeConstFunctionByIndex(
        evalCtx,
        nameStart,
        nameEnd,
        fnIndex,
        args,
        argCount,
        invokeHasCallContext ? invokeCallArgs : NULL,
        invokeHasCallContext ? invokeCallArgCount : 0u,
        invokeHasCallContext ? &invokeBinding : NULL,
        invokeHasCallContext ? invokePackParamNameStart : 0u,
        invokeHasCallContext ? invokePackParamNameEnd : 0u,
        outValue,
        outIsConst);
}

int H2TCResolveConstCall(
    void*    ctx,
    uint32_t nameStart,
    uint32_t nameEnd,
    const H2CTFEValue* _Nullable args,
    uint32_t     argCount,
    H2CTFEValue* outValue,
    int*         outIsConst,
    H2Diag* _Nullable diag) {
    return H2TCResolveConstCallMir(
        ctx, NULL, NULL, NULL, nameStart, nameEnd, args, argCount, outValue, outIsConst, diag);
}

static int H2TCConstEvalDirectCall(
    H2TCConstEvalCtx* evalCtx, int32_t exprNode, H2CTFEValue* outValue, int* outIsConst) {
    H2TypeCheckCtx*  c;
    int32_t          calleeNode;
    const H2AstNode* callee;
    H2CTFEValue      calleeValue;
    int              calleeIsConst = 0;
    uint32_t         calleeFnIndex = UINT32_MAX;
    int32_t          argNode;
    uint32_t         argCount = 0;
    uint32_t         argIndex = 0;
    H2CTFEValue*     argValues = NULL;
    H2TCCallArgInfo  invokeCallArgs[H2TC_MAX_CALL_ARGS];
    H2TCCallBinding  invokeBinding;
    uint32_t         invokeCallArgCount = 0;
    uint32_t         invokePackParamNameStart = 0;
    uint32_t         invokePackParamNameEnd = 0;
    int              invokeHasCallContext = 0;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return -1;
    }
    calleeNode = H2AstFirstChild(c->ast, exprNode);
    callee = calleeNode >= 0 ? &c->ast->nodes[calleeNode] : NULL;
    if (callee == NULL || callee->kind != H2Ast_IDENT) {
        return 1;
    }

    argNode = H2AstNextSibling(c->ast, calleeNode);
    while (argNode >= 0) {
        argCount++;
        argNode = H2AstNextSibling(c->ast, argNode);
    }
    if (argCount > 0) {
        argValues = (H2CTFEValue*)H2ArenaAlloc(
            c->arena, sizeof(H2CTFEValue) * argCount, (uint32_t)_Alignof(H2CTFEValue));
        if (argValues == NULL) {
            return H2TCFailNode(c, exprNode, H2Diag_ARENA_OOM);
        }
    }

    argNode = H2AstNextSibling(c->ast, calleeNode);
    while (argNode >= 0) {
        int32_t exprArgNode = argNode;
        int     argIsConst = 0;
        if (argIndex >= argCount) {
            return -1;
        }
        if (c->ast->nodes[argNode].kind == H2Ast_CALL_ARG) {
            exprArgNode = H2AstFirstChild(c->ast, argNode);
            if (exprArgNode < 0) {
                return -1;
            }
        }
        if (H2TCEvalConstExprNode(evalCtx, exprArgNode, &argValues[argIndex], &argIsConst) != 0) {
            return -1;
        }
        if (!argIsConst) {
            *outIsConst = 0;
            return 0;
        }
        argIndex++;
        argNode = H2AstNextSibling(c->ast, argNode);
    }

    if (H2TCResolveConstIdent(
            evalCtx, callee->dataStart, callee->dataEnd, &calleeValue, &calleeIsConst, c->diag)
        != 0)
    {
        return -1;
    }
    if (calleeIsConst && H2MirValueAsFunctionRef(&calleeValue, &calleeFnIndex)
        && calleeFnIndex < c->funcLen)
    {
        const H2TCFunction* fn = &c->funcs[calleeFnIndex];
        if (H2TCConstEvalPrepareInvokeCallContext(
                evalCtx,
                exprNode,
                calleeNode,
                (int32_t)calleeFnIndex,
                invokeCallArgs,
                &invokeCallArgCount,
                &invokeBinding,
                &invokePackParamNameStart,
                &invokePackParamNameEnd)
            == 0)
        {
            invokeHasCallContext = 1;
        }
        H2TCConstEvalRememberRootCallNode(evalCtx, exprNode, calleeNode);
        return H2TCInvokeConstFunctionByIndex(
            evalCtx,
            fn->nameStart,
            fn->nameEnd,
            (int32_t)calleeFnIndex,
            argValues,
            argCount,
            invokeHasCallContext ? invokeCallArgs : NULL,
            invokeHasCallContext ? invokeCallArgCount : 0u,
            invokeHasCallContext ? &invokeBinding : NULL,
            invokeHasCallContext ? invokePackParamNameStart : 0u,
            invokeHasCallContext ? invokePackParamNameEnd : 0u,
            outValue,
            outIsConst);
    }

    return H2TCResolveConstCall(
        evalCtx,
        callee->dataStart,
        callee->dataEnd,
        argValues,
        argCount,
        outValue,
        outIsConst,
        c->diag);
}

int H2TCEvalTopLevelConstNodeAt(
    H2TypeCheckCtx*   c,
    H2TCConstEvalCtx* evalCtx,
    int32_t           nodeId,
    int32_t           nameIndex,
    H2CTFEValue*      outValue,
    int*              outIsConst) {
    uint8_t          state;
    int32_t          initNode;
    int              isConst = 0;
    int              mirSupported = 0;
    H2TCVarLikeParts parts;
    if (c == NULL || evalCtx == NULL || outValue == NULL || outIsConst == NULL || nodeId < 0
        || (uint32_t)nodeId >= c->ast->len)
    {
        return -1;
    }
    if (c->constEvalState == NULL || c->constEvalValues == NULL) {
        *outIsConst = 0;
        return 0;
    }
    if (H2TCVarLikeGetParts(c, nodeId, &parts) != 0 || parts.nameCount == 0 || nameIndex < 0
        || (uint32_t)nameIndex >= parts.nameCount)
    {
        *outIsConst = 0;
        return 0;
    }

    state = c->constEvalState[nodeId];
    if (state == H2TCConstEval_READY) {
        *outValue = c->constEvalValues[nodeId];
        *outIsConst = 1;
        return 0;
    }
    if (state == H2TCConstEval_NONCONST || state == H2TCConstEval_VISITING) {
        if (state == H2TCConstEval_VISITING) {
            H2TCConstSetReasonNode(
                evalCtx, nodeId, "cyclic const dependency is not supported in const evaluation");
        }
        *outIsConst = 0;
        return 0;
    }

    c->constEvalState[nodeId] = H2TCConstEval_VISITING;
    initNode = H2TCVarLikeInitExprNodeAt(c, nodeId, nameIndex);
    if (initNode < 0) {
        if (H2TCHasForeignImportDirective(c->ast, c->src, nodeId)) {
            c->constEvalState[nodeId] = H2TCConstEval_NONCONST;
            *outIsConst = 0;
            return 0;
        }
        H2TCConstSetReasonNode(evalCtx, nodeId, "const declaration is missing an initializer");
        c->constEvalState[nodeId] = H2TCConstEval_NONCONST;
        *outIsConst = 0;
        return 0;
    }
    evalCtx->nonConstReason = NULL;
    evalCtx->nonConstStart = 0;
    evalCtx->nonConstEnd = 0;
    evalCtx->nonConstTraceDepth = 0;
    evalCtx->rootCallOwnerFnIndex = -1;
    evalCtx->rootCallStart = 0;
    if (H2TCTryMirTopLevelConst(evalCtx, nodeId, nameIndex, outValue, &isConst, &mirSupported) != 0)
    {
        c->constEvalState[nodeId] = H2TCConstEval_UNSEEN;
        return -1;
    }
    if (mirSupported) {
        if (!isConst) {
            if (evalCtx->nonConstReason == NULL) {
                H2TCConstSetReasonNode(
                    evalCtx, initNode, "const initializer is not const-evaluable");
            }
            c->constEvalState[nodeId] = H2TCConstEval_NONCONST;
            *outIsConst = 0;
            return 0;
        }
        if (!parts.grouped) {
            c->constEvalValues[nodeId] = *outValue;
            c->constEvalState[nodeId] = H2TCConstEval_READY;
        } else {
            c->constEvalState[nodeId] = H2TCConstEval_UNSEEN;
        }
        *outIsConst = 1;
        return 0;
    }
    if (H2TCEvalConstExprNode(evalCtx, initNode, outValue, &isConst) != 0) {
        c->constEvalState[nodeId] = H2TCConstEval_UNSEEN;
        return -1;
    }
    if (!isConst) {
        H2TCConstSetReasonNode(evalCtx, initNode, "const initializer is not const-evaluable");
        c->constEvalState[nodeId] = H2TCConstEval_NONCONST;
        *outIsConst = 0;
        return 0;
    }
    if (!parts.grouped) {
        c->constEvalValues[nodeId] = *outValue;
        c->constEvalState[nodeId] = H2TCConstEval_READY;
    } else {
        c->constEvalState[nodeId] = H2TCConstEval_UNSEEN;
    }
    *outIsConst = 1;
    return 0;
}

int H2TCEvalTopLevelConstNode(
    H2TypeCheckCtx*   c,
    H2TCConstEvalCtx* evalCtx,
    int32_t           nodeId,
    H2CTFEValue*      outValue,
    int*              outIsConst) {
    return H2TCEvalTopLevelConstNodeAt(c, evalCtx, nodeId, 0, outValue, outIsConst);
}

int H2TCConstBoolExpr(H2TypeCheckCtx* c, int32_t nodeId, int* out, int* isConst) {
    H2TCConstEvalCtx  evalCtxStorage;
    H2TCConstEvalCtx* evalCtx = c->activeConstEvalCtx;
    H2CTFEValue       value;
    int               valueIsConst = 0;
    const H2AstNode*  n;
    *isConst = 0;
    *out = 0;
    H2TCClearLastConstEvalReason(c);
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return -1;
    }
    n = &c->ast->nodes[nodeId];
    if (n->kind == H2Ast_UNARY && (H2TokenKind)n->op == H2Tok_NOT) {
        int32_t rhsNode = H2AstFirstChild(c->ast, nodeId);
        int     rhsValue = 0;
        int     rhsIsConst = 0;
        if (rhsNode < 0) {
            return -1;
        }
        if (H2TCConstBoolExpr(c, rhsNode, &rhsValue, &rhsIsConst) != 0) {
            return -1;
        }
        if (rhsIsConst) {
            *out = rhsValue ? 0 : 1;
            *isConst = 1;
        }
        return 0;
    }
    if (n->kind == H2Ast_BINARY
        && ((H2TokenKind)n->op == H2Tok_EQ || (H2TokenKind)n->op == H2Tok_NEQ))
    {
        int32_t lhsNode = H2AstFirstChild(c->ast, nodeId);
        int32_t rhsNode = lhsNode >= 0 ? H2AstNextSibling(c->ast, lhsNode) : -1;
        int32_t extraNode = rhsNode >= 0 ? H2AstNextSibling(c->ast, rhsNode) : -1;
        int32_t lhsTypeId = -1;
        int32_t rhsTypeId = -1;
        int     lhsStatus;
        int     rhsStatus;
        if (lhsNode < 0 || rhsNode < 0 || extraNode >= 0) {
            return -1;
        }
        lhsStatus = H2TCResolveReflectedTypeValueExpr(c, lhsNode, &lhsTypeId);
        if (lhsStatus < 0) {
            return -1;
        }
        rhsStatus = H2TCResolveReflectedTypeValueExpr(c, rhsNode, &rhsTypeId);
        if (rhsStatus < 0) {
            return -1;
        }
        if (lhsStatus == 0 && rhsStatus == 0) {
            *out = (((H2TokenKind)n->op == H2Tok_EQ)
                        ? (lhsTypeId == rhsTypeId)
                        : (lhsTypeId != rhsTypeId))
                     ? 1
                     : 0;
            *isConst = 1;
            return 0;
        }
    }
    if (evalCtx != NULL) {
        evalCtxStorage = *evalCtx;
        evalCtxStorage.tc = c;
        evalCtxStorage.nonConstReason = NULL;
        evalCtxStorage.nonConstStart = 0;
        evalCtxStorage.nonConstEnd = 0;
        evalCtxStorage.nonConstTraceDepth = 0;
        evalCtx = &evalCtxStorage;
    } else {
        memset(&evalCtxStorage, 0, sizeof(evalCtxStorage));
        evalCtxStorage.tc = c;
        evalCtxStorage.rootCallOwnerFnIndex = -1;
        evalCtx = &evalCtxStorage;
    }
    if (H2TCEvalConstExprNode(evalCtx, nodeId, &value, &valueIsConst) != 0) {
        return -1;
    }
    H2TCStoreLastConstEvalReason(c, evalCtx);
    if (!valueIsConst) {
        return 0;
    }
    if (value.kind == H2CTFEValue_OPTIONAL) {
        *out = value.b != 0u ? 1 : 0;
        *isConst = 1;
        return 0;
    }
    if (value.kind != H2CTFEValue_BOOL) {
        H2TCSetLastConstEvalReason(
            c,
            "expression evaluated to a non-boolean value",
            c->ast->nodes[nodeId].start,
            c->ast->nodes[nodeId].end);
        return 0;
    }
    *out = value.b ? 1 : 0;
    *isConst = 1;
    return 0;
}

int H2TCConstIntExpr(H2TypeCheckCtx* c, int32_t nodeId, int64_t* out, int* isConst) {
    H2TCConstEvalCtx  evalCtxStorage;
    H2TCConstEvalCtx* evalCtx = c->activeConstEvalCtx;
    H2CTFEValue       value;
    int               valueIsConst = 0;
    *isConst = 0;
    H2TCClearLastConstEvalReason(c);
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return -1;
    }
    if (evalCtx != NULL) {
        evalCtxStorage = *evalCtx;
        evalCtxStorage.tc = c;
        evalCtxStorage.nonConstReason = NULL;
        evalCtxStorage.nonConstStart = 0;
        evalCtxStorage.nonConstEnd = 0;
        evalCtxStorage.nonConstTraceDepth = 0;
        evalCtx = &evalCtxStorage;
    } else {
        memset(&evalCtxStorage, 0, sizeof(evalCtxStorage));
        evalCtxStorage.tc = c;
        evalCtxStorage.rootCallOwnerFnIndex = -1;
        evalCtx = &evalCtxStorage;
    }
    if (H2TCEvalConstExprNode(evalCtx, nodeId, &value, &valueIsConst) != 0) {
        return -1;
    }
    H2TCStoreLastConstEvalReason(c, evalCtx);
    if (!valueIsConst) {
        return 0;
    }
    if (H2CTFEValueToInt64(&value, out) != 0) {
        H2TCSetLastConstEvalReason(
            c,
            "expression evaluated to a non-integer value",
            c->ast->nodes[nodeId].start,
            c->ast->nodes[nodeId].end);
        return 0;
    }
    *isConst = 1;
    return 0;
}

int H2TCConstFloatExpr(H2TypeCheckCtx* c, int32_t nodeId, double* out, int* isConst) {
    H2TCConstEvalCtx evalCtx;
    H2CTFEValue      value;
    int              valueIsConst = 0;
    *isConst = 0;
    H2TCClearLastConstEvalReason(c);
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return -1;
    }
    memset(&evalCtx, 0, sizeof(evalCtx));
    evalCtx.tc = c;
    evalCtx.rootCallOwnerFnIndex = -1;
    if (H2TCEvalConstExprNode(&evalCtx, nodeId, &value, &valueIsConst) != 0) {
        return -1;
    }
    H2TCStoreLastConstEvalReason(c, &evalCtx);
    if (!valueIsConst) {
        return 0;
    }
    if (value.kind == H2CTFEValue_FLOAT) {
        *out = value.f64;
        *isConst = 1;
        return 0;
    }
    if (value.kind == H2CTFEValue_INT) {
        *out = (double)value.i64;
        *isConst = 1;
        return 0;
    }
    H2TCSetLastConstEvalReason(
        c,
        "expression evaluated to a non-float value",
        c->ast->nodes[nodeId].start,
        c->ast->nodes[nodeId].end);
    return 0;
}

int H2TCConstStringExpr(
    H2TypeCheckCtx* c,
    int32_t         nodeId,
    const uint8_t** outBytes,
    uint32_t*       outLen,
    int*            outIsConst) {
    const H2AstNode* node;
    H2TCConstEvalCtx evalCtx;
    H2CTFEValue      value;
    int              valueIsConst = 0;
    if (outBytes == NULL || outLen == NULL || outIsConst == NULL) {
        return -1;
    }
    *outBytes = NULL;
    *outLen = 0;
    *outIsConst = 0;
    H2TCClearLastConstEvalReason(c);
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return -1;
    }
    node = &c->ast->nodes[nodeId];
    while (node->kind == H2Ast_CALL_ARG) {
        nodeId = H2AstFirstChild(c->ast, nodeId);
        if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
            return -1;
        }
        node = &c->ast->nodes[nodeId];
    }
    memset(&evalCtx, 0, sizeof(evalCtx));
    evalCtx.tc = c;
    evalCtx.rootCallOwnerFnIndex = -1;
    if (H2TCEvalConstExprNode(&evalCtx, nodeId, &value, &valueIsConst) != 0) {
        return -1;
    }
    H2TCStoreLastConstEvalReason(c, &evalCtx);
    if (!valueIsConst || value.kind != H2CTFEValue_STRING) {
        return 0;
    }
    *outBytes = value.s.bytes;
    *outLen = value.s.len;
    *outIsConst = 1;
    return 0;
}

void H2TCMarkRuntimeBoundsCheck(H2TypeCheckCtx* c, int32_t nodeId) {
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return;
    }
    ((H2AstNode*)&c->ast->nodes[nodeId])->flags |= H2AstFlag_INDEX_RUNTIME_BOUNDS;
}

H2_API_END
