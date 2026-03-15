SL_API_BEGIN

enum {
    SL_EVAL_MIR_FN_NONE = UINT32_MAX,
};

typedef struct {
    SLEvalProgram*       p;
    SLMirProgramBuilder  builder;
    uint32_t*            evalToMir;
    uint8_t*             loweringFns;
    uint32_t*            topConstToMir;
    uint8_t*             loweringTopConsts;
    uint32_t*            topVarToMir;
    uint8_t*             loweringTopVars;
    SLEvalMirExecCtx     execCtx;
    SLDiag*              diag;
} SLEvalMirLowerCtx;

static int SLEvalMirInitLowerCtx(
    SLEvalProgram*      p,
    uint32_t            extraMirFuncs,
    SLEvalMirLowerCtx* _Nonnull outCtx) {
    uint32_t*            evalToMir;
    uint8_t*             loweringFns;
    uint32_t*            topConstToMir;
    uint8_t*             loweringTopConsts;
    uint32_t*            topVarToMir;
    uint8_t*             loweringTopVars;
    uint32_t*            mirToEval;
    const SLParsedFile** sourceFiles;
    uint32_t             i;
    uint32_t             mirCap;
    if (p == NULL || outCtx == NULL) {
        return -1;
    }
    memset(outCtx, 0, sizeof(*outCtx));
    mirCap = p->funcLen + p->topConstLen + p->topVarLen + extraMirFuncs;
    if (mirCap < p->funcLen || mirCap < p->topConstLen || mirCap < p->topVarLen) {
        return -1;
    }
    evalToMir = (uint32_t*)SLArenaAlloc(
        p->arena, sizeof(uint32_t) * p->funcLen, (uint32_t)_Alignof(uint32_t));
    loweringFns = (uint8_t*)SLArenaAlloc(
        p->arena, sizeof(uint8_t) * p->funcLen, (uint32_t)_Alignof(uint8_t));
    topConstToMir = (uint32_t*)SLArenaAlloc(
        p->arena, sizeof(uint32_t) * p->topConstLen, (uint32_t)_Alignof(uint32_t));
    loweringTopConsts = (uint8_t*)SLArenaAlloc(
        p->arena, sizeof(uint8_t) * p->topConstLen, (uint32_t)_Alignof(uint8_t));
    topVarToMir = (uint32_t*)SLArenaAlloc(
        p->arena, sizeof(uint32_t) * p->topVarLen, (uint32_t)_Alignof(uint32_t));
    loweringTopVars = (uint8_t*)SLArenaAlloc(
        p->arena, sizeof(uint8_t) * p->topVarLen, (uint32_t)_Alignof(uint8_t));
    mirToEval = (uint32_t*)SLArenaAlloc(
        p->arena, sizeof(uint32_t) * mirCap, (uint32_t)_Alignof(uint32_t));
    sourceFiles = (const SLParsedFile**)SLArenaAlloc(
        p->arena, sizeof(const SLParsedFile*) * mirCap, (uint32_t)_Alignof(const SLParsedFile*));
    if (evalToMir == NULL || loweringFns == NULL || topConstToMir == NULL
        || loweringTopConsts == NULL || topVarToMir == NULL || loweringTopVars == NULL
        || mirToEval == NULL || sourceFiles == NULL)
    {
        return ErrorSimple("out of memory");
    }
    for (i = 0; i < p->funcLen; i++) {
        evalToMir[i] = SL_EVAL_MIR_FN_NONE;
        loweringFns[i] = 0u;
    }
    for (i = 0; i < p->topConstLen; i++) {
        topConstToMir[i] = SL_EVAL_MIR_FN_NONE;
        loweringTopConsts[i] = 0u;
    }
    for (i = 0; i < p->topVarLen; i++) {
        topVarToMir[i] = SL_EVAL_MIR_FN_NONE;
        loweringTopVars[i] = 0u;
    }
    for (i = 0; i < mirCap; i++) {
        mirToEval[i] = UINT32_MAX;
        sourceFiles[i] = NULL;
    }
    SLMirProgramBuilderInit(&outCtx->builder, p->arena);
    outCtx->p = p;
    outCtx->evalToMir = evalToMir;
    outCtx->loweringFns = loweringFns;
    outCtx->topConstToMir = topConstToMir;
    outCtx->loweringTopConsts = loweringTopConsts;
    outCtx->topVarToMir = topVarToMir;
    outCtx->loweringTopVars = loweringTopVars;
    outCtx->execCtx.p = p;
    outCtx->execCtx.mirToEval = mirToEval;
    outCtx->execCtx.mirToEvalLen = mirCap;
    outCtx->execCtx.sourceFiles = sourceFiles;
    outCtx->execCtx.sourceFileCap = mirCap;
    outCtx->execCtx.rootMirFnIndex = UINT32_MAX;
    outCtx->execCtx.savedFileLen = 0u;
    outCtx->diag = p->currentExecCtx != NULL ? p->currentExecCtx->diag : NULL;
    return 0;
}

static int SLEvalMirInternSourceFile(
    SLEvalMirLowerCtx* c, const SLParsedFile* file, uint32_t* _Nonnull outSourceRef) {
    SLMirSourceRef sourceRef = { 0 };
    if (c == NULL || file == NULL || outSourceRef == NULL) {
        return -1;
    }
    sourceRef.src.ptr = file->source;
    sourceRef.src.len = file->sourceLen;
    if (SLMirProgramBuilderAddSource(&c->builder, &sourceRef, outSourceRef) != 0) {
        return -1;
    }
    if (*outSourceRef >= c->execCtx.sourceFileCap) {
        return -1;
    }
    c->execCtx.sourceFiles[*outSourceRef] = file;
    return 0;
}

static void SLEvalMirAdaptOutValue(
    const SLEvalMirExecCtx* c, SLCTFEValue* _Nullable value, int* _Nullable inOutIsConst) {
    uint32_t mirFnIndex = UINT32_MAX;
    uint32_t evalFnIndex = UINT32_MAX;
    if (inOutIsConst != NULL && !*inOutIsConst) {
        return;
    }
    if (c == NULL || value == NULL) {
        return;
    }
    if (!SLMirValueAsFunctionRef(value, &mirFnIndex)) {
        return;
    }
    if (c->mirToEval == NULL || mirFnIndex >= c->mirToEvalLen) {
        if (inOutIsConst != NULL) {
            *inOutIsConst = 0;
        }
        return;
    }
    evalFnIndex = c->mirToEval[mirFnIndex];
    if (evalFnIndex == UINT32_MAX) {
        if (inOutIsConst != NULL) {
            *inOutIsConst = 0;
        }
        return;
    }
    SLEvalValueSetFunctionRef(value, evalFnIndex);
}

static const SLPackage* _Nullable SLEvalMirFindImportTargetByAliasSlice(
    const SLPackage* pkg, const char* src, uint32_t start, uint32_t end) {
    uint32_t i;
    if (pkg == NULL || src == NULL) {
        return NULL;
    }
    for (i = 0; i < pkg->importLen; i++) {
        const SLImportRef* imp = &pkg->imports[i];
        if (imp->bindName == NULL || imp->target == NULL) {
            continue;
        }
        if (strlen(imp->bindName) == (size_t)(end - start)
            && memcmp(imp->bindName, src + start, (size_t)(end - start)) == 0)
        {
            return imp->target;
        }
    }
    return NULL;
}

static int SLEvalMirMatchQualifiedCallBaseSlice(
    const SLAst*          ast,
    const SLMirSymbolRef* symbol,
    int32_t               rootNode,
    uint32_t              encodedArgCount,
    uint32_t* _Nonnull outBaseStart,
    uint32_t* _Nonnull outBaseEnd) {
    int32_t  stack[256];
    uint32_t stackLen = 0;
    int      found = 0;
    if (outBaseStart != NULL) {
        *outBaseStart = 0;
    }
    if (outBaseEnd != NULL) {
        *outBaseEnd = 0;
    }
    if (ast == NULL || symbol == NULL || outBaseStart == NULL || outBaseEnd == NULL
        || rootNode < 0 || (uint32_t)rootNode >= ast->len
        || symbol->flags != SLMirSymbolFlag_CALL_RECEIVER_ARG0)
    {
        return 0;
    }
    stack[stackLen++] = rootNode;
    while (stackLen > 0) {
        int32_t          nodeId = stack[--stackLen];
        const SLAstNode* node = &ast->nodes[nodeId];
        int32_t          child;
        if (node->kind == SLAst_CALL) {
            int32_t calleeNode = node->firstChild;
            int32_t baseNode = calleeNode >= 0 ? ast->nodes[calleeNode].firstChild : -1;
            if (calleeNode >= 0 && (uint32_t)calleeNode < ast->len
                && ast->nodes[calleeNode].kind == SLAst_FIELD_EXPR && baseNode >= 0
                && (uint32_t)baseNode < ast->len && ast->nodes[baseNode].kind == SLAst_IDENT
                && ast->nodes[calleeNode].dataStart == symbol->nameStart
                && ast->nodes[calleeNode].dataEnd == symbol->nameEnd
                && AstListCount(ast, nodeId) == (encodedArgCount & ~SLMirCallArgFlag_RECEIVER_ARG0))
            {
                uint32_t baseStart = ast->nodes[baseNode].dataStart;
                uint32_t baseEnd = ast->nodes[baseNode].dataEnd;
                if (!found) {
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
            child = ast->nodes[child].nextSibling;
        }
    }
    return found;
}

static int SLEvalMirResolveQualifiedCallTargetInNode(
    const SLEvalMirLowerCtx* c,
    const SLPackage*         currentPkg,
    const SLParsedFile*      file,
    int32_t                  rootNode,
    const SLMirInst*         ins,
    uint32_t*                outEvalFnIndex) {
    const SLMirSymbolRef* symbol;
    const SLPackage*      targetPkg;
    int32_t               fnIndex;
    uint32_t              baseStart = 0;
    uint32_t              baseEnd = 0;
    if (outEvalFnIndex != NULL) {
        *outEvalFnIndex = UINT32_MAX;
    }
    if (c == NULL || currentPkg == NULL || file == NULL || ins == NULL || outEvalFnIndex == NULL
        || ins->op != SLMirOp_CALL || c->builder.symbols == NULL
        || ins->aux >= c->builder.symbolLen)
    {
        return 0;
    }
    symbol = &c->builder.symbols[ins->aux];
    if (symbol->kind != SLMirSymbol_CALL
        || !SLEvalMirMatchQualifiedCallBaseSlice(
            &file->ast, symbol, rootNode, (uint32_t)ins->tok, &baseStart, &baseEnd))
    {
        return 0;
    }
    targetPkg = SLEvalMirFindImportTargetByAliasSlice(currentPkg, file->source, baseStart, baseEnd);
    if (targetPkg == NULL) {
        return 0;
    }
    fnIndex = SLEvalResolveFunctionBySlice(
        c->p, targetPkg, file, symbol->nameStart, symbol->nameEnd, NULL, (uint32_t)ins->tok - 1u);
    if (fnIndex == -2) {
        return 0;
    }
    if (fnIndex < 0 || (uint32_t)fnIndex >= c->p->funcLen
        || c->p->funcs[(uint32_t)fnIndex].isBuiltinPackageFn
        || c->p->funcs[(uint32_t)fnIndex].isVariadic)
    {
        return 0;
    }
    *outEvalFnIndex = (uint32_t)fnIndex;
    return 1;
}

static int SLEvalMirResolveDirectCallTarget(
    const SLEvalMirLowerCtx* c,
    const SLEvalFunction*    callerFn,
    int32_t                  callerFnIndex,
    const SLMirInst*         ins,
    uint32_t*                outEvalFnIndex) {
    const SLMirSymbolRef* symbol;
    int32_t               targetFnIndex;
    if (outEvalFnIndex != NULL) {
        *outEvalFnIndex = UINT32_MAX;
    }
    if (c == NULL || callerFn == NULL || ins == NULL || outEvalFnIndex == NULL
        || ins->op != SLMirOp_CALL || c->builder.symbols == NULL
        || ins->aux >= c->builder.symbolLen)
    {
        return 0;
    }
    symbol = &c->builder.symbols[ins->aux];
    if (symbol->kind != SLMirSymbol_CALL) {
        return 0;
    }
    if (symbol->flags == SLMirSymbolFlag_CALL_RECEIVER_ARG0) {
        return SLEvalMirResolveQualifiedCallTargetInNode(
            c, callerFn->pkg, callerFn->file, callerFn->bodyNode, ins, outEvalFnIndex);
    }
    if (symbol->flags != 0u) {
        return 0;
    }
    targetFnIndex = SLEvalResolveFunctionBySlice(
        c->p, NULL, callerFn->file, symbol->nameStart, symbol->nameEnd, NULL, (uint32_t)ins->tok);
    if (targetFnIndex < 0 || (uint32_t)targetFnIndex >= c->p->funcLen)
    {
        return 0;
    }
    if (c->p->funcs[(uint32_t)targetFnIndex].isBuiltinPackageFn
        || c->p->funcs[(uint32_t)targetFnIndex].isVariadic)
    {
        return 0;
    }
    *outEvalFnIndex = (uint32_t)targetFnIndex;
    return 1;
}

static int SLEvalMirResolvePlainDirectCallTargetForFile(
    const SLEvalMirLowerCtx* c,
    const SLParsedFile*      file,
    const SLMirInst*         ins,
    uint32_t*                outEvalFnIndex) {
    const SLMirSymbolRef* symbol;
    int32_t               targetFnIndex;
    if (outEvalFnIndex != NULL) {
        *outEvalFnIndex = UINT32_MAX;
    }
    if (c == NULL || file == NULL || ins == NULL || outEvalFnIndex == NULL || ins->op != SLMirOp_CALL
        || c->builder.symbols == NULL || ins->aux >= c->builder.symbolLen)
    {
        return 0;
    }
    symbol = &c->builder.symbols[ins->aux];
    if (symbol->kind != SLMirSymbol_CALL || symbol->flags != 0u) {
        return 0;
    }
    targetFnIndex = SLEvalResolveFunctionBySlice(
        c->p, NULL, file, symbol->nameStart, symbol->nameEnd, NULL, (uint32_t)ins->tok);
    if (targetFnIndex < 0 || (uint32_t)targetFnIndex >= c->p->funcLen
        || c->p->funcs[(uint32_t)targetFnIndex].isBuiltinPackageFn
        || c->p->funcs[(uint32_t)targetFnIndex].isVariadic)
    {
        return 0;
    }
    *outEvalFnIndex = (uint32_t)targetFnIndex;
    return 1;
}

static int SLEvalMirResolveQualifiedCallTargetForNode(
    const SLEvalMirLowerCtx* c,
    const SLParsedFile*      file,
    int32_t                  rootNode,
    const SLMirInst*         ins,
    uint32_t*                outEvalFnIndex) {
    const SLPackage*      currentPkg;
    if (outEvalFnIndex != NULL) {
        *outEvalFnIndex = UINT32_MAX;
    }
    if (c == NULL || file == NULL || ins == NULL || outEvalFnIndex == NULL || ins->op != SLMirOp_CALL
        || c->builder.symbols == NULL || ins->aux >= c->builder.symbolLen)
    {
        return 0;
    }
    currentPkg = SLEvalFindPackageByFile(c->p, file);
    return SLEvalMirResolveQualifiedCallTargetInNode(
        c, currentPkg, file, rootNode, ins, outEvalFnIndex);
}

static int SLEvalMirRewriteBuiltinHostCallForFile(
    SLEvalMirLowerCtx*  c,
    const SLParsedFile* file,
    SLMirInst*          ins,
    int* _Nonnull outRewritten) {
    const SLMirSymbolRef* symbol;
    SLMirHostRef          host = { 0 };
    uint32_t              hostIndex = UINT32_MAX;
    if (outRewritten != NULL) {
        *outRewritten = 0;
    }
    if (c == NULL || file == NULL || ins == NULL || outRewritten == NULL
        || ins->op != SLMirOp_CALL || c->builder.symbols == NULL
        || ins->aux >= c->builder.symbolLen)
    {
        return 0;
    }
    symbol = &c->builder.symbols[ins->aux];
    if (symbol->kind != SLMirSymbol_CALL || symbol->flags != 0u) {
        return 0;
    }
    if ((uint32_t)ins->tok == 1u
        && SliceEqCStr(file->source, symbol->nameStart, symbol->nameEnd, "print"))
    {
        host.nameStart = symbol->nameStart;
        host.nameEnd = symbol->nameEnd;
        host.kind = SLMirHost_GENERIC;
        host.flags = 0;
        host.target = SL_EVAL_MIR_HOST_PRINT;
        if (SLMirProgramBuilderAddHost(&c->builder, &host, &hostIndex) != 0) {
            return -1;
        }
        ins->op = SLMirOp_CALL_HOST;
        ins->aux = hostIndex;
        *outRewritten = 1;
        return 1;
    }
    if ((uint32_t)ins->tok == 2u
        && SliceEqCStr(file->source, symbol->nameStart, symbol->nameEnd, "copy"))
    {
        host.nameStart = symbol->nameStart;
        host.nameEnd = symbol->nameEnd;
        host.kind = SLMirHost_GENERIC;
        host.flags = 0;
        host.target = SL_EVAL_MIR_HOST_COPY;
        if (SLMirProgramBuilderAddHost(&c->builder, &host, &hostIndex) != 0) {
            return -1;
        }
        ins->op = SLMirOp_CALL_HOST;
        ins->aux = hostIndex;
        *outRewritten = 1;
        return 1;
    }
    if ((uint32_t)ins->tok == 2u
        && SliceEqCStr(file->source, symbol->nameStart, symbol->nameEnd, "concat"))
    {
        host.nameStart = symbol->nameStart;
        host.nameEnd = symbol->nameEnd;
        host.kind = SLMirHost_GENERIC;
        host.flags = 0;
        host.target = SL_EVAL_MIR_HOST_CONCAT;
        if (SLMirProgramBuilderAddHost(&c->builder, &host, &hostIndex) != 0) {
            return -1;
        }
        ins->op = SLMirOp_CALL_HOST;
        ins->aux = hostIndex;
        *outRewritten = 1;
        return 1;
    }
    if (((uint32_t)ins->tok == 1u || (uint32_t)ins->tok == 2u)
        && SliceEqCStr(file->source, symbol->nameStart, symbol->nameEnd, "free"))
    {
        host.nameStart = symbol->nameStart;
        host.nameEnd = symbol->nameEnd;
        host.kind = SLMirHost_GENERIC;
        host.flags = 0;
        host.target = SL_EVAL_MIR_HOST_FREE;
        if (SLMirProgramBuilderAddHost(&c->builder, &host, &hostIndex) != 0) {
            return -1;
        }
        ins->op = SLMirOp_CALL_HOST;
        ins->aux = hostIndex;
        *outRewritten = 1;
        return 1;
    }
    return 0;
}

static int SLEvalMirRewriteBuiltinHostCall(
    SLEvalMirLowerCtx*    c,
    const SLEvalFunction* callerFn,
    SLMirInst*            ins,
    int* _Nonnull outRewritten) {
    if (c == NULL || callerFn == NULL) {
        return 0;
    }
    return SLEvalMirRewriteBuiltinHostCallForFile(c, callerFn->file, ins, outRewritten);
}

static int SLEvalMirRewriteQualifiedHostCallForNode(
    SLEvalMirLowerCtx*  c,
    const SLPackage*    currentPkg,
    const SLParsedFile* file,
    int32_t             rootNode,
    SLMirInst*          ins,
    int* _Nonnull outRewritten) {
    const SLMirSymbolRef* symbol;
    const SLPackage*      targetPkg;
    uint32_t              baseStart = 0;
    uint32_t              baseEnd = 0;
    if (outRewritten != NULL) {
        *outRewritten = 0;
    }
    if (c == NULL || currentPkg == NULL || file == NULL || ins == NULL || outRewritten == NULL
        || ins->op != SLMirOp_CALL || c->builder.symbols == NULL
        || ins->aux >= c->builder.symbolLen)
    {
        return 0;
    }
    symbol = &c->builder.symbols[ins->aux];
    if (symbol->kind != SLMirSymbol_CALL || symbol->flags != SLMirSymbolFlag_CALL_RECEIVER_ARG0
        || rootNode < 0)
    {
        return 0;
    }
    if (!SLEvalMirMatchQualifiedCallBaseSlice(
            &file->ast, symbol, rootNode, (uint32_t)ins->tok, &baseStart, &baseEnd))
    {
        return 0;
    }
    targetPkg = SLEvalMirFindImportTargetByAliasSlice(currentPkg, file->source, baseStart, baseEnd);
    if (targetPkg != NULL && StrEq(targetPkg->name, "platform")
        && SliceEqCStr(file->source, symbol->nameStart, symbol->nameEnd, "exit"))
    {
        SLMirHostRef host = { 0 };
        uint32_t     hostIndex = UINT32_MAX;
        host.nameStart = symbol->nameStart;
        host.nameEnd = symbol->nameEnd;
        host.kind = SLMirHost_GENERIC;
        host.flags = 0;
        host.target = SL_EVAL_MIR_HOST_PLATFORM_EXIT;
        if (SLMirProgramBuilderAddHost(&c->builder, &host, &hostIndex) != 0) {
            return -1;
        }
        ins->op = SLMirOp_CALL_HOST;
        ins->aux = hostIndex;
        ins->tok |= SLMirCallArgFlag_RECEIVER_ARG0;
        *outRewritten = 1;
        return 1;
    }
    return 0;
}

static int SLEvalMirRewriteQualifiedHostCall(
    SLEvalMirLowerCtx*    c,
    const SLEvalFunction* callerFn,
    SLMirInst*            ins,
    int* _Nonnull outRewritten) {
    if (c == NULL || callerFn == NULL) {
        return 0;
    }
    return SLEvalMirRewriteQualifiedHostCallForNode(
        c, callerFn->pkg, callerFn->file, callerFn->bodyNode, ins, outRewritten);
}

static void SLEvalMirRewriteCallToMirFunction(
    const SLEvalMirLowerCtx* c, SLMirInst* ins, uint32_t targetMirFnIndex) {
    int preserveReceiverArg0 = 0;
    if (c != NULL && ins != NULL && c->builder.symbols != NULL && ins->aux < c->builder.symbolLen
        && (c->builder.symbols[ins->aux].flags & SLMirSymbolFlag_CALL_RECEIVER_ARG0) != 0u)
    {
        preserveReceiverArg0 = 1;
    }
    if (ins == NULL) {
        return;
    }
    ins->op = SLMirOp_CALL_FN;
    ins->aux = targetMirFnIndex;
    if (preserveReceiverArg0) {
        ins->tok |= SLMirCallArgFlag_RECEIVER_ARG0;
    }
}

static void SLEvalMirRewriteLoadIdentToMirFunction(SLMirInst* ins, uint32_t targetMirFnIndex);
static int SLEvalMirLowerTopConst(
    SLEvalMirLowerCtx* c, uint32_t topConstIndex, uint32_t* _Nullable outMirFnIndex);
static int SLEvalMirLowerTopVar(
    SLEvalMirLowerCtx* c, uint32_t topVarIndex, uint32_t* _Nullable outMirFnIndex);
static int SLEvalMirResolveTopConstIdentTargetForFile(
    const SLEvalMirLowerCtx* c, const SLParsedFile* file, const SLMirInst* ins, uint32_t* outIndex);
static int SLEvalMirResolveTopInitIdentTargetForFile(
    const SLEvalMirLowerCtx* c,
    const SLParsedFile*      file,
    const SLMirInst*         ins,
    int*                     outIsTopConst,
    uint32_t*                outIndex);
static int SLEvalMirResolveFunctionIdentTargetForFile(
    const SLEvalMirLowerCtx* c, const SLParsedFile* file, const SLMirInst* ins, uint32_t* outIndex);
static int SLEvalMirRewriteLoadIdentToFunctionConst(
    SLEvalMirLowerCtx* c, SLMirInst* ins, uint32_t targetMirFnIndex);
static int SLEvalMirResolveSimpleFunctionValueAliasCallTarget(
    const SLEvalMirLowerCtx* c,
    const SLParsedFile*      file,
    const SLMirInst*         ins,
    uint32_t*                outEvalFnIndex);
static int SLEvalMirRewriteZeroArgRuntimeFunctionValueCall(
    SLEvalMirLowerCtx* c, const SLParsedFile* file, uint32_t mirFnIndex, uint32_t instIndex);

static int SLEvalMirLowerFunction(
    SLEvalMirLowerCtx* c, int32_t evalFnIndex, uint32_t* _Nullable outMirFnIndex) {
    const SLEvalFunction* fn;
    uint32_t              mirFnIndex = UINT32_MAX;
    uint32_t              sourceRefIndex = UINT32_MAX;
    uint32_t              instIndex;
    int                   supported = 0;
    if (outMirFnIndex != NULL) {
        *outMirFnIndex = UINT32_MAX;
    }
    if (c == NULL || evalFnIndex < 0 || (uint32_t)evalFnIndex >= c->p->funcLen
        || c->evalToMir == NULL)
    {
        return -1;
    }
    if (c->loweringFns != NULL && c->loweringFns[(uint32_t)evalFnIndex] != 0u) {
        if (c->evalToMir[(uint32_t)evalFnIndex] != SL_EVAL_MIR_FN_NONE) {
            if (outMirFnIndex != NULL) {
                *outMirFnIndex = c->evalToMir[(uint32_t)evalFnIndex];
            }
            return 1;
        }
        return 0;
    }
    if (c->evalToMir[(uint32_t)evalFnIndex] != SL_EVAL_MIR_FN_NONE) {
        if (outMirFnIndex != NULL) {
            *outMirFnIndex = c->evalToMir[(uint32_t)evalFnIndex];
        }
        return 1;
    }
    fn = &c->p->funcs[(uint32_t)evalFnIndex];
    if (fn->isBuiltinPackageFn || fn->isVariadic) {
        return 0;
    }
    if (c->loweringFns != NULL) {
        c->loweringFns[(uint32_t)evalFnIndex] = 1u;
    }
    if (SLEvalMirInternSourceFile(c, fn->file, &sourceRefIndex) != 0) {
        if (c->loweringFns != NULL) {
            c->loweringFns[(uint32_t)evalFnIndex] = 0u;
        }
        return -1;
    }
    (void)sourceRefIndex;
    if (SLMirLowerAppendSimpleFunction(
            &c->builder,
            c->p->arena,
            &fn->file->ast,
            (SLStrView){ fn->file->source, fn->file->sourceLen },
            fn->fnNode,
            fn->bodyNode,
            &mirFnIndex,
            &supported,
            c->diag)
        != 0)
    {
        if (c->loweringFns != NULL) {
            c->loweringFns[(uint32_t)evalFnIndex] = 0u;
        }
        return -1;
    }
    if (!supported) {
        if (c->loweringFns != NULL) {
            c->loweringFns[(uint32_t)evalFnIndex] = 0u;
        }
        return 0;
    }
    c->evalToMir[(uint32_t)evalFnIndex] = mirFnIndex;
    if (c->execCtx.mirToEval != NULL && mirFnIndex < c->execCtx.mirToEvalLen) {
        c->execCtx.mirToEval[mirFnIndex] = (uint32_t)evalFnIndex;
    }
    for (instIndex = c->builder.funcs[mirFnIndex].instStart;
         instIndex < c->builder.funcs[mirFnIndex].instStart + c->builder.funcs[mirFnIndex].instLen;
         instIndex++)
    {
        SLMirInst* ins = &c->builder.insts[instIndex];
        int        isTopConst = 0;
        uint32_t   targetTopInitIndex = UINT32_MAX;
        uint32_t   targetFnIdentIndex = UINT32_MAX;
        uint32_t   targetEvalFnIndex = UINT32_MAX;
        uint32_t   targetMirFnIndex = UINT32_MAX;
        int        lowerRc;
        int        rewrittenBuiltinHost = 0;
        int        rewrittenHost = 0;
        if (SLEvalMirResolveTopInitIdentTargetForFile(
                c, fn->file, ins, &isTopConst, &targetTopInitIndex))
        {
            lowerRc = isTopConst
                ? SLEvalMirLowerTopConst(c, targetTopInitIndex, &targetMirFnIndex)
                : SLEvalMirLowerTopVar(c, targetTopInitIndex, &targetMirFnIndex);
            if (lowerRc < 0) {
                if (c->loweringFns != NULL) {
                    c->loweringFns[(uint32_t)evalFnIndex] = 0u;
                }
                return -1;
            }
            if (lowerRc == 0) {
                c->evalToMir[(uint32_t)evalFnIndex] = SL_EVAL_MIR_FN_NONE;
                if (c->loweringFns != NULL) {
                    c->loweringFns[(uint32_t)evalFnIndex] = 0u;
                }
                return 0;
            }
            SLEvalMirRewriteLoadIdentToMirFunction(ins, targetMirFnIndex);
            continue;
        }
        if (SLEvalMirResolveFunctionIdentTargetForFile(c, fn->file, ins, &targetFnIdentIndex)) {
            lowerRc = SLEvalMirLowerFunction(c, (int32_t)targetFnIdentIndex, &targetMirFnIndex);
            if (lowerRc < 0) {
                if (c->loweringFns != NULL) {
                    c->loweringFns[(uint32_t)evalFnIndex] = 0u;
                }
                return -1;
            }
            if (lowerRc == 0) {
                c->evalToMir[(uint32_t)evalFnIndex] = SL_EVAL_MIR_FN_NONE;
                if (c->loweringFns != NULL) {
                    c->loweringFns[(uint32_t)evalFnIndex] = 0u;
                }
                return 0;
            }
            if (SLEvalMirRewriteLoadIdentToFunctionConst(c, ins, targetMirFnIndex) != 0) {
                if (c->loweringFns != NULL) {
                    c->loweringFns[(uint32_t)evalFnIndex] = 0u;
                }
                return -1;
            }
            continue;
        }
        if (SLEvalMirResolveSimpleFunctionValueAliasCallTarget(
                c, fn->file, ins, &targetEvalFnIndex))
        {
            lowerRc = SLEvalMirLowerFunction(c, (int32_t)targetEvalFnIndex, &targetMirFnIndex);
            if (lowerRc < 0) {
                if (c->loweringFns != NULL) {
                    c->loweringFns[(uint32_t)evalFnIndex] = 0u;
                }
                return -1;
            }
            if (lowerRc == 0) {
                c->evalToMir[(uint32_t)evalFnIndex] = SL_EVAL_MIR_FN_NONE;
                if (c->loweringFns != NULL) {
                    c->loweringFns[(uint32_t)evalFnIndex] = 0u;
                }
                return 0;
            }
            SLEvalMirRewriteCallToMirFunction(c, ins, targetMirFnIndex);
            continue;
        }
        lowerRc = SLEvalMirRewriteZeroArgRuntimeFunctionValueCall(c, fn->file, mirFnIndex, instIndex);
        if (lowerRc < 0) {
            if (c->loweringFns != NULL) {
                c->loweringFns[(uint32_t)evalFnIndex] = 0u;
            }
            return -1;
        }
        if (lowerRc > 0) {
            instIndex++;
            continue;
        }
        lowerRc = SLEvalMirRewriteBuiltinHostCall(c, fn, ins, &rewrittenHost);
        if (lowerRc < 0) {
            return -1;
        }
        if (rewrittenHost) {
            continue;
        }
        lowerRc = SLEvalMirRewriteQualifiedHostCall(c, fn, ins, &rewrittenHost);
        if (lowerRc < 0) {
            return -1;
        }
        if (rewrittenHost) {
            continue;
        }
        if (!SLEvalMirResolveDirectCallTarget(c, fn, evalFnIndex, ins, &targetEvalFnIndex)) {
            continue;
        }
        lowerRc = SLEvalMirLowerFunction(c, (int32_t)targetEvalFnIndex, &targetMirFnIndex);
        if (lowerRc < 0) {
            if (c->loweringFns != NULL) {
                c->loweringFns[(uint32_t)evalFnIndex] = 0u;
            }
            return -1;
        }
        if (lowerRc == 0) {
            c->evalToMir[(uint32_t)evalFnIndex] = SL_EVAL_MIR_FN_NONE;
            if (c->loweringFns != NULL) {
                c->loweringFns[(uint32_t)evalFnIndex] = 0u;
            }
            return 0;
        }
        SLEvalMirRewriteCallToMirFunction(c, ins, targetMirFnIndex);
    }
    if (c->loweringFns != NULL) {
        c->loweringFns[(uint32_t)evalFnIndex] = 0u;
    }
    if (outMirFnIndex != NULL) {
        *outMirFnIndex = mirFnIndex;
    }
    return 1;
}

static void SLEvalMirRewriteLoadIdentToMirFunction(SLMirInst* ins, uint32_t targetMirFnIndex) {
    if (ins == NULL) {
        return;
    }
    ins->op = SLMirOp_CALL_FN;
    ins->tok = 0u;
    ins->aux = targetMirFnIndex;
}

static int SLEvalMirRewriteLoadIdentToFunctionConst(
    SLEvalMirLowerCtx* c, SLMirInst* ins, uint32_t targetMirFnIndex) {
    SLMirConst value = { 0 };
    uint32_t   constIndex = UINT32_MAX;
    if (c == NULL || ins == NULL) {
        return -1;
    }
    value.kind = SLMirConst_FUNCTION;
    value.bits = targetMirFnIndex;
    if (SLMirProgramBuilderAddConst(&c->builder, &value, &constIndex) != 0) {
        return -1;
    }
    ins->op = SLMirOp_PUSH_CONST;
    ins->tok = 0u;
    ins->aux = constIndex;
    return 0;
}

static int SLEvalMirLowerTopConst(
    SLEvalMirLowerCtx* c, uint32_t topConstIndex, uint32_t* _Nullable outMirFnIndex);
static int SLEvalMirLowerTopVar(
    SLEvalMirLowerCtx* c, uint32_t topVarIndex, uint32_t* _Nullable outMirFnIndex);

static int SLEvalMirResolveTopConstIdentTargetForFile(
    const SLEvalMirLowerCtx* c, const SLParsedFile* file, const SLMirInst* ins, uint32_t* outIndex) {
    const SLPackage* currentPkg;
    int32_t topIndex = -1;
    if (outIndex != NULL) {
        *outIndex = UINT32_MAX;
    }
    if (c == NULL || c->p == NULL || file == NULL || ins == NULL || outIndex == NULL
        || ins->op != SLMirOp_LOAD_IDENT)
    {
        return 0;
    }
    currentPkg = SLEvalFindPackageByFile(c->p, file);
    if (currentPkg == NULL) {
        return 0;
    }
    topIndex = SLEvalFindTopConstBySliceInPackage(c->p, currentPkg, file, ins->start, ins->end);
    if (topIndex >= 0) {
        *outIndex = (uint32_t)topIndex;
        return 1;
    }
    return 0;
}

static int SLEvalMirResolveFunctionIdentTargetForFile(
    const SLEvalMirLowerCtx* c, const SLParsedFile* file, const SLMirInst* ins, uint32_t* outIndex) {
    const SLPackage* currentPkg;
    int32_t fnIndex = -1;
    if (outIndex != NULL) {
        *outIndex = UINT32_MAX;
    }
    if (c == NULL || c->p == NULL || file == NULL || ins == NULL || outIndex == NULL
        || ins->op != SLMirOp_LOAD_IDENT)
    {
        return 0;
    }
    currentPkg = SLEvalFindPackageByFile(c->p, file);
    if (currentPkg == NULL) {
        return 0;
    }
    fnIndex = SLEvalFindAnyFunctionBySliceInPackage(c->p, currentPkg, file, ins->start, ins->end);
    if (fnIndex < 0 || (uint32_t)fnIndex >= c->p->funcLen
        || c->p->funcs[(uint32_t)fnIndex].isBuiltinPackageFn
        || c->p->funcs[(uint32_t)fnIndex].isVariadic)
    {
        return 0;
    }
    *outIndex = (uint32_t)fnIndex;
    return 1;
}

static int SLEvalMirResolveSimpleTopInitFunctionValueTarget(
    const SLEvalMirLowerCtx* c,
    const SLParsedFile*      file,
    int                      isTopConst,
    uint32_t                 topInitIndex,
    uint32_t*                outEvalFnIndex) {
    const SLPackage*   currentPkg;
    int32_t          initExprNode = -1;
    const SLAstNode* initExpr;
    int32_t          targetFnIndex = -1;
    if (outEvalFnIndex != NULL) {
        *outEvalFnIndex = UINT32_MAX;
    }
    if (c == NULL || c->p == NULL || file == NULL || outEvalFnIndex == NULL) {
        return 0;
    }
    currentPkg = SLEvalFindPackageByFile(c->p, file);
    if (currentPkg == NULL) {
        return 0;
    }
    if (isTopConst) {
        if (topInitIndex >= c->p->topConstLen) {
            return 0;
        }
        initExprNode = c->p->topConsts[topInitIndex].initExprNode;
    } else {
        if (topInitIndex >= c->p->topVarLen) {
            return 0;
        }
        initExprNode = c->p->topVars[topInitIndex].initExprNode;
    }
    if (initExprNode < 0 || (uint32_t)initExprNode >= file->ast.len) {
        return 0;
    }
    initExpr = &file->ast.nodes[initExprNode];
    if (initExpr->kind != SLAst_IDENT) {
        return 0;
    }
    targetFnIndex = SLEvalFindAnyFunctionBySliceInPackage(
        c->p, currentPkg, file, initExpr->dataStart, initExpr->dataEnd);
    if (targetFnIndex < 0 || (uint32_t)targetFnIndex >= c->p->funcLen
        || c->p->funcs[(uint32_t)targetFnIndex].isBuiltinPackageFn
        || c->p->funcs[(uint32_t)targetFnIndex].isVariadic)
    {
        return 0;
    }
    *outEvalFnIndex = (uint32_t)targetFnIndex;
    return 1;
}

static int SLEvalMirResolveSimpleFunctionValueAliasCallTarget(
    const SLEvalMirLowerCtx* c,
    const SLParsedFile*      file,
    const SLMirInst*         ins,
    uint32_t*                outEvalFnIndex) {
    const SLPackage*      currentPkg;
    const SLMirSymbolRef* symbol;
    int32_t               topIndex = -1;
    if (outEvalFnIndex != NULL) {
        *outEvalFnIndex = UINT32_MAX;
    }
    if (c == NULL || c->p == NULL || file == NULL || ins == NULL || outEvalFnIndex == NULL
        || ins->op != SLMirOp_CALL || c->builder.symbols == NULL || ins->aux >= c->builder.symbolLen)
    {
        return 0;
    }
    currentPkg = SLEvalFindPackageByFile(c->p, file);
    if (currentPkg == NULL) {
        return 0;
    }
    symbol = &c->builder.symbols[ins->aux];
    if (symbol->kind != SLMirSymbol_CALL || symbol->flags != 0u) {
        return 0;
    }
    topIndex = SLEvalFindTopConstBySliceInPackage(
        c->p, currentPkg, file, symbol->nameStart, symbol->nameEnd);
    if (topIndex >= 0) {
        return SLEvalMirResolveSimpleTopInitFunctionValueTarget(
            c, file, 1, (uint32_t)topIndex, outEvalFnIndex);
    }
    topIndex = SLEvalFindTopVarBySliceInPackage(
        c->p, currentPkg, file, symbol->nameStart, symbol->nameEnd);
    if (topIndex >= 0) {
        return SLEvalMirResolveSimpleTopInitFunctionValueTarget(
            c, file, 0, (uint32_t)topIndex, outEvalFnIndex);
    }
    return 0;
}

static int SLEvalMirRewriteZeroArgTopInitFunctionValueCall(
    SLEvalMirLowerCtx* c, const SLParsedFile* file, uint32_t rootMirFnIndex, uint32_t instIndex) {
    const SLPackage*      currentPkg;
    SLMirInst*            ins;
    const SLMirSymbolRef* symbol;
    int32_t               topIndex = -1;
    uint32_t              targetEvalFnIndex = UINT32_MAX;
    uint32_t              targetMirFnIndex = UINT32_MAX;
    uint32_t              constIndex = UINT32_MAX;
    SLMirConst            value = { 0 };
    int                   lowerRc;
    if (c == NULL || file == NULL || rootMirFnIndex >= c->builder.funcLen || instIndex >= c->builder.instLen) {
        return 0;
    }
    currentPkg = SLEvalFindPackageByFile(c->p, file);
    if (currentPkg == NULL) {
        return 0;
    }
    ins = &c->builder.insts[instIndex];
    if (ins->op != SLMirOp_CALL || (uint32_t)ins->tok != 0u || c->builder.symbols == NULL
        || ins->aux >= c->builder.symbolLen)
    {
        return 0;
    }
    symbol = &c->builder.symbols[ins->aux];
    if (symbol->kind != SLMirSymbol_CALL || symbol->flags != 0u) {
        return 0;
    }
    topIndex = SLEvalFindTopConstBySliceInPackage(
        c->p, currentPkg, file, symbol->nameStart, symbol->nameEnd);
    if (topIndex >= 0) {
        if (!SLEvalMirResolveSimpleTopInitFunctionValueTarget(
                c, file, 1, (uint32_t)topIndex, &targetEvalFnIndex))
        {
            return 0;
        }
    } else {
        topIndex = SLEvalFindTopVarBySliceInPackage(
            c->p, currentPkg, file, symbol->nameStart, symbol->nameEnd);
        if (topIndex < 0
            || !SLEvalMirResolveSimpleTopInitFunctionValueTarget(
                c, file, 0, (uint32_t)topIndex, &targetEvalFnIndex))
        {
            return 0;
        }
    }
    lowerRc = SLEvalMirLowerFunction(c, (int32_t)targetEvalFnIndex, &targetMirFnIndex);
    if (lowerRc <= 0) {
        return lowerRc;
    }
    value.kind = SLMirConst_FUNCTION;
    value.bits = targetMirFnIndex;
    if (SLMirProgramBuilderAddConst(&c->builder, &value, &constIndex) != 0
        || SLMirProgramBuilderInsertInst(
               &c->builder,
               rootMirFnIndex,
               instIndex - c->builder.funcs[rootMirFnIndex].instStart,
               &(SLMirInst){
                   .op = SLMirOp_PUSH_CONST,
                   .aux = constIndex,
                   .start = ins->start,
                   .end = ins->end,
               })
               != 0)
    {
        return -1;
    }
    ins = &c->builder.insts[instIndex + 1u];
    ins->op = SLMirOp_CALL_INDIRECT;
    ins->tok = 0u;
    ins->aux = 0u;
    return 1;
}

static int SLEvalMirRewriteZeroArgRuntimeFunctionValueCall(
    SLEvalMirLowerCtx* c, const SLParsedFile* file, uint32_t mirFnIndex, uint32_t instIndex) {
    const SLPackage*      currentPkg;
    SLMirInst*            ins;
    const SLMirSymbolRef* symbol;
    int32_t               topIndex = -1;
    uint32_t              targetEvalFnIndex = UINT32_MAX;
    uint32_t              targetMirFnIndex = UINT32_MAX;
    uint32_t              constIndex = UINT32_MAX;
    SLMirConst            value = { 0 };
    int                   lowerRc;
    if (c == NULL || file == NULL || mirFnIndex >= c->builder.funcLen || instIndex >= c->builder.instLen) {
        return 0;
    }
    currentPkg = SLEvalFindPackageByFile(c->p, file);
    if (currentPkg == NULL) {
        return 0;
    }
    ins = &c->builder.insts[instIndex];
    if (ins->op != SLMirOp_CALL || (uint32_t)ins->tok != 0u || c->builder.symbols == NULL
        || ins->aux >= c->builder.symbolLen)
    {
        return 0;
    }
    symbol = &c->builder.symbols[ins->aux];
    if (symbol->kind != SLMirSymbol_CALL || symbol->flags != 0u) {
        return 0;
    }
    topIndex = SLEvalFindTopConstBySliceInPackage(
        c->p, currentPkg, file, symbol->nameStart, symbol->nameEnd);
    if (topIndex >= 0) {
        if (!SLEvalMirResolveSimpleTopInitFunctionValueTarget(
                c, file, 1, (uint32_t)topIndex, &targetEvalFnIndex))
        {
            return 0;
        }
    } else {
        topIndex = SLEvalFindTopVarBySliceInPackage(
            c->p, currentPkg, file, symbol->nameStart, symbol->nameEnd);
        if (topIndex < 0
            || !SLEvalMirResolveSimpleTopInitFunctionValueTarget(
                c, file, 0, (uint32_t)topIndex, &targetEvalFnIndex))
        {
            return 0;
        }
    }
    lowerRc = SLEvalMirLowerFunction(c, (int32_t)targetEvalFnIndex, &targetMirFnIndex);
    if (lowerRc <= 0) {
        return lowerRc;
    }
    value.kind = SLMirConst_FUNCTION;
    value.bits = targetMirFnIndex;
    if (SLMirProgramBuilderAddConst(&c->builder, &value, &constIndex) != 0
        || SLMirProgramBuilderInsertInst(
               &c->builder,
               mirFnIndex,
               instIndex - c->builder.funcs[mirFnIndex].instStart,
               &(SLMirInst){
                   .op = SLMirOp_PUSH_CONST,
                   .aux = constIndex,
                   .start = ins->start,
                   .end = ins->end,
               })
               != 0)
    {
        return -1;
    }
    ins = &c->builder.insts[instIndex + 1u];
    ins->op = SLMirOp_CALL_INDIRECT;
    ins->tok = 0u;
    ins->aux = 0u;
    return 1;
}

static int SLEvalMirResolveTopInitIdentTargetForFile(
    const SLEvalMirLowerCtx* c,
    const SLParsedFile*      file,
    const SLMirInst*         ins,
    int*                     outIsTopConst,
    uint32_t*                outIndex) {
    const SLPackage* currentPkg;
    int32_t topIndex = -1;
    if (outIsTopConst != NULL) {
        *outIsTopConst = 0;
    }
    if (outIndex != NULL) {
        *outIndex = UINT32_MAX;
    }
    if (c == NULL || c->p == NULL || file == NULL || ins == NULL || outIsTopConst == NULL
        || outIndex == NULL || ins->op != SLMirOp_LOAD_IDENT)
    {
        return 0;
    }
    currentPkg = SLEvalFindPackageByFile(c->p, file);
    if (currentPkg == NULL) {
        return 0;
    }
    if (SLEvalMirResolveTopConstIdentTargetForFile(c, file, ins, outIndex)) {
        *outIsTopConst = 1;
        return 1;
    }
    topIndex = SLEvalFindTopVarBySliceInPackage(c->p, currentPkg, file, ins->start, ins->end);
    if (topIndex >= 0) {
        *outIsTopConst = 0;
        *outIndex = (uint32_t)topIndex;
        return 1;
    }
    return 0;
}

static int SLEvalMirRewriteTopInitCalls(
    SLEvalMirLowerCtx*    c,
    const SLParsedFile*   file,
    int32_t               initExprNode,
    uint32_t              rootMirFnIndex,
    int* _Nonnull outSupported) {
    uint32_t instIndex;
    if (outSupported != NULL) {
        *outSupported = 0;
    }
    if (c == NULL || file == NULL || outSupported == NULL || rootMirFnIndex >= c->builder.funcLen) {
        return -1;
    }
    for (instIndex = c->builder.funcs[rootMirFnIndex].instStart;
         instIndex
         < c->builder.funcs[rootMirFnIndex].instStart + c->builder.funcs[rootMirFnIndex].instLen;
         instIndex++)
    {
        SLMirInst* ins = &c->builder.insts[instIndex];
        int        isTopConst = 0;
        uint32_t   targetTopInitIndex = UINT32_MAX;
        uint32_t   targetFnIdentIndex = UINT32_MAX;
        uint32_t   targetEvalFnIndex = UINT32_MAX;
        uint32_t   targetMirFnIndex = UINT32_MAX;
        int        lowerRc;
        if (SLEvalMirResolveTopInitIdentTargetForFile(
                c, file, ins, &isTopConst, &targetTopInitIndex))
        {
            lowerRc = isTopConst
                ? SLEvalMirLowerTopConst(c, targetTopInitIndex, &targetMirFnIndex)
                : SLEvalMirLowerTopVar(c, targetTopInitIndex, &targetMirFnIndex);
            if (lowerRc < 0) {
                return -1;
            }
            if (lowerRc == 0) {
                return 0;
            }
            SLEvalMirRewriteLoadIdentToMirFunction(ins, targetMirFnIndex);
            continue;
        }
        if (SLEvalMirResolveFunctionIdentTargetForFile(c, file, ins, &targetFnIdentIndex)) {
            lowerRc = SLEvalMirLowerFunction(c, (int32_t)targetFnIdentIndex, &targetMirFnIndex);
            if (lowerRc < 0) {
                return -1;
            }
            if (lowerRc == 0) {
                return 0;
            }
            if (SLEvalMirRewriteLoadIdentToFunctionConst(c, ins, targetMirFnIndex) != 0) {
                return -1;
            }
            continue;
        }
        if (SLEvalMirResolveSimpleFunctionValueAliasCallTarget(c, file, ins, &targetEvalFnIndex)) {
            lowerRc = SLEvalMirLowerFunction(c, (int32_t)targetEvalFnIndex, &targetMirFnIndex);
            if (lowerRc < 0) {
                return -1;
            }
            if (lowerRc == 0) {
                return 0;
            }
            SLEvalMirRewriteCallToMirFunction(c, ins, targetMirFnIndex);
            continue;
        }
        lowerRc = SLEvalMirRewriteZeroArgTopInitFunctionValueCall(c, file, rootMirFnIndex, instIndex);
        if (lowerRc < 0) {
            return -1;
        }
        if (lowerRc > 0) {
            instIndex++;
            continue;
        }
        if (SLEvalMirResolveQualifiedCallTargetForNode(
                c, file, initExprNode, ins, &targetEvalFnIndex))
        {
            lowerRc = SLEvalMirLowerFunction(c, (int32_t)targetEvalFnIndex, &targetMirFnIndex);
            if (lowerRc < 0) {
                return -1;
            }
            if (lowerRc == 0) {
                return 0;
            }
            SLEvalMirRewriteCallToMirFunction(c, ins, targetMirFnIndex);
            continue;
        }
        if (!SLEvalMirResolvePlainDirectCallTargetForFile(c, file, ins, &targetEvalFnIndex)) {
            continue;
        }
        lowerRc = SLEvalMirLowerFunction(c, (int32_t)targetEvalFnIndex, &targetMirFnIndex);
        if (lowerRc < 0) {
            return -1;
        }
        if (lowerRc == 0) {
            return 0;
        }
        SLEvalMirRewriteCallToMirFunction(c, ins, targetMirFnIndex);
    }
    *outSupported = 1;
    return 0;
}

static int SLEvalMirLowerTopConst(
    SLEvalMirLowerCtx* c, uint32_t topConstIndex, uint32_t* _Nullable outMirFnIndex) {
    const SLEvalTopConst* topConst;
    uint32_t              mirFnIndex = UINT32_MAX;
    uint32_t              sourceRef = UINT32_MAX;
    int                   supported = 0;
    int                   lowerRc;
    if (outMirFnIndex != NULL) {
        *outMirFnIndex = UINT32_MAX;
    }
    if (c == NULL || c->p == NULL || c->topConstToMir == NULL || c->loweringTopConsts == NULL
        || topConstIndex >= c->p->topConstLen)
    {
        return -1;
    }
    if (c->topConstToMir[topConstIndex] != SL_EVAL_MIR_FN_NONE) {
        if (outMirFnIndex != NULL) {
            *outMirFnIndex = c->topConstToMir[topConstIndex];
        }
        return 1;
    }
    if (c->loweringTopConsts[topConstIndex] != 0u) {
        return 0;
    }
    topConst = &c->p->topConsts[topConstIndex];
    if (topConst->initExprNode < 0) {
        return 0;
    }
    c->loweringTopConsts[topConstIndex] = 1u;
    if (SLMirLowerAppendNamedTopInitFunction(
            &c->builder,
            c->p->arena,
            &topConst->file->ast,
            (SLStrView){ topConst->file->source, topConst->file->sourceLen },
            topConst->initExprNode,
            -1,
            topConst->nameStart,
            topConst->nameEnd,
            &mirFnIndex,
            &supported,
            c->diag)
        != 0)
    {
        c->loweringTopConsts[topConstIndex] = 0u;
        return -1;
    }
    if (!supported || mirFnIndex == UINT32_MAX) {
        c->loweringTopConsts[topConstIndex] = 0u;
        return 0;
    }
    c->topConstToMir[topConstIndex] = mirFnIndex;
    sourceRef = c->builder.funcs[mirFnIndex].sourceRef;
    if (sourceRef >= c->execCtx.sourceFileCap) {
        c->loweringTopConsts[topConstIndex] = 0u;
        return -1;
    }
    c->execCtx.sourceFiles[sourceRef] = topConst->file;
    lowerRc = SLEvalMirRewriteTopInitCalls(
        c, topConst->file, topConst->initExprNode, mirFnIndex, &supported);
    c->loweringTopConsts[topConstIndex] = 0u;
    if (lowerRc != 0) {
        return -1;
    }
    if (!supported) {
        c->topConstToMir[topConstIndex] = SL_EVAL_MIR_FN_NONE;
        return 0;
    }
    if (outMirFnIndex != NULL) {
        *outMirFnIndex = mirFnIndex;
    }
    return 1;
}

static int SLEvalMirLowerTopVar(
    SLEvalMirLowerCtx* c, uint32_t topVarIndex, uint32_t* _Nullable outMirFnIndex) {
    const SLEvalTopVar* topVar;
    uint32_t            mirFnIndex = UINT32_MAX;
    uint32_t            sourceRef = UINT32_MAX;
    int                 supported = 0;
    int                 lowerRc;
    if (outMirFnIndex != NULL) {
        *outMirFnIndex = UINT32_MAX;
    }
    if (c == NULL || c->p == NULL || c->topVarToMir == NULL || c->loweringTopVars == NULL
        || topVarIndex >= c->p->topVarLen)
    {
        return -1;
    }
    if (c->topVarToMir[topVarIndex] != SL_EVAL_MIR_FN_NONE) {
        if (outMirFnIndex != NULL) {
            *outMirFnIndex = c->topVarToMir[topVarIndex];
        }
        return 1;
    }
    if (c->loweringTopVars[topVarIndex] != 0u) {
        return 0;
    }
    topVar = &c->p->topVars[topVarIndex];
    c->loweringTopVars[topVarIndex] = 1u;
    if (SLMirLowerAppendNamedTopInitFunction(
            &c->builder,
            c->p->arena,
            &topVar->file->ast,
            (SLStrView){ topVar->file->source, topVar->file->sourceLen },
            topVar->initExprNode,
            topVar->declTypeNode,
            topVar->nameStart,
            topVar->nameEnd,
            &mirFnIndex,
            &supported,
            c->diag)
        != 0)
    {
        c->loweringTopVars[topVarIndex] = 0u;
        return -1;
    }
    if (!supported || mirFnIndex == UINT32_MAX) {
        c->loweringTopVars[topVarIndex] = 0u;
        return 0;
    }
    c->topVarToMir[topVarIndex] = mirFnIndex;
    sourceRef = c->builder.funcs[mirFnIndex].sourceRef;
    if (sourceRef >= c->execCtx.sourceFileCap) {
        c->loweringTopVars[topVarIndex] = 0u;
        return -1;
    }
    c->execCtx.sourceFiles[sourceRef] = topVar->file;
    lowerRc = SLEvalMirRewriteTopInitCalls(
        c, topVar->file, topVar->initExprNode, mirFnIndex, &supported);
    c->loweringTopVars[topVarIndex] = 0u;
    if (lowerRc != 0) {
        return -1;
    }
    if (!supported) {
        c->topVarToMir[topVarIndex] = SL_EVAL_MIR_FN_NONE;
        return 0;
    }
    if (outMirFnIndex != NULL) {
        *outMirFnIndex = mirFnIndex;
    }
    return 1;
}

static int SLEvalMirBuildTopInitProgram(
    SLEvalProgram*      p,
    const SLParsedFile* file,
    int32_t             initExprNode,
    int32_t             declTypeNode,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    SLMirProgram*       outProgram,
    SLEvalMirExecCtx*   outExecCtx,
    uint32_t*           outRootMirFnIndex,
    int*                outSupported) {
    SLEvalMirLowerCtx lowerCtx;
    uint32_t          rootMirFnIndex = UINT32_MAX;
    uint32_t          sourceRef = UINT32_MAX;
    int               supported = 0;
    int               lowerRc;
    if (outProgram != NULL) {
        *outProgram = (SLMirProgram){ 0 };
    }
    if (outExecCtx != NULL) {
        memset(outExecCtx, 0, sizeof(*outExecCtx));
    }
    if (outRootMirFnIndex != NULL) {
        *outRootMirFnIndex = UINT32_MAX;
    }
    if (outSupported != NULL) {
        *outSupported = 0;
    }
    if (p == NULL || file == NULL || outProgram == NULL || outExecCtx == NULL
        || outRootMirFnIndex == NULL || outSupported == NULL)
    {
        return -1;
    }
    if (SLEvalMirInitLowerCtx(p, 1u, &lowerCtx) != 0) {
        return -1;
    }
    if (SLMirLowerBeginNamedTopInitProgram(
            &lowerCtx.builder,
            p->arena,
            &file->ast,
            (SLStrView){ file->source, file->sourceLen },
            initExprNode,
            declTypeNode,
            nameStart,
            nameEnd,
            &rootMirFnIndex,
            &supported,
            lowerCtx.diag)
        != 0)
    {
        return -1;
    }
    if (!supported || rootMirFnIndex == UINT32_MAX) {
        return 0;
    }
    sourceRef = lowerCtx.builder.funcs[rootMirFnIndex].sourceRef;
    if (sourceRef >= lowerCtx.execCtx.sourceFileCap) {
        return -1;
    }
    lowerCtx.execCtx.sourceFiles[sourceRef] = file;
    lowerRc = SLEvalMirRewriteTopInitCalls(
        &lowerCtx, file, initExprNode, rootMirFnIndex, &supported);
    if (lowerRc != 0) {
        return -1;
    }
    if (!supported) {
        return 0;
    }
    lowerCtx.execCtx.rootMirFnIndex = rootMirFnIndex;
    SLMirProgramBuilderFinish(&lowerCtx.builder, outProgram);
    *outExecCtx = lowerCtx.execCtx;
    *outRootMirFnIndex = rootMirFnIndex;
    *outSupported = 1;
    return 0;
}

static int SLEvalTryMirInvokeFunction(
    SLEvalProgram*        p,
    const SLEvalFunction* fn,
    int32_t               fnIndex,
    const SLCTFEValue*    args,
    uint32_t              argCount,
    SLCTFEValue*          outValue,
    int*                  outDidReturn,
    int*                  outIsConst) {
    SLMirProgram         program = { 0 };
    SLMirExecEnv         env = { 0 };
    SLEvalMirLowerCtx    lowerCtx;
    uint32_t             mirFnIndex = UINT32_MAX;
    int                  lowerRc;
    int                  mirIsConst = 0;
    if (p == NULL || fn == NULL || outValue == NULL || outDidReturn == NULL || outIsConst == NULL) {
        return -1;
    }
    *outDidReturn = 0;
    *outIsConst = 0;
    if (SLEvalMirInitLowerCtx(p, 0u, &lowerCtx) != 0) {
        return -1;
    }
    lowerRc = SLEvalMirLowerFunction(&lowerCtx, fnIndex, &mirFnIndex);
    if (lowerRc < 0) {
        return -1;
    }
    if (lowerRc == 0 || mirFnIndex == UINT32_MAX) {
        return 0;
    }
    lowerCtx.execCtx.rootMirFnIndex = mirFnIndex;
    SLMirProgramBuilderFinish(&lowerCtx.builder, &program);
    SLEvalMirInitExecEnv(p, fn->file, &env, &lowerCtx.execCtx);
    if (SLMirEvalFunction(
            p->arena, &program, mirFnIndex, args, argCount, &env, outValue, &mirIsConst)
        != 0)
    {
        return -1;
    }
    SLEvalMirAdaptOutValue(&lowerCtx.execCtx, outValue, &mirIsConst);
    *outIsConst = mirIsConst;
    if (mirIsConst) {
        *outDidReturn = outValue->kind != SLCTFEValue_INVALID;
    }
    return 0;
}

SL_API_END
