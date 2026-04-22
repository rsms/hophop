#include "libsl-impl.h"
#include "mir.h"

SL_API_BEGIN

static void MirWWrite(SLWriter* w, const char* s, uint32_t len) {
    w->write(w->ctx, s, len);
}

static void MirWCStr(SLWriter* w, const char* s) {
    uint32_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    MirWWrite(w, s, n);
}

static void MirWU32(SLWriter* w, uint32_t v) {
    char     buf[16];
    uint32_t n = 0;
    if (v == 0) {
        MirWWrite(w, "0", 1);
        return;
    }
    while (v > 0 && n < (uint32_t)sizeof(buf)) {
        buf[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0) {
        n--;
        MirWWrite(w, &buf[n], 1);
    }
}

static void MirWU64(SLWriter* w, uint64_t v) {
    char     buf[32];
    uint32_t n = 0;
    if (v == 0) {
        MirWWrite(w, "0", 1);
        return;
    }
    while (v > 0 && n < (uint32_t)sizeof(buf)) {
        buf[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0) {
        n--;
        MirWWrite(w, &buf[n], 1);
    }
}

static void MirWI64(SLWriter* w, int64_t v) {
    uint64_t mag;
    if (v < 0) {
        MirWWrite(w, "-", 1);
        mag = (uint64_t)(-(v + 1)) + 1u;
    } else {
        mag = (uint64_t)v;
    }
    MirWU64(w, mag);
}

static void MirWHexU64(SLWriter* w, uint64_t v) {
    char               buf[16];
    uint32_t           i;
    static const char* digits = "0123456789abcdef";
    MirWCStr(w, "0x");
    for (i = 0; i < 16u; i++) {
        uint32_t shift = (15u - i) * 4u;
        buf[i] = digits[(uint32_t)((v >> shift) & 0x0fu)];
    }
    MirWWrite(w, buf, 16);
}

static void MirWIndent(SLWriter* w, uint32_t depth) {
    uint32_t i;
    for (i = 0; i < depth; i++) {
        MirWWrite(w, "  ", 2);
    }
}

static void MirWEscapedView(SLWriter* w, SLStrView src, uint32_t start, uint32_t end) {
    uint32_t i;
    MirWWrite(w, "\"", 1);
    for (i = start; i < end && i < src.len; i++) {
        unsigned char c = (unsigned char)src.ptr[i];
        switch (c) {
            case '\"': MirWWrite(w, "\\\"", 2); break;
            case '\\': MirWWrite(w, "\\\\", 2); break;
            case '\n': MirWWrite(w, "\\n", 2); break;
            case '\r': MirWWrite(w, "\\r", 2); break;
            case '\t': MirWWrite(w, "\\t", 2); break;
            default:
                if (c >= 0x20 && c <= 0x7e) {
                    MirWWrite(w, (const char*)&src.ptr[i], 1);
                } else {
                    char               hex[4];
                    static const char* digits = "0123456789abcdef";
                    hex[0] = '\\';
                    hex[1] = 'x';
                    hex[2] = digits[(c >> 4) & 0x0f];
                    hex[3] = digits[c & 0x0f];
                    MirWWrite(w, hex, 4);
                }
                break;
        }
    }
    MirWWrite(w, "\"", 1);
}

static void MirWEscapedBytes(SLWriter* w, SLStrView bytes) {
    MirWEscapedView(w, bytes, 0, bytes.len);
}

static const char* SLMirOpName(SLMirOp op) {
    switch (op) {
        case SLMirOp_INVALID:         return "INVALID";
        case SLMirOp_PUSH_CONST:      return "PUSH_CONST";
        case SLMirOp_PUSH_INT:        return "PUSH_INT";
        case SLMirOp_PUSH_FLOAT:      return "PUSH_FLOAT";
        case SLMirOp_PUSH_BOOL:       return "PUSH_BOOL";
        case SLMirOp_PUSH_STRING:     return "PUSH_STRING";
        case SLMirOp_PUSH_NULL:       return "PUSH_NULL";
        case SLMirOp_LOAD_IDENT:      return "LOAD_IDENT";
        case SLMirOp_STORE_IDENT:     return "STORE_IDENT";
        case SLMirOp_CALL:            return "CALL";
        case SLMirOp_UNARY:           return "UNARY";
        case SLMirOp_BINARY:          return "BINARY";
        case SLMirOp_INDEX:           return "INDEX";
        case SLMirOp_SEQ_LEN:         return "SEQ_LEN";
        case SLMirOp_STR_CSTR:        return "STR_CSTR";
        case SLMirOp_ITER_INIT:       return "ITER_INIT";
        case SLMirOp_ITER_NEXT:       return "ITER_NEXT";
        case SLMirOp_CAST:            return "CAST";
        case SLMirOp_COERCE:          return "COERCE";
        case SLMirOp_LOCAL_ZERO:      return "LOCAL_ZERO";
        case SLMirOp_LOCAL_LOAD:      return "LOCAL_LOAD";
        case SLMirOp_LOCAL_STORE:     return "LOCAL_STORE";
        case SLMirOp_LOCAL_ADDR:      return "LOCAL_ADDR";
        case SLMirOp_DROP:            return "DROP";
        case SLMirOp_JUMP:            return "JUMP";
        case SLMirOp_JUMP_IF_FALSE:   return "JUMP_IF_FALSE";
        case SLMirOp_ASSERT:          return "ASSERT";
        case SLMirOp_CALL_FN:         return "CALL_FN";
        case SLMirOp_CALL_HOST:       return "CALL_HOST";
        case SLMirOp_CALL_INDIRECT:   return "CALL_INDIRECT";
        case SLMirOp_DEREF_LOAD:      return "DEREF_LOAD";
        case SLMirOp_DEREF_STORE:     return "DEREF_STORE";
        case SLMirOp_ADDR_OF:         return "ADDR_OF";
        case SLMirOp_AGG_MAKE:        return "AGG_MAKE";
        case SLMirOp_AGG_ZERO:        return "AGG_ZERO";
        case SLMirOp_AGG_GET:         return "AGG_GET";
        case SLMirOp_AGG_SET:         return "AGG_SET";
        case SLMirOp_AGG_ADDR:        return "AGG_ADDR";
        case SLMirOp_ARRAY_ZERO:      return "ARRAY_ZERO";
        case SLMirOp_ARRAY_GET:       return "ARRAY_GET";
        case SLMirOp_ARRAY_SET:       return "ARRAY_SET";
        case SLMirOp_ARRAY_ADDR:      return "ARRAY_ADDR";
        case SLMirOp_TUPLE_MAKE:      return "TUPLE_MAKE";
        case SLMirOp_SLICE_MAKE:      return "SLICE_MAKE";
        case SLMirOp_OPTIONAL_WRAP:   return "OPTIONAL_WRAP";
        case SLMirOp_OPTIONAL_UNWRAP: return "OPTIONAL_UNWRAP";
        case SLMirOp_TAGGED_MAKE:     return "TAGGED_MAKE";
        case SLMirOp_TAGGED_TAG:      return "TAGGED_TAG";
        case SLMirOp_TAGGED_PAYLOAD:  return "TAGGED_PAYLOAD";
        case SLMirOp_ALLOC_NEW:       return "ALLOC_NEW";
        case SLMirOp_CTX_GET:         return "CTX_GET";
        case SLMirOp_CTX_ADDR:        return "CTX_ADDR";
        case SLMirOp_CTX_SET:         return "CTX_SET";
        case SLMirOp_RETURN:          return "RETURN";
        case SLMirOp_RETURN_VOID:     return "RETURN_VOID";
    }
    return "UNKNOWN";
}

static const char* SLMirConstKindName(SLMirConstKind kind) {
    switch (kind) {
        case SLMirConst_INVALID:  return "INVALID";
        case SLMirConst_INT:      return "INT";
        case SLMirConst_FLOAT:    return "FLOAT";
        case SLMirConst_BOOL:     return "BOOL";
        case SLMirConst_STRING:   return "STRING";
        case SLMirConst_NULL:     return "NULL";
        case SLMirConst_TYPE:     return "TYPE";
        case SLMirConst_FUNCTION: return "FUNCTION";
        case SLMirConst_HOST:     return "HOST";
    }
    return "UNKNOWN";
}

static const char* SLMirHostKindName(SLMirHostKind kind) {
    switch (kind) {
        case SLMirHost_INVALID: return "INVALID";
        case SLMirHost_GENERIC: return "GENERIC";
    }
    return "UNKNOWN";
}

static const char* SLMirHostTargetName(SLMirHostTarget target) {
    switch (target) {
        case SLMirHostTarget_INVALID:              return "INVALID";
        case SLMirHostTarget_PRINT:                return "PRINT";
        case SLMirHostTarget_PLATFORM_EXIT:        return "PLATFORM_EXIT";
        case SLMirHostTarget_FREE:                 return "FREE";
        case SLMirHostTarget_CONCAT:               return "CONCAT";
        case SLMirHostTarget_COPY:                 return "COPY";
        case SLMirHostTarget_PLATFORM_CONSOLE_LOG: return "PLATFORM_CONSOLE_LOG";
    }
    return "UNKNOWN";
}

static const char* SLMirSymbolKindName(SLMirSymbolKind kind) {
    switch (kind) {
        case SLMirSymbol_INVALID: return "INVALID";
        case SLMirSymbol_IDENT:   return "IDENT";
        case SLMirSymbol_CALL:    return "CALL";
        case SLMirSymbol_HOST:    return "HOST";
    }
    return "UNKNOWN";
}

static const char* SLMirCastTargetName(SLMirCastTarget target) {
    switch (target) {
        case SLMirCastTarget_INVALID:  return "INVALID";
        case SLMirCastTarget_INT:      return "INT";
        case SLMirCastTarget_FLOAT:    return "FLOAT";
        case SLMirCastTarget_BOOL:     return "BOOL";
        case SLMirCastTarget_STR_VIEW: return "STR_VIEW";
        case SLMirCastTarget_PTR_LIKE: return "PTR_LIKE";
    }
    return "UNKNOWN";
}

static const char* SLMirContextFieldName(SLMirContextField field) {
    switch (field) {
        case SLMirContextField_INVALID:        return "INVALID";
        case SLMirContextField_ALLOCATOR:      return "ALLOCATOR";
        case SLMirContextField_TEMP_ALLOCATOR: return "TEMP_ALLOCATOR";
        case SLMirContextField_LOGGER:         return "LOGGER";
    }
    return "UNKNOWN";
}

static void SLMirWriteRange(SLWriter* w, uint32_t start, uint32_t end) {
    MirWWrite(w, "[", 1);
    MirWU32(w, start);
    MirWWrite(w, ",", 1);
    MirWU32(w, end);
    MirWWrite(w, "]", 1);
}

static int SLMirRangeInSrc(SLStrView src, uint32_t start, uint32_t end) {
    return src.ptr != NULL && end >= start && end <= src.len;
}

static void SLMirWriteSliceOrRange(
    SLWriter* w, SLStrView src, uint32_t start, uint32_t end, int writeRange) {
    if (SLMirRangeInSrc(src, start, end)) {
        MirWEscapedView(w, src, start, end);
    } else {
        MirWCStr(w, "<slice ");
        SLMirWriteRange(w, start, end);
        MirWWrite(w, ">", 1);
    }
    if (writeRange) {
        MirWWrite(w, " ", 1);
        SLMirWriteRange(w, start, end);
    }
}

static void SLMirWriteCallFlags(SLWriter* w, uint16_t tok) {
    int wrote = 0;
    if (SLMirCallTokDropsReceiverArg0(tok)) {
        MirWCStr(w, " receiver_arg0");
        wrote = 1;
    }
    if (SLMirCallTokHasSpreadLast(tok)) {
        MirWCStr(w, wrote ? "|spread_last" : " spread_last");
    }
}

static void SLMirWriteIterFlags(SLWriter* w, uint16_t tok) {
    int wrote = 0;
    if ((tok & SLMirIterFlag_HAS_KEY) != 0u) {
        MirWCStr(w, wrote ? "|HAS_KEY" : " HAS_KEY");
        wrote = 1;
    }
    if ((tok & SLMirIterFlag_KEY_REF) != 0u) {
        MirWCStr(w, wrote ? "|KEY_REF" : " KEY_REF");
        wrote = 1;
    }
    if ((tok & SLMirIterFlag_VALUE_REF) != 0u) {
        MirWCStr(w, wrote ? "|VALUE_REF" : " VALUE_REF");
        wrote = 1;
    }
    if ((tok & SLMirIterFlag_VALUE_DISCARD) != 0u) {
        MirWCStr(w, wrote ? "|VALUE_DISCARD" : " VALUE_DISCARD");
    }
}

static SLStrView SLMirFunctionSource(
    const SLMirProgram* program, uint32_t funcIndex, SLStrView fallbackSrc) {
    if (program != NULL && funcIndex < program->funcLen) {
        const SLMirFunction* fn = &program->funcs[funcIndex];
        if (fn->sourceRef < program->sourceLen) {
            return program->sources[fn->sourceRef].src;
        }
    }
    return fallbackSrc;
}

static void SLMirDumpConst(const SLMirConst* value, uint32_t index, SLWriter* w, uint32_t depth) {
    MirWIndent(w, depth);
    MirWCStr(w, "#");
    MirWU32(w, index);
    MirWCStr(w, " ");
    MirWCStr(w, SLMirConstKindName(value->kind));
    switch (value->kind) {
        case SLMirConst_INT:
            MirWCStr(w, " value=");
            MirWI64(w, (int64_t)value->bits);
            break;
        case SLMirConst_FLOAT:
            MirWCStr(w, " bits=");
            MirWHexU64(w, value->bits);
            break;
        case SLMirConst_BOOL: MirWCStr(w, value->bits != 0 ? " true" : " false"); break;
        case SLMirConst_STRING:
            MirWCStr(w, " bytes=");
            MirWEscapedBytes(w, value->bytes);
            break;
        case SLMirConst_TYPE:
        case SLMirConst_FUNCTION:
        case SLMirConst_HOST:
            MirWCStr(w, " aux=");
            MirWU32(w, value->aux);
            break;
        default: break;
    }
    MirWWrite(w, "\n", 1);
}

static void SLMirDumpInst(
    const SLMirProgram*  program,
    const SLMirFunction* fn,
    uint32_t             funcIndex,
    uint32_t             pc,
    const SLMirInst*     ins,
    SLStrView            fallbackSrc,
    SLWriter*            w,
    uint32_t             depth) {
    SLStrView src = SLMirFunctionSource(program, funcIndex, fallbackSrc);
    MirWIndent(w, depth);
    MirWU32(w, pc);
    MirWCStr(w, ": ");
    MirWCStr(w, SLMirOpName(ins->op));
    switch (ins->op) {
        case SLMirOp_PUSH_CONST:
            MirWCStr(w, " const=#");
            MirWU32(w, ins->aux);
            break;
        case SLMirOp_LOAD_IDENT:
        case SLMirOp_STORE_IDENT:
        case SLMirOp_CALL:
            MirWCStr(w, " symbol=#");
            MirWU32(w, ins->aux);
            if (ins->aux < program->symbolLen) {
                const SLMirSymbolRef* sym = &program->symbols[ins->aux];
                MirWCStr(w, " ");
                SLMirWriteSliceOrRange(w, src, sym->nameStart, sym->nameEnd, 1);
                if (ins->op == SLMirOp_CALL) {
                    MirWCStr(w, " argc=");
                    MirWU32(w, SLMirCallArgCountFromTok(ins->tok));
                    SLMirWriteCallFlags(w, ins->tok);
                }
            }
            break;
        case SLMirOp_CALL_FN:
            MirWCStr(w, " fn=#");
            MirWU32(w, ins->aux);
            MirWCStr(w, " argc=");
            MirWU32(w, SLMirCallArgCountFromTok(ins->tok));
            SLMirWriteCallFlags(w, ins->tok);
            break;
        case SLMirOp_CALL_HOST:
            MirWCStr(w, " host=#");
            MirWU32(w, ins->aux);
            MirWCStr(w, " argc=");
            MirWU32(w, SLMirCallArgCountFromTok(ins->tok));
            SLMirWriteCallFlags(w, ins->tok);
            break;
        case SLMirOp_CALL_INDIRECT:
            MirWCStr(w, " argc=");
            MirWU32(w, SLMirCallArgCountFromTok(ins->tok));
            SLMirWriteCallFlags(w, ins->tok);
            break;
        case SLMirOp_LOCAL_ZERO:
        case SLMirOp_LOCAL_LOAD:
        case SLMirOp_LOCAL_STORE:
        case SLMirOp_LOCAL_ADDR:
            MirWCStr(w, " local=#");
            MirWU32(w, ins->aux);
            if (ins->aux < fn->localCount) {
                const SLMirLocal* local = &program->locals[fn->localStart + ins->aux];
                MirWCStr(w, " ");
                SLMirWriteSliceOrRange(w, src, local->nameStart, local->nameEnd, 1);
            }
            break;
        case SLMirOp_JUMP:
        case SLMirOp_JUMP_IF_FALSE:
            MirWCStr(w, " target=");
            MirWU32(w, ins->aux);
            break;
        case SLMirOp_CAST:
            MirWCStr(w, " cast=");
            MirWCStr(w, SLMirCastTargetName((SLMirCastTarget)ins->tok));
            MirWCStr(w, " type=#");
            MirWU32(w, ins->aux);
            break;
        case SLMirOp_COERCE:
            MirWCStr(w, " type=#");
            MirWU32(w, ins->aux);
            break;
        case SLMirOp_UNARY:
        case SLMirOp_BINARY:
            MirWCStr(w, " op=");
            MirWCStr(w, SLTokenKindName((SLTokenKind)ins->tok));
            break;
        case SLMirOp_ITER_INIT:
            MirWCStr(w, " flags=");
            SLMirWriteIterFlags(w, ins->tok);
            MirWCStr(w, " aux=");
            MirWU32(w, ins->aux);
            break;
        case SLMirOp_ITER_NEXT:
            MirWCStr(w, " flags=");
            SLMirWriteIterFlags(w, ins->tok);
            break;
        case SLMirOp_AGG_GET:
        case SLMirOp_AGG_ADDR:
            MirWCStr(w, " field=#");
            MirWU32(w, ins->aux);
            if (ins->aux < program->fieldLen) {
                const SLMirField* field = &program->fields[ins->aux];
                MirWCStr(w, " ");
                SLMirWriteSliceOrRange(w, src, field->nameStart, field->nameEnd, 1);
            }
            break;
        case SLMirOp_AGG_MAKE:
        case SLMirOp_TUPLE_MAKE:
        case SLMirOp_TAGGED_MAKE:
        case SLMirOp_ARRAY_ZERO:
        case SLMirOp_OPTIONAL_WRAP:
            MirWCStr(w, " count=");
            MirWU32(w, ins->tok);
            break;
        case SLMirOp_AGG_ZERO:
        case SLMirOp_SLICE_MAKE:
        case SLMirOp_ARRAY_GET:
        case SLMirOp_ARRAY_SET:
        case SLMirOp_ARRAY_ADDR:
        case SLMirOp_INDEX:
        case SLMirOp_ASSERT:
        case SLMirOp_OPTIONAL_UNWRAP:
        case SLMirOp_TAGGED_TAG:
        case SLMirOp_TAGGED_PAYLOAD:
        case SLMirOp_ALLOC_NEW:
            if (ins->tok != 0) {
                MirWCStr(w, " tok=");
                MirWU32(w, ins->tok);
            }
            if (ins->aux != 0) {
                MirWCStr(w, " aux=");
                MirWU32(w, ins->aux);
            }
            break;
        case SLMirOp_CTX_GET:
        case SLMirOp_CTX_ADDR:
        case SLMirOp_CTX_SET:
            MirWCStr(w, " field=");
            MirWCStr(w, SLMirContextFieldName((SLMirContextField)ins->aux));
            break;
        default:
            if (ins->tok != 0) {
                MirWCStr(w, " tok=");
                MirWU32(w, ins->tok);
            }
            if (ins->aux != 0) {
                MirWCStr(w, " aux=");
                MirWU32(w, ins->aux);
            }
            break;
    }
    MirWCStr(w, " span=");
    SLMirWriteRange(w, ins->start, ins->end);
    MirWWrite(w, "\n", 1);
}

int SLMirDumpProgram(
    const SLMirProgram* program, SLStrView src, SLWriter* w, SLDiag* _Nullable diag) {
    uint32_t i;
    if (diag != NULL) {
        *diag = (SLDiag){ 0 };
    }
    if (program == NULL || w == NULL || w->write == NULL) {
        if (diag != NULL) {
            diag->code = SLDiag_UNEXPECTED_TOKEN;
            diag->type = SLDiagTypeOfCode(SLDiag_UNEXPECTED_TOKEN);
        }
        return -1;
    }
    MirWCStr(w, "mir\n");
    MirWCStr(w, "  dynamic_resolution=");
    MirWCStr(w, SLMirProgramNeedsDynamicResolution(program) ? "yes\n" : "no\n");
    MirWCStr(w, "  insts=");
    MirWU32(w, program->instLen);
    MirWCStr(w, " consts=");
    MirWU32(w, program->constLen);
    MirWCStr(w, " sources=");
    MirWU32(w, program->sourceLen);
    MirWCStr(w, " funcs=");
    MirWU32(w, program->funcLen);
    MirWCStr(w, " locals=");
    MirWU32(w, program->localLen);
    MirWCStr(w, " fields=");
    MirWU32(w, program->fieldLen);
    MirWCStr(w, " types=");
    MirWU32(w, program->typeLen);
    MirWCStr(w, " hosts=");
    MirWU32(w, program->hostLen);
    MirWCStr(w, " symbols=");
    MirWU32(w, program->symbolLen);
    MirWWrite(w, "\n", 1);

    MirWCStr(w, "sources\n");
    for (i = 0; i < program->sourceLen; i++) {
        MirWIndent(w, 1);
        MirWCStr(w, "#");
        MirWU32(w, i);
        MirWCStr(w, " len=");
        MirWU32(w, program->sources[i].src.len);
        MirWWrite(w, "\n", 1);
    }

    MirWCStr(w, "types\n");
    for (i = 0; i < program->typeLen; i++) {
        const SLMirTypeRef* typeRef = &program->types[i];
        MirWIndent(w, 1);
        MirWCStr(w, "#");
        MirWU32(w, i);
        MirWCStr(w, " astNode=");
        MirWU32(w, typeRef->astNode);
        MirWCStr(w, " source=#");
        MirWU32(w, typeRef->sourceRef);
        if (typeRef->flags != 0) {
            MirWCStr(w, " flags=");
            MirWU32(w, typeRef->flags);
        }
        if (typeRef->aux != 0 && SLMirTypeRefIsFixedArray(typeRef)) {
            MirWCStr(w, " aux=");
            MirWU32(w, typeRef->aux);
        }
        MirWWrite(w, "\n", 1);
    }

    MirWCStr(w, "hosts\n");
    for (i = 0; i < program->hostLen; i++) {
        const SLMirHostRef* host = &program->hosts[i];
        MirWIndent(w, 1);
        MirWCStr(w, "#");
        MirWU32(w, i);
        MirWCStr(w, " ");
        MirWCStr(w, SLMirHostKindName(host->kind));
        MirWCStr(w, " target=");
        MirWCStr(w, SLMirHostTargetName((SLMirHostTarget)host->target));
        MirWCStr(w, " name=");
        SLMirWriteSliceOrRange(w, src, host->nameStart, host->nameEnd, 1);
        if (host->flags != 0) {
            MirWCStr(w, " flags=");
            MirWU32(w, host->flags);
        }
        MirWWrite(w, "\n", 1);
    }

    MirWCStr(w, "symbols\n");
    for (i = 0; i < program->symbolLen; i++) {
        const SLMirSymbolRef* sym = &program->symbols[i];
        MirWIndent(w, 1);
        MirWCStr(w, "#");
        MirWU32(w, i);
        MirWCStr(w, " ");
        MirWCStr(w, SLMirSymbolKindName(sym->kind));
        MirWCStr(w, " target=");
        MirWU32(w, sym->target);
        MirWCStr(w, " name=");
        SLMirWriteSliceOrRange(w, src, sym->nameStart, sym->nameEnd, 1);
        if (sym->flags != 0) {
            MirWCStr(w, " flags=");
            MirWU32(w, sym->flags);
        }
        MirWWrite(w, "\n", 1);
    }

    MirWCStr(w, "fields\n");
    for (i = 0; i < program->fieldLen; i++) {
        const SLMirField* field = &program->fields[i];
        MirWIndent(w, 1);
        MirWCStr(w, "#");
        MirWU32(w, i);
        MirWCStr(w, " ownerType=#");
        MirWU32(w, field->ownerTypeRef);
        MirWCStr(w, " type=#");
        MirWU32(w, field->typeRef);
        MirWCStr(w, " name=");
        SLMirWriteSliceOrRange(w, src, field->nameStart, field->nameEnd, 1);
        MirWWrite(w, "\n", 1);
    }

    MirWCStr(w, "consts\n");
    for (i = 0; i < program->constLen; i++) {
        SLMirDumpConst(&program->consts[i], i, w, 1);
    }

    MirWCStr(w, "functions\n");
    for (i = 0; i < program->funcLen; i++) {
        uint32_t             j;
        const SLMirFunction* fn = &program->funcs[i];
        SLStrView            fnSrc = SLMirFunctionSource(program, i, src);
        MirWIndent(w, 1);
        MirWCStr(w, "#");
        MirWU32(w, i);
        MirWCStr(w, " name=");
        SLMirWriteSliceOrRange(w, fnSrc, fn->nameStart, fn->nameEnd, 1);
        MirWCStr(w, " source=#");
        MirWU32(w, fn->sourceRef);
        MirWCStr(w, " type=#");
        MirWU32(w, fn->typeRef);
        MirWCStr(w, " params=");
        MirWU32(w, fn->paramCount);
        MirWCStr(w, " temps=");
        MirWU32(w, fn->tempCount);
        MirWCStr(w, " locals=#");
        MirWU32(w, fn->localStart);
        MirWCStr(w, "+");
        MirWU32(w, fn->localCount);
        MirWCStr(w, " insts=#");
        MirWU32(w, fn->instStart);
        MirWCStr(w, "+");
        MirWU32(w, fn->instLen);
        if (fn->flags != 0) {
            MirWCStr(w, " flags=");
            MirWU32(w, fn->flags);
        }
        MirWWrite(w, "\n", 1);

        MirWIndent(w, 2);
        MirWCStr(w, "locals\n");
        for (j = 0; j < fn->localCount; j++) {
            const SLMirLocal* local = &program->locals[fn->localStart + j];
            MirWIndent(w, 3);
            MirWCStr(w, "#");
            MirWU32(w, j);
            MirWCStr(w, " name=");
            SLMirWriteSliceOrRange(w, fnSrc, local->nameStart, local->nameEnd, 1);
            MirWCStr(w, " type=#");
            MirWU32(w, local->typeRef);
            if (local->flags != 0) {
                MirWCStr(w, " flags=");
                MirWU32(w, local->flags);
            }
            MirWWrite(w, "\n", 1);
        }

        MirWIndent(w, 2);
        MirWCStr(w, "insts\n");
        for (j = 0; j < fn->instLen; j++) {
            SLMirDumpInst(program, fn, i, j, &program->insts[fn->instStart + j], src, w, 3);
        }
    }
    return 0;
}

SL_API_END
