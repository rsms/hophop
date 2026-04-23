#pragma once

#include <stdint.h>

typedef enum {
    H2FmtTok_LITERAL = 0,
    H2FmtTok_PLACEHOLDER_I,
    H2FmtTok_PLACEHOLDER_F,
    H2FmtTok_PLACEHOLDER_S,
    H2FmtTok_PLACEHOLDER_R,
} H2FmtTokKind;

typedef struct {
    H2FmtTokKind kind;
    uint32_t     start;
    uint32_t     end;
} H2FmtToken;

typedef enum {
    H2FmtParseErr_NONE = 0,
    H2FmtParseErr_INVALID_PLACEHOLDER,
    H2FmtParseErr_UNMATCHED_CLOSE_BRACE,
    H2FmtParseErr_TOKEN_OVERFLOW,
} H2FmtParseErrCode;

typedef struct {
    H2FmtParseErrCode code;
    uint32_t          start;
    uint32_t          end;
} H2FmtParseError;

int H2FmtParseBytes(
    const uint8_t* _Nonnull bytes,
    uint32_t len,
    H2FmtToken* _Nonnull outTokens,
    uint32_t tokenCap,
    uint32_t* _Nonnull outTokenLen,
    H2FmtParseError* _Nullable outErr);
