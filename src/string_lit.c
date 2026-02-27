#include "libsl-impl.h"

#if SL_LIBC
    #include <stdlib.h>
#endif

typedef int (*SLStringLitEmitByteFn)(void* _Nullable ctx, uint8_t b);

typedef struct {
    uint8_t  firstByte;
    uint8_t  need;
    uint8_t  have;
    uint32_t firstStart;
    uint32_t firstEnd;
} SLUTF8State;

typedef struct {
    SLArena* _Nonnull arena;
    uint8_t* _Nullable v;
    uint32_t len;
    uint32_t cap;
} SLArenaByteBuf;

#if SL_LIBC
typedef struct {
    uint8_t* _Nullable v;
    uint32_t len;
    uint32_t cap;
} SLMallocByteBuf;
#endif

static void SLSetStringLitErr(
    SLStringLitErr* _Nullable err, SLStringLitErrKind kind, uint32_t start, uint32_t end) {
    if (err == NULL) {
        return;
    }
    err->kind = kind;
    err->start = start;
    err->end = end;
}

static int SLArenaByteBufEnsure(SLArenaByteBuf* _Nonnull b, uint32_t need) {
    uint32_t newCap;
    uint8_t* p;
    if (need <= b->cap) {
        return 0;
    }
    newCap = b->cap == 0 ? 16u : b->cap;
    while (newCap < need) {
        if (newCap > UINT32_MAX / 2u) {
            newCap = need;
            break;
        }
        newCap *= 2u;
    }
    p = (uint8_t*)SLArenaAlloc(b->arena, newCap, (uint32_t)_Alignof(uint8_t));
    if (p == NULL) {
        return -1;
    }
    if (b->v != NULL && b->len > 0) {
        memcpy(p, b->v, b->len);
    }
    b->v = p;
    b->cap = newCap;
    return 0;
}

static int SLArenaByteEmit(void* _Nullable ctx, uint8_t b) {
    SLArenaByteBuf* buf = (SLArenaByteBuf*)ctx;
    if (buf == NULL) {
        return -1;
    }
    if (SLArenaByteBufEnsure(buf, buf->len + 1u) != 0) {
        return -1;
    }
    buf->v[buf->len++] = b;
    return 0;
}

#if SL_LIBC
static int SLMallocByteBufEnsure(SLMallocByteBuf* _Nonnull b, uint32_t need) {
    uint32_t newCap;
    void*    p;
    if (need <= b->cap) {
        return 0;
    }
    newCap = b->cap == 0 ? 16u : b->cap;
    while (newCap < need) {
        if (newCap > UINT32_MAX / 2u) {
            newCap = need;
            break;
        }
        newCap *= 2u;
    }
    p = realloc(b->v, (size_t)newCap);
    if (p == NULL) {
        return -1;
    }
    b->v = (uint8_t*)p;
    b->cap = newCap;
    return 0;
}

static int SLMallocByteEmit(void* _Nullable ctx, uint8_t b) {
    SLMallocByteBuf* buf = (SLMallocByteBuf*)ctx;
    if (buf == NULL) {
        return -1;
    }
    if (SLMallocByteBufEnsure(buf, buf->len + 1u) != 0) {
        return -1;
    }
    buf->v[buf->len++] = b;
    return 0;
}
#endif

static int SLHexDigit(unsigned char c) {
    if (c >= (unsigned char)'0' && c <= (unsigned char)'9') {
        return (int)(c - (unsigned char)'0');
    }
    if (c >= (unsigned char)'a' && c <= (unsigned char)'f') {
        return 10 + (int)(c - (unsigned char)'a');
    }
    if (c >= (unsigned char)'A' && c <= (unsigned char)'F') {
        return 10 + (int)(c - (unsigned char)'A');
    }
    return -1;
}

static int SLOctalDigit(unsigned char c) {
    return c >= (unsigned char)'0' && c <= (unsigned char)'7';
}

static int SLUTF8SecondByteValid(uint8_t first, uint8_t second) {
    if (first >= 0xC2u && first <= 0xDFu) {
        return second >= 0x80u && second <= 0xBFu;
    }
    if (first == 0xE0u) {
        return second >= 0xA0u && second <= 0xBFu;
    }
    if ((first >= 0xE1u && first <= 0xECu) || (first >= 0xEEu && first <= 0xEFu)) {
        return second >= 0x80u && second <= 0xBFu;
    }
    if (first == 0xEDu) {
        return second >= 0x80u && second <= 0x9Fu;
    }
    if (first == 0xF0u) {
        return second >= 0x90u && second <= 0xBFu;
    }
    if (first >= 0xF1u && first <= 0xF3u) {
        return second >= 0x80u && second <= 0xBFu;
    }
    if (first == 0xF4u) {
        return second >= 0x80u && second <= 0x8Fu;
    }
    return 0;
}

static int SLUTF8Feed(
    SLUTF8State* _Nonnull st,
    uint8_t  b,
    uint32_t start,
    uint32_t end,
    SLStringLitErr* _Nullable err) {
    if (st->need == 0u) {
        if (b <= 0x7Fu) {
            return 0;
        }
        if (b >= 0xC2u && b <= 0xDFu) {
            st->firstByte = b;
            st->need = 2u;
            st->have = 1u;
            st->firstStart = start;
            st->firstEnd = end;
            return 0;
        }
        if (b >= 0xE0u && b <= 0xEFu) {
            st->firstByte = b;
            st->need = 3u;
            st->have = 1u;
            st->firstStart = start;
            st->firstEnd = end;
            return 0;
        }
        if (b >= 0xF0u && b <= 0xF4u) {
            st->firstByte = b;
            st->need = 4u;
            st->have = 1u;
            st->firstStart = start;
            st->firstEnd = end;
            return 0;
        }
        SLSetStringLitErr(err, SLStringLitErr_INVALID_UTF8, start, end);
        return -1;
    }

    if (st->have == 1u) {
        if (!SLUTF8SecondByteValid(st->firstByte, b)) {
            SLSetStringLitErr(err, SLStringLitErr_INVALID_UTF8, start, end);
            return -1;
        }
    } else if (b < 0x80u || b > 0xBFu) {
        SLSetStringLitErr(err, SLStringLitErr_INVALID_UTF8, start, end);
        return -1;
    }

    st->have++;
    if (st->have == st->need) {
        st->need = 0u;
        st->have = 0u;
    }
    return 0;
}

static int SLUTF8Finish(const SLUTF8State* _Nonnull st, SLStringLitErr* _Nullable err) {
    if (st->need == 0u) {
        return 0;
    }
    SLSetStringLitErr(err, SLStringLitErr_INVALID_UTF8, st->firstStart, st->firstEnd);
    return -1;
}

static int SLRuneToUTF8(uint32_t rune, uint8_t* _Nonnull out, uint32_t* _Nonnull outLen) {
    if (rune <= 0x7Fu) {
        out[0] = (uint8_t)rune;
        *outLen = 1u;
        return 0;
    }
    if (rune <= 0x7FFu) {
        out[0] = (uint8_t)(0xC0u | ((rune >> 6u) & 0x1Fu));
        out[1] = (uint8_t)(0x80u | (rune & 0x3Fu));
        *outLen = 2u;
        return 0;
    }
    if (rune <= 0xFFFFu) {
        out[0] = (uint8_t)(0xE0u | ((rune >> 12u) & 0x0Fu));
        out[1] = (uint8_t)(0x80u | ((rune >> 6u) & 0x3Fu));
        out[2] = (uint8_t)(0x80u | (rune & 0x3Fu));
        *outLen = 3u;
        return 0;
    }
    if (rune <= 0x10FFFFu) {
        out[0] = (uint8_t)(0xF0u | ((rune >> 18u) & 0x07u));
        out[1] = (uint8_t)(0x80u | ((rune >> 12u) & 0x3Fu));
        out[2] = (uint8_t)(0x80u | ((rune >> 6u) & 0x3Fu));
        out[3] = (uint8_t)(0x80u | (rune & 0x3Fu));
        *outLen = 4u;
        return 0;
    }
    return -1;
}

static int SLEmitAndValidateByte(
    SLStringLitEmitByteFn _Nullable emit,
    void* _Nullable emitCtx,
    SLUTF8State* _Nonnull utf8,
    uint8_t  b,
    uint32_t start,
    uint32_t end,
    SLStringLitErr* _Nullable err) {
    if (SLUTF8Feed(utf8, b, start, end, err) != 0) {
        return -1;
    }
    if (emit != NULL && emit(emitCtx, b) != 0) {
        SLSetStringLitErr(err, SLStringLitErr_ARENA_OOM, start, end);
        return -1;
    }
    return 0;
}

static int SLEmitRuneUTF8(
    SLStringLitEmitByteFn _Nullable emit,
    void* _Nullable emitCtx,
    SLUTF8State* _Nonnull utf8,
    uint32_t rune,
    uint32_t start,
    uint32_t end,
    SLStringLitErr* _Nullable err) {
    uint8_t  bytes[4];
    uint32_t n = 0;
    uint32_t i;
    if (SLRuneToUTF8(rune, bytes, &n) != 0) {
        SLSetStringLitErr(err, SLStringLitErr_INVALID_CODEPOINT, start, end);
        return -1;
    }
    for (i = 0; i < n; i++) {
        if (SLEmitAndValidateByte(emit, emitCtx, utf8, bytes[i], start, end, err) != 0) {
            return -1;
        }
    }
    return 0;
}

static int SLDecodeHex(
    const char* _Nonnull src,
    uint32_t start,
    uint32_t digits,
    uint32_t* _Nonnull outValue,
    SLStringLitErr* _Nonnull err) {
    uint32_t i;
    uint32_t v = 0u;
    for (i = 0; i < digits; i++) {
        int d = SLHexDigit((unsigned char)src[start + i]);
        if (d < 0) {
            SLSetStringLitErr(err, SLStringLitErr_INVALID_ESCAPE, start, start + digits);
            return -1;
        }
        v = (v << 4u) | (uint32_t)d;
    }
    *outValue = v;
    return 0;
}

static int SLDecodeStringLiteralImpl(
    const char* _Nonnull src,
    uint32_t start,
    uint32_t end,
    SLStringLitEmitByteFn _Nullable emit,
    void* _Nullable emitCtx,
    SLStringLitErr* _Nullable outErr) {
    uint32_t       i;
    SLUTF8State    utf8 = { 0 };
    unsigned char  delim;
    SLStringLitErr err = {
        .kind = SLStringLitErr_NONE,
        .start = start,
        .end = end,
    };

    if (src == NULL || end <= start + 1u) {
        SLSetStringLitErr(&err, SLStringLitErr_UNTERMINATED, start, end);
        goto fail;
    }
    delim = (unsigned char)src[start];
    if ((delim != (unsigned char)'"' && delim != (unsigned char)'`' && delim != (unsigned char)'\'')
        || (unsigned char)src[end - 1u] != delim)
    {
        SLSetStringLitErr(&err, SLStringLitErr_UNTERMINATED, start, end);
        goto fail;
    }

    i = start + 1u;
    while (i < end - 1u) {
        unsigned char c = (unsigned char)src[i];
        uint32_t      spanStart = i;
        uint32_t      spanEnd = i + 1u;

        if (delim == (unsigned char)'`') {
            if (c == (unsigned char)'\\' && i + 1u < end - 1u
                && (unsigned char)src[i + 1u] == (unsigned char)'`')
            {
                c = (unsigned char)'`';
                spanEnd = i + 2u;
                i += 2u;
            } else if (c == (unsigned char)'\r') {
                c = (unsigned char)'\n';
                i++;
                if (i < end - 1u && (unsigned char)src[i] == (unsigned char)'\n') {
                    i++;
                }
                spanEnd = i;
            } else {
                i++;
            }
            if (SLEmitAndValidateByte(emit, emitCtx, &utf8, (uint8_t)c, spanStart, spanEnd, &err)
                != 0)
            {
                goto fail;
            }
            continue;
        }

        if (c == (unsigned char)'\\') {
            uint32_t escapeStart = i;
            i++;
            if (i >= end - 1u) {
                SLSetStringLitErr(&err, SLStringLitErr_INVALID_ESCAPE, escapeStart, i);
                goto fail;
            }
            c = (unsigned char)src[i];
            if (c == (unsigned char)'\n') {
                i++;
                continue;
            }
            if (c == (unsigned char)'\r') {
                i++;
                if (i < end - 1u && (unsigned char)src[i] == (unsigned char)'\n') {
                    i++;
                }
                continue;
            }
            switch (c) {
                case (unsigned char)'a':
                    c = 0x07u;
                    i++;
                    break;
                case (unsigned char)'b':
                    c = 0x08u;
                    i++;
                    break;
                case (unsigned char)'f':
                    c = 0x0Cu;
                    i++;
                    break;
                case (unsigned char)'n':
                    c = (unsigned char)'\n';
                    i++;
                    break;
                case (unsigned char)'r':
                    c = (unsigned char)'\r';
                    i++;
                    break;
                case (unsigned char)'t':
                    c = (unsigned char)'\t';
                    i++;
                    break;
                case (unsigned char)'v':
                    c = 0x0Bu;
                    i++;
                    break;
                case (unsigned char)'\\':
                case (unsigned char)'"':
                case (unsigned char)'\'': i++; break;
                case (unsigned char)'x':  {
                    int hi;
                    int lo;
                    if (i + 2u >= end - 1u) {
                        SLSetStringLitErr(&err, SLStringLitErr_INVALID_ESCAPE, escapeStart, end);
                        goto fail;
                    }
                    hi = SLHexDigit((unsigned char)src[i + 1u]);
                    lo = SLHexDigit((unsigned char)src[i + 2u]);
                    if (hi < 0 || lo < 0) {
                        SLSetStringLitErr(&err, SLStringLitErr_INVALID_ESCAPE, escapeStart, i + 3u);
                        goto fail;
                    }
                    c = (unsigned char)(((uint32_t)hi << 4u) | (uint32_t)lo);
                    spanStart = escapeStart;
                    i += 3u;
                    spanEnd = i;
                    if (SLEmitAndValidateByte(
                            emit, emitCtx, &utf8, (uint8_t)c, spanStart, spanEnd, &err)
                        != 0)
                    {
                        goto fail;
                    }
                    continue;
                }
                case (unsigned char)'u':
                case (unsigned char)'U': {
                    uint32_t rune;
                    uint32_t digits = (c == (unsigned char)'u') ? 4u : 8u;
                    if (i + digits >= end - 1u) {
                        SLSetStringLitErr(&err, SLStringLitErr_INVALID_ESCAPE, escapeStart, end);
                        goto fail;
                    }
                    if (SLDecodeHex(src, i + 1u, digits, &rune, &err) != 0) {
                        err.start = escapeStart;
                        err.end = i + 1u + digits;
                        goto fail;
                    }
                    if (rune > 0x10FFFFu || (rune >= 0xD800u && rune <= 0xDFFFu)) {
                        SLSetStringLitErr(
                            &err, SLStringLitErr_INVALID_CODEPOINT, escapeStart, i + 1u + digits);
                        goto fail;
                    }
                    spanStart = escapeStart;
                    i += 1u + digits;
                    spanEnd = i;
                    if (SLEmitRuneUTF8(emit, emitCtx, &utf8, rune, spanStart, spanEnd, &err) != 0) {
                        goto fail;
                    }
                    continue;
                }
                default:
                    if (!SLOctalDigit(c)) {
                        SLSetStringLitErr(&err, SLStringLitErr_INVALID_ESCAPE, escapeStart, i + 1u);
                        goto fail;
                    }
                    if (i + 2u >= end - 1u || !SLOctalDigit((unsigned char)src[i + 1u])
                        || !SLOctalDigit((unsigned char)src[i + 2u]))
                    {
                        SLSetStringLitErr(&err, SLStringLitErr_INVALID_ESCAPE, escapeStart, i + 3u);
                        goto fail;
                    }
                    c = (unsigned char)((((unsigned char)src[i] - (unsigned char)'0') << 6u)
                                        | (((unsigned char)src[i + 1u] - (unsigned char)'0') << 3u)
                                        | ((unsigned char)src[i + 2u] - (unsigned char)'0'));
                    spanStart = escapeStart;
                    i += 3u;
                    spanEnd = i;
                    if (SLEmitAndValidateByte(
                            emit, emitCtx, &utf8, (uint8_t)c, spanStart, spanEnd, &err)
                        != 0)
                    {
                        goto fail;
                    }
                    continue;
            }
            spanStart = escapeStart;
            spanEnd = i;
            if (SLEmitAndValidateByte(emit, emitCtx, &utf8, (uint8_t)c, spanStart, spanEnd, &err)
                != 0)
            {
                goto fail;
            }
            continue;
        }

        if (c == (unsigned char)'\r') {
            c = (unsigned char)'\n';
            i++;
            if (i < end - 1u && (unsigned char)src[i] == (unsigned char)'\n') {
                i++;
            }
            spanEnd = i;
        } else {
            i++;
        }
        if (SLEmitAndValidateByte(emit, emitCtx, &utf8, (uint8_t)c, spanStart, spanEnd, &err) != 0)
        {
            goto fail;
        }
    }

    if (SLUTF8Finish(&utf8, &err) != 0) {
        goto fail;
    }
    if (outErr != NULL) {
        *outErr = err;
    }
    return 0;

fail:
    if (outErr != NULL) {
        *outErr = err;
    }
    return -1;
}

typedef struct {
    uint32_t rune;
    uint32_t codepointCount;
    uint32_t currentRune;
    uint8_t  seqNeed;
    uint8_t  seqHave;
} SLRuneCollector;

static int SLRuneCollectorEmit(void* _Nullable ctx, uint8_t b) {
    SLRuneCollector* c = (SLRuneCollector*)ctx;
    if (c == NULL) {
        return -1;
    }
    if (c->seqNeed == 0u) {
        if (b <= 0x7Fu) {
            if (c->codepointCount == 0u) {
                c->rune = (uint32_t)b;
            }
            c->codepointCount++;
            return 0;
        }
        if (b >= 0xC2u && b <= 0xDFu) {
            c->seqNeed = 2u;
            c->seqHave = 1u;
            c->currentRune = (uint32_t)(b & 0x1Fu);
            return 0;
        }
        if (b >= 0xE0u && b <= 0xEFu) {
            c->seqNeed = 3u;
            c->seqHave = 1u;
            c->currentRune = (uint32_t)(b & 0x0Fu);
            return 0;
        }
        if (b >= 0xF0u && b <= 0xF4u) {
            c->seqNeed = 4u;
            c->seqHave = 1u;
            c->currentRune = (uint32_t)(b & 0x07u);
            return 0;
        }
        return -1;
    }

    c->currentRune = (c->currentRune << 6u) | (uint32_t)(b & 0x3Fu);
    c->seqHave++;
    if (c->seqHave == c->seqNeed) {
        if (c->codepointCount == 0u) {
            c->rune = c->currentRune;
        }
        c->codepointCount++;
        c->seqNeed = 0u;
        c->seqHave = 0u;
        c->currentRune = 0u;
    }
    return 0;
}

SLDiagCode SLStringLitErrDiagCode(SLStringLitErrKind kind) {
    switch (kind) {
        case SLStringLitErr_NONE:              return SLDiag_NONE;
        case SLStringLitErr_UNTERMINATED:      return SLDiag_UNTERMINATED_STRING;
        case SLStringLitErr_INVALID_ESCAPE:    return SLDiag_INVALID_STRING_ESCAPE;
        case SLStringLitErr_INVALID_CODEPOINT: return SLDiag_INVALID_STRING_CODEPOINT;
        case SLStringLitErr_INVALID_UTF8:      return SLDiag_INVALID_UTF8_STRING;
        case SLStringLitErr_ARENA_OOM:         return SLDiag_ARENA_OOM;
    }
    return SLDiag_UNTERMINATED_STRING;
}

SLDiagCode SLRuneLitErrDiagCode(SLRuneLitErrKind kind) {
    switch (kind) {
        case SLRuneLitErr_NONE:                return SLDiag_NONE;
        case SLRuneLitErr_UNTERMINATED:        return SLDiag_UNTERMINATED_RUNE;
        case SLRuneLitErr_EMPTY:               return SLDiag_EMPTY_RUNE;
        case SLRuneLitErr_MULTIPLE_CODEPOINTS: return SLDiag_RUNE_CODEPOINT_COUNT;
        case SLRuneLitErr_INVALID_ESCAPE:      return SLDiag_INVALID_RUNE_ESCAPE;
        case SLRuneLitErr_INVALID_CODEPOINT:   return SLDiag_INVALID_RUNE_CODEPOINT;
        case SLRuneLitErr_INVALID_UTF8:        return SLDiag_INVALID_UTF8_RUNE;
    }
    return SLDiag_UNTERMINATED_RUNE;
}

int SLDecodeStringLiteralValidate(
    const char* _Nonnull src, uint32_t start, uint32_t end, SLStringLitErr* _Nullable outErr) {
    return SLDecodeStringLiteralImpl(src, start, end, NULL, NULL, outErr);
}

int SLDecodeRuneLiteralValidate(
    const char* _Nonnull src,
    uint32_t start,
    uint32_t end,
    uint32_t* _Nonnull outRune,
    SLRuneLitErr* _Nullable outErr) {
    SLStringLitErr  stringErr = { 0 };
    SLRuneCollector collector = { 0 };
    if (outErr != NULL) {
        outErr->kind = SLRuneLitErr_NONE;
        outErr->start = start;
        outErr->end = end;
    }
    if (src == NULL || outRune == NULL) {
        if (outErr != NULL) {
            outErr->kind = SLRuneLitErr_UNTERMINATED;
        }
        return -1;
    }
    *outRune = 0u;
    if (SLDecodeStringLiteralImpl(src, start, end, SLRuneCollectorEmit, &collector, &stringErr)
        != 0)
    {
        if (outErr != NULL) {
            outErr->start = stringErr.start;
            outErr->end = stringErr.end;
            switch (stringErr.kind) {
                case SLStringLitErr_UNTERMINATED: outErr->kind = SLRuneLitErr_UNTERMINATED; break;
                case SLStringLitErr_INVALID_ESCAPE:
                    outErr->kind = SLRuneLitErr_INVALID_ESCAPE;
                    break;
                case SLStringLitErr_INVALID_CODEPOINT:
                    outErr->kind = SLRuneLitErr_INVALID_CODEPOINT;
                    break;
                case SLStringLitErr_INVALID_UTF8: outErr->kind = SLRuneLitErr_INVALID_UTF8; break;
                case SLStringLitErr_NONE:
                case SLStringLitErr_ARENA_OOM:    outErr->kind = SLRuneLitErr_INVALID_UTF8; break;
            }
        }
        return -1;
    }
    if (collector.codepointCount == 0u) {
        if (outErr != NULL) {
            outErr->kind = SLRuneLitErr_EMPTY;
            outErr->start = start;
            outErr->end = end;
        }
        return -1;
    }
    if (collector.codepointCount != 1u) {
        if (outErr != NULL) {
            outErr->kind = SLRuneLitErr_MULTIPLE_CODEPOINTS;
            outErr->start = start;
            outErr->end = end;
        }
        return -1;
    }
    *outRune = collector.rune;
    return 0;
}

int SLDecodeStringLiteralArena(
    SLArena* _Nonnull arena,
    const char* _Nonnull src,
    uint32_t start,
    uint32_t end,
    uint8_t* _Nullable* _Nonnull outBytes,
    uint32_t* _Nonnull outLen,
    SLStringLitErr* _Nullable outErr) {
    SLArenaByteBuf buf = {
        .arena = arena,
        .v = NULL,
        .len = 0,
        .cap = 0,
    };
    if (outBytes != NULL) {
        *outBytes = NULL;
    }
    if (outLen != NULL) {
        *outLen = 0;
    }
    if (arena == NULL || outBytes == NULL || outLen == NULL) {
        SLSetStringLitErr(outErr, SLStringLitErr_ARENA_OOM, start, end);
        return -1;
    }
    if (SLDecodeStringLiteralImpl(src, start, end, SLArenaByteEmit, &buf, outErr) != 0) {
        return -1;
    }
    *outBytes = buf.v;
    *outLen = buf.len;
    return 0;
}

int SLDecodeStringLiteralMalloc(
    const char* _Nonnull src,
    uint32_t start,
    uint32_t end,
    uint8_t* _Nullable* _Nonnull outBytes,
    uint32_t* _Nonnull outLen,
    SLStringLitErr* _Nullable outErr) {
    if (outBytes != NULL) {
        *outBytes = NULL;
    }
    if (outLen != NULL) {
        *outLen = 0;
    }
    if (outBytes == NULL || outLen == NULL) {
        SLSetStringLitErr(outErr, SLStringLitErr_ARENA_OOM, start, end);
        return -1;
    }
#if !SL_LIBC
    (void)src;
    (void)start;
    (void)end;
    SLSetStringLitErr(outErr, SLStringLitErr_ARENA_OOM, start, end);
    return -1;
#else
    SLMallocByteBuf buf = {
        .v = NULL,
        .len = 0,
        .cap = 0,
    };
    if (SLDecodeStringLiteralImpl(src, start, end, SLMallocByteEmit, &buf, outErr) != 0) {
        free(buf.v);
        return -1;
    }
    *outBytes = buf.v;
    *outLen = buf.len;
    return 0;
#endif
}

static int SLIsStringLiteralConcatChainRec(const SLAst* _Nonnull ast, int32_t nodeId) {
    const SLAstNode* n;
    int32_t          lhs;
    int32_t          rhs;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return 0;
    }
    n = &ast->nodes[nodeId];
    if ((n->flags & SLAstFlag_PAREN) != 0) {
        return 0;
    }
    if (n->kind == SLAst_STRING) {
        return 1;
    }
    if (n->kind != SLAst_BINARY || (SLTokenKind)n->op != SLTok_ADD) {
        return 0;
    }
    lhs = n->firstChild;
    if (lhs < 0) {
        return 0;
    }
    rhs = ast->nodes[lhs].nextSibling;
    if (rhs < 0 || ast->nodes[rhs].nextSibling >= 0) {
        return 0;
    }
    return SLIsStringLiteralConcatChainRec(ast, lhs) && SLIsStringLiteralConcatChainRec(ast, rhs);
}

int SLIsStringLiteralConcatChain(const SLAst* _Nonnull ast, int32_t nodeId) {
    return SLIsStringLiteralConcatChainRec(ast, nodeId);
}
