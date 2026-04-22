#include "libhop-impl.h"
#include "mir.h"

HOP_API_BEGIN

static void MirWWrite(HOPWriter* w, const char* s, uint32_t len) {
    w->write(w->ctx, s, len);
}

static void MirWCStr(HOPWriter* w, const char* s) {
    uint32_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    MirWWrite(w, s, n);
}

static void MirWU32(HOPWriter* w, uint32_t v) {
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

static void MirWU64(HOPWriter* w, uint64_t v) {
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

static void MirWI64(HOPWriter* w, int64_t v) {
    uint64_t mag;
    if (v < 0) {
        MirWWrite(w, "-", 1);
        mag = (uint64_t)(-(v + 1)) + 1u;
    } else {
        mag = (uint64_t)v;
    }
    MirWU64(w, mag);
}

static void MirWHexU64(HOPWriter* w, uint64_t v) {
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

static void MirWIndent(HOPWriter* w, uint32_t depth) {
    uint32_t i;
    for (i = 0; i < depth; i++) {
        MirWWrite(w, "  ", 2);
    }
}

static void MirWEscapedView(HOPWriter* w, HOPStrView src, uint32_t start, uint32_t end) {
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

static void MirWEscapedBytes(HOPWriter* w, HOPStrView bytes) {
    MirWEscapedView(w, bytes, 0, bytes.len);
}

static const char* HOPMirOpName(HOPMirOp op) {
    switch (op) {
        case HOPMirOp_INVALID:         return "INVALID";
        case HOPMirOp_PUSH_CONST:      return "PUSH_CONST";
        case HOPMirOp_PUSH_INT:        return "PUSH_INT";
        case HOPMirOp_PUSH_FLOAT:      return "PUSH_FLOAT";
        case HOPMirOp_PUSH_BOOL:       return "PUSH_BOOL";
        case HOPMirOp_PUSH_STRING:     return "PUSH_STRING";
        case HOPMirOp_PUSH_NULL:       return "PUSH_NULL";
        case HOPMirOp_LOAD_IDENT:      return "LOAD_IDENT";
        case HOPMirOp_STORE_IDENT:     return "STORE_IDENT";
        case HOPMirOp_CALL:            return "CALL";
        case HOPMirOp_UNARY:           return "UNARY";
        case HOPMirOp_BINARY:          return "BINARY";
        case HOPMirOp_INDEX:           return "INDEX";
        case HOPMirOp_SEQ_LEN:         return "SEQ_LEN";
        case HOPMirOp_STR_CSTR:        return "STR_CSTR";
        case HOPMirOp_ITER_INIT:       return "ITER_INIT";
        case HOPMirOp_ITER_NEXT:       return "ITER_NEXT";
        case HOPMirOp_CAST:            return "CAST";
        case HOPMirOp_COERCE:          return "COERCE";
        case HOPMirOp_LOCAL_ZERO:      return "LOCAL_ZERO";
        case HOPMirOp_LOCAL_LOAD:      return "LOCAL_LOAD";
        case HOPMirOp_LOCAL_STORE:     return "LOCAL_STORE";
        case HOPMirOp_LOCAL_ADDR:      return "LOCAL_ADDR";
        case HOPMirOp_DROP:            return "DROP";
        case HOPMirOp_JUMP:            return "JUMP";
        case HOPMirOp_JUMP_IF_FALSE:   return "JUMP_IF_FALSE";
        case HOPMirOp_ASSERT:          return "ASSERT";
        case HOPMirOp_CALL_FN:         return "CALL_FN";
        case HOPMirOp_CALL_HOST:       return "CALL_HOST";
        case HOPMirOp_CALL_INDIRECT:   return "CALL_INDIRECT";
        case HOPMirOp_DEREF_LOAD:      return "DEREF_LOAD";
        case HOPMirOp_DEREF_STORE:     return "DEREF_STORE";
        case HOPMirOp_ADDR_OF:         return "ADDR_OF";
        case HOPMirOp_AGG_MAKE:        return "AGG_MAKE";
        case HOPMirOp_AGG_ZERO:        return "AGG_ZERO";
        case HOPMirOp_AGG_GET:         return "AGG_GET";
        case HOPMirOp_AGG_SET:         return "AGG_SET";
        case HOPMirOp_AGG_ADDR:        return "AGG_ADDR";
        case HOPMirOp_ARRAY_ZERO:      return "ARRAY_ZERO";
        case HOPMirOp_ARRAY_GET:       return "ARRAY_GET";
        case HOPMirOp_ARRAY_SET:       return "ARRAY_SET";
        case HOPMirOp_ARRAY_ADDR:      return "ARRAY_ADDR";
        case HOPMirOp_TUPLE_MAKE:      return "TUPLE_MAKE";
        case HOPMirOp_SLICE_MAKE:      return "SLICE_MAKE";
        case HOPMirOp_OPTIONAL_WRAP:   return "OPTIONAL_WRAP";
        case HOPMirOp_OPTIONAL_UNWRAP: return "OPTIONAL_UNWRAP";
        case HOPMirOp_TAGGED_MAKE:     return "TAGGED_MAKE";
        case HOPMirOp_TAGGED_TAG:      return "TAGGED_TAG";
        case HOPMirOp_TAGGED_PAYLOAD:  return "TAGGED_PAYLOAD";
        case HOPMirOp_ALLOC_NEW:       return "ALLOC_NEW";
        case HOPMirOp_CTX_GET:         return "CTX_GET";
        case HOPMirOp_CTX_ADDR:        return "CTX_ADDR";
        case HOPMirOp_CTX_SET:         return "CTX_SET";
        case HOPMirOp_RETURN:          return "RETURN";
        case HOPMirOp_RETURN_VOID:     return "RETURN_VOID";
    }
    return "UNKNOWN";
}

static const char* HOPMirConstKindName(HOPMirConstKind kind) {
    switch (kind) {
        case HOPMirConst_INVALID:  return "INVALID";
        case HOPMirConst_INT:      return "INT";
        case HOPMirConst_FLOAT:    return "FLOAT";
        case HOPMirConst_BOOL:     return "BOOL";
        case HOPMirConst_STRING:   return "STRING";
        case HOPMirConst_NULL:     return "NULL";
        case HOPMirConst_TYPE:     return "TYPE";
        case HOPMirConst_FUNCTION: return "FUNCTION";
        case HOPMirConst_HOST:     return "HOST";
    }
    return "UNKNOWN";
}

static const char* HOPMirHostKindName(HOPMirHostKind kind) {
    switch (kind) {
        case HOPMirHost_INVALID: return "INVALID";
        case HOPMirHost_GENERIC: return "GENERIC";
    }
    return "UNKNOWN";
}

static const char* HOPMirHostTargetName(HOPMirHostTarget target) {
    switch (target) {
        case HOPMirHostTarget_INVALID:              return "INVALID";
        case HOPMirHostTarget_PRINT:                return "PRINT";
        case HOPMirHostTarget_PLATFORM_EXIT:        return "PLATFORM_EXIT";
        case HOPMirHostTarget_FREE:                 return "FREE";
        case HOPMirHostTarget_CONCAT:               return "CONCAT";
        case HOPMirHostTarget_COPY:                 return "COPY";
        case HOPMirHostTarget_PLATFORM_CONSOLE_LOG: return "PLATFORM_CONSOLE_LOG";
    }
    return "UNKNOWN";
}

static const char* HOPMirSymbolKindName(HOPMirSymbolKind kind) {
    switch (kind) {
        case HOPMirSymbol_INVALID: return "INVALID";
        case HOPMirSymbol_IDENT:   return "IDENT";
        case HOPMirSymbol_CALL:    return "CALL";
        case HOPMirSymbol_HOST:    return "HOST";
    }
    return "UNKNOWN";
}

static const char* HOPMirCastTargetName(HOPMirCastTarget target) {
    switch (target) {
        case HOPMirCastTarget_INVALID:  return "INVALID";
        case HOPMirCastTarget_INT:      return "INT";
        case HOPMirCastTarget_FLOAT:    return "FLOAT";
        case HOPMirCastTarget_BOOL:     return "BOOL";
        case HOPMirCastTarget_STR_VIEW: return "STR_VIEW";
        case HOPMirCastTarget_PTR_LIKE: return "PTR_LIKE";
    }
    return "UNKNOWN";
}

static const char* HOPMirContextFieldName(HOPMirContextField field) {
    switch (field) {
        case HOPMirContextField_INVALID:        return "INVALID";
        case HOPMirContextField_ALLOCATOR:      return "ALLOCATOR";
        case HOPMirContextField_TEMP_ALLOCATOR: return "TEMP_ALLOCATOR";
        case HOPMirContextField_LOGGER:         return "LOGGER";
    }
    return "UNKNOWN";
}

static void HOPMirWriteRange(HOPWriter* w, uint32_t start, uint32_t end) {
    MirWWrite(w, "[", 1);
    MirWU32(w, start);
    MirWWrite(w, ",", 1);
    MirWU32(w, end);
    MirWWrite(w, "]", 1);
}

static int HOPMirRangeInSrc(HOPStrView src, uint32_t start, uint32_t end) {
    return src.ptr != NULL && end >= start && end <= src.len;
}

static void HOPMirWriteSliceOrRange(
    HOPWriter* w, HOPStrView src, uint32_t start, uint32_t end, int writeRange) {
    if (HOPMirRangeInSrc(src, start, end)) {
        MirWEscapedView(w, src, start, end);
    } else {
        MirWCStr(w, "<slice ");
        HOPMirWriteRange(w, start, end);
        MirWWrite(w, ">", 1);
    }
    if (writeRange) {
        MirWWrite(w, " ", 1);
        HOPMirWriteRange(w, start, end);
    }
}

static void HOPMirWriteCallFlags(HOPWriter* w, uint16_t tok) {
    int wrote = 0;
    if (HOPMirCallTokDropsReceiverArg0(tok)) {
        MirWCStr(w, " receiver_arg0");
        wrote = 1;
    }
    if (HOPMirCallTokHasSpreadLast(tok)) {
        MirWCStr(w, wrote ? "|spread_last" : " spread_last");
    }
}

static void HOPMirWriteIterFlags(HOPWriter* w, uint16_t tok) {
    int wrote = 0;
    if ((tok & HOPMirIterFlag_HAS_KEY) != 0u) {
        MirWCStr(w, wrote ? "|HAS_KEY" : " HAS_KEY");
        wrote = 1;
    }
    if ((tok & HOPMirIterFlag_KEY_REF) != 0u) {
        MirWCStr(w, wrote ? "|KEY_REF" : " KEY_REF");
        wrote = 1;
    }
    if ((tok & HOPMirIterFlag_VALUE_REF) != 0u) {
        MirWCStr(w, wrote ? "|VALUE_REF" : " VALUE_REF");
        wrote = 1;
    }
    if ((tok & HOPMirIterFlag_VALUE_DISCARD) != 0u) {
        MirWCStr(w, wrote ? "|VALUE_DISCARD" : " VALUE_DISCARD");
    }
}

static HOPStrView HOPMirFunctionSource(
    const HOPMirProgram* program, uint32_t funcIndex, HOPStrView fallbackSrc) {
    if (program != NULL && funcIndex < program->funcLen) {
        const HOPMirFunction* fn = &program->funcs[funcIndex];
        if (fn->sourceRef < program->sourceLen) {
            return program->sources[fn->sourceRef].src;
        }
    }
    return fallbackSrc;
}

static void HOPMirDumpConst(
    const HOPMirConst* value, uint32_t index, HOPWriter* w, uint32_t depth) {
    MirWIndent(w, depth);
    MirWCStr(w, "#");
    MirWU32(w, index);
    MirWCStr(w, " ");
    MirWCStr(w, HOPMirConstKindName(value->kind));
    switch (value->kind) {
        case HOPMirConst_INT:
            MirWCStr(w, " value=");
            MirWI64(w, (int64_t)value->bits);
            break;
        case HOPMirConst_FLOAT:
            MirWCStr(w, " bits=");
            MirWHexU64(w, value->bits);
            break;
        case HOPMirConst_BOOL: MirWCStr(w, value->bits != 0 ? " true" : " false"); break;
        case HOPMirConst_STRING:
            MirWCStr(w, " bytes=");
            MirWEscapedBytes(w, value->bytes);
            break;
        case HOPMirConst_TYPE:
        case HOPMirConst_FUNCTION:
        case HOPMirConst_HOST:
            MirWCStr(w, " aux=");
            MirWU32(w, value->aux);
            break;
        default: break;
    }
    MirWWrite(w, "\n", 1);
}

static void HOPMirDumpInst(
    const HOPMirProgram*  program,
    const HOPMirFunction* fn,
    uint32_t              funcIndex,
    uint32_t              pc,
    const HOPMirInst*     ins,
    HOPStrView            fallbackSrc,
    HOPWriter*            w,
    uint32_t              depth) {
    HOPStrView src = HOPMirFunctionSource(program, funcIndex, fallbackSrc);
    MirWIndent(w, depth);
    MirWU32(w, pc);
    MirWCStr(w, ": ");
    MirWCStr(w, HOPMirOpName(ins->op));
    switch (ins->op) {
        case HOPMirOp_PUSH_CONST:
            MirWCStr(w, " const=#");
            MirWU32(w, ins->aux);
            break;
        case HOPMirOp_LOAD_IDENT:
        case HOPMirOp_STORE_IDENT:
        case HOPMirOp_CALL:
            MirWCStr(w, " symbol=#");
            MirWU32(w, ins->aux);
            if (ins->aux < program->symbolLen) {
                const HOPMirSymbolRef* sym = &program->symbols[ins->aux];
                MirWCStr(w, " ");
                HOPMirWriteSliceOrRange(w, src, sym->nameStart, sym->nameEnd, 1);
                if (ins->op == HOPMirOp_CALL) {
                    MirWCStr(w, " argc=");
                    MirWU32(w, HOPMirCallArgCountFromTok(ins->tok));
                    HOPMirWriteCallFlags(w, ins->tok);
                }
            }
            break;
        case HOPMirOp_CALL_FN:
            MirWCStr(w, " fn=#");
            MirWU32(w, ins->aux);
            MirWCStr(w, " argc=");
            MirWU32(w, HOPMirCallArgCountFromTok(ins->tok));
            HOPMirWriteCallFlags(w, ins->tok);
            break;
        case HOPMirOp_CALL_HOST:
            MirWCStr(w, " host=#");
            MirWU32(w, ins->aux);
            MirWCStr(w, " argc=");
            MirWU32(w, HOPMirCallArgCountFromTok(ins->tok));
            HOPMirWriteCallFlags(w, ins->tok);
            break;
        case HOPMirOp_CALL_INDIRECT:
            MirWCStr(w, " argc=");
            MirWU32(w, HOPMirCallArgCountFromTok(ins->tok));
            HOPMirWriteCallFlags(w, ins->tok);
            break;
        case HOPMirOp_LOCAL_ZERO:
        case HOPMirOp_LOCAL_LOAD:
        case HOPMirOp_LOCAL_STORE:
        case HOPMirOp_LOCAL_ADDR:
            MirWCStr(w, " local=#");
            MirWU32(w, ins->aux);
            if (ins->aux < fn->localCount) {
                const HOPMirLocal* local = &program->locals[fn->localStart + ins->aux];
                MirWCStr(w, " ");
                HOPMirWriteSliceOrRange(w, src, local->nameStart, local->nameEnd, 1);
            }
            break;
        case HOPMirOp_JUMP:
        case HOPMirOp_JUMP_IF_FALSE:
            MirWCStr(w, " target=");
            MirWU32(w, ins->aux);
            break;
        case HOPMirOp_CAST:
            MirWCStr(w, " cast=");
            MirWCStr(w, HOPMirCastTargetName((HOPMirCastTarget)ins->tok));
            MirWCStr(w, " type=#");
            MirWU32(w, ins->aux);
            break;
        case HOPMirOp_COERCE:
            MirWCStr(w, " type=#");
            MirWU32(w, ins->aux);
            break;
        case HOPMirOp_UNARY:
        case HOPMirOp_BINARY:
            MirWCStr(w, " op=");
            MirWCStr(w, HOPTokenKindName((HOPTokenKind)ins->tok));
            break;
        case HOPMirOp_ITER_INIT:
            MirWCStr(w, " flags=");
            HOPMirWriteIterFlags(w, ins->tok);
            MirWCStr(w, " aux=");
            MirWU32(w, ins->aux);
            break;
        case HOPMirOp_ITER_NEXT:
            MirWCStr(w, " flags=");
            HOPMirWriteIterFlags(w, ins->tok);
            break;
        case HOPMirOp_AGG_GET:
        case HOPMirOp_AGG_ADDR:
            MirWCStr(w, " field=#");
            MirWU32(w, ins->aux);
            if (ins->aux < program->fieldLen) {
                const HOPMirField* field = &program->fields[ins->aux];
                MirWCStr(w, " ");
                HOPMirWriteSliceOrRange(w, src, field->nameStart, field->nameEnd, 1);
            }
            break;
        case HOPMirOp_AGG_MAKE:
        case HOPMirOp_TUPLE_MAKE:
        case HOPMirOp_TAGGED_MAKE:
        case HOPMirOp_ARRAY_ZERO:
        case HOPMirOp_OPTIONAL_WRAP:
            MirWCStr(w, " count=");
            MirWU32(w, ins->tok);
            break;
        case HOPMirOp_AGG_ZERO:
        case HOPMirOp_SLICE_MAKE:
        case HOPMirOp_ARRAY_GET:
        case HOPMirOp_ARRAY_SET:
        case HOPMirOp_ARRAY_ADDR:
        case HOPMirOp_INDEX:
        case HOPMirOp_ASSERT:
        case HOPMirOp_OPTIONAL_UNWRAP:
        case HOPMirOp_TAGGED_TAG:
        case HOPMirOp_TAGGED_PAYLOAD:
        case HOPMirOp_ALLOC_NEW:
            if (ins->tok != 0) {
                MirWCStr(w, " tok=");
                MirWU32(w, ins->tok);
            }
            if (ins->aux != 0) {
                MirWCStr(w, " aux=");
                MirWU32(w, ins->aux);
            }
            break;
        case HOPMirOp_CTX_GET:
        case HOPMirOp_CTX_ADDR:
        case HOPMirOp_CTX_SET:
            MirWCStr(w, " field=");
            MirWCStr(w, HOPMirContextFieldName((HOPMirContextField)ins->aux));
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
    HOPMirWriteRange(w, ins->start, ins->end);
    MirWWrite(w, "\n", 1);
}

int HOPMirDumpProgram(
    const HOPMirProgram* program, HOPStrView src, HOPWriter* w, HOPDiag* _Nullable diag) {
    uint32_t i;
    if (diag != NULL) {
        *diag = (HOPDiag){ 0 };
    }
    if (program == NULL || w == NULL || w->write == NULL) {
        if (diag != NULL) {
            diag->code = HOPDiag_UNEXPECTED_TOKEN;
            diag->type = HOPDiagTypeOfCode(HOPDiag_UNEXPECTED_TOKEN);
        }
        return -1;
    }
    MirWCStr(w, "mir\n");
    MirWCStr(w, "  dynamic_resolution=");
    MirWCStr(w, HOPMirProgramNeedsDynamicResolution(program) ? "yes\n" : "no\n");
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
        const HOPMirTypeRef* typeRef = &program->types[i];
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
        if (typeRef->aux != 0 && HOPMirTypeRefIsFixedArray(typeRef)) {
            MirWCStr(w, " aux=");
            MirWU32(w, typeRef->aux);
        }
        MirWWrite(w, "\n", 1);
    }

    MirWCStr(w, "hosts\n");
    for (i = 0; i < program->hostLen; i++) {
        const HOPMirHostRef* host = &program->hosts[i];
        MirWIndent(w, 1);
        MirWCStr(w, "#");
        MirWU32(w, i);
        MirWCStr(w, " ");
        MirWCStr(w, HOPMirHostKindName(host->kind));
        MirWCStr(w, " target=");
        MirWCStr(w, HOPMirHostTargetName((HOPMirHostTarget)host->target));
        MirWCStr(w, " name=");
        HOPMirWriteSliceOrRange(w, src, host->nameStart, host->nameEnd, 1);
        if (host->flags != 0) {
            MirWCStr(w, " flags=");
            MirWU32(w, host->flags);
        }
        MirWWrite(w, "\n", 1);
    }

    MirWCStr(w, "symbols\n");
    for (i = 0; i < program->symbolLen; i++) {
        const HOPMirSymbolRef* sym = &program->symbols[i];
        MirWIndent(w, 1);
        MirWCStr(w, "#");
        MirWU32(w, i);
        MirWCStr(w, " ");
        MirWCStr(w, HOPMirSymbolKindName(sym->kind));
        MirWCStr(w, " target=");
        MirWU32(w, sym->target);
        MirWCStr(w, " name=");
        HOPMirWriteSliceOrRange(w, src, sym->nameStart, sym->nameEnd, 1);
        if (sym->flags != 0) {
            MirWCStr(w, " flags=");
            MirWU32(w, sym->flags);
        }
        MirWWrite(w, "\n", 1);
    }

    MirWCStr(w, "fields\n");
    for (i = 0; i < program->fieldLen; i++) {
        const HOPMirField* field = &program->fields[i];
        MirWIndent(w, 1);
        MirWCStr(w, "#");
        MirWU32(w, i);
        MirWCStr(w, " ownerType=#");
        MirWU32(w, field->ownerTypeRef);
        MirWCStr(w, " type=#");
        MirWU32(w, field->typeRef);
        MirWCStr(w, " name=");
        HOPMirWriteSliceOrRange(w, src, field->nameStart, field->nameEnd, 1);
        MirWWrite(w, "\n", 1);
    }

    MirWCStr(w, "consts\n");
    for (i = 0; i < program->constLen; i++) {
        HOPMirDumpConst(&program->consts[i], i, w, 1);
    }

    MirWCStr(w, "functions\n");
    for (i = 0; i < program->funcLen; i++) {
        uint32_t              j;
        const HOPMirFunction* fn = &program->funcs[i];
        HOPStrView            fnSrc = HOPMirFunctionSource(program, i, src);
        MirWIndent(w, 1);
        MirWCStr(w, "#");
        MirWU32(w, i);
        MirWCStr(w, " name=");
        HOPMirWriteSliceOrRange(w, fnSrc, fn->nameStart, fn->nameEnd, 1);
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
            const HOPMirLocal* local = &program->locals[fn->localStart + j];
            MirWIndent(w, 3);
            MirWCStr(w, "#");
            MirWU32(w, j);
            MirWCStr(w, " name=");
            HOPMirWriteSliceOrRange(w, fnSrc, local->nameStart, local->nameEnd, 1);
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
            HOPMirDumpInst(program, fn, i, j, &program->insts[fn->instStart + j], src, w, 3);
        }
    }
    return 0;
}

HOP_API_END
