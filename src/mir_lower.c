#include "libsl-impl.h"
#include "mir.h"
#include "mir_lower.h"

SL_API_BEGIN

static void SLMirLowerSetDiag(SLDiag* diag, SLDiagCode code, uint32_t start, uint32_t end) {
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

static int SLMirLowerParseIntLiteral(SLStrView src, uint32_t start, uint32_t end, int64_t* out) {
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

static int SLMirLowerParseFloatLiteral(SLStrView src, uint32_t start, uint32_t end, double* out) {
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

static int SLMirLowerParseBoolLiteral(SLStrView src, uint32_t start, uint32_t end, uint8_t* out) {
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

static int SLMirLowerRewriteConstInst(
    SLMirProgramBuilder* _Nonnull builder,
    SLArena* _Nonnull arena,
    SLStrView src,
    const SLMirInst* _Nonnull in,
    SLMirInst* _Nonnull out,
    SLDiag* _Nullable diag) {
    SLMirConst     value = { 0 };
    uint32_t       constIndex = 0;
    SLRuneLitErr   runeErr = { 0 };
    SLStringLitErr litErr = { 0 };
    int64_t        intValue = 0;
    double         floatValue = 0.0;
    uint8_t        boolValue = 0;
    uint32_t       rune = 0;
    uint8_t*       bytes = NULL;
    uint32_t       len = 0;
    memcpy(out, in, sizeof(*out));
    switch (in->op) {
        case SLMirOp_PUSH_INT:
            value.kind = SLMirConst_INT;
            if ((SLTokenKind)in->tok == SLTok_RUNE) {
                if (SLDecodeRuneLiteralValidate(src.ptr, in->start, in->end, &rune, &runeErr) != 0)
                {
                    SLMirLowerSetDiag(
                        diag, SLRuneLitErrDiagCode(runeErr.kind), runeErr.start, runeErr.end);
                    return -1;
                }
                value.bits = (uint64_t)(int64_t)rune;
            } else if (SLMirLowerParseIntLiteral(src, in->start, in->end, &intValue) != 0) {
                return 0;
            } else {
                value.bits = (uint64_t)intValue;
            }
            break;
        case SLMirOp_PUSH_FLOAT:
            value.kind = SLMirConst_FLOAT;
            if (SLMirLowerParseFloatLiteral(src, in->start, in->end, &floatValue) != 0) {
                return 0;
            }
            memcpy(&value.bits, &floatValue, sizeof(floatValue));
            break;
        case SLMirOp_PUSH_BOOL:
            value.kind = SLMirConst_BOOL;
            if (SLMirLowerParseBoolLiteral(src, in->start, in->end, &boolValue) != 0) {
                return 0;
            }
            value.bits = (uint64_t)boolValue;
            break;
        case SLMirOp_PUSH_STRING:
            value.kind = SLMirConst_STRING;
            if (SLDecodeStringLiteralArena(
                    arena, src.ptr, in->start, in->end, &bytes, &len, &litErr)
                != 0)
            {
                SLMirLowerSetDiag(
                    diag, SLStringLitErrDiagCode(litErr.kind), litErr.start, litErr.end);
                return -1;
            }
            value.bytes.ptr = (const char*)bytes;
            value.bytes.len = len;
            break;
        case SLMirOp_PUSH_NULL: value.kind = SLMirConst_NULL; break;
        default:                return 0;
    }
    if (SLMirProgramBuilderAddConst(builder, &value, &constIndex) != 0) {
        SLMirLowerSetDiag(diag, SLDiag_ARENA_OOM, in->start, in->end);
        return -1;
    }
    out->op = SLMirOp_PUSH_CONST;
    out->tok = 0;
    out->aux = constIndex;
    return 1;
}

static int SLMirLowerInternSymbol(
    SLMirProgramBuilder* _Nonnull builder,
    SLMirSymbolKind kind,
    uint32_t        nameStart,
    uint32_t        nameEnd,
    uint32_t        flags,
    uint32_t        target,
    uint32_t* _Nonnull outIndex,
    SLDiag* _Nullable diag) {
    uint32_t       i;
    SLMirSymbolRef symbol;
    if (builder == NULL || outIndex == NULL) {
        return -1;
    }
    for (i = 0; i < builder->symbolLen; i++) {
        const SLMirSymbolRef* existing = &builder->symbols[i];
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
    if (SLMirProgramBuilderAddSymbol(builder, &symbol, outIndex) != 0) {
        SLMirLowerSetDiag(diag, SLDiag_ARENA_OOM, nameStart, nameEnd);
        return -1;
    }
    return 0;
}

static int SLMirLowerRewriteSymbolInst(
    SLMirProgramBuilder* _Nonnull builder,
    const SLMirInst* _Nonnull in,
    SLMirInst* _Nonnull out,
    SLDiag* _Nullable diag) {
    SLMirSymbolKind kind = SLMirSymbol_INVALID;
    uint32_t        symbolIndex = 0;
    memcpy(out, in, sizeof(*out));
    switch (in->op) {
        case SLMirOp_LOAD_IDENT: kind = SLMirSymbol_IDENT; break;
        case SLMirOp_CALL:       kind = SLMirSymbol_CALL; break;
        default:                 return 0;
    }
    if (SLMirLowerInternSymbol(
            builder, kind, in->start, in->end, in->aux, (uint32_t)in->tok, &symbolIndex, diag)
        != 0)
    {
        return -1;
    }
    out->aux = symbolIndex;
    return 1;
}

static int SLMirLowerInternType(
    SLMirProgramBuilder* _Nonnull builder,
    uint32_t astNode,
    uint32_t flags,
    uint32_t* _Nonnull outIndex,
    SLDiag* _Nullable diag) {
    uint32_t     i;
    SLMirTypeRef typeRef;
    if (builder == NULL || outIndex == NULL) {
        return -1;
    }
    for (i = 0; i < builder->typeLen; i++) {
        const SLMirTypeRef* existing = &builder->types[i];
        if (existing->astNode == astNode && existing->flags == flags) {
            *outIndex = i;
            return 0;
        }
    }
    typeRef.astNode = astNode;
    typeRef.flags = flags;
    if (SLMirProgramBuilderAddType(builder, &typeRef, outIndex) != 0) {
        SLMirLowerSetDiag(diag, SLDiag_ARENA_OOM, 0, 0);
        return -1;
    }
    return 0;
}

static int SLMirLowerRewriteTypeInst(
    SLMirProgramBuilder* _Nonnull builder,
    const SLMirInst* _Nonnull in,
    SLMirInst* _Nonnull out,
    SLDiag* _Nullable diag) {
    uint32_t typeIndex = 0;
    memcpy(out, in, sizeof(*out));
    if (in->op != SLMirOp_CAST) {
        return 0;
    }
    if (SLMirLowerInternType(builder, in->aux, 0u, &typeIndex, diag) != 0) {
        return -1;
    }
    out->aux = typeIndex;
    return 1;
}

int SLMirLowerExprAsFunction(
    SLArena* _Nonnull arena,
    const SLAst* _Nonnull ast,
    SLStrView src,
    int32_t   nodeId,
    SLMirProgram* _Nonnull outProgram,
    int* _Nonnull outSupported,
    SLDiag* _Nullable diag) {
    SLMirChunk          chunk;
    SLMirFunction       function = { 0 };
    SLMirProgramBuilder builder;
    uint32_t            functionIndex = 0;
    uint32_t            i;

    if (outProgram != NULL) {
        *outProgram = (SLMirProgram){ 0 };
    }
    if (arena == NULL || ast == NULL || outProgram == NULL || outSupported == NULL) {
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

    *outSupported = 0;
    if (SLMirBuildExpr(arena, ast, src, nodeId, &chunk, outSupported, diag) != 0) {
        return -1;
    }
    if (!*outSupported) {
        return 0;
    }

    SLMirProgramBuilderInit(&builder, arena);
    function.instStart = 0;
    function.instLen = 0;
    function.paramCount = 0;
    function.localCount = 0;
    function.tempCount = 0;
    function.typeRef = 0;
    function.nameStart = 0;
    function.nameEnd = 0;
    if (SLMirProgramBuilderBeginFunction(&builder, &function, &functionIndex) != 0) {
        if (diag != NULL) {
            diag->code = SLDiag_ARENA_OOM;
            diag->type = SLDiagTypeOfCode(diag->code);
            diag->start = 0;
            diag->end = 0;
            diag->argStart = 0;
            diag->argEnd = 0;
        }
        return -1;
    }
    for (i = 0; i < chunk.len; i++) {
        SLMirInst loweredInst;
        int       rewriteStatus = SLMirLowerRewriteConstInst(
            &builder, arena, src, &chunk.v[i], &loweredInst, diag);
        if (rewriteStatus < 0) {
            return -1;
        }
        if (rewriteStatus == 0) {
            memcpy(&loweredInst, &chunk.v[i], sizeof(loweredInst));
            rewriteStatus = SLMirLowerRewriteSymbolInst(&builder, &chunk.v[i], &loweredInst, diag);
            if (rewriteStatus < 0) {
                return -1;
            }
            if (rewriteStatus == 0) {
                rewriteStatus = SLMirLowerRewriteTypeInst(
                    &builder, &chunk.v[i], &loweredInst, diag);
                if (rewriteStatus < 0) {
                    return -1;
                }
            }
        }
        if (SLMirProgramBuilderAppendInst(&builder, &loweredInst) != 0) {
            if (diag != NULL) {
                diag->code = SLDiag_ARENA_OOM;
                diag->type = SLDiagTypeOfCode(diag->code);
                diag->start = loweredInst.start;
                diag->end = loweredInst.end;
                diag->argStart = 0;
                diag->argEnd = 0;
            }
            return -1;
        }
    }
    if (SLMirProgramBuilderEndFunction(&builder) != 0 || functionIndex != 0u) {
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
    SLMirProgramBuilderFinish(&builder, outProgram);
    return 0;
}

SL_API_END
