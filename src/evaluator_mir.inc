SL_API_BEGIN

enum {
    SL_EVAL_MIR_FN_NONE = UINT32_MAX,
    SL_EVAL_MIR_FN_IN_PROGRESS = UINT32_MAX - 1u,
};

typedef struct {
    SLEvalProgram*       p;
    SLMirProgramBuilder  builder;
    uint32_t*            evalToMir;
    uint32_t*            mirToEval;
    const SLParsedFile** sourceFiles;
    uint32_t             sourceFileCap;
    const SLParsedFile*  savedFiles[SL_EVAL_CALL_MAX_DEPTH];
    uint8_t              pushedFrames[SL_EVAL_CALL_MAX_DEPTH];
    uint32_t             savedFileLen;
    uint32_t             rootMirFnIndex;
    SLDiag*              diag;
} SLEvalMirLowerCtx;

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
    if (*outSourceRef >= c->sourceFileCap) {
        return -1;
    }
    c->sourceFiles[*outSourceRef] = file;
    return 0;
}

static int SLEvalMirEnterFunction(
    void* ctx, uint32_t functionIndex, uint32_t sourceRef, SLDiag* diag) {
    SLEvalMirLowerCtx* c = (SLEvalMirLowerCtx*)ctx;
    uint8_t            pushed = 0;
    if (c == NULL || c->p == NULL || sourceRef >= c->sourceFileCap
        || c->savedFileLen >= SL_EVAL_CALL_MAX_DEPTH)
    {
        if (diag != NULL) {
            diag->code = SLDiag_UNEXPECTED_TOKEN;
            diag->type = SLDiagTypeOfCode(diag->code);
            diag->start = 0;
            diag->end = 0;
            diag->argStart = 0;
            diag->argEnd = 0;
        }
        return -1;
    }
    if (functionIndex != c->rootMirFnIndex) {
        uint32_t evalFnIndex;
        if (c->mirToEval == NULL || functionIndex >= c->p->funcLen) {
            if (diag != NULL) {
                diag->code = SLDiag_UNEXPECTED_TOKEN;
                diag->type = SLDiagTypeOfCode(diag->code);
                diag->start = 0;
                diag->end = 0;
                diag->argStart = 0;
                diag->argEnd = 0;
            }
            return -1;
        }
        evalFnIndex = c->mirToEval[functionIndex];
        if (evalFnIndex >= c->p->funcLen || c->p->callDepth >= SL_EVAL_CALL_MAX_DEPTH) {
            if (diag != NULL) {
                diag->code = SLDiag_UNEXPECTED_TOKEN;
                diag->type = SLDiagTypeOfCode(diag->code);
                diag->start = 0;
                diag->end = 0;
                diag->argStart = 0;
                diag->argEnd = 0;
            }
            return -1;
        }
        c->p->callStack[c->p->callDepth++] = evalFnIndex;
        pushed = 1;
    }
    c->savedFiles[c->savedFileLen++] = c->p->currentFile;
    c->pushedFrames[c->savedFileLen - 1u] = pushed;
    if (c->sourceFiles[sourceRef] != NULL) {
        c->p->currentFile = c->sourceFiles[sourceRef];
    }
    return 0;
}

static void SLEvalMirLeaveFunction(void* ctx) {
    SLEvalMirLowerCtx* c = (SLEvalMirLowerCtx*)ctx;
    if (c == NULL || c->p == NULL || c->savedFileLen == 0) {
        return;
    }
    if (c->pushedFrames[c->savedFileLen - 1u] && c->p->callDepth > 0) {
        c->p->callDepth--;
    }
    c->p->currentFile = c->savedFiles[--c->savedFileLen];
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
        const SLAst*     ast = &callerFn->file->ast;
        const SLPackage* targetPkg = NULL;
        int32_t          resolvedFnIndex = -1;
        int              found = 0;
        int              ambiguous = 0;
        int32_t          stack[256];
        uint32_t         stackLen = 0;
        if (callerFn->bodyNode < 0 || (uint32_t)callerFn->bodyNode >= ast->len) {
            return 0;
        }
        stack[stackLen++] = callerFn->bodyNode;
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
                           == (uint32_t)(ins->tok & ~SLMirCallArgFlag_RECEIVER_ARG0))
                {
                    const SLPackage* currentPkg = callerFn->pkg;
                    uint32_t         i;
                    targetPkg = NULL;
                    if (currentPkg != NULL) {
                        for (i = 0; i < currentPkg->importLen; i++) {
                            const SLImportRef* imp = &currentPkg->imports[i];
                            if (imp->bindName == NULL || imp->target == NULL) {
                                continue;
                            }
                            if (strlen(imp->bindName)
                                    == (size_t)(ast->nodes[baseNode].dataEnd
                                                - ast->nodes[baseNode].dataStart)
                                && memcmp(
                                       imp->bindName,
                                       callerFn->file->source + ast->nodes[baseNode].dataStart,
                                       (size_t)(ast->nodes[baseNode].dataEnd
                                                - ast->nodes[baseNode].dataStart))
                                       == 0)
                            {
                                targetPkg = imp->target;
                                break;
                            }
                        }
                    }
                    if (targetPkg != NULL) {
                        int32_t fnIndex = SLEvalResolveFunctionBySlice(
                            c->p,
                            targetPkg,
                            callerFn->file,
                            symbol->nameStart,
                            symbol->nameEnd,
                            NULL,
                            (uint32_t)ins->tok - 1u);
                        if (fnIndex == -2) {
                            return 0;
                        }
                        if (fnIndex >= 0 && (uint32_t)fnIndex < c->p->funcLen
                            && !c->p->funcs[(uint32_t)fnIndex].isBuiltinPackageFn
                            && !c->p->funcs[(uint32_t)fnIndex].isVariadic)
                        {
                            if (!found) {
                                resolvedFnIndex = fnIndex;
                                found = 1;
                            } else if (resolvedFnIndex != fnIndex) {
                                ambiguous = 1;
                                break;
                            }
                        }
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
        if (!found || ambiguous || resolvedFnIndex < 0 || resolvedFnIndex == callerFnIndex) {
            return 0;
        }
        *outEvalFnIndex = (uint32_t)resolvedFnIndex;
        return 1;
    }
    if (symbol->flags != 0u) {
        return 0;
    }
    targetFnIndex = SLEvalResolveFunctionBySlice(
        c->p, NULL, callerFn->file, symbol->nameStart, symbol->nameEnd, NULL, (uint32_t)ins->tok);
    if (targetFnIndex < 0 || targetFnIndex == callerFnIndex
        || (uint32_t)targetFnIndex >= c->p->funcLen)
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
    const SLAst*          ast;
    int32_t               stack[256];
    uint32_t              stackLen = 0;
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
    ast = &callerFn->file->ast;
    if ((uint32_t)callerFn->bodyNode >= ast->len) {
        return 0;
    }
    stack[stackLen++] = callerFn->bodyNode;
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
                       == (uint32_t)(ins->tok & ~SLMirCallArgFlag_RECEIVER_ARG0))
            {
                const SLPackage* currentPkg = callerFn->pkg;
                uint32_t         i;
                for (i = 0; currentPkg != NULL && i < currentPkg->importLen; i++) {
                    const SLImportRef* imp = &currentPkg->imports[i];
                    if (imp->bindName == NULL || imp->target == NULL) {
                        continue;
                    }
                    if (strlen(imp->bindName)
                            == (size_t)(ast->nodes[baseNode].dataEnd
                                        - ast->nodes[baseNode].dataStart)
                        && memcmp(
                               imp->bindName,
                               callerFn->file->source + ast->nodes[baseNode].dataStart,
                               (size_t)(ast->nodes[baseNode].dataEnd
                                        - ast->nodes[baseNode].dataStart))
                               == 0
                        && StrEq(imp->target->name, "platform")
                        && SliceEqCStr(
                            callerFn->file->source, symbol->nameStart, symbol->nameEnd, "exit"))
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
    if (c->evalToMir[(uint32_t)evalFnIndex] == SL_EVAL_MIR_FN_IN_PROGRESS) {
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
    c->evalToMir[(uint32_t)evalFnIndex] = SL_EVAL_MIR_FN_IN_PROGRESS;
    if (SLEvalMirInternSourceFile(c, fn->file, &sourceRefIndex) != 0) {
        c->evalToMir[(uint32_t)evalFnIndex] = SL_EVAL_MIR_FN_NONE;
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
        c->evalToMir[(uint32_t)evalFnIndex] = SL_EVAL_MIR_FN_NONE;
        return -1;
    }
    if (!supported) {
        c->evalToMir[(uint32_t)evalFnIndex] = SL_EVAL_MIR_FN_NONE;
        return 0;
    }
    c->evalToMir[(uint32_t)evalFnIndex] = mirFnIndex;
    if (c->mirToEval != NULL && mirFnIndex < c->p->funcLen) {
        c->mirToEval[mirFnIndex] = (uint32_t)evalFnIndex;
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
            return -1;
        }
        if (lowerRc == 0) {
            c->evalToMir[(uint32_t)evalFnIndex] = SL_EVAL_MIR_FN_NONE;
            return 0;
        }
        ins->op = SLMirOp_CALL_FN;
        ins->aux = targetMirFnIndex;
        if (dropReceiverArg0) {
            ins->tok |= SLMirCallArgFlag_RECEIVER_ARG0;
        }
    }
    if (outMirFnIndex != NULL) {
        *outMirFnIndex = mirFnIndex;
    }
    return 1;
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
    SLMirProgramBuilder  builder;
    SLMirExecEnv         env = { 0 };
    SLEvalMirLowerCtx    lowerCtx;
    uint32_t*            evalToMir;
    uint32_t*            mirToEval;
    const SLParsedFile** sourceFiles;
    uint32_t             mirFnIndex = UINT32_MAX;
    uint32_t             i;
    int                  lowerRc;
    int                  mirIsConst = 0;
    if (p == NULL || fn == NULL || outValue == NULL || outDidReturn == NULL || outIsConst == NULL) {
        return -1;
    }
    *outDidReturn = 0;
    *outIsConst = 0;
    evalToMir = (uint32_t*)SLArenaAlloc(
        p->arena, sizeof(uint32_t) * p->funcLen, (uint32_t)_Alignof(uint32_t));
    mirToEval = (uint32_t*)SLArenaAlloc(
        p->arena, sizeof(uint32_t) * p->funcLen, (uint32_t)_Alignof(uint32_t));
    sourceFiles = (const SLParsedFile**)SLArenaAlloc(
        p->arena,
        sizeof(const SLParsedFile*) * p->funcLen,
        (uint32_t)_Alignof(const SLParsedFile*));
    if (evalToMir == NULL || mirToEval == NULL || sourceFiles == NULL) {
        return ErrorSimple("out of memory");
    }
    for (i = 0; i < p->funcLen; i++) {
        evalToMir[i] = SL_EVAL_MIR_FN_NONE;
        mirToEval[i] = UINT32_MAX;
        sourceFiles[i] = NULL;
    }
    SLMirProgramBuilderInit(&builder, p->arena);
    memset(&lowerCtx, 0, sizeof(lowerCtx));
    lowerCtx.p = p;
    lowerCtx.builder = builder;
    lowerCtx.evalToMir = evalToMir;
    lowerCtx.mirToEval = mirToEval;
    lowerCtx.sourceFiles = sourceFiles;
    lowerCtx.sourceFileCap = p->funcLen;
    lowerCtx.rootMirFnIndex = UINT32_MAX;
    lowerCtx.diag = p->currentExecCtx != NULL ? p->currentExecCtx->diag : NULL;
    lowerRc = SLEvalMirLowerFunction(&lowerCtx, fnIndex, &mirFnIndex);
    if (lowerRc < 0) {
        return -1;
    }
    if (lowerRc == 0 || mirFnIndex == UINT32_MAX) {
        return 0;
    }
    lowerCtx.rootMirFnIndex = mirFnIndex;
    SLMirProgramBuilderFinish(&lowerCtx.builder, &program);
    env.src = (SLStrView){ fn->file->source, fn->file->sourceLen };
    env.resolveIdent = SLEvalResolveIdent;
    env.resolveCall = SLEvalResolveCall;
    env.resolveCtx = p;
    env.hostCall = SLEvalMirHostCall;
    env.hostCtx = p;
    env.zeroInitLocal = SLEvalMirZeroInitLocal;
    env.zeroInitCtx = p;
    env.coerceValueForType = SLEvalMirCoerceValueForType;
    env.coerceValueCtx = p;
    env.enterFunction = SLEvalMirEnterFunction;
    env.leaveFunction = SLEvalMirLeaveFunction;
    env.functionCtx = &lowerCtx;
    env.diag = p->currentExecCtx != NULL ? p->currentExecCtx->diag : NULL;
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
