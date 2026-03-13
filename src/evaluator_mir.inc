SL_API_BEGIN

enum {
    SL_EVAL_MIR_FN_NONE = UINT32_MAX,
};

typedef struct {
    SLEvalProgram*       p;
    SLMirProgramBuilder  builder;
    uint32_t*            evalToMir;
    uint8_t*             loweringFns;
    SLEvalMirExecCtx     execCtx;
    SLDiag*              diag;
} SLEvalMirLowerCtx;

static int SLEvalMirInitLowerCtx(
    SLEvalProgram*      p,
    uint32_t            extraMirFuncs,
    SLEvalMirLowerCtx* _Nonnull outCtx) {
    uint32_t*            evalToMir;
    uint8_t*             loweringFns;
    uint32_t*            mirToEval;
    const SLParsedFile** sourceFiles;
    uint32_t             i;
    uint32_t             mirCap;
    if (p == NULL || outCtx == NULL) {
        return -1;
    }
    memset(outCtx, 0, sizeof(*outCtx));
    mirCap = p->funcLen + extraMirFuncs;
    if (mirCap < p->funcLen) {
        return -1;
    }
    evalToMir = (uint32_t*)SLArenaAlloc(
        p->arena, sizeof(uint32_t) * p->funcLen, (uint32_t)_Alignof(uint32_t));
    loweringFns = (uint8_t*)SLArenaAlloc(
        p->arena, sizeof(uint8_t) * p->funcLen, (uint32_t)_Alignof(uint8_t));
    mirToEval = (uint32_t*)SLArenaAlloc(
        p->arena, sizeof(uint32_t) * mirCap, (uint32_t)_Alignof(uint32_t));
    sourceFiles = (const SLParsedFile**)SLArenaAlloc(
        p->arena, sizeof(const SLParsedFile*) * mirCap, (uint32_t)_Alignof(const SLParsedFile*));
    if (evalToMir == NULL || loweringFns == NULL || mirToEval == NULL || sourceFiles == NULL) {
        return ErrorSimple("out of memory");
    }
    for (i = 0; i < p->funcLen; i++) {
        evalToMir[i] = SL_EVAL_MIR_FN_NONE;
        loweringFns[i] = 0u;
    }
    for (i = 0; i < mirCap; i++) {
        mirToEval[i] = UINT32_MAX;
        sourceFiles[i] = NULL;
    }
    SLMirProgramBuilderInit(&outCtx->builder, p->arena);
    outCtx->p = p;
    outCtx->evalToMir = evalToMir;
    outCtx->loweringFns = loweringFns;
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
                && AstListCount(ast, nodeId) + 1u
                       == (encodedArgCount & ~SLMirCallArgFlag_RECEIVER_ARG0))
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

static int SLEvalMirRewriteBuiltinHostCall(
    SLEvalMirLowerCtx*    c,
    const SLEvalFunction* callerFn,
    SLMirInst*            ins,
    int* _Nonnull outRewritten) {
    const SLMirSymbolRef* symbol;
    SLMirHostRef          host = { 0 };
    uint32_t              hostIndex = UINT32_MAX;
    if (outRewritten != NULL) {
        *outRewritten = 0;
    }
    if (c == NULL || callerFn == NULL || ins == NULL || outRewritten == NULL
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
        && SliceEqCStr(callerFn->file->source, symbol->nameStart, symbol->nameEnd, "print"))
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
    return 0;
}

static int SLEvalMirRewriteQualifiedHostCall(
    SLEvalMirLowerCtx*    c,
    const SLEvalFunction* callerFn,
    SLMirInst*            ins,
    int* _Nonnull outRewritten) {
    const SLMirSymbolRef* symbol;
    const SLPackage*      targetPkg;
    uint32_t              baseStart = 0;
    uint32_t              baseEnd = 0;
    if (outRewritten != NULL) {
        *outRewritten = 0;
    }
    if (c == NULL || callerFn == NULL || ins == NULL || outRewritten == NULL
        || ins->op != SLMirOp_CALL || c->builder.symbols == NULL
        || ins->aux >= c->builder.symbolLen)
    {
        return 0;
    }
    symbol = &c->builder.symbols[ins->aux];
    if (symbol->kind != SLMirSymbol_CALL || symbol->flags != SLMirSymbolFlag_CALL_RECEIVER_ARG0
        || callerFn->bodyNode < 0)
    {
        return 0;
    }
    if (!SLEvalMirMatchQualifiedCallBaseSlice(
            &callerFn->file->ast,
            symbol,
            callerFn->bodyNode,
            (uint32_t)ins->tok,
            &baseStart,
            &baseEnd))
    {
        return 0;
    }
    targetPkg = SLEvalMirFindImportTargetByAliasSlice(
        callerFn->pkg, callerFn->file->source, baseStart, baseEnd);
    if (targetPkg != NULL && StrEq(targetPkg->name, "platform")
        && SliceEqCStr(callerFn->file->source, symbol->nameStart, symbol->nameEnd, "exit"))
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
        uint32_t   targetEvalFnIndex = UINT32_MAX;
        uint32_t   targetMirFnIndex = UINT32_MAX;
        int        lowerRc;
        int        rewrittenHost = 0;
        int        dropReceiverArg0 = 0;
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
        if (ins->op == SLMirOp_CALL && c->builder.symbols != NULL && ins->aux < c->builder.symbolLen
            && (c->builder.symbols[ins->aux].flags & SLMirSymbolFlag_CALL_RECEIVER_ARG0) != 0u)
        {
            dropReceiverArg0 = 1;
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
        ins->op = SLMirOp_CALL_FN;
        ins->aux = targetMirFnIndex;
        if (dropReceiverArg0) {
            ins->tok |= SLMirCallArgFlag_RECEIVER_ARG0;
        }
    }
    if (c->loweringFns != NULL) {
        c->loweringFns[(uint32_t)evalFnIndex] = 0u;
    }
    if (outMirFnIndex != NULL) {
        *outMirFnIndex = mirFnIndex;
    }
    return 1;
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
        uint32_t   targetEvalFnIndex = UINT32_MAX;
        uint32_t   targetMirFnIndex = UINT32_MAX;
        int        lowerRc;
        int        dropReceiverArg0 = 0;
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
            if (c->builder.symbols != NULL && ins->aux < c->builder.symbolLen
                && (c->builder.symbols[ins->aux].flags & SLMirSymbolFlag_CALL_RECEIVER_ARG0) != 0u)
            {
                dropReceiverArg0 = 1;
            }
            ins->op = SLMirOp_CALL_FN;
            ins->aux = targetMirFnIndex;
            if (dropReceiverArg0) {
                ins->tok |= SLMirCallArgFlag_RECEIVER_ARG0;
            }
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
        ins->op = SLMirOp_CALL_FN;
        ins->aux = targetMirFnIndex;
    }
    *outSupported = 1;
    return 0;
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
    if (SLMirLowerAppendNamedTopInitFunction(
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
    *outIsConst = mirIsConst;
    if (mirIsConst) {
        *outDidReturn = outValue->kind != SLCTFEValue_INVALID;
    }
    return 0;
}

SL_API_END
