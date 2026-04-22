#include "fmt_parse.h"
#include <stddef.h>

static int HOPFmtPushToken(
    HOPFmtToken* _Nonnull outTokens,
    uint32_t tokenCap,
    uint32_t* _Nonnull ioLen,
    HOPFmtTokKind kind,
    uint32_t      start,
    uint32_t      end,
    HOPFmtParseError* _Nullable outErr) {
    uint32_t n;
    if (ioLen == NULL) {
        return -1;
    }
    n = *ioLen;
    if (n >= tokenCap) {
        if (outErr != NULL) {
            outErr->code = HOPFmtParseErr_TOKEN_OVERFLOW;
            outErr->start = start;
            outErr->end = end;
        }
        return -1;
    }
    outTokens[n].kind = kind;
    outTokens[n].start = start;
    outTokens[n].end = end;
    *ioLen = n + 1u;
    return 0;
}

int HOPFmtParseBytes(
    const uint8_t* _Nonnull bytes,
    uint32_t len,
    HOPFmtToken* _Nonnull outTokens,
    uint32_t tokenCap,
    uint32_t* _Nonnull outTokenLen,
    HOPFmtParseError* _Nullable outErr) {
    uint32_t i = 0;
    uint32_t literalStart = 0;
    uint32_t tokenLen = 0;
    if (bytes == NULL || outTokens == NULL || outTokenLen == NULL) {
        return -1;
    }
    if (outErr != NULL) {
        outErr->code = HOPFmtParseErr_NONE;
        outErr->start = 0;
        outErr->end = 0;
    }
    while (i < len) {
        if (bytes[i] == (uint8_t)'{') {
            if (i + 1u < len && bytes[i + 1u] == (uint8_t)'{') {
                i += 2u;
                continue;
            }
            if (literalStart < i
                && HOPFmtPushToken(
                       outTokens, tokenCap, &tokenLen, HOPFmtTok_LITERAL, literalStart, i, outErr)
                       != 0)
            {
                return -1;
            }
            if (i + 2u < len
                && (bytes[i + 1u] == (uint8_t)'i' || bytes[i + 1u] == (uint8_t)'f'
                    || bytes[i + 1u] == (uint8_t)'s' || bytes[i + 1u] == (uint8_t)'r')
                && bytes[i + 2u] == (uint8_t)'}')
            {
                HOPFmtTokKind kind = HOPFmtTok_PLACEHOLDER_R;
                if (bytes[i + 1u] == (uint8_t)'i') {
                    kind = HOPFmtTok_PLACEHOLDER_I;
                } else if (bytes[i + 1u] == (uint8_t)'f') {
                    kind = HOPFmtTok_PLACEHOLDER_F;
                } else if (bytes[i + 1u] == (uint8_t)'s') {
                    kind = HOPFmtTok_PLACEHOLDER_S;
                }
                if (HOPFmtPushToken(outTokens, tokenCap, &tokenLen, kind, i, i + 3u, outErr) != 0) {
                    return -1;
                }
                i += 3u;
                literalStart = i;
                continue;
            }
            if (outErr != NULL) {
                outErr->code = HOPFmtParseErr_INVALID_PLACEHOLDER;
                outErr->start = i;
                outErr->end = i + 1u < len ? i + 2u : i + 1u;
            }
            return -1;
        }
        if (bytes[i] == (uint8_t)'}') {
            if (i + 1u < len && bytes[i + 1u] == (uint8_t)'}') {
                i += 2u;
                continue;
            }
            if (outErr != NULL) {
                outErr->code = HOPFmtParseErr_UNMATCHED_CLOSE_BRACE;
                outErr->start = i;
                outErr->end = i + 1u;
            }
            return -1;
        }
        i++;
    }
    if (literalStart < len
        && HOPFmtPushToken(
               outTokens, tokenCap, &tokenLen, HOPFmtTok_LITERAL, literalStart, len, outErr)
               != 0)
    {
        return -1;
    }
    *outTokenLen = tokenLen;
    return 0;
}
