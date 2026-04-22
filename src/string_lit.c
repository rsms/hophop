#include "libhop-impl.h"

#if HOP_LIBC
    #include <stdlib.h>
#endif

typedef int (*HOPStringLitEmitByteFn)(void* _Nullable ctx, uint8_t b);

typedef struct {
    uint8_t  firstByte;
    uint8_t  need;
    uint8_t  have;
    uint32_t firstStart;
    uint32_t firstEnd;
} HOPUTF8State;

typedef struct {
    HOPArena* _Nonnull arena;
    uint8_t* _Nullable v;
    uint32_t len;
    uint32_t cap;
} HOPArenaByteBuf;

#if HOP_LIBC
typedef struct {
    uint8_t* _Nullable v;
    uint32_t len;
    uint32_t cap;
} HOPMallocByteBuf;
#endif

static void HOPSetStringLitErr(
    HOPStringLitErr* _Nullable err, HOPStringLitErrKind kind, uint32_t start, uint32_t end) {
    if (err == NULL) {
        return;
    }
    err->kind = kind;
    err->start = start;
    err->end = end;
}

static int HOPArenaByteBufEnsure(HOPArenaByteBuf* _Nonnull b, uint32_t need) {
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
    p = (uint8_t*)HOPArenaAlloc(b->arena, newCap, (uint32_t)_Alignof(uint8_t));
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

static int HOPArenaByteEmit(void* _Nullable ctx, uint8_t b) {
    HOPArenaByteBuf* buf = (HOPArenaByteBuf*)ctx;
    if (buf == NULL) {
        return -1;
    }
    if (HOPArenaByteBufEnsure(buf, buf->len + 1u) != 0) {
        return -1;
    }
    buf->v[buf->len++] = b;
    return 0;
}

#if HOP_LIBC
static int HOPMallocByteBufEnsure(HOPMallocByteBuf* _Nonnull b, uint32_t need) {
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

static int HOPMallocByteEmit(void* _Nullable ctx, uint8_t b) {
    HOPMallocByteBuf* buf = (HOPMallocByteBuf*)ctx;
    if (buf == NULL) {
        return -1;
    }
    if (HOPMallocByteBufEnsure(buf, buf->len + 1u) != 0) {
        return -1;
    }
    buf->v[buf->len++] = b;
    return 0;
}
#endif

static int HOPHexDigit(unsigned char c) {
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

static int HOPOctalDigit(unsigned char c) {
    return c >= (unsigned char)'0' && c <= (unsigned char)'7';
}

static int HOPUTF8SecondByteValid(uint8_t first, uint8_t second) {
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

static int HOPUTF8Feed(
    HOPUTF8State* _Nonnull st,
    uint8_t  b,
    uint32_t start,
    uint32_t end,
    HOPStringLitErr* _Nullable err) {
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
        HOPSetStringLitErr(err, HOPStringLitErr_INVALID_UTF8, start, end);
        return -1;
    }

    if (st->have == 1u) {
        if (!HOPUTF8SecondByteValid(st->firstByte, b)) {
            HOPSetStringLitErr(err, HOPStringLitErr_INVALID_UTF8, start, end);
            return -1;
        }
    } else if (b < 0x80u || b > 0xBFu) {
        HOPSetStringLitErr(err, HOPStringLitErr_INVALID_UTF8, start, end);
        return -1;
    }

    st->have++;
    if (st->have == st->need) {
        st->need = 0u;
        st->have = 0u;
    }
    return 0;
}

static int HOPUTF8Finish(const HOPUTF8State* _Nonnull st, HOPStringLitErr* _Nullable err) {
    if (st->need == 0u) {
        return 0;
    }
    HOPSetStringLitErr(err, HOPStringLitErr_INVALID_UTF8, st->firstStart, st->firstEnd);
    return -1;
}

static int HOPRuneToUTF8(uint32_t rune, uint8_t* _Nonnull out, uint32_t* _Nonnull outLen) {
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

static int HOPEmitAndValidateByte(
    HOPStringLitEmitByteFn _Nullable emit,
    void* _Nullable emitCtx,
    HOPUTF8State* _Nonnull utf8,
    uint8_t  b,
    uint32_t start,
    uint32_t end,
    HOPStringLitErr* _Nullable err) {
    if (HOPUTF8Feed(utf8, b, start, end, err) != 0) {
        return -1;
    }
    if (emit != NULL && emit(emitCtx, b) != 0) {
        HOPSetStringLitErr(err, HOPStringLitErr_ARENA_OOM, start, end);
        return -1;
    }
    return 0;
}

static int HOPEmitRuneUTF8(
    HOPStringLitEmitByteFn _Nullable emit,
    void* _Nullable emitCtx,
    HOPUTF8State* _Nonnull utf8,
    uint32_t rune,
    uint32_t start,
    uint32_t end,
    HOPStringLitErr* _Nullable err) {
    uint8_t  bytes[4];
    uint32_t n = 0;
    uint32_t i;
    if (HOPRuneToUTF8(rune, bytes, &n) != 0) {
        HOPSetStringLitErr(err, HOPStringLitErr_INVALID_CODEPOINT, start, end);
        return -1;
    }
    for (i = 0; i < n; i++) {
        if (HOPEmitAndValidateByte(emit, emitCtx, utf8, bytes[i], start, end, err) != 0) {
            return -1;
        }
    }
    return 0;
}

static int HOPDecodeHex(
    const char* _Nonnull src,
    uint32_t start,
    uint32_t digits,
    uint32_t* _Nonnull outValue,
    HOPStringLitErr* _Nonnull err) {
    uint32_t i;
    uint32_t v = 0u;
    for (i = 0; i < digits; i++) {
        int d = HOPHexDigit((unsigned char)src[start + i]);
        if (d < 0) {
            HOPSetStringLitErr(err, HOPStringLitErr_INVALID_ESCAPE, start, start + digits);
            return -1;
        }
        v = (v << 4u) | (uint32_t)d;
    }
    *outValue = v;
    return 0;
}

static int HOPDecodeStringLiteralImpl(
    const char* _Nonnull src,
    uint32_t start,
    uint32_t end,
    HOPStringLitEmitByteFn _Nullable emit,
    void* _Nullable emitCtx,
    HOPStringLitErr* _Nullable outErr) {
    uint32_t        i;
    HOPUTF8State    utf8 = { 0 };
    unsigned char   delim;
    HOPStringLitErr err = {
        .kind = HOPStringLitErr_NONE,
        .start = start,
        .end = end,
    };

    if (src == NULL || end <= start + 1u) {
        HOPSetStringLitErr(&err, HOPStringLitErr_UNTERMINATED, start, end);
        goto fail;
    }
    delim = (unsigned char)src[start];
    if ((delim != (unsigned char)'"' && delim != (unsigned char)'`' && delim != (unsigned char)'\'')
        || (unsigned char)src[end - 1u] != delim)
    {
        HOPSetStringLitErr(&err, HOPStringLitErr_UNTERMINATED, start, end);
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
            if (HOPEmitAndValidateByte(emit, emitCtx, &utf8, (uint8_t)c, spanStart, spanEnd, &err)
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
                HOPSetStringLitErr(&err, HOPStringLitErr_INVALID_ESCAPE, escapeStart, i);
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
                        HOPSetStringLitErr(&err, HOPStringLitErr_INVALID_ESCAPE, escapeStart, end);
                        goto fail;
                    }
                    hi = HOPHexDigit((unsigned char)src[i + 1u]);
                    lo = HOPHexDigit((unsigned char)src[i + 2u]);
                    if (hi < 0 || lo < 0) {
                        HOPSetStringLitErr(
                            &err, HOPStringLitErr_INVALID_ESCAPE, escapeStart, i + 3u);
                        goto fail;
                    }
                    c = (unsigned char)(((uint32_t)hi << 4u) | (uint32_t)lo);
                    spanStart = escapeStart;
                    i += 3u;
                    spanEnd = i;
                    if (HOPEmitAndValidateByte(
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
                        HOPSetStringLitErr(&err, HOPStringLitErr_INVALID_ESCAPE, escapeStart, end);
                        goto fail;
                    }
                    if (HOPDecodeHex(src, i + 1u, digits, &rune, &err) != 0) {
                        err.start = escapeStart;
                        err.end = i + 1u + digits;
                        goto fail;
                    }
                    if (rune > 0x10FFFFu || (rune >= 0xD800u && rune <= 0xDFFFu)) {
                        HOPSetStringLitErr(
                            &err, HOPStringLitErr_INVALID_CODEPOINT, escapeStart, i + 1u + digits);
                        goto fail;
                    }
                    spanStart = escapeStart;
                    i += 1u + digits;
                    spanEnd = i;
                    if (HOPEmitRuneUTF8(emit, emitCtx, &utf8, rune, spanStart, spanEnd, &err) != 0)
                    {
                        goto fail;
                    }
                    continue;
                }
                default:
                    if (!HOPOctalDigit(c)) {
                        HOPSetStringLitErr(
                            &err, HOPStringLitErr_INVALID_ESCAPE, escapeStart, i + 1u);
                        goto fail;
                    }
                    if (i + 2u >= end - 1u || !HOPOctalDigit((unsigned char)src[i + 1u])
                        || !HOPOctalDigit((unsigned char)src[i + 2u]))
                    {
                        HOPSetStringLitErr(
                            &err, HOPStringLitErr_INVALID_ESCAPE, escapeStart, i + 3u);
                        goto fail;
                    }
                    c = (unsigned char)((((unsigned char)src[i] - (unsigned char)'0') << 6u)
                                        | (((unsigned char)src[i + 1u] - (unsigned char)'0') << 3u)
                                        | ((unsigned char)src[i + 2u] - (unsigned char)'0'));
                    spanStart = escapeStart;
                    i += 3u;
                    spanEnd = i;
                    if (HOPEmitAndValidateByte(
                            emit, emitCtx, &utf8, (uint8_t)c, spanStart, spanEnd, &err)
                        != 0)
                    {
                        goto fail;
                    }
                    continue;
            }
            spanStart = escapeStart;
            spanEnd = i;
            if (HOPEmitAndValidateByte(emit, emitCtx, &utf8, (uint8_t)c, spanStart, spanEnd, &err)
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
        if (HOPEmitAndValidateByte(emit, emitCtx, &utf8, (uint8_t)c, spanStart, spanEnd, &err) != 0)
        {
            goto fail;
        }
    }

    if (HOPUTF8Finish(&utf8, &err) != 0) {
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
} HOPRuneCollector;

static int HOPRuneCollectorEmit(void* _Nullable ctx, uint8_t b) {
    HOPRuneCollector* c = (HOPRuneCollector*)ctx;
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

HOPDiagCode HOPStringLitErrDiagCode(HOPStringLitErrKind kind) {
    switch (kind) {
        case HOPStringLitErr_NONE:              return HOPDiag_NONE;
        case HOPStringLitErr_UNTERMINATED:      return HOPDiag_UNTERMINATED_STRING;
        case HOPStringLitErr_INVALID_ESCAPE:    return HOPDiag_INVALID_STRING_ESCAPE;
        case HOPStringLitErr_INVALID_CODEPOINT: return HOPDiag_INVALID_STRING_CODEPOINT;
        case HOPStringLitErr_INVALID_UTF8:      return HOPDiag_INVALID_UTF8_STRING;
        case HOPStringLitErr_ARENA_OOM:         return HOPDiag_ARENA_OOM;
    }
    return HOPDiag_UNTERMINATED_STRING;
}

HOPDiagCode HOPRuneLitErrDiagCode(HOPRuneLitErrKind kind) {
    switch (kind) {
        case HOPRuneLitErr_NONE:                return HOPDiag_NONE;
        case HOPRuneLitErr_UNTERMINATED:        return HOPDiag_UNTERMINATED_RUNE;
        case HOPRuneLitErr_EMPTY:               return HOPDiag_EMPTY_RUNE;
        case HOPRuneLitErr_MULTIPLE_CODEPOINTS: return HOPDiag_RUNE_CODEPOINT_COUNT;
        case HOPRuneLitErr_INVALID_ESCAPE:      return HOPDiag_INVALID_RUNE_ESCAPE;
        case HOPRuneLitErr_INVALID_CODEPOINT:   return HOPDiag_INVALID_RUNE_CODEPOINT;
        case HOPRuneLitErr_INVALID_UTF8:        return HOPDiag_INVALID_UTF8_RUNE;
    }
    return HOPDiag_UNTERMINATED_RUNE;
}

int HOPDecodeStringLiteralValidate(
    const char* _Nonnull src, uint32_t start, uint32_t end, HOPStringLitErr* _Nullable outErr) {
    return HOPDecodeStringLiteralImpl(src, start, end, NULL, NULL, outErr);
}

int HOPDecodeRuneLiteralValidate(
    const char* _Nonnull src,
    uint32_t start,
    uint32_t end,
    uint32_t* _Nonnull outRune,
    HOPRuneLitErr* _Nullable outErr) {
    HOPStringLitErr  stringErr = { 0 };
    HOPRuneCollector collector = { 0 };
    if (outErr != NULL) {
        outErr->kind = HOPRuneLitErr_NONE;
        outErr->start = start;
        outErr->end = end;
    }
    if (src == NULL || outRune == NULL) {
        if (outErr != NULL) {
            outErr->kind = HOPRuneLitErr_UNTERMINATED;
        }
        return -1;
    }
    *outRune = 0u;
    if (HOPDecodeStringLiteralImpl(src, start, end, HOPRuneCollectorEmit, &collector, &stringErr)
        != 0)
    {
        if (outErr != NULL) {
            outErr->start = stringErr.start;
            outErr->end = stringErr.end;
            switch (stringErr.kind) {
                case HOPStringLitErr_UNTERMINATED: outErr->kind = HOPRuneLitErr_UNTERMINATED; break;
                case HOPStringLitErr_INVALID_ESCAPE:
                    outErr->kind = HOPRuneLitErr_INVALID_ESCAPE;
                    break;
                case HOPStringLitErr_INVALID_CODEPOINT:
                    outErr->kind = HOPRuneLitErr_INVALID_CODEPOINT;
                    break;
                case HOPStringLitErr_INVALID_UTF8: outErr->kind = HOPRuneLitErr_INVALID_UTF8; break;
                case HOPStringLitErr_NONE:
                case HOPStringLitErr_ARENA_OOM:    outErr->kind = HOPRuneLitErr_INVALID_UTF8; break;
            }
        }
        return -1;
    }
    if (collector.codepointCount == 0u) {
        if (outErr != NULL) {
            outErr->kind = HOPRuneLitErr_EMPTY;
            outErr->start = start;
            outErr->end = end;
        }
        return -1;
    }
    if (collector.codepointCount != 1u) {
        if (outErr != NULL) {
            outErr->kind = HOPRuneLitErr_MULTIPLE_CODEPOINTS;
            outErr->start = start;
            outErr->end = end;
        }
        return -1;
    }
    *outRune = collector.rune;
    return 0;
}

int HOPDecodeStringLiteralArena(
    HOPArena* _Nonnull arena,
    const char* _Nonnull src,
    uint32_t start,
    uint32_t end,
    uint8_t* _Nullable* _Nonnull outBytes,
    uint32_t* _Nonnull outLen,
    HOPStringLitErr* _Nullable outErr) {
    HOPArenaByteBuf buf = {
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
        HOPSetStringLitErr(outErr, HOPStringLitErr_ARENA_OOM, start, end);
        return -1;
    }
    if (HOPDecodeStringLiteralImpl(src, start, end, HOPArenaByteEmit, &buf, outErr) != 0) {
        return -1;
    }
    *outBytes = buf.v;
    *outLen = buf.len;
    return 0;
}

int HOPDecodeStringLiteralMalloc(
    const char* _Nonnull src,
    uint32_t start,
    uint32_t end,
    uint8_t* _Nullable* _Nonnull outBytes,
    uint32_t* _Nonnull outLen,
    HOPStringLitErr* _Nullable outErr) {
    if (outBytes != NULL) {
        *outBytes = NULL;
    }
    if (outLen != NULL) {
        *outLen = 0;
    }
    if (outBytes == NULL || outLen == NULL) {
        HOPSetStringLitErr(outErr, HOPStringLitErr_ARENA_OOM, start, end);
        return -1;
    }
#if !HOP_LIBC
    (void)src;
    (void)start;
    (void)end;
    HOPSetStringLitErr(outErr, HOPStringLitErr_ARENA_OOM, start, end);
    return -1;
#else
    HOPMallocByteBuf buf = {
        .v = NULL,
        .len = 0,
        .cap = 0,
    };
    if (HOPDecodeStringLiteralImpl(src, start, end, HOPMallocByteEmit, &buf, outErr) != 0) {
        free(buf.v);
        return -1;
    }
    *outBytes = buf.v;
    *outLen = buf.len;
    return 0;
#endif
}

static int HOPIsStringLiteralConcatChainRec(const HOPAst* _Nonnull ast, int32_t nodeId) {
    const HOPAstNode* n;
    int32_t           lhs;
    int32_t           rhs;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return 0;
    }
    n = &ast->nodes[nodeId];
    if ((n->flags & HOPAstFlag_PAREN) != 0) {
        return 0;
    }
    if (n->kind == HOPAst_STRING) {
        return 1;
    }
    if (n->kind != HOPAst_BINARY || (HOPTokenKind)n->op != HOPTok_ADD) {
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
    return HOPIsStringLiteralConcatChainRec(ast, lhs) && HOPIsStringLiteralConcatChainRec(ast, rhs);
}

int HOPIsStringLiteralConcatChain(const HOPAst* _Nonnull ast, int32_t nodeId) {
    return HOPIsStringLiteralConcatChainRec(ast, nodeId);
}
