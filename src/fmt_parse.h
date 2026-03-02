#pragma once

#include <stdint.h>

typedef enum {
    SLFmtTok_LITERAL = 0,
    SLFmtTok_PLACEHOLDER_I,
    SLFmtTok_PLACEHOLDER_F,
    SLFmtTok_PLACEHOLDER_S,
    SLFmtTok_PLACEHOLDER_R,
} SLFmtTokKind;

typedef struct {
    SLFmtTokKind kind;
    uint32_t     start;
    uint32_t     end;
} SLFmtToken;

typedef enum {
    SLFmtParseErr_NONE = 0,
    SLFmtParseErr_INVALID_PLACEHOLDER,
    SLFmtParseErr_UNMATCHED_CLOSE_BRACE,
    SLFmtParseErr_TOKEN_OVERFLOW,
} SLFmtParseErrCode;

typedef struct {
    SLFmtParseErrCode code;
    uint32_t          start;
    uint32_t          end;
} SLFmtParseError;

int SLFmtParseBytes(
    const uint8_t* _Nonnull bytes,
    uint32_t len,
    SLFmtToken* _Nonnull outTokens,
    uint32_t tokenCap,
    uint32_t* _Nonnull outTokenLen,
    SLFmtParseError* _Nullable outErr);
