#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libhop-impl.h"
#include "ctfe.h"
#include "ctfe_exec.h"
#include "evaluator_internal.inc.h"
#include "mir_exec.h"
#include "mir_lower.h"
#include "mir_lower_pkg.h"
#include "mir_lower_stmt.h"

H2_API_BEGIN

typedef struct {
    HOPEvalProgram*     p;
    const H2ParsedFile* file;
} HOPEvalMirLowerConstCtx;

static int HOPEvalMirLowerConstExpr(
    void* _Nullable ctx, int32_t exprNode, H2MirConst* _Nonnull outValue, H2Diag* _Nullable diag) {
    HOPEvalMirLowerConstCtx* lowerCtx = (HOPEvalMirLowerConstCtx*)ctx;
    H2CTFEValue              value;
    int32_t                  typeNode;
    int                      rc;
    (void)diag;
    if (lowerCtx == NULL || lowerCtx->p == NULL || lowerCtx->file == NULL || outValue == NULL
        || exprNode < 0 || (uint32_t)exprNode >= lowerCtx->file->ast.len)
    {
        return -1;
    }
    if (lowerCtx->file->ast.nodes[exprNode].kind != H2Ast_TYPE_VALUE) {
        return 0;
    }
    typeNode = ASTFirstChild(&lowerCtx->file->ast, exprNode);
    rc = HOPEvalTypeValueFromTypeNode(lowerCtx->p, lowerCtx->file, typeNode, &value);
    if (rc < 0) {
        return -1;
    }
    if (rc == 0 || value.kind != H2CTFEValue_TYPE) {
        return 0;
    }
    outValue->kind = H2MirConst_TYPE;
    outValue->bits = value.typeTag;
    outValue->bytes.ptr = (const char*)value.s.bytes;
    outValue->bytes.len = value.s.len;
    return 1;
}

static const uint32_t HOP_EVAL_MIR_FN_NONE = UINT32_MAX;

typedef struct {
    HOPEvalProgram*     p;
    H2MirProgramBuilder builder;
    uint32_t*           evalToMir;
    uint8_t*            loweringFns;
    uint32_t*           topConstToMir;
    uint8_t*            loweringTopConsts;
    uint32_t*           topVarToMir;
    uint8_t*            loweringTopVars;
    HOPEvalMirExecCtx   execCtx;
    H2Diag*             diag;
} HOPEvalMirLowerCtx;

static int HOPEvalBuiltinPackageFnHasHopEvaluatorBody(const HOPEvalFunction* fn) {
    return fn != NULL && fn->isBuiltinPackageFn && fn->file != NULL && fn->file->path != NULL
        && HasSuffix(fn->file->path, "/builtin/format.hop");
}

static int HOPEvalMirInitLowerCtx(
    HOPEvalProgram* p, uint32_t extraMirFuncs, HOPEvalMirLowerCtx* _Nonnull outCtx) {
    uint32_t*            evalToMir;
    uint8_t*             loweringFns;
    uint32_t*            topConstToMir;
    uint8_t*             loweringTopConsts;
    uint32_t*            topVarToMir;
    uint8_t*             loweringTopVars;
    uint32_t*            mirToEval;
    const H2ParsedFile** sourceFiles;
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
    evalToMir = (uint32_t*)H2ArenaAlloc(
        p->arena, sizeof(uint32_t) * p->funcLen, (uint32_t)_Alignof(uint32_t));
    loweringFns = (uint8_t*)H2ArenaAlloc(
        p->arena, sizeof(uint8_t) * p->funcLen, (uint32_t)_Alignof(uint8_t));
    topConstToMir = (uint32_t*)H2ArenaAlloc(
        p->arena, sizeof(uint32_t) * p->topConstLen, (uint32_t)_Alignof(uint32_t));
    loweringTopConsts = (uint8_t*)H2ArenaAlloc(
        p->arena, sizeof(uint8_t) * p->topConstLen, (uint32_t)_Alignof(uint8_t));
    topVarToMir = (uint32_t*)H2ArenaAlloc(
        p->arena, sizeof(uint32_t) * p->topVarLen, (uint32_t)_Alignof(uint32_t));
    loweringTopVars = (uint8_t*)H2ArenaAlloc(
        p->arena, sizeof(uint8_t) * p->topVarLen, (uint32_t)_Alignof(uint8_t));
    mirToEval = (uint32_t*)H2ArenaAlloc(
        p->arena, sizeof(uint32_t) * mirCap, (uint32_t)_Alignof(uint32_t));
    sourceFiles = (const H2ParsedFile**)H2ArenaAlloc(
        p->arena, sizeof(const H2ParsedFile*) * mirCap, (uint32_t)_Alignof(const H2ParsedFile*));
    if (evalToMir == NULL || loweringFns == NULL || topConstToMir == NULL
        || loweringTopConsts == NULL || topVarToMir == NULL || loweringTopVars == NULL
        || mirToEval == NULL || sourceFiles == NULL)
    {
        return ErrorSimple("out of memory");
    }
    for (i = 0; i < p->funcLen; i++) {
        evalToMir[i] = HOP_EVAL_MIR_FN_NONE;
        loweringFns[i] = 0u;
    }
    for (i = 0; i < p->topConstLen; i++) {
        topConstToMir[i] = HOP_EVAL_MIR_FN_NONE;
        loweringTopConsts[i] = 0u;
    }
    for (i = 0; i < p->topVarLen; i++) {
        topVarToMir[i] = HOP_EVAL_MIR_FN_NONE;
        loweringTopVars[i] = 0u;
    }
    for (i = 0; i < mirCap; i++) {
        mirToEval[i] = UINT32_MAX;
        sourceFiles[i] = NULL;
    }
    H2MirProgramBuilderInit(&outCtx->builder, p->arena);
    outCtx->p = p;
    outCtx->evalToMir = evalToMir;
    outCtx->loweringFns = loweringFns;
    outCtx->topConstToMir = topConstToMir;
    outCtx->loweringTopConsts = loweringTopConsts;
    outCtx->topVarToMir = topVarToMir;
    outCtx->loweringTopVars = loweringTopVars;
    outCtx->execCtx.p = p;
    outCtx->execCtx.evalToMir = evalToMir;
    outCtx->execCtx.evalToMirLen = p->funcLen;
    outCtx->execCtx.mirToEval = mirToEval;
    outCtx->execCtx.mirToEvalLen = mirCap;
    outCtx->execCtx.sourceFiles = sourceFiles;
    outCtx->execCtx.sourceFileCap = mirCap;
    outCtx->execCtx.rootMirFnIndex = UINT32_MAX;
    outCtx->execCtx.savedFileLen = 0u;
    outCtx->diag = p->currentExecCtx != NULL ? p->currentExecCtx->diag : NULL;
    return 0;
}

static int HOPEvalMirInternSourceFile(
    HOPEvalMirLowerCtx* c, const H2ParsedFile* file, uint32_t* _Nonnull outSourceRef) {
    H2MirSourceRef sourceRef = { 0 };
    if (c == NULL || file == NULL || outSourceRef == NULL) {
        return -1;
    }
    sourceRef.src.ptr = file->source;
    sourceRef.src.len = file->sourceLen;
    if (H2MirProgramBuilderAddSource(&c->builder, &sourceRef, outSourceRef) != 0) {
        return -1;
    }
    if (*outSourceRef >= c->execCtx.sourceFileCap) {
        return -1;
    }
    c->execCtx.sourceFiles[*outSourceRef] = file;
    return 0;
}

static int HOPEvalMirAdaptAggregateValue(
    const HOPEvalMirExecCtx* c, HOPEvalAggregate* _Nullable agg, uint32_t depth);

static int HOPEvalMirAdaptValue(
    const HOPEvalMirExecCtx* c, H2CTFEValue* _Nullable value, uint32_t depth) {
    uint32_t           mirFnIndex = UINT32_MAX;
    uint32_t           evalFnIndex = UINT32_MAX;
    H2CTFEValue*       payload;
    H2CTFEValue*       target;
    HOPEvalArray*      array;
    HOPEvalTaggedEnum* tagged;
    uint32_t           i;

    if (c == NULL || value == NULL) {
        return 1;
    }
    if (depth > 64u) {
        return 0;
    }

    if (H2MirValueAsFunctionRef(value, &mirFnIndex)) {
        if (c->mirToEval == NULL || mirFnIndex >= c->mirToEvalLen) {
            return 0;
        }
        evalFnIndex = c->mirToEval[mirFnIndex];
        if (evalFnIndex == UINT32_MAX) {
            return 0;
        }
        HOPEvalValueSetFunctionRef(value, evalFnIndex);
        return 1;
    }

    switch (value->kind) {
        case H2CTFEValue_OPTIONAL:
            if (value->b == 0u || value->s.bytes == NULL) {
                return 1;
            }
            payload = (H2CTFEValue*)value->s.bytes;
            return HOPEvalMirAdaptValue(c, payload, depth + 1u);

        case H2CTFEValue_AGGREGATE:
            return HOPEvalMirAdaptAggregateValue(c, HOPEvalValueAsAggregate(value), depth + 1u);

        case H2CTFEValue_ARRAY:
            array = HOPEvalValueAsArray(value);
            if (array == NULL || array->len == 0u || array->elems == NULL) {
                return 1;
            }
            for (i = 0; i < array->len; i++) {
                if (!HOPEvalMirAdaptValue(c, &array->elems[i], depth + 1u)) {
                    return 0;
                }
            }
            return 1;

        case H2CTFEValue_REFERENCE:
            target = HOPEvalValueReferenceTarget(value);
            return target == NULL ? 1 : HOPEvalMirAdaptValue(c, target, depth + 1u);

        case H2CTFEValue_TYPE:
            tagged = HOPEvalValueAsTaggedEnum(value);
            if (tagged != NULL && tagged->payload != NULL) {
                return HOPEvalMirAdaptValue(c, tagged->payload, depth + 1u);
            }
            return 1;

        default: return 1;
    }
}

static int HOPEvalMirAdaptAggregateValue(
    const HOPEvalMirExecCtx* c, HOPEvalAggregate* _Nullable agg, uint32_t depth) {
    uint32_t i;

    if (agg == NULL) {
        return 1;
    }
    for (i = 0; i < agg->fieldLen; i++) {
        if (!HOPEvalMirAdaptValue(c, &agg->fields[i].value, depth + 1u)) {
            return 0;
        }
    }
    return 1;
}

void HOPEvalMirAdaptOutValue(
    const HOPEvalMirExecCtx* c, H2CTFEValue* _Nullable value, int* _Nullable inOutIsConst) {
    if (inOutIsConst != NULL && !*inOutIsConst) {
        return;
    }
    if (c == NULL || value == NULL) {
        return;
    }
    if (!HOPEvalMirAdaptValue(c, value, 0u)) {
        if (inOutIsConst != NULL) {
            *inOutIsConst = 0;
        }
    }
}

static const H2Package* _Nullable HOPEvalMirFindImportTargetByAliasSlice(
    const H2Package* pkg, const char* src, uint32_t start, uint32_t end) {
    uint32_t i;
    if (pkg == NULL || src == NULL) {
        return NULL;
    }
    for (i = 0; i < pkg->importLen; i++) {
        const H2ImportRef* imp = &pkg->imports[i];
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

static int HOPEvalMirMatchQualifiedCallBaseSlice(
    const H2Ast*          ast,
    const H2MirSymbolRef* symbol,
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
    if (ast == NULL || symbol == NULL || outBaseStart == NULL || outBaseEnd == NULL || rootNode < 0
        || (uint32_t)rootNode >= ast->len || symbol->flags != H2MirSymbolFlag_CALL_RECEIVER_ARG0)
    {
        return 0;
    }
    stack[stackLen++] = rootNode;
    while (stackLen > 0) {
        int32_t          nodeId = stack[--stackLen];
        const H2AstNode* node = &ast->nodes[nodeId];
        int32_t          child;
        if (node->kind == H2Ast_CALL) {
            int32_t calleeNode = node->firstChild;
            int32_t baseNode = calleeNode >= 0 ? ast->nodes[calleeNode].firstChild : -1;
            if (calleeNode >= 0 && (uint32_t)calleeNode < ast->len
                && ast->nodes[calleeNode].kind == H2Ast_FIELD_EXPR && baseNode >= 0
                && (uint32_t)baseNode < ast->len && ast->nodes[baseNode].kind == H2Ast_IDENT
                && ast->nodes[calleeNode].dataStart == symbol->nameStart
                && ast->nodes[calleeNode].dataEnd == symbol->nameEnd
                && AstListCount(ast, nodeId) == H2MirCallArgCountFromTok((uint16_t)encodedArgCount))
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

static int HOPEvalMirResolveQualifiedCallTargetInNode(
    const HOPEvalMirLowerCtx* c,
    const H2Package*          currentPkg,
    const H2ParsedFile*       file,
    int32_t                   rootNode,
    const H2MirInst*          ins,
    uint32_t*                 outEvalFnIndex) {
    const H2MirSymbolRef* symbol;
    const H2Package*      targetPkg;
    int32_t               fnIndex;
    uint32_t              baseStart = 0;
    uint32_t              baseEnd = 0;
    if (outEvalFnIndex != NULL) {
        *outEvalFnIndex = UINT32_MAX;
    }
    if (c == NULL || currentPkg == NULL || file == NULL || ins == NULL || outEvalFnIndex == NULL
        || ins->op != H2MirOp_CALL || c->builder.symbols == NULL
        || ins->aux >= c->builder.symbolLen)
    {
        return 0;
    }
    symbol = &c->builder.symbols[ins->aux];
    if (symbol->kind != H2MirSymbol_CALL
        || !HOPEvalMirMatchQualifiedCallBaseSlice(
            &file->ast, symbol, rootNode, (uint32_t)ins->tok, &baseStart, &baseEnd))
    {
        return 0;
    }
    targetPkg = HOPEvalMirFindImportTargetByAliasSlice(
        currentPkg, file->source, baseStart, baseEnd);
    if (targetPkg == NULL) {
        return 0;
    }
    fnIndex = HOPEvalResolveFunctionBySlice(
        c->p,
        targetPkg,
        file,
        symbol->nameStart,
        symbol->nameEnd,
        NULL,
        H2MirCallArgCountFromTok(ins->tok) - 1u);
    if (fnIndex == -2) {
        return 0;
    }
    if (fnIndex < 0 || (uint32_t)fnIndex >= c->p->funcLen
        || c->p->funcs[(uint32_t)fnIndex].isBuiltinPackageFn)
    {
        return 0;
    }
    *outEvalFnIndex = (uint32_t)fnIndex;
    return 1;
}

static int HOPEvalMirResolveDirectCallTarget(
    const HOPEvalMirLowerCtx* c,
    const HOPEvalFunction*    callerFn,
    int32_t                   callerFnIndex,
    const H2MirInst*          ins,
    uint32_t*                 outEvalFnIndex) {
    const H2MirSymbolRef* symbol;
    int32_t               targetFnIndex;
    if (outEvalFnIndex != NULL) {
        *outEvalFnIndex = UINT32_MAX;
    }
    if (c == NULL || callerFn == NULL || ins == NULL || outEvalFnIndex == NULL
        || ins->op != H2MirOp_CALL || c->builder.symbols == NULL
        || ins->aux >= c->builder.symbolLen)
    {
        return 0;
    }
    symbol = &c->builder.symbols[ins->aux];
    if (symbol->kind != H2MirSymbol_CALL) {
        return 0;
    }
    if (symbol->flags == H2MirSymbolFlag_CALL_RECEIVER_ARG0) {
        return HOPEvalMirResolveQualifiedCallTargetInNode(
            c, callerFn->pkg, callerFn->file, callerFn->bodyNode, ins, outEvalFnIndex);
    }
    if (symbol->flags != 0u) {
        return 0;
    }
    targetFnIndex = HOPEvalResolveFunctionBySlice(
        c->p,
        NULL,
        callerFn->file,
        symbol->nameStart,
        symbol->nameEnd,
        NULL,
        H2MirCallArgCountFromTok(ins->tok));
    if (targetFnIndex < 0 || (uint32_t)targetFnIndex >= c->p->funcLen) {
        return 0;
    }
    if (c->p->funcs[(uint32_t)targetFnIndex].isBuiltinPackageFn) {
        return 0;
    }
    *outEvalFnIndex = (uint32_t)targetFnIndex;
    return 1;
}

static int HOPEvalMirResolvePlainDirectCallTargetForFile(
    const HOPEvalMirLowerCtx* c,
    const H2ParsedFile*       file,
    const H2MirInst*          ins,
    uint32_t*                 outEvalFnIndex) {
    const H2MirSymbolRef* symbol;
    int32_t               targetFnIndex;
    if (outEvalFnIndex != NULL) {
        *outEvalFnIndex = UINT32_MAX;
    }
    if (c == NULL || file == NULL || ins == NULL || outEvalFnIndex == NULL
        || ins->op != H2MirOp_CALL || c->builder.symbols == NULL
        || ins->aux >= c->builder.symbolLen)
    {
        return 0;
    }
    symbol = &c->builder.symbols[ins->aux];
    if (symbol->kind != H2MirSymbol_CALL || symbol->flags != 0u) {
        return 0;
    }
    targetFnIndex = HOPEvalResolveFunctionBySlice(
        c->p,
        NULL,
        file,
        symbol->nameStart,
        symbol->nameEnd,
        NULL,
        H2MirCallArgCountFromTok(ins->tok));
    if (targetFnIndex < 0 || (uint32_t)targetFnIndex >= c->p->funcLen
        || c->p->funcs[(uint32_t)targetFnIndex].isBuiltinPackageFn)
    {
        return 0;
    }
    *outEvalFnIndex = (uint32_t)targetFnIndex;
    return 1;
}

static int HOPEvalMirResolveQualifiedCallTargetForNode(
    const HOPEvalMirLowerCtx* c,
    const H2ParsedFile*       file,
    int32_t                   rootNode,
    const H2MirInst*          ins,
    uint32_t*                 outEvalFnIndex) {
    const H2Package* currentPkg;
    if (outEvalFnIndex != NULL) {
        *outEvalFnIndex = UINT32_MAX;
    }
    if (c == NULL || file == NULL || ins == NULL || outEvalFnIndex == NULL
        || ins->op != H2MirOp_CALL || c->builder.symbols == NULL
        || ins->aux >= c->builder.symbolLen)
    {
        return 0;
    }
    currentPkg = HOPEvalFindPackageByFile(c->p, file);
    return HOPEvalMirResolveQualifiedCallTargetInNode(
        c, currentPkg, file, rootNode, ins, outEvalFnIndex);
}

static int HOPEvalMirRewriteBuiltinHostCallForFile(
    HOPEvalMirLowerCtx* c, const H2ParsedFile* file, H2MirInst* ins, int* _Nonnull outRewritten) {
    const H2MirSymbolRef* symbol;
    H2MirHostRef          host = { 0 };
    uint32_t              hostIndex = UINT32_MAX;
    if (outRewritten != NULL) {
        *outRewritten = 0;
    }
    if (c == NULL || file == NULL || ins == NULL || outRewritten == NULL || ins->op != H2MirOp_CALL
        || c->builder.symbols == NULL || ins->aux >= c->builder.symbolLen)
    {
        return 0;
    }
    symbol = &c->builder.symbols[ins->aux];
    if (symbol->kind != H2MirSymbol_CALL || symbol->flags != 0u) {
        return 0;
    }
    if (H2MirCallArgCountFromTok(ins->tok) == 1u
        && SliceEqCStr(file->source, symbol->nameStart, symbol->nameEnd, "print"))
    {
        host.nameStart = symbol->nameStart;
        host.nameEnd = symbol->nameEnd;
        host.kind = H2MirHost_GENERIC;
        host.flags = 0;
        host.target = HOP_EVAL_MIR_HOST_PRINT;
        if (H2MirProgramBuilderAddHost(&c->builder, &host, &hostIndex) != 0) {
            return -1;
        }
        ins->op = H2MirOp_CALL_HOST;
        ins->aux = hostIndex;
        *outRewritten = 1;
        return 1;
    }
    if (H2MirCallArgCountFromTok(ins->tok) == 2u
        && SliceEqCStr(file->source, symbol->nameStart, symbol->nameEnd, "copy"))
    {
        host.nameStart = symbol->nameStart;
        host.nameEnd = symbol->nameEnd;
        host.kind = H2MirHost_GENERIC;
        host.flags = 0;
        host.target = HOP_EVAL_MIR_HOST_COPY;
        if (H2MirProgramBuilderAddHost(&c->builder, &host, &hostIndex) != 0) {
            return -1;
        }
        ins->op = H2MirOp_CALL_HOST;
        ins->aux = hostIndex;
        *outRewritten = 1;
        return 1;
    }
    if (H2MirCallArgCountFromTok(ins->tok) == 2u
        && SliceEqCStr(file->source, symbol->nameStart, symbol->nameEnd, "concat"))
    {
        host.nameStart = symbol->nameStart;
        host.nameEnd = symbol->nameEnd;
        host.kind = H2MirHost_GENERIC;
        host.flags = 0;
        host.target = HOP_EVAL_MIR_HOST_CONCAT;
        if (H2MirProgramBuilderAddHost(&c->builder, &host, &hostIndex) != 0) {
            return -1;
        }
        ins->op = H2MirOp_CALL_HOST;
        ins->aux = hostIndex;
        *outRewritten = 1;
        return 1;
    }
    if ((H2MirCallArgCountFromTok(ins->tok) == 1u || H2MirCallArgCountFromTok(ins->tok) == 2u)
        && SliceEqCStr(file->source, symbol->nameStart, symbol->nameEnd, "free"))
    {
        host.nameStart = symbol->nameStart;
        host.nameEnd = symbol->nameEnd;
        host.kind = H2MirHost_GENERIC;
        host.flags = 0;
        host.target = HOP_EVAL_MIR_HOST_FREE;
        if (H2MirProgramBuilderAddHost(&c->builder, &host, &hostIndex) != 0) {
            return -1;
        }
        ins->op = H2MirOp_CALL_HOST;
        ins->aux = hostIndex;
        *outRewritten = 1;
        return 1;
    }
    return 0;
}

static int HOPEvalMirRewriteBuiltinHostCall(
    HOPEvalMirLowerCtx*    c,
    const HOPEvalFunction* callerFn,
    H2MirInst*             ins,
    int* _Nonnull outRewritten) {
    if (c == NULL || callerFn == NULL) {
        return 0;
    }
    return HOPEvalMirRewriteBuiltinHostCallForFile(c, callerFn->file, ins, outRewritten);
}

static int HOPEvalMirRewriteQualifiedHostCallForNode(
    HOPEvalMirLowerCtx* c,
    const H2Package*    currentPkg,
    const H2ParsedFile* file,
    int32_t             rootNode,
    H2MirInst*          ins,
    int* _Nonnull outRewritten) {
    const H2MirSymbolRef* symbol;
    const H2Package*      targetPkg;
    uint32_t              baseStart = 0;
    uint32_t              baseEnd = 0;
    if (outRewritten != NULL) {
        *outRewritten = 0;
    }
    if (c == NULL || currentPkg == NULL || file == NULL || ins == NULL || outRewritten == NULL
        || ins->op != H2MirOp_CALL || c->builder.symbols == NULL
        || ins->aux >= c->builder.symbolLen)
    {
        return 0;
    }
    symbol = &c->builder.symbols[ins->aux];
    if (symbol->kind != H2MirSymbol_CALL || symbol->flags != H2MirSymbolFlag_CALL_RECEIVER_ARG0
        || rootNode < 0)
    {
        return 0;
    }
    if (!HOPEvalMirMatchQualifiedCallBaseSlice(
            &file->ast, symbol, rootNode, (uint32_t)ins->tok, &baseStart, &baseEnd))
    {
        return 0;
    }
    targetPkg = HOPEvalMirFindImportTargetByAliasSlice(
        currentPkg, file->source, baseStart, baseEnd);
    if (targetPkg != NULL && StrEq(targetPkg->name, "platform")
        && SliceEqCStr(file->source, symbol->nameStart, symbol->nameEnd, "exit"))
    {
        H2MirHostRef host = { 0 };
        uint32_t     hostIndex = UINT32_MAX;
        host.nameStart = symbol->nameStart;
        host.nameEnd = symbol->nameEnd;
        host.kind = H2MirHost_GENERIC;
        host.flags = 0;
        host.target = HOP_EVAL_MIR_HOST_PLATFORM_EXIT;
        if (H2MirProgramBuilderAddHost(&c->builder, &host, &hostIndex) != 0) {
            return -1;
        }
        ins->op = H2MirOp_CALL_HOST;
        ins->aux = hostIndex;
        ins->tok |= H2MirCallArgFlag_RECEIVER_ARG0;
        *outRewritten = 1;
        return 1;
    }
    if (targetPkg != NULL && StrEq(targetPkg->name, "platform")
        && SliceEqCStr(file->source, symbol->nameStart, symbol->nameEnd, "console_log")
        && H2MirCallArgCountFromTok(ins->tok) == 3u)
    {
        H2MirHostRef host = { 0 };
        uint32_t     hostIndex = UINT32_MAX;
        host.nameStart = symbol->nameStart;
        host.nameEnd = symbol->nameEnd;
        host.kind = H2MirHost_GENERIC;
        host.flags = 0;
        host.target = HOP_EVAL_MIR_HOST_PLATFORM_CONSOLE_LOG;
        if (H2MirProgramBuilderAddHost(&c->builder, &host, &hostIndex) != 0) {
            return -1;
        }
        ins->op = H2MirOp_CALL_HOST;
        ins->aux = hostIndex;
        ins->tok |= H2MirCallArgFlag_RECEIVER_ARG0;
        *outRewritten = 1;
        return 1;
    }
    return 0;
}

static int HOPEvalMirRewriteQualifiedHostCall(
    HOPEvalMirLowerCtx*    c,
    const HOPEvalFunction* callerFn,
    H2MirInst*             ins,
    int* _Nonnull outRewritten) {
    if (c == NULL || callerFn == NULL) {
        return 0;
    }
    return HOPEvalMirRewriteQualifiedHostCallForNode(
        c, callerFn->pkg, callerFn->file, callerFn->bodyNode, ins, outRewritten);
}

static void HOPEvalMirRewriteCallToMirFunction(
    const HOPEvalMirLowerCtx* c, H2MirInst* ins, uint32_t targetMirFnIndex) {
    int preserveReceiverArg0 = 0;
    if (c != NULL && ins != NULL && c->builder.symbols != NULL && ins->aux < c->builder.symbolLen
        && (c->builder.symbols[ins->aux].flags & H2MirSymbolFlag_CALL_RECEIVER_ARG0) != 0u)
    {
        preserveReceiverArg0 = 1;
    }
    if (ins == NULL) {
        return;
    }
    ins->op = H2MirOp_CALL_FN;
    ins->aux = targetMirFnIndex;
    if (preserveReceiverArg0) {
        ins->tok |= H2MirCallArgFlag_RECEIVER_ARG0;
    }
}

static void HOPEvalMirRewriteLoadIdentToMirFunction(H2MirInst* ins, uint32_t targetMirFnIndex);
static int  HOPEvalMirLowerTopConst(
    HOPEvalMirLowerCtx* c, uint32_t topConstIndex, uint32_t* _Nullable outMirFnIndex);
static int HOPEvalMirLowerTopVar(
    HOPEvalMirLowerCtx* c, uint32_t topVarIndex, uint32_t* _Nullable outMirFnIndex);
static int HOPEvalMirFindAnyFunctionBySliceInPackage(
    const HOPEvalProgram* p,
    const H2Package*      pkg,
    const H2ParsedFile*   callerFile,
    uint32_t              nameStart,
    uint32_t              nameEnd);
static int HOPEvalMirResolveTopConstIdentTargetForFile(
    const HOPEvalMirLowerCtx* c,
    const H2ParsedFile*       file,
    const H2MirInst*          ins,
    uint32_t*                 outIndex);
static int HOPEvalMirResolveTopInitSliceTargetForPackage(
    const HOPEvalMirLowerCtx* c,
    const H2Package*          currentPkg,
    const H2ParsedFile*       file,
    uint32_t                  nameStart,
    uint32_t                  nameEnd,
    int*                      outIsTopConst,
    uint32_t*                 outIndex);
static int HOPEvalMirResolveTopInitIdentTargetForFile(
    const HOPEvalMirLowerCtx* c,
    const H2ParsedFile*       file,
    const H2MirInst*          ins,
    int*                      outIsTopConst,
    uint32_t*                 outIndex);
static int HOPEvalMirResolveFunctionIdentTargetForFile(
    const HOPEvalMirLowerCtx* c,
    const H2ParsedFile*       file,
    const H2MirInst*          ins,
    uint32_t*                 outIndex);
static int HOPEvalMirRewriteLoadIdentToFunctionConst(
    HOPEvalMirLowerCtx* c, H2MirInst* ins, uint32_t targetMirFnIndex);
static int HOPEvalMirResolveQualifiedValueLoadTargetForFile(
    const HOPEvalMirLowerCtx* c,
    const H2ParsedFile*       file,
    const H2MirInst*          loadIns,
    const H2MirInst* _Nullable fieldIns,
    int*      outTargetKind,
    uint32_t* outTargetIndex);
static int HOPEvalMirRewriteQualifiedValueLoad(
    HOPEvalMirLowerCtx* c,
    uint32_t            ownerMirFnIndex,
    uint32_t            loadInstIndex,
    int                 rewriteAsFunctionConst,
    uint32_t            targetMirFnIndex);
static int HOPEvalMirResolveSimpleFunctionValueAliasCallTarget(
    const HOPEvalMirLowerCtx* c,
    const H2ParsedFile*       file,
    const H2MirInst*          ins,
    uint32_t*                 outEvalFnIndex);
static int HOPEvalMirRewriteZeroArgFunctionValueCall(
    HOPEvalMirLowerCtx* c, const H2ParsedFile* file, uint32_t ownerMirFnIndex, uint32_t instIndex);

static int HOPEvalMirLowerFunction(
    HOPEvalMirLowerCtx* c, int32_t evalFnIndex, uint32_t* _Nullable outMirFnIndex) {
    const HOPEvalFunction* fn;
    uint32_t               mirFnIndex = UINT32_MAX;
    uint32_t               sourceRefIndex = UINT32_MAX;
    uint32_t               instIndex;
    int                    supported = 0;
    if (outMirFnIndex != NULL) {
        *outMirFnIndex = UINT32_MAX;
    }
    if (c == NULL || evalFnIndex < 0 || (uint32_t)evalFnIndex >= c->p->funcLen
        || c->evalToMir == NULL)
    {
        return -1;
    }
    if (c->loweringFns != NULL && c->loweringFns[(uint32_t)evalFnIndex] != 0u) {
        if (c->evalToMir[(uint32_t)evalFnIndex] != HOP_EVAL_MIR_FN_NONE) {
            if (outMirFnIndex != NULL) {
                *outMirFnIndex = c->evalToMir[(uint32_t)evalFnIndex];
            }
            return 1;
        }
        return 0;
    }
    if (c->evalToMir[(uint32_t)evalFnIndex] != HOP_EVAL_MIR_FN_NONE) {
        if (outMirFnIndex != NULL) {
            *outMirFnIndex = c->evalToMir[(uint32_t)evalFnIndex];
        }
        return 1;
    }
    fn = &c->p->funcs[(uint32_t)evalFnIndex];
    if (fn->isBuiltinPackageFn && !HOPEvalBuiltinPackageFnHasHopEvaluatorBody(fn)) {
        return 0;
    }
    if (c->loweringFns != NULL) {
        c->loweringFns[(uint32_t)evalFnIndex] = 1u;
    }
    if (HOPEvalMirInternSourceFile(c, fn->file, &sourceRefIndex) != 0) {
        if (c->loweringFns != NULL) {
            c->loweringFns[(uint32_t)evalFnIndex] = 0u;
        }
        return -1;
    }
    (void)sourceRefIndex;
    {
        HOPEvalMirLowerConstCtx lowerConstCtx;
        H2MirLowerOptions       lowerOptions;
        memset(&lowerConstCtx, 0, sizeof(lowerConstCtx));
        memset(&lowerOptions, 0, sizeof(lowerOptions));
        lowerConstCtx.p = c->p;
        lowerConstCtx.file = fn->file;
        lowerOptions.lowerConstExpr = HOPEvalMirLowerConstExpr;
        lowerOptions.lowerConstExprCtx = &lowerConstCtx;
        if (H2MirLowerAppendSimpleFunctionWithOptions(
                &c->builder,
                c->p->arena,
                &fn->file->ast,
                (H2StrView){ fn->file->source, fn->file->sourceLen },
                fn->fnNode,
                fn->bodyNode,
                &lowerOptions,
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
        H2MirInst* ins = &c->builder.insts[instIndex];
        H2MirInst* nextIns =
            instIndex + 1u < c->builder.instLen ? &c->builder.insts[instIndex + 1u] : NULL;
        int      isTopConst = 0;
        uint32_t targetTopInitIndex = UINT32_MAX;
        uint32_t targetFnIdentIndex = UINT32_MAX;
        uint32_t targetEvalFnIndex = UINT32_MAX;
        uint32_t targetMirFnIndex = UINT32_MAX;
        int      qualifiedValueTargetKind = 0;
        int      lowerRc;
        int      rewrittenBuiltinHost = 0;
        int      rewrittenHost = 0;
        if (HOPEvalMirResolveQualifiedValueLoadTargetForFile(
                c, fn->file, ins, nextIns, &qualifiedValueTargetKind, &targetTopInitIndex))
        {
            if (qualifiedValueTargetKind == 1) {
                lowerRc = HOPEvalMirLowerTopConst(c, targetTopInitIndex, &targetMirFnIndex);
            } else if (qualifiedValueTargetKind == 2) {
                lowerRc = HOPEvalMirLowerTopVar(c, targetTopInitIndex, &targetMirFnIndex);
            } else {
                lowerRc = HOPEvalMirLowerFunction(
                    c, (int32_t)targetTopInitIndex, &targetMirFnIndex);
            }
            if (lowerRc < 0) {
                if (c->loweringFns != NULL) {
                    c->loweringFns[(uint32_t)evalFnIndex] = 0u;
                }
                return -1;
            }
            if (qualifiedValueTargetKind == 2) {
                continue;
            }
            if (lowerRc == 0) {
                c->evalToMir[(uint32_t)evalFnIndex] = HOP_EVAL_MIR_FN_NONE;
                if (c->loweringFns != NULL) {
                    c->loweringFns[(uint32_t)evalFnIndex] = 0u;
                }
                return 0;
            }
            if (HOPEvalMirRewriteQualifiedValueLoad(
                    c, mirFnIndex, instIndex, qualifiedValueTargetKind == 3, targetMirFnIndex)
                != 0)
            {
                if (c->loweringFns != NULL) {
                    c->loweringFns[(uint32_t)evalFnIndex] = 0u;
                }
                return -1;
            }
            instIndex += 2u;
            continue;
        }
        if (HOPEvalMirResolveTopInitIdentTargetForFile(
                c, fn->file, ins, &isTopConst, &targetTopInitIndex))
        {
            if (!isTopConst) {
                continue;
            }
            lowerRc = isTopConst ? HOPEvalMirLowerTopConst(c, targetTopInitIndex, &targetMirFnIndex)
                                 : HOPEvalMirLowerTopVar(c, targetTopInitIndex, &targetMirFnIndex);
            if (lowerRc < 0) {
                if (c->loweringFns != NULL) {
                    c->loweringFns[(uint32_t)evalFnIndex] = 0u;
                }
                return -1;
            }
            if (lowerRc == 0) {
                c->evalToMir[(uint32_t)evalFnIndex] = HOP_EVAL_MIR_FN_NONE;
                if (c->loweringFns != NULL) {
                    c->loweringFns[(uint32_t)evalFnIndex] = 0u;
                }
                return 0;
            }
            ins = &c->builder.insts[instIndex];
            HOPEvalMirRewriteLoadIdentToMirFunction(ins, targetMirFnIndex);
            continue;
        }
        if (HOPEvalMirResolveFunctionIdentTargetForFile(c, fn->file, ins, &targetFnIdentIndex)) {
            lowerRc = HOPEvalMirLowerFunction(c, (int32_t)targetFnIdentIndex, &targetMirFnIndex);
            if (lowerRc < 0) {
                if (c->loweringFns != NULL) {
                    c->loweringFns[(uint32_t)evalFnIndex] = 0u;
                }
                return -1;
            }
            if (lowerRc == 0) {
                c->evalToMir[(uint32_t)evalFnIndex] = HOP_EVAL_MIR_FN_NONE;
                if (c->loweringFns != NULL) {
                    c->loweringFns[(uint32_t)evalFnIndex] = 0u;
                }
                return 0;
            }
            ins = &c->builder.insts[instIndex];
            if (HOPEvalMirRewriteLoadIdentToFunctionConst(c, ins, targetMirFnIndex) != 0) {
                if (c->loweringFns != NULL) {
                    c->loweringFns[(uint32_t)evalFnIndex] = 0u;
                }
                return -1;
            }
            continue;
        }
        if (HOPEvalMirResolveSimpleFunctionValueAliasCallTarget(
                c, fn->file, ins, &targetEvalFnIndex))
        {
            lowerRc = HOPEvalMirLowerFunction(c, (int32_t)targetEvalFnIndex, &targetMirFnIndex);
            if (lowerRc < 0) {
                if (c->loweringFns != NULL) {
                    c->loweringFns[(uint32_t)evalFnIndex] = 0u;
                }
                return -1;
            }
            if (lowerRc == 0) {
                c->evalToMir[(uint32_t)evalFnIndex] = HOP_EVAL_MIR_FN_NONE;
                if (c->loweringFns != NULL) {
                    c->loweringFns[(uint32_t)evalFnIndex] = 0u;
                }
                return 0;
            }
            ins = &c->builder.insts[instIndex];
            HOPEvalMirRewriteCallToMirFunction(c, ins, targetMirFnIndex);
            continue;
        }
        lowerRc = HOPEvalMirRewriteZeroArgFunctionValueCall(c, fn->file, mirFnIndex, instIndex);
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
        lowerRc = HOPEvalMirRewriteBuiltinHostCall(c, fn, ins, &rewrittenHost);
        if (lowerRc < 0) {
            return -1;
        }
        if (rewrittenHost) {
            continue;
        }
        lowerRc = HOPEvalMirRewriteQualifiedHostCall(c, fn, ins, &rewrittenHost);
        if (lowerRc < 0) {
            return -1;
        }
        if (rewrittenHost) {
            continue;
        }
        if (!HOPEvalMirResolveDirectCallTarget(c, fn, evalFnIndex, ins, &targetEvalFnIndex)) {
            continue;
        }
        lowerRc = HOPEvalMirLowerFunction(c, (int32_t)targetEvalFnIndex, &targetMirFnIndex);
        if (lowerRc < 0) {
            if (c->loweringFns != NULL) {
                c->loweringFns[(uint32_t)evalFnIndex] = 0u;
            }
            return -1;
        }
        if (lowerRc == 0) {
            c->evalToMir[(uint32_t)evalFnIndex] = HOP_EVAL_MIR_FN_NONE;
            if (c->loweringFns != NULL) {
                c->loweringFns[(uint32_t)evalFnIndex] = 0u;
            }
            return 0;
        }
        ins = &c->builder.insts[instIndex];
        HOPEvalMirRewriteCallToMirFunction(c, ins, targetMirFnIndex);
    }
    if (c->loweringFns != NULL) {
        c->loweringFns[(uint32_t)evalFnIndex] = 0u;
    }
    if (outMirFnIndex != NULL) {
        *outMirFnIndex = mirFnIndex;
    }
    return 1;
}

static void HOPEvalMirRewriteLoadIdentToMirFunction(H2MirInst* ins, uint32_t targetMirFnIndex) {
    if (ins == NULL) {
        return;
    }
    ins->op = H2MirOp_CALL_FN;
    ins->tok = 0u;
    ins->aux = targetMirFnIndex;
}

static int HOPEvalMirRewriteLoadIdentToFunctionConst(
    HOPEvalMirLowerCtx* c, H2MirInst* ins, uint32_t targetMirFnIndex) {
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

static int HOPEvalMirFindAnyFunctionBySliceInPackage(
    const HOPEvalProgram* p,
    const H2Package*      pkg,
    const H2ParsedFile*   callerFile,
    uint32_t              nameStart,
    uint32_t              nameEnd) {
    uint32_t i;
    int32_t  found = -1;
    if (p == NULL || pkg == NULL || callerFile == NULL || nameEnd < nameStart
        || nameEnd > callerFile->sourceLen)
    {
        return -1;
    }
    for (i = 0; i < p->funcLen; i++) {
        const HOPEvalFunction* fn = &p->funcs[i];
        if (fn->pkg != pkg
            || !SliceEqSlice(
                callerFile->source,
                nameStart,
                nameEnd,
                fn->file->source,
                fn->nameStart,
                fn->nameEnd))
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

static int HOPEvalMirResolveQualifiedValueLoadTargetForFile(
    const HOPEvalMirLowerCtx* c,
    const H2ParsedFile*       file,
    const H2MirInst*          loadIns,
    const H2MirInst* _Nullable fieldIns,
    int*      outTargetKind,
    uint32_t* outTargetIndex) {
    const H2Package* currentPkg;
    const H2Package* targetPkg;
    uint32_t         fieldStart;
    uint32_t         fieldEnd;
    int32_t          targetIndex = -1;
    if (outTargetKind != NULL) {
        *outTargetKind = 0;
    }
    if (outTargetIndex != NULL) {
        *outTargetIndex = UINT32_MAX;
    }
    if (c == NULL || c->p == NULL || file == NULL || loadIns == NULL || fieldIns == NULL
        || outTargetKind == NULL || outTargetIndex == NULL || loadIns->op != H2MirOp_LOAD_IDENT
        || fieldIns->op != H2MirOp_AGG_GET)
    {
        return 0;
    }
    currentPkg = HOPEvalFindPackageByFile(c->p, file);
    if (currentPkg == NULL) {
        return 0;
    }
    targetPkg = HOPEvalMirFindImportTargetByAliasSlice(
        currentPkg, file->source, loadIns->start, loadIns->end);
    if (targetPkg == NULL) {
        return 0;
    }
    if (c->builder.fields != NULL && fieldIns->aux < c->builder.fieldLen) {
        fieldStart = c->builder.fields[fieldIns->aux].nameStart;
        fieldEnd = c->builder.fields[fieldIns->aux].nameEnd;
    } else {
        fieldStart = fieldIns->start;
        fieldEnd = fieldIns->end;
    }
    targetIndex = HOPEvalFindTopConstBySliceInPackage(c->p, targetPkg, file, fieldStart, fieldEnd);
    if (targetIndex >= 0) {
        *outTargetKind = 1;
        *outTargetIndex = (uint32_t)targetIndex;
        return 1;
    }
    targetIndex = HOPEvalFindTopVarBySliceInPackage(c->p, targetPkg, file, fieldStart, fieldEnd);
    if (targetIndex >= 0) {
        *outTargetKind = 2;
        *outTargetIndex = (uint32_t)targetIndex;
        return 1;
    }
    targetIndex = HOPEvalMirFindAnyFunctionBySliceInPackage(
        c->p, targetPkg, file, fieldStart, fieldEnd);
    if (targetIndex < 0 || c->p->funcs[(uint32_t)targetIndex].isBuiltinPackageFn
        || c->p->funcs[(uint32_t)targetIndex].isVariadic)
    {
        return 0;
    }
    *outTargetKind = 3;
    *outTargetIndex = (uint32_t)targetIndex;
    return 1;
}

static int HOPEvalMirRewriteQualifiedValueLoad(
    HOPEvalMirLowerCtx* c,
    uint32_t            ownerMirFnIndex,
    uint32_t            loadInstIndex,
    int                 rewriteAsFunctionConst,
    uint32_t            targetMirFnIndex) {
    H2MirInst* loadIns;
    H2MirInst* fieldIns;
    H2MirInst  inserted = { 0 };
    if (c == NULL || ownerMirFnIndex >= c->builder.funcLen || loadInstIndex >= c->builder.instLen
        || loadInstIndex + 1u >= c->builder.instLen)
    {
        return -1;
    }
    fieldIns = &c->builder.insts[loadInstIndex + 1u];
    inserted.start = fieldIns->start;
    inserted.end = fieldIns->end;
    if (rewriteAsFunctionConst) {
        H2MirConst value = { 0 };
        uint32_t   constIndex = UINT32_MAX;
        value.kind = H2MirConst_FUNCTION;
        value.bits = targetMirFnIndex;
        if (H2MirProgramBuilderAddConst(&c->builder, &value, &constIndex) != 0) {
            return -1;
        }
        inserted.op = H2MirOp_PUSH_CONST;
        inserted.aux = constIndex;
    } else {
        inserted.op = H2MirOp_CALL_FN;
        inserted.aux = targetMirFnIndex;
    }
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

static int HOPEvalMirLowerTopConst(
    HOPEvalMirLowerCtx* c, uint32_t topConstIndex, uint32_t* _Nullable outMirFnIndex);
static int HOPEvalMirLowerTopVar(
    HOPEvalMirLowerCtx* c, uint32_t topVarIndex, uint32_t* _Nullable outMirFnIndex);

static int HOPEvalMirResolveTopConstIdentTargetForFile(
    const HOPEvalMirLowerCtx* c,
    const H2ParsedFile*       file,
    const H2MirInst*          ins,
    uint32_t*                 outIndex) {
    const H2Package* currentPkg;
    int32_t          topIndex = -1;
    if (outIndex != NULL) {
        *outIndex = UINT32_MAX;
    }
    if (c == NULL || c->p == NULL || file == NULL || ins == NULL || outIndex == NULL
        || ins->op != H2MirOp_LOAD_IDENT)
    {
        return 0;
    }
    currentPkg = HOPEvalFindPackageByFile(c->p, file);
    if (currentPkg == NULL) {
        return 0;
    }
    topIndex = HOPEvalFindTopConstBySliceInPackage(c->p, currentPkg, file, ins->start, ins->end);
    if (topIndex >= 0) {
        *outIndex = (uint32_t)topIndex;
        return 1;
    }
    return 0;
}

static int HOPEvalMirResolveTopInitSliceTargetForPackage(
    const HOPEvalMirLowerCtx* c,
    const H2Package*          currentPkg,
    const H2ParsedFile*       file,
    uint32_t                  nameStart,
    uint32_t                  nameEnd,
    int*                      outIsTopConst,
    uint32_t*                 outIndex) {
    int32_t topIndex = -1;
    if (outIsTopConst != NULL) {
        *outIsTopConst = 0;
    }
    if (outIndex != NULL) {
        *outIndex = UINT32_MAX;
    }
    if (c == NULL || c->p == NULL || currentPkg == NULL || file == NULL || outIsTopConst == NULL
        || outIndex == NULL)
    {
        return 0;
    }
    topIndex = HOPEvalFindTopConstBySliceInPackage(c->p, currentPkg, file, nameStart, nameEnd);
    if (topIndex >= 0) {
        *outIsTopConst = 1;
        *outIndex = (uint32_t)topIndex;
        return 1;
    }
    topIndex = HOPEvalFindTopVarBySliceInPackage(c->p, currentPkg, file, nameStart, nameEnd);
    if (topIndex >= 0) {
        *outIsTopConst = 0;
        *outIndex = (uint32_t)topIndex;
        return 1;
    }
    return 0;
}

static int HOPEvalMirResolveFunctionIdentTargetForFile(
    const HOPEvalMirLowerCtx* c,
    const H2ParsedFile*       file,
    const H2MirInst*          ins,
    uint32_t*                 outIndex) {
    const H2Package* currentPkg;
    int32_t          fnIndex = -1;
    if (outIndex != NULL) {
        *outIndex = UINT32_MAX;
    }
    if (c == NULL || c->p == NULL || file == NULL || ins == NULL || outIndex == NULL
        || ins->op != H2MirOp_LOAD_IDENT)
    {
        return 0;
    }
    currentPkg = HOPEvalFindPackageByFile(c->p, file);
    fnIndex =
        currentPkg != NULL
            ? HOPEvalFindAnyFunctionBySliceInPackage(c->p, currentPkg, file, ins->start, ins->end)
            : HOPEvalFindAnyFunctionBySlice(c->p, file, ins->start, ins->end);
    if (fnIndex < 0 || (uint32_t)fnIndex >= c->p->funcLen
        || c->p->funcs[(uint32_t)fnIndex].isBuiltinPackageFn
        || c->p->funcs[(uint32_t)fnIndex].isVariadic)
    {
        return 0;
    }
    *outIndex = (uint32_t)fnIndex;
    return 1;
}

static int HOPEvalMirResolveSimpleTopInitFunctionValueTarget(
    const HOPEvalMirLowerCtx* c,
    const H2ParsedFile*       file,
    int                       isTopConst,
    uint32_t                  topInitIndex,
    uint32_t*                 outEvalFnIndex) {
    const H2Package* currentPkg;
    const H2Package* targetPkg;
    int32_t          initExprNode = -1;
    const H2AstNode* initExpr;
    const H2AstNode* baseExpr;
    int32_t          targetFnIndex = -1;
    if (outEvalFnIndex != NULL) {
        *outEvalFnIndex = UINT32_MAX;
    }
    if (c == NULL || c->p == NULL || file == NULL || outEvalFnIndex == NULL) {
        return 0;
    }
    currentPkg = HOPEvalFindPackageByFile(c->p, file);
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
    if (initExpr->kind == H2Ast_IDENT) {
        targetFnIndex =
            currentPkg != NULL
                ? HOPEvalFindAnyFunctionBySliceInPackage(
                      c->p, currentPkg, file, initExpr->dataStart, initExpr->dataEnd)
                : HOPEvalFindAnyFunctionBySlice(c->p, file, initExpr->dataStart, initExpr->dataEnd);
    } else if (initExpr->kind == H2Ast_FIELD_EXPR) {
        int32_t baseNode = initExpr->firstChild;
        if (baseNode < 0 || (uint32_t)baseNode >= file->ast.len) {
            return 0;
        }
        baseExpr = &file->ast.nodes[baseNode];
        if (currentPkg == NULL || baseExpr->kind != H2Ast_IDENT) {
            return 0;
        }
        targetPkg = HOPEvalMirFindImportTargetByAliasSlice(
            currentPkg, file->source, baseExpr->dataStart, baseExpr->dataEnd);
        if (targetPkg == NULL) {
            return 0;
        }
        targetFnIndex = HOPEvalMirFindAnyFunctionBySliceInPackage(
            c->p, targetPkg, file, initExpr->dataStart, initExpr->dataEnd);
    } else {
        return 0;
    }
    if (targetFnIndex < 0 || (uint32_t)targetFnIndex >= c->p->funcLen
        || c->p->funcs[(uint32_t)targetFnIndex].isBuiltinPackageFn
        || c->p->funcs[(uint32_t)targetFnIndex].isVariadic)
    {
        return 0;
    }
    *outEvalFnIndex = (uint32_t)targetFnIndex;
    return 1;
}

static int HOPEvalMirResolveSimpleFunctionValueAliasCallTarget(
    const HOPEvalMirLowerCtx* c,
    const H2ParsedFile*       file,
    const H2MirInst*          ins,
    uint32_t*                 outEvalFnIndex) {
    const H2Package*      currentPkg;
    const H2MirSymbolRef* symbol;
    uint32_t              topIndex = UINT32_MAX;
    int                   isTopConst = 0;
    if (outEvalFnIndex != NULL) {
        *outEvalFnIndex = UINT32_MAX;
    }
    if (c == NULL || c->p == NULL || file == NULL || ins == NULL || outEvalFnIndex == NULL
        || ins->op != H2MirOp_CALL || c->builder.symbols == NULL
        || ins->aux >= c->builder.symbolLen)
    {
        return 0;
    }
    currentPkg = HOPEvalFindPackageByFile(c->p, file);
    if (currentPkg == NULL) {
        return 0;
    }
    symbol = &c->builder.symbols[ins->aux];
    if (symbol->kind != H2MirSymbol_CALL || symbol->flags != 0u) {
        return 0;
    }
    if (HOPEvalMirResolveTopInitSliceTargetForPackage(
            c, currentPkg, file, symbol->nameStart, symbol->nameEnd, &isTopConst, &topIndex))
    {
        return HOPEvalMirResolveSimpleTopInitFunctionValueTarget(
            c, file, isTopConst, topIndex, outEvalFnIndex);
    }
    return 0;
}

static int HOPEvalMirRewriteZeroArgFunctionValueCall(
    HOPEvalMirLowerCtx* c, const H2ParsedFile* file, uint32_t ownerMirFnIndex, uint32_t instIndex) {
    const H2Package*      currentPkg;
    H2MirInst*            ins;
    const H2MirSymbolRef* symbol;
    uint32_t              topIndex = UINT32_MAX;
    int                   isTopConst = 0;
    uint32_t              targetEvalFnIndex = UINT32_MAX;
    uint32_t              targetMirFnIndex = UINT32_MAX;
    uint32_t              constIndex = UINT32_MAX;
    H2MirConst            value = { 0 };
    int                   lowerRc;
    if (c == NULL || file == NULL || ownerMirFnIndex >= c->builder.funcLen
        || instIndex >= c->builder.instLen)
    {
        return 0;
    }
    currentPkg = HOPEvalFindPackageByFile(c->p, file);
    if (currentPkg == NULL) {
        return 0;
    }
    ins = &c->builder.insts[instIndex];
    if (ins->op != H2MirOp_CALL || (uint32_t)ins->tok != 0u || c->builder.symbols == NULL
        || ins->aux >= c->builder.symbolLen)
    {
        return 0;
    }
    symbol = &c->builder.symbols[ins->aux];
    if (symbol->kind != H2MirSymbol_CALL || symbol->flags != 0u) {
        return 0;
    }
    if (!HOPEvalMirResolveTopInitSliceTargetForPackage(
            c, currentPkg, file, symbol->nameStart, symbol->nameEnd, &isTopConst, &topIndex)
        || !HOPEvalMirResolveSimpleTopInitFunctionValueTarget(
            c, file, isTopConst, topIndex, &targetEvalFnIndex))
    {
        return 0;
    }
    lowerRc = HOPEvalMirLowerFunction(c, (int32_t)targetEvalFnIndex, &targetMirFnIndex);
    if (lowerRc <= 0) {
        return lowerRc;
    }
    value.kind = H2MirConst_FUNCTION;
    value.bits = targetMirFnIndex;
    if (H2MirProgramBuilderAddConst(&c->builder, &value, &constIndex) != 0
        || H2MirProgramBuilderInsertInst(
               &c->builder,
               ownerMirFnIndex,
               instIndex - c->builder.funcs[ownerMirFnIndex].instStart,
               &(H2MirInst){
                   .op = H2MirOp_PUSH_CONST,
                   .aux = constIndex,
                   .start = ins->start,
                   .end = ins->end,
               })
               != 0)
    {
        return -1;
    }
    ins = &c->builder.insts[instIndex + 1u];
    ins->op = H2MirOp_CALL_INDIRECT;
    ins->tok = 0u;
    ins->aux = 0u;
    return 1;
}

static int HOPEvalMirResolveTopInitIdentTargetForFile(
    const HOPEvalMirLowerCtx* c,
    const H2ParsedFile*       file,
    const H2MirInst*          ins,
    int*                      outIsTopConst,
    uint32_t*                 outIndex) {
    const H2Package* currentPkg;
    if (outIsTopConst != NULL) {
        *outIsTopConst = 0;
    }
    if (outIndex != NULL) {
        *outIndex = UINT32_MAX;
    }
    if (c == NULL || c->p == NULL || file == NULL || ins == NULL || outIsTopConst == NULL
        || outIndex == NULL || ins->op != H2MirOp_LOAD_IDENT)
    {
        return 0;
    }
    currentPkg = HOPEvalFindPackageByFile(c->p, file);
    if (currentPkg == NULL) {
        return 0;
    }
    return HOPEvalMirResolveTopInitSliceTargetForPackage(
        c, currentPkg, file, ins->start, ins->end, outIsTopConst, outIndex);
}

static int HOPEvalMirRewriteTopInitCalls(
    HOPEvalMirLowerCtx* c,
    const H2ParsedFile* file,
    int32_t             initExprNode,
    uint32_t            rootMirFnIndex,
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
        H2MirInst* ins = &c->builder.insts[instIndex];
        H2MirInst* nextIns =
            instIndex + 1u < c->builder.instLen ? &c->builder.insts[instIndex + 1u] : NULL;
        int      isTopConst = 0;
        uint32_t targetTopInitIndex = UINT32_MAX;
        uint32_t targetFnIdentIndex = UINT32_MAX;
        uint32_t targetEvalFnIndex = UINT32_MAX;
        uint32_t targetMirFnIndex = UINT32_MAX;
        int      qualifiedValueTargetKind = 0;
        int      lowerRc;
        if (HOPEvalMirResolveQualifiedValueLoadTargetForFile(
                c, file, ins, nextIns, &qualifiedValueTargetKind, &targetTopInitIndex))
        {
            if (qualifiedValueTargetKind == 1) {
                lowerRc = HOPEvalMirLowerTopConst(c, targetTopInitIndex, &targetMirFnIndex);
            } else if (qualifiedValueTargetKind == 2) {
                lowerRc = HOPEvalMirLowerTopVar(c, targetTopInitIndex, &targetMirFnIndex);
            } else {
                lowerRc = HOPEvalMirLowerFunction(
                    c, (int32_t)targetTopInitIndex, &targetMirFnIndex);
            }
            if (lowerRc < 0) {
                return -1;
            }
            if (lowerRc == 0) {
                return 0;
            }
            if (HOPEvalMirRewriteQualifiedValueLoad(
                    c, rootMirFnIndex, instIndex, qualifiedValueTargetKind == 3, targetMirFnIndex)
                != 0)
            {
                return -1;
            }
            instIndex += 2u;
            continue;
        }
        if (HOPEvalMirResolveTopInitIdentTargetForFile(
                c, file, ins, &isTopConst, &targetTopInitIndex))
        {
            lowerRc = isTopConst ? HOPEvalMirLowerTopConst(c, targetTopInitIndex, &targetMirFnIndex)
                                 : HOPEvalMirLowerTopVar(c, targetTopInitIndex, &targetMirFnIndex);
            if (lowerRc < 0) {
                return -1;
            }
            if (lowerRc == 0) {
                return 0;
            }
            ins = &c->builder.insts[instIndex];
            HOPEvalMirRewriteLoadIdentToMirFunction(ins, targetMirFnIndex);
            continue;
        }
        if (HOPEvalMirResolveFunctionIdentTargetForFile(c, file, ins, &targetFnIdentIndex)) {
            lowerRc = HOPEvalMirLowerFunction(c, (int32_t)targetFnIdentIndex, &targetMirFnIndex);
            if (lowerRc < 0) {
                return -1;
            }
            if (lowerRc == 0) {
                return 0;
            }
            ins = &c->builder.insts[instIndex];
            if (HOPEvalMirRewriteLoadIdentToFunctionConst(c, ins, targetMirFnIndex) != 0) {
                return -1;
            }
            continue;
        }
        if (HOPEvalMirResolveSimpleFunctionValueAliasCallTarget(c, file, ins, &targetEvalFnIndex)) {
            lowerRc = HOPEvalMirLowerFunction(c, (int32_t)targetEvalFnIndex, &targetMirFnIndex);
            if (lowerRc < 0) {
                return -1;
            }
            if (lowerRc == 0) {
                return 0;
            }
            ins = &c->builder.insts[instIndex];
            HOPEvalMirRewriteCallToMirFunction(c, ins, targetMirFnIndex);
            continue;
        }
        lowerRc = HOPEvalMirRewriteZeroArgFunctionValueCall(c, file, rootMirFnIndex, instIndex);
        if (lowerRc < 0) {
            return -1;
        }
        if (lowerRc > 0) {
            instIndex++;
            continue;
        }
        if (HOPEvalMirResolveQualifiedCallTargetForNode(
                c, file, initExprNode, ins, &targetEvalFnIndex))
        {
            lowerRc = HOPEvalMirLowerFunction(c, (int32_t)targetEvalFnIndex, &targetMirFnIndex);
            if (lowerRc < 0) {
                return -1;
            }
            if (lowerRc == 0) {
                return 0;
            }
            ins = &c->builder.insts[instIndex];
            HOPEvalMirRewriteCallToMirFunction(c, ins, targetMirFnIndex);
            continue;
        }
        if (!HOPEvalMirResolvePlainDirectCallTargetForFile(c, file, ins, &targetEvalFnIndex)) {
            continue;
        }
        lowerRc = HOPEvalMirLowerFunction(c, (int32_t)targetEvalFnIndex, &targetMirFnIndex);
        if (lowerRc < 0) {
            return -1;
        }
        if (lowerRc == 0) {
            return 0;
        }
        ins = &c->builder.insts[instIndex];
        HOPEvalMirRewriteCallToMirFunction(c, ins, targetMirFnIndex);
    }
    *outSupported = 1;
    return 0;
}

static int HOPEvalMirFinalizeLoweredTopInit(
    HOPEvalMirLowerCtx* c,
    const H2ParsedFile* file,
    int32_t             initExprNode,
    uint32_t            mirFnIndex,
    int*                outSupported) {
    uint32_t sourceRef = UINT32_MAX;
    if (outSupported != NULL) {
        *outSupported = 0;
    }
    if (c == NULL || file == NULL || outSupported == NULL || mirFnIndex >= c->builder.funcLen) {
        return -1;
    }
    sourceRef = c->builder.funcs[mirFnIndex].sourceRef;
    if (sourceRef >= c->execCtx.sourceFileCap) {
        return -1;
    }
    c->execCtx.sourceFiles[sourceRef] = file;
    return HOPEvalMirRewriteTopInitCalls(c, file, initExprNode, mirFnIndex, outSupported);
}

static int HOPEvalMirLowerNamedTopInitVarLike(
    HOPEvalMirLowerCtx* c,
    const H2ParsedFile* file,
    int32_t             nodeId,
    int32_t             initExprNode,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    uint32_t*           mirMap,
    uint8_t*            lowering,
    uint32_t            itemIndex,
    uint32_t* _Nullable outMirFnIndex) {
    uint32_t mirFnIndex = UINT32_MAX;
    int      supported = 0;
    if (outMirFnIndex != NULL) {
        *outMirFnIndex = UINT32_MAX;
    }
    if (c == NULL || c->p == NULL || file == NULL || mirMap == NULL || lowering == NULL) {
        return -1;
    }
    if (mirMap[itemIndex] != HOP_EVAL_MIR_FN_NONE) {
        if (outMirFnIndex != NULL) {
            *outMirFnIndex = mirMap[itemIndex];
        }
        return 1;
    }
    if (lowering[itemIndex] != 0u || nodeId < 0) {
        return 0;
    }
    lowering[itemIndex] = 1u;
    if (H2MirLowerAppendNamedVarLikeTopInitFunctionBySlice(
            &c->builder,
            c->p->arena,
            &file->ast,
            (H2StrView){ file->source, file->sourceLen },
            nodeId,
            nameStart,
            nameEnd,
            &mirFnIndex,
            &supported,
            c->diag)
        != 0)
    {
        lowering[itemIndex] = 0u;
        return -1;
    }
    if (!supported || mirFnIndex == UINT32_MAX) {
        lowering[itemIndex] = 0u;
        return 0;
    }
    mirMap[itemIndex] = mirFnIndex;
    if (HOPEvalMirFinalizeLoweredTopInit(c, file, initExprNode, mirFnIndex, &supported) != 0) {
        lowering[itemIndex] = 0u;
        return -1;
    }
    lowering[itemIndex] = 0u;
    if (!supported) {
        mirMap[itemIndex] = HOP_EVAL_MIR_FN_NONE;
        return 0;
    }
    if (outMirFnIndex != NULL) {
        *outMirFnIndex = mirFnIndex;
    }
    return 1;
}

static int HOPEvalMirLowerTopConst(
    HOPEvalMirLowerCtx* c, uint32_t topConstIndex, uint32_t* _Nullable outMirFnIndex) {
    const HOPEvalTopConst* topConst;
    if (outMirFnIndex != NULL) {
        *outMirFnIndex = UINT32_MAX;
    }
    if (c == NULL || c->p == NULL || c->topConstToMir == NULL || c->loweringTopConsts == NULL
        || topConstIndex >= c->p->topConstLen)
    {
        return -1;
    }
    topConst = &c->p->topConsts[topConstIndex];
    if (topConst->nodeId < 0) {
        return 0;
    }
    return HOPEvalMirLowerNamedTopInitVarLike(
        c,
        topConst->file,
        topConst->nodeId,
        topConst->initExprNode,
        topConst->nameStart,
        topConst->nameEnd,
        c->topConstToMir,
        c->loweringTopConsts,
        topConstIndex,
        outMirFnIndex);
}

static int HOPEvalMirLowerTopVar(
    HOPEvalMirLowerCtx* c, uint32_t topVarIndex, uint32_t* _Nullable outMirFnIndex) {
    const HOPEvalTopVar* topVar;
    if (outMirFnIndex != NULL) {
        *outMirFnIndex = UINT32_MAX;
    }
    if (c == NULL || c->p == NULL || c->topVarToMir == NULL || c->loweringTopVars == NULL
        || topVarIndex >= c->p->topVarLen)
    {
        return -1;
    }
    topVar = &c->p->topVars[topVarIndex];
    return HOPEvalMirLowerNamedTopInitVarLike(
        c,
        topVar->file,
        topVar->nodeId,
        topVar->initExprNode,
        topVar->nameStart,
        topVar->nameEnd,
        c->topVarToMir,
        c->loweringTopVars,
        topVarIndex,
        outMirFnIndex);
}

int HOPEvalMirBuildTopInitProgram(
    HOPEvalProgram*     p,
    const H2ParsedFile* file,
    int32_t             initExprNode,
    int32_t             declTypeNode,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    H2MirProgram*       outProgram,
    HOPEvalMirExecCtx*  outExecCtx,
    uint32_t*           outRootMirFnIndex,
    int*                outSupported) {
    HOPEvalMirLowerCtx lowerCtx;
    uint32_t           rootMirFnIndex = UINT32_MAX;
    int                supported = 0;
    if (outProgram != NULL) {
        *outProgram = (H2MirProgram){ 0 };
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
    if (HOPEvalMirInitLowerCtx(p, 1u, &lowerCtx) != 0) {
        return -1;
    }
    if (H2MirLowerBeginNamedTopInitProgram(
            &lowerCtx.builder,
            p->arena,
            &file->ast,
            (H2StrView){ file->source, file->sourceLen },
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
    if (HOPEvalMirFinalizeLoweredTopInit(&lowerCtx, file, initExprNode, rootMirFnIndex, &supported)
        != 0)
    {
        return -1;
    }
    if (!supported) {
        return 0;
    }
    lowerCtx.execCtx.rootMirFnIndex = rootMirFnIndex;
    H2MirProgramBuilderFinish(&lowerCtx.builder, outProgram);
    *outExecCtx = lowerCtx.execCtx;
    *outRootMirFnIndex = rootMirFnIndex;
    *outSupported = 1;
    return 0;
}

int HOPEvalTryMirInvokeFunction(
    HOPEvalProgram*        p,
    const HOPEvalFunction* fn,
    int32_t                fnIndex,
    const H2CTFEValue*     args,
    uint32_t               argCount,
    H2CTFEValue*           outValue,
    int*                   outDidReturn,
    int*                   outIsConst) {
    H2MirProgram       program = { 0 };
    H2MirExecEnv       env = { 0 };
    HOPEvalMirLowerCtx lowerCtx;
    uint32_t           mirFnIndex = UINT32_MAX;
    int                lowerRc;
    int                mirIsConst = 0;
    if (p == NULL || fn == NULL || outValue == NULL || outDidReturn == NULL || outIsConst == NULL) {
        return -1;
    }
    *outDidReturn = 0;
    *outIsConst = 0;
    if (HOPEvalMirInitLowerCtx(p, 0u, &lowerCtx) != 0) {
        return -1;
    }
    lowerRc = HOPEvalMirLowerFunction(&lowerCtx, fnIndex, &mirFnIndex);
    if (lowerRc < 0) {
        return -1;
    }
    if (lowerRc == 0 || mirFnIndex == UINT32_MAX) {
        if (p->currentExecCtx != NULL && lowerCtx.diag != NULL && lowerCtx.diag->detail != NULL) {
            H2CTFEExecSetReason(
                p->currentExecCtx, lowerCtx.diag->start, lowerCtx.diag->end, lowerCtx.diag->detail);
        }
        return 0;
    }
    lowerCtx.execCtx.rootMirFnIndex = mirFnIndex;
    H2MirProgramBuilderFinish(&lowerCtx.builder, &program);
    HOPEvalMirInitExecEnv(p, fn->file, &env, &lowerCtx.execCtx);
    if (!H2MirProgramNeedsDynamicResolution(&program)) {
        H2MirExecEnvDisableDynamicResolution(&env);
    }
    if (H2MirEvalFunction(
            p->arena, &program, mirFnIndex, args, argCount, &env, outValue, &mirIsConst)
        != 0)
    {
        return -1;
    }
    HOPEvalMirAdaptOutValue(&lowerCtx.execCtx, outValue, &mirIsConst);
    *outIsConst = mirIsConst;
    if (mirIsConst) {
        *outDidReturn = outValue->kind != H2CTFEValue_INVALID;
    }
    return 0;
}

H2_API_END
