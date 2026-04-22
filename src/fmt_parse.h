#pragma once

#include <stdint.h>

typedef enum {
    HOPFmtTok_LITERAL = 0,
    HOPFmtTok_PLACEHOLDER_I,
    HOPFmtTok_PLACEHOLDER_F,
    HOPFmtTok_PLACEHOLDER_S,
    HOPFmtTok_PLACEHOLDER_R,
} HOPFmtTokKind;

typedef struct {
    HOPFmtTokKind kind;
    uint32_t      start;
    uint32_t      end;
} HOPFmtToken;

typedef enum {
    HOPFmtParseErr_NONE = 0,
    HOPFmtParseErr_INVALID_PLACEHOLDER,
    HOPFmtParseErr_UNMATCHED_CLOSE_BRACE,
    HOPFmtParseErr_TOKEN_OVERFLOW,
} HOPFmtParseErrCode;

typedef struct {
    HOPFmtParseErrCode code;
    uint32_t           start;
    uint32_t           end;
} HOPFmtParseError;

int HOPFmtParseBytes(
    const uint8_t* _Nonnull bytes,
    uint32_t len,
    HOPFmtToken* _Nonnull outTokens,
    uint32_t tokenCap,
    uint32_t* _Nonnull outTokenLen,
    HOPFmtParseError* _Nullable outErr);
