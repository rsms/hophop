#include "libsl-impl.h"
SL_API_BEGIN

typedef struct {
    SLToken* v;
    uint32_t len;
    uint32_t cap;
} SLTokenBuf;

static void SLSetDiag(SLDiag* diag, SLDiagCode code, uint32_t start, uint32_t end) {
    if (diag == NULL) {
        return;
    }
    diag->code = code;
    diag->type = SLDiagTypeOfCode(code);
    diag->start = start;
    diag->end = end;
    diag->argStart = 0;
    diag->argEnd = 0;
    diag->detail = NULL;
    diag->hintOverride = NULL;
}

void SLDiagClear(SLDiag* diag) {
    if (diag == NULL) {
        return;
    }
    diag->code = SLDiag_NONE;
    diag->type = SLDiagType_ERROR;
    diag->start = 0;
    diag->end = 0;
    diag->argStart = 0;
    diag->argEnd = 0;
    diag->detail = NULL;
    diag->hintOverride = NULL;
}

static uint32_t SLArenaAlignUpU32(uint32_t value, uint32_t align) {
    uint64_t aligned;
    if (align == 0) {
        align = 1;
    }
    aligned = ((uint64_t)value + (uint64_t)(align - 1u)) & ~((uint64_t)align - 1u);
    if (aligned > UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)aligned;
}

static uint32_t SLArenaGrowHeaderSize(void) {
    uint32_t align = (uint32_t)_Alignof(max_align_t);
    return SLArenaAlignUpU32((uint32_t)sizeof(SLArenaBlock), align);
}

static void SLArenaInitBlock(
    SLArenaBlock* block,
    uint8_t* _Nullable mem,
    uint32_t cap,
    uint32_t allocSize,
    uint8_t  owned,
    SLArenaBlock* _Nullable next) {
    block->mem = mem;
    block->cap = cap;
    block->len = 0;
    block->allocSize = allocSize;
    block->next = next;
    block->owned = owned;
}

static void* _Nullable SLArenaTryAllocInBlock(SLArenaBlock* block, uint32_t size, uint32_t align) {
    uint64_t aligned;
    uint64_t end;

    if (block == NULL || block->mem == NULL) {
        return NULL;
    }

    aligned = ((uint64_t)block->len + (uint64_t)(align - 1u)) & ~((uint64_t)align - 1u);
    end = aligned + (uint64_t)size;
    if (end > (uint64_t)block->cap) {
        return NULL;
    }

    block->len = (uint32_t)end;
    return block->mem + aligned;
}

void SLArenaInit(SLArena* arena, void* storage, uint32_t storageSize) {
    SLArenaInitEx(arena, storage, storageSize, NULL, NULL, NULL);
}

void SLArenaInitEx(
    SLArena* arena,
    void* _Nullable storage,
    uint32_t storageSize,
    void* _Nullable allocatorCtx,
    SLArenaGrowFn _Nullable growFn,
    SLArenaFreeFn _Nullable freeFn) {
    arena->allocatorCtx = allocatorCtx;
    arena->grow = growFn;
    arena->free = freeFn;
    SLArenaInitBlock(&arena->inlineBlock, (uint8_t*)storage, storageSize, storageSize, 0, NULL);
    arena->first = &arena->inlineBlock;
    arena->current = &arena->inlineBlock;
}

void SLArenaSetAllocator(
    SLArena* arena,
    void* _Nullable allocatorCtx,
    SLArenaGrowFn _Nullable growFn,
    SLArenaFreeFn _Nullable freeFn) {
    arena->allocatorCtx = allocatorCtx;
    arena->grow = growFn;
    arena->free = freeFn;
}

void SLArenaReset(SLArena* arena) {
    SLArenaBlock* block = arena->first;
    while (block != NULL) {
        block->len = 0;
        block = block->next;
    }
    arena->current = arena->first;
}

void SLArenaDispose(SLArena* arena) {
    SLArenaBlock* block;

    if (arena->first == NULL) {
        return;
    }

    block = arena->first->next;
    while (block != NULL) {
        SLArenaBlock* next = block->next;
        if (block->owned && arena->free != NULL) {
            arena->free(arena->allocatorCtx, block, block->allocSize);
        }
        block = next;
    }

    arena->inlineBlock.next = NULL;
    arena->inlineBlock.len = 0;
    arena->first = &arena->inlineBlock;
    arena->current = &arena->inlineBlock;
}

void* _Nullable SLArenaAlloc(SLArena* arena, uint32_t size, uint32_t align) {
    SLArenaBlock* block;

    if (align == 0) {
        align = 1;
    }
    if ((align & (align - 1u)) != 0) {
        return NULL;
    }

    block = arena->current != NULL ? arena->current : arena->first;
    if (block == NULL) {
        return NULL;
    }

    for (;;) {
        void* p = SLArenaTryAllocInBlock(block, size, align);
        if (p != NULL) {
            arena->current = block;
            return p;
        }
        if (block->next == NULL) {
            break;
        }
        block = block->next;
    }

    if (arena->grow != NULL) {
        uint32_t headerSize = SLArenaGrowHeaderSize();
        uint32_t minPayload;
        uint32_t targetPayload;
        uint64_t requestedSize64;
        uint32_t requestedSize;
        uint32_t allocSize = 0;
        void* _Nullable allocMem;
        SLArenaBlock* newBlock;
        void* _Nullable p;

        if (size > UINT32_MAX - (align - 1u)) {
            return NULL;
        }
        minPayload = size + align - 1u;
        targetPayload = minPayload;

        if (block->cap > 0 && block->cap > targetPayload) {
            targetPayload = block->cap;
            if (targetPayload <= UINT32_MAX / 2u) {
                targetPayload *= 2u;
            } else {
                targetPayload = UINT32_MAX;
            }
            if (targetPayload < minPayload) {
                targetPayload = minPayload;
            }
        }

        requestedSize64 = (uint64_t)headerSize + (uint64_t)targetPayload;
        if (requestedSize64 > UINT32_MAX) {
            return NULL;
        }
        requestedSize = (uint32_t)requestedSize64;
        allocMem = arena->grow(arena->allocatorCtx, requestedSize, &allocSize);
        if (allocMem == NULL) {
            return NULL;
        }
        if (allocSize < headerSize + minPayload) {
            if (arena->free != NULL) {
                arena->free(arena->allocatorCtx, allocMem, allocSize);
            }
            return NULL;
        }

        newBlock = (SLArenaBlock*)allocMem;
        SLArenaInitBlock(
            newBlock, (uint8_t*)allocMem + headerSize, allocSize - headerSize, allocSize, 1, NULL);
        block->next = newBlock;
        arena->current = newBlock;
        p = SLArenaTryAllocInBlock(newBlock, size, align);
        if (p != NULL) {
            return p;
        }
    }

    return NULL;
}

static int SLIsAlpha(unsigned char c) {
    return (c >= (unsigned char)'A' && c <= (unsigned char)'Z')
        || (c >= (unsigned char)'a' && c <= (unsigned char)'z');
}

static int SLIsDigit(unsigned char c) {
    return c >= (unsigned char)'0' && c <= (unsigned char)'9';
}

static int SLIsAlnum(unsigned char c) {
    return SLIsAlpha(c) || SLIsDigit(c);
}

static int SLIsAsciiSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static int SLIsHexDigit(unsigned char c) {
    return SLIsDigit(c) || (c >= (unsigned char)'a' && c <= (unsigned char)'f')
        || (c >= (unsigned char)'A' && c <= (unsigned char)'F');
}

static int SLStrEq(const char* a, uint32_t aLen, const char* b) {
    uint32_t i = 0;
    while (i < aLen) {
        if (b[i] == '\0' || a[i] != b[i]) {
            return 0;
        }
        i++;
    }
    return b[i] == '\0';
}

static int SLIsValidImportPathChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'
        || c == '.' || c == '/' || c == '-';
}

int SLNormalizeImportPath(
    const char* importPath,
    char*       out,
    uint32_t    outCap,
    const char* _Nullable* _Nullable outErrReason) {
    uint32_t len = 0;
    uint32_t i;
    uint32_t outLen = 0;
    uint32_t segStart = 0;

    if (outErrReason != NULL) {
        *outErrReason = NULL;
    }

    if (importPath == NULL || importPath[0] == '\0') {
        if (outErrReason != NULL) {
            *outErrReason = "empty path";
        }
        return -1;
    }
    if (out == NULL || outCap == 0) {
        if (outErrReason != NULL) {
            *outErrReason = "output buffer too small";
        }
        return -1;
    }

    while (importPath[len] != '\0') {
        len++;
    }

    if (importPath[0] == '/') {
        if (outErrReason != NULL) {
            *outErrReason = "absolute path";
        }
        return -1;
    }
    if (importPath[0] == '.' && importPath[1] == '\0') {
        if (outErrReason != NULL) {
            *outErrReason = "cannot import itself";
        }
        return -1;
    }
    if (importPath[0] == '.' && importPath[1] == '.' && importPath[2] == '\0') {
        if (outErrReason != NULL) {
            *outErrReason = "cannot import parent root";
        }
        return -1;
    }
    if (SLIsAsciiSpace(importPath[0]) || SLIsAsciiSpace(importPath[len - 1u])) {
        if (outErrReason != NULL) {
            *outErrReason = "leading/trailing whitespace";
        }
        return -1;
    }

    for (i = 0; i < len; i++) {
        if (!SLIsValidImportPathChar(importPath[i])) {
            if (outErrReason != NULL) {
                *outErrReason = "invalid character";
            }
            return -1;
        }
    }

    for (;;) {
        uint32_t segEnd = segStart;
        uint32_t segLen;
        while (segEnd < len && importPath[segEnd] != '/') {
            segEnd++;
        }
        segLen = segEnd - segStart;
        if (segLen == 0) {
            if (outErrReason != NULL) {
                *outErrReason = "empty segment";
            }
            return -1;
        }
        if (segLen == 1 && importPath[segStart] == '.') {
            /* skip "." */
        } else if (segLen == 2 && importPath[segStart] == '.' && importPath[segStart + 1u] == '.') {
            if (outLen == 0) {
                if (outErrReason != NULL) {
                    *outErrReason = "escapes root";
                }
                return -1;
            }
            while (outLen > 0 && out[outLen - 1u] != '/') {
                outLen--;
            }
            if (outLen > 0) {
                outLen--;
            }
        } else {
            if (outLen > 0) {
                if (outLen + 1u >= outCap) {
                    if (outErrReason != NULL) {
                        *outErrReason = "output buffer too small";
                    }
                    return -1;
                }
                out[outLen++] = '/';
            }
            if (segLen > outCap - outLen - 1u) {
                if (outErrReason != NULL) {
                    *outErrReason = "output buffer too small";
                }
                return -1;
            }
            memcpy(out + outLen, importPath + segStart, segLen);
            outLen += segLen;
        }

        if (segEnd >= len) {
            break;
        }
        segStart = segEnd + 1u;
    }

    if (outLen == 0) {
        if (outErrReason != NULL) {
            *outErrReason = "empty path";
        }
        return -1;
    }

    out[outLen] = '\0';
    return 0;
}

static SLTokenKind SLKeywordKind(const char* s, uint32_t len) {
    if (len == 2) {
        if (SLStrEq(s, len, "as")) {
            return SLTok_AS;
        }
        if (SLStrEq(s, len, "in")) {
            return SLTok_IN;
        }
        if (SLStrEq(s, len, "fn")) {
            return SLTok_FN;
        }
        if (SLStrEq(s, len, "if")) {
            return SLTok_IF;
        }
    } else if (len == 3) {
        if (SLStrEq(s, len, "for")) {
            return SLTok_FOR;
        }
        if (SLStrEq(s, len, "new")) {
            return SLTok_NEW;
        }
        if (SLStrEq(s, len, "var")) {
            return SLTok_VAR;
        }
        if (SLStrEq(s, len, "mut")) {
            return SLTok_MUT;
        }
        if (SLStrEq(s, len, "pub")) {
            return SLTok_PUB;
        }
    } else if (len == 4) {
        if (SLStrEq(s, len, "enum")) {
            return SLTok_ENUM;
        }
        if (SLStrEq(s, len, "else")) {
            return SLTok_ELSE;
        }
        if (SLStrEq(s, len, "case")) {
            return SLTok_CASE;
        }
        if (SLStrEq(s, len, "true")) {
            return SLTok_TRUE;
        }
        if (SLStrEq(s, len, "null")) {
            return SLTok_NULL;
        }
        if (SLStrEq(s, len, "type")) {
            return SLTok_TYPE;
        }
        if (SLStrEq(s, len, "with")) {
            return SLTok_WITH;
        }
    } else if (len == 5) {
        if (SLStrEq(s, len, "break")) {
            return SLTok_BREAK;
        }
        if (SLStrEq(s, len, "const")) {
            return SLTok_CONST;
        }
        if (SLStrEq(s, len, "defer")) {
            return SLTok_DEFER;
        }
        if (SLStrEq(s, len, "false")) {
            return SLTok_FALSE;
        }
        if (SLStrEq(s, len, "union")) {
            return SLTok_UNION;
        }
    } else if (len == 6) {
        if (SLStrEq(s, len, "import")) {
            return SLTok_IMPORT;
        }
        if (SLStrEq(s, len, "sizeof")) {
            return SLTok_SIZEOF;
        }
        if (SLStrEq(s, len, "return")) {
            return SLTok_RETURN;
        }
        if (SLStrEq(s, len, "switch")) {
            return SLTok_SWITCH;
        }
        if (SLStrEq(s, len, "struct")) {
            return SLTok_STRUCT;
        }
        if (SLStrEq(s, len, "assert")) {
            return SLTok_ASSERT;
        }
    } else if (len == 7) {
        if (SLStrEq(s, len, "anytype")) {
            return SLTok_ANYTYPE;
        }
        if (SLStrEq(s, len, "context")) {
            return SLTok_CONTEXT;
        }
        if (SLStrEq(s, len, "default")) {
            return SLTok_DEFAULT;
        }
    } else if (len == 8) {
        if (SLStrEq(s, len, "continue")) {
            return SLTok_CONTINUE;
        }
    }
    return SLTok_IDENT;
}

static int SLTokenCanEndStmt(SLTokenKind kind) {
    switch (kind) {
        case SLTok_IDENT:
        case SLTok_INT:
        case SLTok_FLOAT:
        case SLTok_STRING:
        case SLTok_RUNE:
        case SLTok_TRUE:
        case SLTok_FALSE:
        case SLTok_BREAK:
        case SLTok_CONTINUE:
        case SLTok_RETURN:
        case SLTok_RPAREN:
        case SLTok_RBRACK:
        case SLTok_RBRACE:
        case SLTok_NOT:
        case SLTok_NULL:
        case SLTok_CONTEXT:
        case SLTok_TYPE:     return 1;
        default:             return 0;
    }
}

static int SLPushToken(
    SLTokenBuf* out, SLDiag* diag, SLTokenKind kind, uint32_t start, uint32_t end) {
    if (out->len >= out->cap) {
        SLSetDiag(diag, SLDiag_ARENA_OOM, start, end);
        return -1;
    }

    out->v[out->len].kind = kind;
    out->v[out->len].start = start;
    out->v[out->len].end = end;
    out->len++;
    return 0;
}

static int SLSkipBlockComment(
    SLStrView src, uint32_t* ioPos, int* ioSawNewline, uint32_t* ioNewlinePos) {
    uint32_t pos = *ioPos + 2u;
    uint32_t depth = 1u;
    while (pos < src.len) {
        unsigned char c = (unsigned char)src.ptr[pos];
        if (c == (unsigned char)'\n' && !*ioSawNewline) {
            *ioSawNewline = 1;
            *ioNewlinePos = pos;
        }
        if (c == (unsigned char)'/' && pos + 1u < src.len
            && (unsigned char)src.ptr[pos + 1u] == (unsigned char)'*')
        {
            depth++;
            pos += 2u;
            continue;
        }
        if (c == (unsigned char)'*' && pos + 1u < src.len
            && (unsigned char)src.ptr[pos + 1u] == (unsigned char)'/')
        {
            depth--;
            pos += 2u;
            if (depth == 0u) {
                *ioPos = pos;
                return 0;
            }
            continue;
        }
        pos++;
    }
    *ioPos = pos;
    return -1;
}

const char* SLTokenKindName(SLTokenKind kind) {
    switch (kind) {
        case SLTok_INVALID:       return "INVALID";
        case SLTok_EOF:           return "EOF";
        case SLTok_IDENT:         return "IDENT";
        case SLTok_INT:           return "INT";
        case SLTok_FLOAT:         return "FLOAT";
        case SLTok_STRING:        return "STRING";
        case SLTok_RUNE:          return "RUNE";
        case SLTok_IMPORT:        return "IMPORT";
        case SLTok_PUB:           return "PUB";
        case SLTok_STRUCT:        return "STRUCT";
        case SLTok_UNION:         return "UNION";
        case SLTok_ENUM:          return "ENUM";
        case SLTok_FN:            return "FN";
        case SLTok_VAR:           return "VAR";
        case SLTok_CONST:         return "CONST";
        case SLTok_TYPE:          return "TYPE";
        case SLTok_MUT:           return "MUT";
        case SLTok_IF:            return "IF";
        case SLTok_ELSE:          return "ELSE";
        case SLTok_FOR:           return "FOR";
        case SLTok_SWITCH:        return "SWITCH";
        case SLTok_CASE:          return "CASE";
        case SLTok_DEFAULT:       return "DEFAULT";
        case SLTok_BREAK:         return "BREAK";
        case SLTok_CONTINUE:      return "CONTINUE";
        case SLTok_RETURN:        return "RETURN";
        case SLTok_DEFER:         return "DEFER";
        case SLTok_ASSERT:        return "ASSERT";
        case SLTok_SIZEOF:        return "SIZEOF";
        case SLTok_NEW:           return "NEW";
        case SLTok_TRUE:          return "TRUE";
        case SLTok_FALSE:         return "FALSE";
        case SLTok_IN:            return "IN";
        case SLTok_AS:            return "AS";
        case SLTok_CONTEXT:       return "CONTEXT";
        case SLTok_WITH:          return "WITH";
        case SLTok_ANYTYPE:       return "ANYTYPE";
        case SLTok_LPAREN:        return "LPAREN";
        case SLTok_RPAREN:        return "RPAREN";
        case SLTok_LBRACE:        return "LBRACE";
        case SLTok_RBRACE:        return "RBRACE";
        case SLTok_LBRACK:        return "LBRACK";
        case SLTok_RBRACK:        return "RBRACK";
        case SLTok_COMMA:         return "COMMA";
        case SLTok_DOT:           return "DOT";
        case SLTok_ELLIPSIS:      return "ELLIPSIS";
        case SLTok_SEMICOLON:     return "SEMICOLON";
        case SLTok_COLON:         return "COLON";
        case SLTok_ASSIGN:        return "ASSIGN";
        case SLTok_ADD:           return "ADD";
        case SLTok_SUB:           return "SUB";
        case SLTok_MUL:           return "MUL";
        case SLTok_DIV:           return "DIV";
        case SLTok_MOD:           return "MOD";
        case SLTok_AND:           return "AND";
        case SLTok_OR:            return "OR";
        case SLTok_XOR:           return "XOR";
        case SLTok_NOT:           return "NOT";
        case SLTok_LSHIFT:        return "LSHIFT";
        case SLTok_RSHIFT:        return "RSHIFT";
        case SLTok_EQ:            return "EQ";
        case SLTok_NEQ:           return "NEQ";
        case SLTok_LT:            return "LT";
        case SLTok_GT:            return "GT";
        case SLTok_LTE:           return "LTE";
        case SLTok_GTE:           return "GTE";
        case SLTok_LOGICAL_AND:   return "LOGICAL_AND";
        case SLTok_LOGICAL_OR:    return "LOGICAL_OR";
        case SLTok_ADD_ASSIGN:    return "ADD_ASSIGN";
        case SLTok_SUB_ASSIGN:    return "SUB_ASSIGN";
        case SLTok_MUL_ASSIGN:    return "MUL_ASSIGN";
        case SLTok_DIV_ASSIGN:    return "DIV_ASSIGN";
        case SLTok_MOD_ASSIGN:    return "MOD_ASSIGN";
        case SLTok_AND_ASSIGN:    return "AND_ASSIGN";
        case SLTok_OR_ASSIGN:     return "OR_ASSIGN";
        case SLTok_XOR_ASSIGN:    return "XOR_ASSIGN";
        case SLTok_LSHIFT_ASSIGN: return "LSHIFT_ASSIGN";
        case SLTok_RSHIFT_ASSIGN: return "RSHIFT_ASSIGN";
        case SLTok_QUESTION:      return "QUESTION";
        case SLTok_NULL:          return "NULL";
    }
    return "UNKNOWN";
}

int SLLex(SLArena* arena, SLStrView src, SLTokenStream* out, SLDiag* diag) {
    SLTokenBuf  tokbuf;
    uint32_t    pos = 0;
    int         insertedEOFSemicolon = 0;
    SLTokenKind prevKind = SLTok_INVALID;

    if (diag != NULL) {
        *diag = (SLDiag){ 0 };
    }
    out->v = NULL;
    out->len = 0;

    tokbuf.len = 0;
    tokbuf.cap = src.len + 2;
    if (tokbuf.cap < 8) {
        tokbuf.cap = 8;
    }
    tokbuf.v = (SLToken*)SLArenaAlloc(
        arena, tokbuf.cap * (uint32_t)sizeof(SLToken), (uint32_t)_Alignof(SLToken));
    if (tokbuf.v == NULL) {
        SLSetDiag(diag, SLDiag_ARENA_OOM, 0, 0);
        return -1;
    }

    for (;;) {
        int      sawNewline = 0;
        uint32_t newlinePos = 0;

        for (;;) {
            unsigned char c;
            if (pos >= src.len) {
                break;
            }

            c = (unsigned char)src.ptr[pos];
            if (c == (unsigned char)' ' || c == (unsigned char)'\t' || c == (unsigned char)'\r'
                || c == (unsigned char)'\f' || c == (unsigned char)'\v')
            {
                pos++;
                continue;
            }

            if (c == (unsigned char)'\n') {
                if (!sawNewline) {
                    sawNewline = 1;
                    newlinePos = pos;
                }
                pos++;
                continue;
            }

            if (c == (unsigned char)'/' && pos + 1 < src.len
                && (unsigned char)src.ptr[pos + 1] == (unsigned char)'/')
            {
                pos += 2;
                while (pos < src.len && (unsigned char)src.ptr[pos] != (unsigned char)'\n') {
                    pos++;
                }
                continue;
            }
            if (c == (unsigned char)'/' && pos + 1u < src.len
                && (unsigned char)src.ptr[pos + 1u] == (unsigned char)'*')
            {
                uint32_t commentStart = pos;
                if (SLSkipBlockComment(src, &pos, &sawNewline, &newlinePos) != 0) {
                    SLSetDiag(diag, SLDiag_UNTERMINATED_BLOCK_COMMENT, commentStart, pos);
                    return -1;
                }
                continue;
            }
            break;
        }

        if (sawNewline && SLTokenCanEndStmt(prevKind)) {
            if (SLPushToken(&tokbuf, diag, SLTok_SEMICOLON, newlinePos, newlinePos) != 0) {
                return -1;
            }
            prevKind = SLTok_SEMICOLON;
            continue;
        }

        if (pos >= src.len) {
            if (!insertedEOFSemicolon && SLTokenCanEndStmt(prevKind)) {
                if (SLPushToken(&tokbuf, diag, SLTok_SEMICOLON, src.len, src.len) != 0) {
                    return -1;
                }
                prevKind = SLTok_SEMICOLON;
                insertedEOFSemicolon = 1;
                continue;
            }
            if (SLPushToken(&tokbuf, diag, SLTok_EOF, src.len, src.len) != 0) {
                return -1;
            }
            break;
        }

        {
            SLTokenKind   kind = SLTok_INVALID;
            uint32_t      start = pos;
            unsigned char c = (unsigned char)src.ptr[pos];

            if (SLIsAlpha(c) || c == (unsigned char)'_') {
                pos++;
                while (pos < src.len) {
                    c = (unsigned char)src.ptr[pos];
                    if (!SLIsAlnum(c) && c != (unsigned char)'_') {
                        break;
                    }
                    pos++;
                }
                kind = SLKeywordKind(src.ptr + start, pos - start);
            } else if (SLIsDigit(c)) {
                kind = SLTok_INT;

                if (c == (unsigned char)'0' && pos + 1 < src.len
                    && ((unsigned char)src.ptr[pos + 1] == (unsigned char)'x'
                        || (unsigned char)src.ptr[pos + 1] == (unsigned char)'X'))
                {
                    uint32_t digitsStart;
                    pos += 2;
                    digitsStart = pos;
                    while (pos < src.len && SLIsHexDigit((unsigned char)src.ptr[pos])) {
                        pos++;
                    }
                    if (pos == digitsStart) {
                        SLSetDiag(diag, SLDiag_INVALID_NUMBER, start, pos);
                        return -1;
                    }
                } else {
                    while (pos < src.len && SLIsDigit((unsigned char)src.ptr[pos])) {
                        pos++;
                    }

                    if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'.') {
                        kind = SLTok_FLOAT;
                        pos++;
                        while (pos < src.len && SLIsDigit((unsigned char)src.ptr[pos])) {
                            pos++;
                        }
                    }

                    if (pos < src.len
                        && ((unsigned char)src.ptr[pos] == (unsigned char)'e'
                            || (unsigned char)src.ptr[pos] == (unsigned char)'E'))
                    {
                        uint32_t expStart;
                        kind = SLTok_FLOAT;
                        pos++;
                        if (pos < src.len
                            && ((unsigned char)src.ptr[pos] == (unsigned char)'+'
                                || (unsigned char)src.ptr[pos] == (unsigned char)'-'))
                        {
                            pos++;
                        }
                        expStart = pos;
                        while (pos < src.len && SLIsDigit((unsigned char)src.ptr[pos])) {
                            pos++;
                        }
                        if (pos == expStart) {
                            SLSetDiag(diag, SLDiag_INVALID_NUMBER, start, pos);
                            return -1;
                        }
                    }
                }
            } else if (c == (unsigned char)'\'') {
                kind = SLTok_RUNE;
                pos++;
                while (pos < src.len) {
                    c = (unsigned char)src.ptr[pos];
                    if (c == (unsigned char)'\'') {
                        pos++;
                        break;
                    }
                    if (c == (unsigned char)'\\') {
                        pos++;
                        if (pos >= src.len) {
                            SLSetDiag(diag, SLDiag_UNTERMINATED_RUNE, start, pos);
                            return -1;
                        }
                        if ((unsigned char)src.ptr[pos] == (unsigned char)'\r' && pos + 1u < src.len
                            && (unsigned char)src.ptr[pos + 1u] == (unsigned char)'\n')
                        {
                            pos += 2u;
                        } else {
                            pos++;
                        }
                        continue;
                    }
                    pos++;
                }
                if (pos > src.len || (unsigned char)src.ptr[pos - 1u] != (unsigned char)'\'') {
                    SLSetDiag(diag, SLDiag_UNTERMINATED_RUNE, start, pos);
                    return -1;
                }
                {
                    SLRuneLitErr runeErr = { 0 };
                    uint32_t     rune = 0;
                    if (SLDecodeRuneLiteralValidate(src.ptr, start, pos, &rune, &runeErr) != 0) {
                        SLSetDiag(
                            diag, SLRuneLitErrDiagCode(runeErr.kind), runeErr.start, runeErr.end);
                        return -1;
                    }
                }
            } else if (c == (unsigned char)'"' || c == (unsigned char)'`') {
                unsigned char quote = c;
                kind = SLTok_STRING;
                pos++;
                while (pos < src.len) {
                    c = (unsigned char)src.ptr[pos];
                    if (quote == (unsigned char)'`') {
                        if (c == (unsigned char)'\\' && pos + 1u < src.len
                            && (unsigned char)src.ptr[pos + 1u] == (unsigned char)'`')
                        {
                            pos += 2u;
                            continue;
                        }
                        if (c == (unsigned char)'`') {
                            pos++;
                            break;
                        }
                        pos++;
                        continue;
                    }
                    if (c == quote) {
                        pos++;
                        break;
                    }
                    if (c == (unsigned char)'\\') {
                        pos++;
                        if (pos >= src.len) {
                            SLSetDiag(diag, SLDiag_UNTERMINATED_STRING, start, pos);
                            return -1;
                        }
                        if ((unsigned char)src.ptr[pos] == (unsigned char)'\r' && pos + 1u < src.len
                            && (unsigned char)src.ptr[pos + 1u] == (unsigned char)'\n')
                        {
                            pos += 2u;
                        } else {
                            pos++;
                        }
                        continue;
                    }
                    pos++;
                }
                if (pos > src.len || (unsigned char)src.ptr[pos - 1u] != quote) {
                    SLSetDiag(diag, SLDiag_UNTERMINATED_STRING, start, pos);
                    return -1;
                }
                {
                    SLStringLitErr litErr = { 0 };
                    if (SLDecodeStringLiteralValidate(src.ptr, start, pos, &litErr) != 0) {
                        SLSetDiag(
                            diag, SLStringLitErrDiagCode(litErr.kind), litErr.start, litErr.end);
                        return -1;
                    }
                }
            } else {
                pos++;
                switch (c) {
                    case (unsigned char)'(': kind = SLTok_LPAREN; break;
                    case (unsigned char)')': kind = SLTok_RPAREN; break;
                    case (unsigned char)'{': kind = SLTok_LBRACE; break;
                    case (unsigned char)'}': kind = SLTok_RBRACE; break;
                    case (unsigned char)'[': kind = SLTok_LBRACK; break;
                    case (unsigned char)']': kind = SLTok_RBRACK; break;
                    case (unsigned char)',': kind = SLTok_COMMA; break;
                    case (unsigned char)'.':
                        if (pos + 1u < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'.'
                            && (unsigned char)src.ptr[pos + 1u] == (unsigned char)'.')
                        {
                            pos += 2u;
                            kind = SLTok_ELLIPSIS;
                        } else {
                            kind = SLTok_DOT;
                        }
                        break;
                    case (unsigned char)';': kind = SLTok_SEMICOLON; break;
                    case (unsigned char)':': kind = SLTok_COLON; break;

                    case (unsigned char)'+':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = SLTok_ADD_ASSIGN;
                        } else {
                            kind = SLTok_ADD;
                        }
                        break;
                    case (unsigned char)'-':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = SLTok_SUB_ASSIGN;
                        } else {
                            kind = SLTok_SUB;
                        }
                        break;
                    case (unsigned char)'*':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = SLTok_MUL_ASSIGN;
                        } else {
                            kind = SLTok_MUL;
                        }
                        break;
                    case (unsigned char)'/':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = SLTok_DIV_ASSIGN;
                        } else {
                            kind = SLTok_DIV;
                        }
                        break;
                    case (unsigned char)'%':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = SLTok_MOD_ASSIGN;
                        } else {
                            kind = SLTok_MOD;
                        }
                        break;
                    case (unsigned char)'&':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'&') {
                            pos++;
                            kind = SLTok_LOGICAL_AND;
                        } else if (
                            pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=')
                        {
                            pos++;
                            kind = SLTok_AND_ASSIGN;
                        } else {
                            kind = SLTok_AND;
                        }
                        break;
                    case (unsigned char)'|':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'|') {
                            pos++;
                            kind = SLTok_LOGICAL_OR;
                        } else if (
                            pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=')
                        {
                            pos++;
                            kind = SLTok_OR_ASSIGN;
                        } else {
                            kind = SLTok_OR;
                        }
                        break;
                    case (unsigned char)'^':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = SLTok_XOR_ASSIGN;
                        } else {
                            kind = SLTok_XOR;
                        }
                        break;
                    case (unsigned char)'?': kind = SLTok_QUESTION; break;
                    case (unsigned char)'!':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = SLTok_NEQ;
                        } else {
                            kind = SLTok_NOT;
                        }
                        break;
                    case (unsigned char)'=':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = SLTok_EQ;
                        } else {
                            kind = SLTok_ASSIGN;
                        }
                        break;
                    case (unsigned char)'<':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'<') {
                            pos++;
                            if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=')
                            {
                                pos++;
                                kind = SLTok_LSHIFT_ASSIGN;
                            } else {
                                kind = SLTok_LSHIFT;
                            }
                        } else if (
                            pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=')
                        {
                            pos++;
                            kind = SLTok_LTE;
                        } else {
                            kind = SLTok_LT;
                        }
                        break;
                    case (unsigned char)'>':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'>') {
                            pos++;
                            if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=')
                            {
                                pos++;
                                kind = SLTok_RSHIFT_ASSIGN;
                            } else {
                                kind = SLTok_RSHIFT;
                            }
                        } else if (
                            pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=')
                        {
                            pos++;
                            kind = SLTok_GTE;
                        } else {
                            kind = SLTok_GT;
                        }
                        break;

                    default: SLSetDiag(diag, SLDiag_UNEXPECTED_CHAR, start, pos); return -1;
                }
            }

            if (SLPushToken(&tokbuf, diag, kind, start, pos) != 0) {
                return -1;
            }
            prevKind = kind;
        }
    }

    out->v = tokbuf.v;
    out->len = tokbuf.len;
    return 0;
}

static void SLWWrite(SLWriter* w, const char* s, uint32_t len) {
    w->write(w->ctx, s, len);
}

static void SLWCStr(SLWriter* w, const char* s) {
    uint32_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    SLWWrite(w, s, n);
}

static void SLWU32(SLWriter* w, uint32_t v) {
    char     buf[16];
    uint32_t n = 0;
    if (v == 0) {
        SLWWrite(w, "0", 1);
        return;
    }
    while (v > 0 && n < (uint32_t)sizeof(buf)) {
        buf[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0) {
        n--;
        SLWWrite(w, &buf[n], 1);
    }
}

static void SLWIndent(SLWriter* w, uint32_t depth) {
    uint32_t i;
    for (i = 0; i < depth; i++) {
        SLWWrite(w, "  ", 2);
    }
}

static void SLWEscaped(SLWriter* w, SLStrView src, uint32_t start, uint32_t end) {
    uint32_t i;
    SLWWrite(w, "\"", 1);
    for (i = start; i < end && i < src.len; i++) {
        unsigned char c = (unsigned char)src.ptr[i];
        switch (c) {
            case '\"': SLWWrite(w, "\\\"", 2); break;
            case '\\': SLWWrite(w, "\\\\", 2); break;
            case '\n': SLWWrite(w, "\\n", 2); break;
            case '\r': SLWWrite(w, "\\r", 2); break;
            case '\t': SLWWrite(w, "\\t", 2); break;
            default:
                if (c >= 0x20 && c <= 0x7e) {
                    SLWWrite(w, (const char*)&src.ptr[i], 1);
                } else {
                    char               hex[4];
                    static const char* digits = "0123456789abcdef";
                    hex[0] = '\\';
                    hex[1] = 'x';
                    hex[2] = digits[(c >> 4) & 0x0f];
                    hex[3] = digits[c & 0x0f];
                    SLWWrite(w, hex, 4);
                }
                break;
        }
    }
    SLWWrite(w, "\"", 1);
}

static int SLAstDumpNode(
    const SLAst* ast, int32_t idx, uint32_t depth, SLStrView src, SLWriter* w) {
    const SLAstNode* n;
    int32_t          c;
    if (idx < 0 || (uint32_t)idx >= ast->len) {
        return -1;
    }
    n = &ast->nodes[idx];
    SLWIndent(w, depth);
    SLWCStr(w, SLAstKindName(n->kind));

    if (n->op != 0) {
        SLWCStr(w, " op=");
        SLWCStr(w, SLTokenKindName((SLTokenKind)n->op));
    }
    if (n->flags != 0) {
        SLWCStr(w, " flags=");
        SLWU32(w, n->flags);
    }
    if (n->dataEnd > n->dataStart) {
        SLWCStr(w, " ");
        SLWEscaped(w, src, n->dataStart, n->dataEnd);
    }
    SLWCStr(w, " [");
    SLWU32(w, n->start);
    SLWCStr(w, ",");
    SLWU32(w, n->end);
    SLWCStr(w, "]\n");

    c = n->firstChild;
    while (c >= 0) {
        if (SLAstDumpNode(ast, c, depth + 1, src, w) != 0) {
            return -1;
        }
        c = ast->nodes[c].nextSibling;
    }
    return 0;
}

int SLAstDump(const SLAst* ast, SLStrView src, SLWriter* w, SLDiag* diag) {
    if (diag != NULL) {
        *diag = (SLDiag){ 0 };
    }
    if (ast == NULL || w == NULL || w->write == NULL || ast->nodes == NULL || ast->root < 0) {
        SLSetDiag(diag, SLDiag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }
    return SLAstDumpNode(ast, ast->root, 0, src, w);
}

SL_API_END
