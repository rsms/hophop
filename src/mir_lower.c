#include "libhop-impl.h"
#include "mir.h"
#include "mir_lower.h"

H2_API_BEGIN

static void H2MirLowerSetDiag(H2Diag* diag, H2DiagCode code, uint32_t start, uint32_t end) {
    if (diag == NULL) {
        return;
    }
    diag->code = code;
    diag->type = H2DiagTypeOfCode(code);
    diag->start = start;
    diag->end = end;
    diag->argStart = 0;
    diag->argEnd = 0;
    diag->argText = NULL;
    diag->argTextLen = 0;
}

static int H2MirLowerParseIntLiteral(H2StrView src, uint32_t start, uint32_t end, int64_t* out) {
    uint64_t v = 0;
    uint32_t i;
    uint32_t base = 10;
    if (out == NULL || end <= start || end > src.len) {
        return -1;
    }
    if (end - start >= 3 && src.ptr[start] == '0'
        && (src.ptr[start + 1] == 'x' || src.ptr[start + 1] == 'X'))
    {
        base = 16;
        start += 2;
        if (end <= start) {
            return -1;
        }
    }
    for (i = start; i < end; i++) {
        unsigned char ch = (unsigned char)src.ptr[i];
        uint32_t      digit;
        if (ch >= (unsigned char)'0' && ch <= (unsigned char)'9') {
            digit = (uint32_t)(ch - (unsigned char)'0');
        } else if (base == 16 && ch >= (unsigned char)'a' && ch <= (unsigned char)'f') {
            digit = 10u + (uint32_t)(ch - (unsigned char)'a');
        } else if (base == 16 && ch >= (unsigned char)'A' && ch <= (unsigned char)'F') {
            digit = 10u + (uint32_t)(ch - (unsigned char)'A');
        } else {
            return -1;
        }
        if (digit >= base) {
            return -1;
        }
        if (v > (uint64_t)INT64_MAX / (uint64_t)base
            || (v == (uint64_t)INT64_MAX / (uint64_t)base
                && (uint64_t)digit > (uint64_t)INT64_MAX % (uint64_t)base))
        {
            return -1;
        }
        v = v * (uint64_t)base + (uint64_t)digit;
    }
    *out = (int64_t)v;
    return 0;
}

static int H2MirLowerParseFloatLiteral(H2StrView src, uint32_t start, uint32_t end, double* out) {
    uint32_t i;
    double   v = 0.0;
    int      sawDigit = 0;
    int32_t  expSign = 1;
    uint32_t expValue = 0;
    uint32_t expIter;
    if (out == NULL || end <= start || end > src.len) {
        return -1;
    }
    i = start;
    while (i < end && (unsigned char)src.ptr[i] >= (unsigned char)'0'
           && (unsigned char)src.ptr[i] <= (unsigned char)'9')
    {
        v = v * 10.0 + (double)(src.ptr[i] - '0');
        sawDigit = 1;
        i++;
    }
    if (i < end && src.ptr[i] == '.') {
        double place = 0.1;
        i++;
        while (i < end && (unsigned char)src.ptr[i] >= (unsigned char)'0'
               && (unsigned char)src.ptr[i] <= (unsigned char)'9')
        {
            v += (double)(src.ptr[i] - '0') * place;
            place *= 0.1;
            sawDigit = 1;
            i++;
        }
    }
    if (!sawDigit) {
        return -1;
    }
    if (i < end && (src.ptr[i] == 'e' || src.ptr[i] == 'E')) {
        i++;
        if (i < end && (src.ptr[i] == '+' || src.ptr[i] == '-')) {
            expSign = src.ptr[i] == '-' ? -1 : 1;
            i++;
        }
        if (i >= end || (unsigned char)src.ptr[i] < (unsigned char)'0'
            || (unsigned char)src.ptr[i] > (unsigned char)'9')
        {
            return -1;
        }
        while (i < end && (unsigned char)src.ptr[i] >= (unsigned char)'0'
               && (unsigned char)src.ptr[i] <= (unsigned char)'9')
        {
            if (expValue > UINT32_MAX / 10u) {
                return -1;
            }
            expValue = expValue * 10u + (uint32_t)(src.ptr[i] - '0');
            i++;
        }
    }
    if (i != end) {
        return -1;
    }
    for (expIter = 0; expIter < expValue; expIter++) {
        v = expSign < 0 ? v / 10.0 : v * 10.0;
    }
    *out = v;
    return 0;
}

static int H2MirLowerParseBoolLiteral(H2StrView src, uint32_t start, uint32_t end, uint8_t* out) {
    uint32_t len = end > start ? end - start : 0;
    if (out == NULL) {
        return -1;
    }
    if (len == 4 && memcmp(src.ptr + start, "true", 4) == 0) {
        *out = 1;
        return 0;
    }
    if (len == 5 && memcmp(src.ptr + start, "false", 5) == 0) {
        *out = 0;
        return 0;
    }
    return -1;
}

static int H2MirLowerRewriteConstInst(
    H2MirProgramBuilder* _Nonnull builder,
    H2Arena* _Nonnull arena,
    H2StrView src,
    const H2MirInst* _Nonnull in,
    H2MirInst* _Nonnull out,
    H2Diag* _Nullable diag) {
    H2MirConst     value = { 0 };
    uint32_t       constIndex = 0;
    H2RuneLitErr   runeErr = { 0 };
    H2StringLitErr litErr = { 0 };
    int64_t        intValue = 0;
    double         floatValue = 0.0;
    uint8_t        boolValue = 0;
    uint32_t       rune = 0;
    uint8_t*       bytes = NULL;
    uint32_t       len = 0;
    memcpy(out, in, sizeof(*out));
    switch (in->op) {
        case H2MirOp_PUSH_INT:
            value.kind = H2MirConst_INT;
            if ((H2TokenKind)in->tok == H2Tok_RUNE) {
                if (H2DecodeRuneLiteralValidate(src.ptr, in->start, in->end, &rune, &runeErr) != 0)
                {
                    H2MirLowerSetDiag(
                        diag, H2RuneLitErrDiagCode(runeErr.kind), runeErr.start, runeErr.end);
                    return -1;
                }
                value.bits = (uint64_t)(int64_t)rune;
            } else if ((H2TokenKind)in->tok == H2Tok_INVALID) {
                value.bits = (uint64_t)(int64_t)(int32_t)in->aux;
            } else if (H2MirLowerParseIntLiteral(src, in->start, in->end, &intValue) != 0) {
                return 0;
            } else {
                value.bits = (uint64_t)intValue;
            }
            break;
        case H2MirOp_PUSH_FLOAT:
            value.kind = H2MirConst_FLOAT;
            if (H2MirLowerParseFloatLiteral(src, in->start, in->end, &floatValue) != 0) {
                return 0;
            }
            memcpy(&value.bits, &floatValue, sizeof(floatValue));
            break;
        case H2MirOp_PUSH_BOOL:
            value.kind = H2MirConst_BOOL;
            if (H2MirLowerParseBoolLiteral(src, in->start, in->end, &boolValue) != 0) {
                return 0;
            }
            value.bits = (uint64_t)boolValue;
            break;
        case H2MirOp_PUSH_STRING:
            value.kind = H2MirConst_STRING;
            if (H2DecodeStringLiteralArena(
                    arena, src.ptr, in->start, in->end, &bytes, &len, &litErr)
                != 0)
            {
                H2MirLowerSetDiag(
                    diag, H2StringLitErrDiagCode(litErr.kind), litErr.start, litErr.end);
                return -1;
            }
            value.bytes.ptr = (const char*)bytes;
            value.bytes.len = len;
            break;
        case H2MirOp_PUSH_NULL: value.kind = H2MirConst_NULL; break;
        default:                return 0;
    }
    if (H2MirProgramBuilderAddConst(builder, &value, &constIndex) != 0) {
        H2MirLowerSetDiag(diag, H2Diag_ARENA_OOM, in->start, in->end);
        return -1;
    }
    out->op = H2MirOp_PUSH_CONST;
    out->tok = 0;
    out->aux = constIndex;
    return 1;
}

static int H2MirLowerInternSymbol(
    H2MirProgramBuilder* _Nonnull builder,
    H2MirSymbolKind kind,
    uint32_t        nameStart,
    uint32_t        nameEnd,
    uint32_t        flags,
    uint32_t        target,
    uint32_t* _Nonnull outIndex,
    H2Diag* _Nullable diag) {
    uint32_t       i;
    H2MirSymbolRef symbol;
    if (builder == NULL || outIndex == NULL) {
        return -1;
    }
    for (i = 0; i < builder->symbolLen; i++) {
        const H2MirSymbolRef* existing = &builder->symbols[i];
        if (existing->kind == kind && existing->nameStart == nameStart
            && existing->nameEnd == nameEnd && existing->flags == flags
            && existing->target == target)
        {
            *outIndex = i;
            return 0;
        }
    }
    symbol.kind = kind;
    symbol.nameStart = nameStart;
    symbol.nameEnd = nameEnd;
    symbol.flags = flags;
    symbol.target = target;
    if (H2MirProgramBuilderAddSymbol(builder, &symbol, outIndex) != 0) {
        H2MirLowerSetDiag(diag, H2Diag_ARENA_OOM, nameStart, nameEnd);
        return -1;
    }
    return 0;
}

static int H2MirLowerRewriteSymbolInst(
    H2MirProgramBuilder* _Nonnull builder,
    const H2MirInst* _Nonnull in,
    H2MirInst* _Nonnull out,
    H2Diag* _Nullable diag) {
    H2MirSymbolKind kind = H2MirSymbol_INVALID;
    uint32_t        flags = 0;
    uint32_t        target = 0;
    uint32_t        symbolIndex = 0;
    memcpy(out, in, sizeof(*out));
    switch (in->op) {
        case H2MirOp_LOAD_IDENT:
        case H2MirOp_STORE_IDENT:
            kind = H2MirSymbol_IDENT;
            target = in->aux;
            break;
        case H2MirOp_CALL:
            kind = H2MirSymbol_CALL;
            flags = H2MirRawCallAuxFlags(in->aux);
            target = H2MirRawCallAuxNode(in->aux);
            break;
        default: return 0;
    }
    if (H2MirLowerInternSymbol(builder, kind, in->start, in->end, flags, target, &symbolIndex, diag)
        != 0)
    {
        return -1;
    }
    out->aux = symbolIndex;
    return 1;
}

static int H2MirLowerInternType(
    H2MirProgramBuilder* _Nonnull builder,
    uint32_t astNode,
    uint32_t sourceRef,
    uint32_t flags,
    uint32_t* _Nonnull outIndex,
    H2Diag* _Nullable diag) {
    uint32_t     i;
    H2MirTypeRef typeRef;
    if (builder == NULL || outIndex == NULL) {
        return -1;
    }
    for (i = 0; i < builder->typeLen; i++) {
        const H2MirTypeRef* existing = &builder->types[i];
        if (existing->astNode == astNode && existing->sourceRef == sourceRef
            && existing->flags == flags)
        {
            *outIndex = i;
            return 0;
        }
    }
    typeRef.astNode = astNode;
    typeRef.sourceRef = sourceRef;
    typeRef.flags = flags;
    typeRef.aux = 0u;
    if (H2MirProgramBuilderAddType(builder, &typeRef, outIndex) != 0) {
        H2MirLowerSetDiag(diag, H2Diag_ARENA_OOM, 0, 0);
        return -1;
    }
    return 0;
}

static uint32_t H2MirLowerFindSourceRef(
    const H2MirProgramBuilder* _Nonnull builder, H2StrView src) {
    uint32_t i;
    if (builder == NULL || src.ptr == NULL) {
        return 0u;
    }
    for (i = 0; i < builder->sourceLen; i++) {
        if (builder->sources[i].src.ptr == src.ptr && builder->sources[i].src.len == src.len) {
            return i;
        }
    }
    return 0u;
}

static int H2MirLowerRewriteTypeInst(
    H2MirProgramBuilder* _Nonnull builder,
    H2StrView src,
    const H2MirInst* _Nonnull in,
    H2MirInst* _Nonnull out,
    H2Diag* _Nullable diag) {
    uint32_t typeIndex = 0;
    uint32_t sourceRef = 0u;
    memcpy(out, in, sizeof(*out));
    if (in->op != H2MirOp_CAST) {
        return 0;
    }
    sourceRef = H2MirLowerFindSourceRef(builder, src);
    if (H2MirLowerInternType(builder, in->aux, sourceRef, 0u, &typeIndex, diag) != 0) {
        return -1;
    }
    out->aux = typeIndex;
    return 1;
}

static int H2MirLowerInternHost(
    H2MirProgramBuilder* _Nonnull builder,
    uint32_t      nameStart,
    uint32_t      nameEnd,
    H2MirHostKind kind,
    uint32_t      flags,
    uint32_t      target,
    uint32_t* _Nonnull outIndex,
    H2Diag* _Nullable diag) {
    uint32_t     i;
    H2MirHostRef host;
    if (builder == NULL || outIndex == NULL) {
        return -1;
    }
    for (i = 0; i < builder->hostLen; i++) {
        const H2MirHostRef* existing = &builder->hosts[i];
        if (existing->nameStart == nameStart && existing->nameEnd == nameEnd
            && existing->kind == kind && existing->flags == flags && existing->target == target)
        {
            *outIndex = i;
            return 0;
        }
    }
    host.nameStart = nameStart;
    host.nameEnd = nameEnd;
    host.kind = kind;
    host.flags = flags;
    host.target = target;
    if (H2MirProgramBuilderAddHost(builder, &host, outIndex) != 0) {
        H2MirLowerSetDiag(diag, H2Diag_ARENA_OOM, nameStart, nameEnd);
        return -1;
    }
    return 0;
}

static int H2MirLowerRewriteHostInst(
    H2MirProgramBuilder* _Nonnull builder,
    H2StrView src,
    const H2MirInst* _Nonnull in,
    H2MirInst* _Nonnull out,
    H2Diag* _Nullable diag) {
    uint32_t hostIndex = 0;
    uint32_t hostTarget = H2MirHostTarget_INVALID;
    memcpy(out, in, sizeof(*out));
    if (in->op != H2MirOp_CALL || in->aux >= builder->symbolLen) {
        return 0;
    }
    if (in->end == in->start + 5u && memcmp(src.ptr + in->start, "print", 5) == 0) {
        hostTarget = H2MirHostTarget_PRINT;
    } else if (in->end == in->start + 4u && memcmp(src.ptr + in->start, "copy", 4) == 0) {
        hostTarget = H2MirHostTarget_COPY;
    } else if (in->end == in->start + 6u && memcmp(src.ptr + in->start, "concat", 6) == 0) {
        hostTarget = H2MirHostTarget_CONCAT;
    } else if (in->end == in->start + 4u && memcmp(src.ptr + in->start, "free", 4) == 0) {
        hostTarget = H2MirHostTarget_FREE;
    } else {
        return 0;
    }
    if (builder->symbols[in->aux].kind != H2MirSymbol_CALL || builder->symbols[in->aux].flags != 0u)
    {
        return 0;
    }
    if (H2MirLowerInternHost(
            builder, in->start, in->end, H2MirHost_GENERIC, 0u, hostTarget, &hostIndex, diag)
        != 0)
    {
        return -1;
    }
    out->op = H2MirOp_CALL_HOST;
    out->aux = hostIndex;
    return 1;
}

static int H2MirLowerInternField(
    H2MirProgramBuilder* _Nonnull builder,
    uint32_t nameStart,
    uint32_t nameEnd,
    uint32_t* _Nonnull outIndex,
    H2Diag* _Nullable diag) {
    uint32_t   i;
    H2MirField fieldRef;
    if (builder == NULL || outIndex == NULL) {
        return -1;
    }
    for (i = 0; i < builder->fieldLen; i++) {
        const H2MirField* existing = &builder->fields[i];
        if (existing->nameStart == nameStart && existing->nameEnd == nameEnd
            && existing->sourceRef == 0u && existing->ownerTypeRef == UINT32_MAX
            && existing->typeRef == UINT32_MAX)
        {
            *outIndex = i;
            return 0;
        }
    }
    fieldRef.nameStart = nameStart;
    fieldRef.nameEnd = nameEnd;
    fieldRef.sourceRef = 0u;
    fieldRef.ownerTypeRef = UINT32_MAX;
    fieldRef.typeRef = UINT32_MAX;
    if (H2MirProgramBuilderAddField(builder, &fieldRef, outIndex) != 0) {
        H2MirLowerSetDiag(diag, H2Diag_ARENA_OOM, nameStart, nameEnd);
        return -1;
    }
    return 0;
}

static int H2MirLowerRewriteFieldInst(
    H2MirProgramBuilder* _Nonnull builder,
    const H2MirInst* _Nonnull in,
    H2MirInst* _Nonnull out,
    H2Diag* _Nullable diag) {
    uint32_t fieldIndex = 0;
    memcpy(out, in, sizeof(*out));
    if (in->op != H2MirOp_AGG_GET && in->op != H2MirOp_AGG_ADDR) {
        return 0;
    }
    if (H2MirLowerInternField(builder, in->start, in->end, &fieldIndex, diag) != 0) {
        return -1;
    }
    out->aux = fieldIndex;
    return 1;
}

int H2MirLowerAppendInst(
    H2MirProgramBuilder* _Nonnull builder,
    H2Arena* _Nonnull arena,
    H2StrView src,
    const H2MirInst* _Nonnull in,
    H2Diag* _Nullable diag) {
    H2MirInst loweredInst;
    int       rewriteStatus;
    if (builder == NULL || arena == NULL || in == NULL) {
        H2MirLowerSetDiag(diag, H2Diag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }
    rewriteStatus = H2MirLowerRewriteConstInst(builder, arena, src, in, &loweredInst, diag);
    if (rewriteStatus < 0) {
        return -1;
    }
    if (rewriteStatus == 0) {
        memcpy(&loweredInst, in, sizeof(loweredInst));
    }
    rewriteStatus = H2MirLowerRewriteSymbolInst(builder, &loweredInst, &loweredInst, diag);
    if (rewriteStatus < 0) {
        return -1;
    }
    rewriteStatus = H2MirLowerRewriteTypeInst(builder, src, &loweredInst, &loweredInst, diag);
    if (rewriteStatus < 0) {
        return -1;
    }
    rewriteStatus = H2MirLowerRewriteHostInst(builder, src, &loweredInst, &loweredInst, diag);
    if (rewriteStatus < 0) {
        return -1;
    }
    rewriteStatus = H2MirLowerRewriteFieldInst(builder, &loweredInst, &loweredInst, diag);
    if (rewriteStatus < 0) {
        return -1;
    }
    if (H2MirProgramBuilderAppendInst(builder, &loweredInst) != 0) {
        H2MirLowerSetDiag(diag, H2Diag_ARENA_OOM, loweredInst.start, loweredInst.end);
        return -1;
    }
    return 0;
}

int H2MirLowerAppendExprAsFunction(
    H2MirProgramBuilder* _Nonnull builder,
    H2Arena* _Nonnull arena,
    const H2Ast* _Nonnull ast,
    H2StrView src,
    int32_t   nodeId,
    int32_t   resultTypeNode,
    uint32_t* _Nonnull outFunctionIndex,
    int* _Nonnull outSupported,
    H2Diag* _Nullable diag) {
    H2MirChunk     chunk;
    H2MirFunction  function = { 0 };
    H2MirSourceRef sourceRef = { 0 };
    H2MirTypeRef   typeRef = { 0 };
    uint32_t       functionIndex = 0;
    uint32_t       sourceIndex = 0;
    uint32_t       i;

    if (outFunctionIndex != NULL) {
        *outFunctionIndex = UINT32_MAX;
    }
    if (arena == NULL || builder == NULL || ast == NULL || outFunctionIndex == NULL
        || outSupported == NULL)
    {
        if (diag != NULL) {
            diag->code = H2Diag_UNEXPECTED_TOKEN;
            diag->type = H2DiagTypeOfCode(diag->code);
            diag->start = 0;
            diag->end = 0;
            diag->argStart = 0;
            diag->argEnd = 0;
            diag->argText = NULL;
            diag->argTextLen = 0;
        }
        return -1;
    }

    *outSupported = 0;
    if (H2MirBuildExpr(arena, ast, src, nodeId, &chunk, outSupported, diag) != 0) {
        return -1;
    }
    if (!*outSupported) {
        return 0;
    }

    sourceRef.src = src;
    if (H2MirProgramBuilderAddSource(builder, &sourceRef, &sourceIndex) != 0) {
        if (diag != NULL) {
            diag->code = H2Diag_ARENA_OOM;
            diag->type = H2DiagTypeOfCode(diag->code);
            diag->start = 0;
            diag->end = 0;
            diag->argStart = 0;
            diag->argEnd = 0;
            diag->argText = NULL;
            diag->argTextLen = 0;
        }
        return -1;
    }
    function.instStart = 0;
    function.instLen = 0;
    function.sourceRef = sourceIndex;
    function.paramCount = 0;
    function.localCount = 0;
    function.tempCount = 0;
    function.typeRef = UINT32_MAX;
    function.nameStart = 0;
    function.nameEnd = 0;
    if (resultTypeNode >= 0) {
        const H2AstNode* typeNode = &ast->nodes[resultTypeNode];
        typeRef.astNode = (uint32_t)resultTypeNode;
        typeRef.sourceRef = sourceIndex;
        typeRef.flags = 0;
        typeRef.aux = 0;
        if (H2MirProgramBuilderAddType(builder, &typeRef, &function.typeRef) != 0) {
            if (diag != NULL) {
                diag->code = H2Diag_ARENA_OOM;
                diag->type = H2DiagTypeOfCode(diag->code);
                diag->start = typeNode->start;
                diag->end = typeNode->end;
                diag->argStart = 0;
                diag->argEnd = 0;
                diag->argText = NULL;
                diag->argTextLen = 0;
            }
            return -1;
        }
    }
    if (H2MirProgramBuilderBeginFunction(builder, &function, &functionIndex) != 0) {
        if (diag != NULL) {
            diag->code = H2Diag_ARENA_OOM;
            diag->type = H2DiagTypeOfCode(diag->code);
            diag->start = 0;
            diag->end = 0;
            diag->argStart = 0;
            diag->argEnd = 0;
            diag->argText = NULL;
            diag->argTextLen = 0;
        }
        return -1;
    }
    for (i = 0; i < chunk.len; i++) {
        if (H2MirLowerAppendInst(builder, arena, src, &chunk.v[i], diag) != 0) {
            return -1;
        }
    }
    if (H2MirProgramBuilderEndFunction(builder) != 0) {
        if (diag != NULL) {
            diag->code = H2Diag_UNEXPECTED_TOKEN;
            diag->type = H2DiagTypeOfCode(diag->code);
            diag->start = 0;
            diag->end = 0;
            diag->argStart = 0;
            diag->argEnd = 0;
            diag->argText = NULL;
            diag->argTextLen = 0;
        }
        return -1;
    }
    *outFunctionIndex = functionIndex;
    return 0;
}

int H2MirLowerExprAsFunction(
    H2Arena* _Nonnull arena,
    const H2Ast* _Nonnull ast,
    H2StrView src,
    int32_t   nodeId,
    H2MirProgram* _Nonnull outProgram,
    int* _Nonnull outSupported,
    H2Diag* _Nullable diag) {
    H2MirProgramBuilder builder;
    uint32_t            functionIndex = UINT32_MAX;
    if (outProgram != NULL) {
        *outProgram = (H2MirProgram){ 0 };
    }
    if (arena == NULL || ast == NULL || outProgram == NULL || outSupported == NULL) {
        if (diag != NULL) {
            diag->code = H2Diag_UNEXPECTED_TOKEN;
            diag->type = H2DiagTypeOfCode(diag->code);
            diag->start = 0;
            diag->end = 0;
            diag->argStart = 0;
            diag->argEnd = 0;
            diag->argText = NULL;
            diag->argTextLen = 0;
        }
        return -1;
    }
    H2MirProgramBuilderInit(&builder, arena);
    if (H2MirLowerAppendExprAsFunction(
            &builder, arena, ast, src, nodeId, -1, &functionIndex, outSupported, diag)
        != 0)
    {
        return -1;
    }
    if (!*outSupported || functionIndex == UINT32_MAX) {
        return 0;
    }
    H2MirProgramBuilderFinish(&builder, outProgram);
    return 0;
}

H2_API_END
