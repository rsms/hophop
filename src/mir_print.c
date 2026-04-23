#include "libhop-impl.h"
#include "mir.h"

H2_API_BEGIN

static void MirWWrite(H2Writer* w, const char* s, uint32_t len) {
    w->write(w->ctx, s, len);
}

static void MirWCStr(H2Writer* w, const char* s) {
    uint32_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    MirWWrite(w, s, n);
}

static void MirWU32(H2Writer* w, uint32_t v) {
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

static void MirWU64(H2Writer* w, uint64_t v) {
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

static void MirWI64(H2Writer* w, int64_t v) {
    uint64_t mag;
    if (v < 0) {
        MirWWrite(w, "-", 1);
        mag = (uint64_t)(-(v + 1)) + 1u;
    } else {
        mag = (uint64_t)v;
    }
    MirWU64(w, mag);
}

static void MirWHexU64(H2Writer* w, uint64_t v) {
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

static void MirWIndent(H2Writer* w, uint32_t depth) {
    uint32_t i;
    for (i = 0; i < depth; i++) {
        MirWWrite(w, "  ", 2);
    }
}

static void MirWEscapedView(H2Writer* w, H2StrView src, uint32_t start, uint32_t end) {
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

static void MirWEscapedBytes(H2Writer* w, H2StrView bytes) {
    MirWEscapedView(w, bytes, 0, bytes.len);
}

static const char* H2MirOpName(H2MirOp op) {
    switch (op) {
        case H2MirOp_INVALID:         return "INVALID";
        case H2MirOp_PUSH_CONST:      return "PUSH_CONST";
        case H2MirOp_PUSH_INT:        return "PUSH_INT";
        case H2MirOp_PUSH_FLOAT:      return "PUSH_FLOAT";
        case H2MirOp_PUSH_BOOL:       return "PUSH_BOOL";
        case H2MirOp_PUSH_STRING:     return "PUSH_STRING";
        case H2MirOp_PUSH_NULL:       return "PUSH_NULL";
        case H2MirOp_LOAD_IDENT:      return "LOAD_IDENT";
        case H2MirOp_STORE_IDENT:     return "STORE_IDENT";
        case H2MirOp_CALL:            return "CALL";
        case H2MirOp_UNARY:           return "UNARY";
        case H2MirOp_BINARY:          return "BINARY";
        case H2MirOp_INDEX:           return "INDEX";
        case H2MirOp_SEQ_LEN:         return "SEQ_LEN";
        case H2MirOp_STR_CSTR:        return "STR_CSTR";
        case H2MirOp_ITER_INIT:       return "ITER_INIT";
        case H2MirOp_ITER_NEXT:       return "ITER_NEXT";
        case H2MirOp_CAST:            return "CAST";
        case H2MirOp_COERCE:          return "COERCE";
        case H2MirOp_LOCAL_ZERO:      return "LOCAL_ZERO";
        case H2MirOp_LOCAL_LOAD:      return "LOCAL_LOAD";
        case H2MirOp_LOCAL_STORE:     return "LOCAL_STORE";
        case H2MirOp_LOCAL_ADDR:      return "LOCAL_ADDR";
        case H2MirOp_DROP:            return "DROP";
        case H2MirOp_JUMP:            return "JUMP";
        case H2MirOp_JUMP_IF_FALSE:   return "JUMP_IF_FALSE";
        case H2MirOp_ASSERT:          return "ASSERT";
        case H2MirOp_CALL_FN:         return "CALL_FN";
        case H2MirOp_CALL_HOST:       return "CALL_HOST";
        case H2MirOp_CALL_INDIRECT:   return "CALL_INDIRECT";
        case H2MirOp_DEREF_LOAD:      return "DEREF_LOAD";
        case H2MirOp_DEREF_STORE:     return "DEREF_STORE";
        case H2MirOp_ADDR_OF:         return "ADDR_OF";
        case H2MirOp_AGG_MAKE:        return "AGG_MAKE";
        case H2MirOp_AGG_ZERO:        return "AGG_ZERO";
        case H2MirOp_AGG_GET:         return "AGG_GET";
        case H2MirOp_AGG_SET:         return "AGG_SET";
        case H2MirOp_AGG_ADDR:        return "AGG_ADDR";
        case H2MirOp_ARRAY_ZERO:      return "ARRAY_ZERO";
        case H2MirOp_ARRAY_GET:       return "ARRAY_GET";
        case H2MirOp_ARRAY_SET:       return "ARRAY_SET";
        case H2MirOp_ARRAY_ADDR:      return "ARRAY_ADDR";
        case H2MirOp_TUPLE_MAKE:      return "TUPLE_MAKE";
        case H2MirOp_SLICE_MAKE:      return "SLICE_MAKE";
        case H2MirOp_OPTIONAL_WRAP:   return "OPTIONAL_WRAP";
        case H2MirOp_OPTIONAL_UNWRAP: return "OPTIONAL_UNWRAP";
        case H2MirOp_TAGGED_MAKE:     return "TAGGED_MAKE";
        case H2MirOp_TAGGED_TAG:      return "TAGGED_TAG";
        case H2MirOp_TAGGED_PAYLOAD:  return "TAGGED_PAYLOAD";
        case H2MirOp_ALLOC_NEW:       return "ALLOC_NEW";
        case H2MirOp_CTX_GET:         return "CTX_GET";
        case H2MirOp_CTX_ADDR:        return "CTX_ADDR";
        case H2MirOp_CTX_SET:         return "CTX_SET";
        case H2MirOp_RETURN:          return "RETURN";
        case H2MirOp_RETURN_VOID:     return "RETURN_VOID";
    }
    return "UNKNOWN";
}

static const char* H2MirConstKindName(H2MirConstKind kind) {
    switch (kind) {
        case H2MirConst_INVALID:  return "INVALID";
        case H2MirConst_INT:      return "INT";
        case H2MirConst_FLOAT:    return "FLOAT";
        case H2MirConst_BOOL:     return "BOOL";
        case H2MirConst_STRING:   return "STRING";
        case H2MirConst_NULL:     return "NULL";
        case H2MirConst_TYPE:     return "TYPE";
        case H2MirConst_FUNCTION: return "FUNCTION";
        case H2MirConst_HOST:     return "HOST";
    }
    return "UNKNOWN";
}

static const char* H2MirHostKindName(H2MirHostKind kind) {
    switch (kind) {
        case H2MirHost_INVALID: return "INVALID";
        case H2MirHost_GENERIC: return "GENERIC";
    }
    return "UNKNOWN";
}

static const char* H2MirHostTargetName(H2MirHostTarget target) {
    switch (target) {
        case H2MirHostTarget_INVALID:              return "INVALID";
        case H2MirHostTarget_PRINT:                return "PRINT";
        case H2MirHostTarget_PLATFORM_EXIT:        return "PLATFORM_EXIT";
        case H2MirHostTarget_FREE:                 return "FREE";
        case H2MirHostTarget_CONCAT:               return "CONCAT";
        case H2MirHostTarget_COPY:                 return "COPY";
        case H2MirHostTarget_PLATFORM_CONSOLE_LOG: return "PLATFORM_CONSOLE_LOG";
    }
    return "UNKNOWN";
}

static const char* H2MirSymbolKindName(H2MirSymbolKind kind) {
    switch (kind) {
        case H2MirSymbol_INVALID: return "INVALID";
        case H2MirSymbol_IDENT:   return "IDENT";
        case H2MirSymbol_CALL:    return "CALL";
        case H2MirSymbol_HOST:    return "HOST";
    }
    return "UNKNOWN";
}

static const char* H2MirCastTargetName(H2MirCastTarget target) {
    switch (target) {
        case H2MirCastTarget_INVALID:  return "INVALID";
        case H2MirCastTarget_INT:      return "INT";
        case H2MirCastTarget_FLOAT:    return "FLOAT";
        case H2MirCastTarget_BOOL:     return "BOOL";
        case H2MirCastTarget_STR_VIEW: return "STR_VIEW";
        case H2MirCastTarget_PTR_LIKE: return "PTR_LIKE";
    }
    return "UNKNOWN";
}

static const char* H2MirContextFieldName(H2MirContextField field) {
    switch (field) {
        case H2MirContextField_INVALID:        return "INVALID";
        case H2MirContextField_ALLOCATOR:      return "ALLOCATOR";
        case H2MirContextField_TEMP_ALLOCATOR: return "TEMP_ALLOCATOR";
        case H2MirContextField_LOGGER:         return "LOGGER";
    }
    return "UNKNOWN";
}

static void H2MirWriteRange(H2Writer* w, uint32_t start, uint32_t end) {
    MirWWrite(w, "[", 1);
    MirWU32(w, start);
    MirWWrite(w, ",", 1);
    MirWU32(w, end);
    MirWWrite(w, "]", 1);
}

static int H2MirRangeInSrc(H2StrView src, uint32_t start, uint32_t end) {
    return src.ptr != NULL && end >= start && end <= src.len;
}

static void H2MirWriteSliceOrRange(
    H2Writer* w, H2StrView src, uint32_t start, uint32_t end, int writeRange) {
    if (H2MirRangeInSrc(src, start, end)) {
        MirWEscapedView(w, src, start, end);
    } else {
        MirWCStr(w, "<slice ");
        H2MirWriteRange(w, start, end);
        MirWWrite(w, ">", 1);
    }
    if (writeRange) {
        MirWWrite(w, " ", 1);
        H2MirWriteRange(w, start, end);
    }
}

static void H2MirWriteCallFlags(H2Writer* w, uint16_t tok) {
    int wrote = 0;
    if (H2MirCallTokDropsReceiverArg0(tok)) {
        MirWCStr(w, " receiver_arg0");
        wrote = 1;
    }
    if (H2MirCallTokHasSpreadLast(tok)) {
        MirWCStr(w, wrote ? "|spread_last" : " spread_last");
    }
}

static void H2MirWriteIterFlags(H2Writer* w, uint16_t tok) {
    int wrote = 0;
    if ((tok & H2MirIterFlag_HAS_KEY) != 0u) {
        MirWCStr(w, wrote ? "|HAS_KEY" : " HAS_KEY");
        wrote = 1;
    }
    if ((tok & H2MirIterFlag_KEY_REF) != 0u) {
        MirWCStr(w, wrote ? "|KEY_REF" : " KEY_REF");
        wrote = 1;
    }
    if ((tok & H2MirIterFlag_VALUE_REF) != 0u) {
        MirWCStr(w, wrote ? "|VALUE_REF" : " VALUE_REF");
        wrote = 1;
    }
    if ((tok & H2MirIterFlag_VALUE_DISCARD) != 0u) {
        MirWCStr(w, wrote ? "|VALUE_DISCARD" : " VALUE_DISCARD");
    }
}

static H2StrView H2MirFunctionSource(
    const H2MirProgram* program, uint32_t funcIndex, H2StrView fallbackSrc) {
    if (program != NULL && funcIndex < program->funcLen) {
        const H2MirFunction* fn = &program->funcs[funcIndex];
        if (fn->sourceRef < program->sourceLen) {
            return program->sources[fn->sourceRef].src;
        }
    }
    return fallbackSrc;
}

static void H2MirDumpConst(const H2MirConst* value, uint32_t index, H2Writer* w, uint32_t depth) {
    MirWIndent(w, depth);
    MirWCStr(w, "#");
    MirWU32(w, index);
    MirWCStr(w, " ");
    MirWCStr(w, H2MirConstKindName(value->kind));
    switch (value->kind) {
        case H2MirConst_INT:
            MirWCStr(w, " value=");
            MirWI64(w, (int64_t)value->bits);
            break;
        case H2MirConst_FLOAT:
            MirWCStr(w, " bits=");
            MirWHexU64(w, value->bits);
            break;
        case H2MirConst_BOOL: MirWCStr(w, value->bits != 0 ? " true" : " false"); break;
        case H2MirConst_STRING:
            MirWCStr(w, " bytes=");
            MirWEscapedBytes(w, value->bytes);
            break;
        case H2MirConst_TYPE:
        case H2MirConst_FUNCTION:
        case H2MirConst_HOST:
            MirWCStr(w, " aux=");
            MirWU32(w, value->aux);
            break;
        default: break;
    }
    MirWWrite(w, "\n", 1);
}

static void H2MirDumpInst(
    const H2MirProgram*  program,
    const H2MirFunction* fn,
    uint32_t             funcIndex,
    uint32_t             pc,
    const H2MirInst*     ins,
    H2StrView            fallbackSrc,
    H2Writer*            w,
    uint32_t             depth) {
    H2StrView src = H2MirFunctionSource(program, funcIndex, fallbackSrc);
    MirWIndent(w, depth);
    MirWU32(w, pc);
    MirWCStr(w, ": ");
    MirWCStr(w, H2MirOpName(ins->op));
    switch (ins->op) {
        case H2MirOp_PUSH_CONST:
            MirWCStr(w, " const=#");
            MirWU32(w, ins->aux);
            break;
        case H2MirOp_LOAD_IDENT:
        case H2MirOp_STORE_IDENT:
        case H2MirOp_CALL:
            MirWCStr(w, " symbol=#");
            MirWU32(w, ins->aux);
            if (ins->aux < program->symbolLen) {
                const H2MirSymbolRef* sym = &program->symbols[ins->aux];
                MirWCStr(w, " ");
                H2MirWriteSliceOrRange(w, src, sym->nameStart, sym->nameEnd, 1);
                if (ins->op == H2MirOp_CALL) {
                    MirWCStr(w, " argc=");
                    MirWU32(w, H2MirCallArgCountFromTok(ins->tok));
                    H2MirWriteCallFlags(w, ins->tok);
                }
            }
            break;
        case H2MirOp_CALL_FN:
            MirWCStr(w, " fn=#");
            MirWU32(w, ins->aux);
            MirWCStr(w, " argc=");
            MirWU32(w, H2MirCallArgCountFromTok(ins->tok));
            H2MirWriteCallFlags(w, ins->tok);
            break;
        case H2MirOp_CALL_HOST:
            MirWCStr(w, " host=#");
            MirWU32(w, ins->aux);
            MirWCStr(w, " argc=");
            MirWU32(w, H2MirCallArgCountFromTok(ins->tok));
            H2MirWriteCallFlags(w, ins->tok);
            break;
        case H2MirOp_CALL_INDIRECT:
            MirWCStr(w, " argc=");
            MirWU32(w, H2MirCallArgCountFromTok(ins->tok));
            H2MirWriteCallFlags(w, ins->tok);
            break;
        case H2MirOp_LOCAL_ZERO:
        case H2MirOp_LOCAL_LOAD:
        case H2MirOp_LOCAL_STORE:
        case H2MirOp_LOCAL_ADDR:
            MirWCStr(w, " local=#");
            MirWU32(w, ins->aux);
            if (ins->aux < fn->localCount) {
                const H2MirLocal* local = &program->locals[fn->localStart + ins->aux];
                MirWCStr(w, " ");
                H2MirWriteSliceOrRange(w, src, local->nameStart, local->nameEnd, 1);
            }
            break;
        case H2MirOp_JUMP:
        case H2MirOp_JUMP_IF_FALSE:
            MirWCStr(w, " target=");
            MirWU32(w, ins->aux);
            break;
        case H2MirOp_CAST:
            MirWCStr(w, " cast=");
            MirWCStr(w, H2MirCastTargetName((H2MirCastTarget)ins->tok));
            MirWCStr(w, " type=#");
            MirWU32(w, ins->aux);
            break;
        case H2MirOp_COERCE:
            MirWCStr(w, " type=#");
            MirWU32(w, ins->aux);
            break;
        case H2MirOp_UNARY:
        case H2MirOp_BINARY:
            MirWCStr(w, " op=");
            MirWCStr(w, H2TokenKindName((H2TokenKind)ins->tok));
            break;
        case H2MirOp_ITER_INIT:
            MirWCStr(w, " flags=");
            H2MirWriteIterFlags(w, ins->tok);
            MirWCStr(w, " aux=");
            MirWU32(w, ins->aux);
            break;
        case H2MirOp_ITER_NEXT:
            MirWCStr(w, " flags=");
            H2MirWriteIterFlags(w, ins->tok);
            break;
        case H2MirOp_AGG_GET:
        case H2MirOp_AGG_ADDR:
            MirWCStr(w, " field=#");
            MirWU32(w, ins->aux);
            if (ins->aux < program->fieldLen) {
                const H2MirField* field = &program->fields[ins->aux];
                MirWCStr(w, " ");
                H2MirWriteSliceOrRange(w, src, field->nameStart, field->nameEnd, 1);
            }
            break;
        case H2MirOp_AGG_MAKE:
        case H2MirOp_TUPLE_MAKE:
        case H2MirOp_TAGGED_MAKE:
        case H2MirOp_ARRAY_ZERO:
        case H2MirOp_OPTIONAL_WRAP:
            MirWCStr(w, " count=");
            MirWU32(w, ins->tok);
            break;
        case H2MirOp_AGG_ZERO:
        case H2MirOp_SLICE_MAKE:
        case H2MirOp_ARRAY_GET:
        case H2MirOp_ARRAY_SET:
        case H2MirOp_ARRAY_ADDR:
        case H2MirOp_INDEX:
        case H2MirOp_ASSERT:
        case H2MirOp_OPTIONAL_UNWRAP:
        case H2MirOp_TAGGED_TAG:
        case H2MirOp_TAGGED_PAYLOAD:
        case H2MirOp_ALLOC_NEW:
            if (ins->tok != 0) {
                MirWCStr(w, " tok=");
                MirWU32(w, ins->tok);
            }
            if (ins->aux != 0) {
                MirWCStr(w, " aux=");
                MirWU32(w, ins->aux);
            }
            break;
        case H2MirOp_CTX_GET:
        case H2MirOp_CTX_ADDR:
        case H2MirOp_CTX_SET:
            MirWCStr(w, " field=");
            MirWCStr(w, H2MirContextFieldName((H2MirContextField)ins->aux));
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
    H2MirWriteRange(w, ins->start, ins->end);
    MirWWrite(w, "\n", 1);
}

int H2MirDumpProgram(
    const H2MirProgram* program, H2StrView src, H2Writer* w, H2Diag* _Nullable diag) {
    uint32_t i;
    if (diag != NULL) {
        *diag = (H2Diag){ 0 };
    }
    if (program == NULL || w == NULL || w->write == NULL) {
        if (diag != NULL) {
            diag->code = H2Diag_UNEXPECTED_TOKEN;
            diag->type = H2DiagTypeOfCode(H2Diag_UNEXPECTED_TOKEN);
        }
        return -1;
    }
    MirWCStr(w, "mir\n");
    MirWCStr(w, "  dynamic_resolution=");
    MirWCStr(w, H2MirProgramNeedsDynamicResolution(program) ? "yes\n" : "no\n");
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
        const H2MirTypeRef* typeRef = &program->types[i];
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
        if (typeRef->aux != 0 && H2MirTypeRefIsFixedArray(typeRef)) {
            MirWCStr(w, " aux=");
            MirWU32(w, typeRef->aux);
        }
        MirWWrite(w, "\n", 1);
    }

    MirWCStr(w, "hosts\n");
    for (i = 0; i < program->hostLen; i++) {
        const H2MirHostRef* host = &program->hosts[i];
        MirWIndent(w, 1);
        MirWCStr(w, "#");
        MirWU32(w, i);
        MirWCStr(w, " ");
        MirWCStr(w, H2MirHostKindName(host->kind));
        MirWCStr(w, " target=");
        MirWCStr(w, H2MirHostTargetName((H2MirHostTarget)host->target));
        MirWCStr(w, " name=");
        H2MirWriteSliceOrRange(w, src, host->nameStart, host->nameEnd, 1);
        if (host->flags != 0) {
            MirWCStr(w, " flags=");
            MirWU32(w, host->flags);
        }
        MirWWrite(w, "\n", 1);
    }

    MirWCStr(w, "symbols\n");
    for (i = 0; i < program->symbolLen; i++) {
        const H2MirSymbolRef* sym = &program->symbols[i];
        MirWIndent(w, 1);
        MirWCStr(w, "#");
        MirWU32(w, i);
        MirWCStr(w, " ");
        MirWCStr(w, H2MirSymbolKindName(sym->kind));
        MirWCStr(w, " target=");
        MirWU32(w, sym->target);
        MirWCStr(w, " name=");
        H2MirWriteSliceOrRange(w, src, sym->nameStart, sym->nameEnd, 1);
        if (sym->flags != 0) {
            MirWCStr(w, " flags=");
            MirWU32(w, sym->flags);
        }
        MirWWrite(w, "\n", 1);
    }

    MirWCStr(w, "fields\n");
    for (i = 0; i < program->fieldLen; i++) {
        const H2MirField* field = &program->fields[i];
        H2StrView         fieldSrc =
            (program != NULL && field->sourceRef < program->sourceLen)
                ? program->sources[field->sourceRef].src
                : src;
        MirWIndent(w, 1);
        MirWCStr(w, "#");
        MirWU32(w, i);
        MirWCStr(w, " ownerType=#");
        MirWU32(w, field->ownerTypeRef);
        MirWCStr(w, " type=#");
        MirWU32(w, field->typeRef);
        MirWCStr(w, " name=");
        H2MirWriteSliceOrRange(w, fieldSrc, field->nameStart, field->nameEnd, 1);
        MirWWrite(w, "\n", 1);
    }

    MirWCStr(w, "consts\n");
    for (i = 0; i < program->constLen; i++) {
        H2MirDumpConst(&program->consts[i], i, w, 1);
    }

    MirWCStr(w, "functions\n");
    for (i = 0; i < program->funcLen; i++) {
        uint32_t             j;
        const H2MirFunction* fn = &program->funcs[i];
        H2StrView            fnSrc = H2MirFunctionSource(program, i, src);
        MirWIndent(w, 1);
        MirWCStr(w, "#");
        MirWU32(w, i);
        MirWCStr(w, " name=");
        H2MirWriteSliceOrRange(w, fnSrc, fn->nameStart, fn->nameEnd, 1);
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
            const H2MirLocal* local = &program->locals[fn->localStart + j];
            MirWIndent(w, 3);
            MirWCStr(w, "#");
            MirWU32(w, j);
            MirWCStr(w, " name=");
            H2MirWriteSliceOrRange(w, fnSrc, local->nameStart, local->nameEnd, 1);
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
            H2MirDumpInst(program, fn, i, j, &program->insts[fn->instStart + j], src, w, 3);
        }
    }
    return 0;
}

H2_API_END
