#include "libhop-impl.h"
HOP_API_BEGIN

typedef struct {
    HOPToken* v;
    uint32_t  len;
    uint32_t  cap;
} HOPTokenBuf;

static void HOPSetDiag(HOPDiag* diag, HOPDiagCode code, uint32_t start, uint32_t end) {
    if (diag == NULL) {
        return;
    }
    diag->code = code;
    diag->type = HOPDiagTypeOfCode(code);
    diag->start = start;
    diag->end = end;
    diag->argStart = 0;
    diag->argEnd = 0;
    diag->relatedStart = 0;
    diag->relatedEnd = 0;
    diag->detail = NULL;
    diag->hintOverride = NULL;
}

void HOPDiagClear(HOPDiag* _Nullable diag) {
    if (diag == NULL) {
        return;
    }
    diag->code = HOPDiag_NONE;
    diag->type = HOPDiagType_ERROR;
    diag->start = 0;
    diag->end = 0;
    diag->argStart = 0;
    diag->argEnd = 0;
    diag->relatedStart = 0;
    diag->relatedEnd = 0;
    diag->detail = NULL;
    diag->hintOverride = NULL;
}

static uint32_t HOPArenaAlignUpU32(uint32_t value, uint32_t align) {
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

static uint32_t HOPArenaGrowHeaderSize(void) {
    uint32_t align = (uint32_t)_Alignof(max_align_t);
    return HOPArenaAlignUpU32((uint32_t)sizeof(HOPArenaBlock), align);
}

static void HOPArenaInitBlock(
    HOPArenaBlock* block,
    uint8_t* _Nullable mem,
    uint32_t cap,
    uint32_t allocSize,
    uint8_t  owned,
    HOPArenaBlock* _Nullable next) {
    block->mem = mem;
    block->cap = cap;
    block->len = 0;
    block->allocSize = allocSize;
    block->next = next;
    block->owned = owned;
}

static void* _Nullable HOPArenaTryAllocInBlock(
    HOPArenaBlock* block, uint32_t size, uint32_t align) {
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

void HOPArenaInit(HOPArena* arena, void* storage, uint32_t storageSize) {
    HOPArenaInitEx(arena, storage, storageSize, NULL, NULL, NULL);
}

void HOPArenaInitEx(
    HOPArena* arena,
    void* _Nullable storage,
    uint32_t storageSize,
    void* _Nullable allocatorCtx,
    HOPArenaGrowFn _Nullable growFn,
    HOPArenaFreeFn _Nullable freeFn) {
    arena->allocatorCtx = allocatorCtx;
    arena->grow = growFn;
    arena->free = freeFn;
    HOPArenaInitBlock(&arena->inlineBlock, (uint8_t*)storage, storageSize, storageSize, 0, NULL);
    arena->first = &arena->inlineBlock;
    arena->current = &arena->inlineBlock;
}

void HOPArenaSetAllocator(
    HOPArena* arena,
    void* _Nullable allocatorCtx,
    HOPArenaGrowFn _Nullable growFn,
    HOPArenaFreeFn _Nullable freeFn) {
    arena->allocatorCtx = allocatorCtx;
    arena->grow = growFn;
    arena->free = freeFn;
}

void HOPArenaReset(HOPArena* arena) {
    HOPArenaBlock* block = arena->first;
    while (block != NULL) {
        block->len = 0;
        block = block->next;
    }
    arena->current = arena->first;
}

void HOPArenaDispose(HOPArena* arena) {
    HOPArenaBlock* block;

    if (arena->first == NULL) {
        return;
    }

    block = arena->first->next;
    while (block != NULL) {
        HOPArenaBlock* next = block->next;
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

void* _Nullable HOPArenaAlloc(HOPArena* arena, uint32_t size, uint32_t align) {
    HOPArenaBlock* block;

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
        void* p = HOPArenaTryAllocInBlock(block, size, align);
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
        uint32_t headerSize = HOPArenaGrowHeaderSize();
        uint32_t minPayload;
        uint32_t targetPayload;
        uint64_t requestedSize64;
        uint32_t requestedSize;
        uint32_t allocSize = 0;
        void* _Nullable allocMem;
        HOPArenaBlock* newBlock;
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

        newBlock = (HOPArenaBlock*)allocMem;
        HOPArenaInitBlock(
            newBlock, (uint8_t*)allocMem + headerSize, allocSize - headerSize, allocSize, 1, NULL);
        block->next = newBlock;
        arena->current = newBlock;
        p = HOPArenaTryAllocInBlock(newBlock, size, align);
        if (p != NULL) {
            return p;
        }
    }

    return NULL;
}

static int HOPIsAlpha(unsigned char c) {
    return (c >= (unsigned char)'A' && c <= (unsigned char)'Z')
        || (c >= (unsigned char)'a' && c <= (unsigned char)'z');
}

static int HOPIsDigit(unsigned char c) {
    return c >= (unsigned char)'0' && c <= (unsigned char)'9';
}

static int HOPIsAlnum(unsigned char c) {
    return HOPIsAlpha(c) || HOPIsDigit(c);
}

static int HOPIsAsciiSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static int HOPIsHexDigit(unsigned char c) {
    return HOPIsDigit(c) || (c >= (unsigned char)'a' && c <= (unsigned char)'f')
        || (c >= (unsigned char)'A' && c <= (unsigned char)'F');
}

static int HOPStrEq(const char* a, uint32_t aLen, const char* b) {
    uint32_t i = 0;
    while (i < aLen) {
        if (b[i] == '\0' || a[i] != b[i]) {
            return 0;
        }
        i++;
    }
    return b[i] == '\0';
}

static int HOPIsValidImportPathChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'
        || c == '.' || c == '/' || c == '-';
}

int HOPNormalizeImportPath(
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
    if (HOPIsAsciiSpace(importPath[0]) || HOPIsAsciiSpace(importPath[len - 1u])) {
        if (outErrReason != NULL) {
            *outErrReason = "leading/trailing whitespace";
        }
        return -1;
    }

    for (i = 0; i < len; i++) {
        if (!HOPIsValidImportPathChar(importPath[i])) {
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

static HOPTokenKind HOPKeywordKind(const char* s, uint32_t len) {
    if (len == 2) {
        if (HOPStrEq(s, len, "as")) {
            return HOPTok_AS;
        }
        if (HOPStrEq(s, len, "in")) {
            return HOPTok_IN;
        }
        if (HOPStrEq(s, len, "fn")) {
            return HOPTok_FN;
        }
        if (HOPStrEq(s, len, "if")) {
            return HOPTok_IF;
        }
    } else if (len == 3) {
        if (HOPStrEq(s, len, "for")) {
            return HOPTok_FOR;
        }
        if (HOPStrEq(s, len, "new")) {
            return HOPTok_NEW;
        }
        if (HOPStrEq(s, len, "del")) {
            return HOPTok_DEL;
        }
        if (HOPStrEq(s, len, "var")) {
            return HOPTok_VAR;
        }
        if (HOPStrEq(s, len, "mut")) {
            return HOPTok_MUT;
        }
        if (HOPStrEq(s, len, "pub")) {
            return HOPTok_PUB;
        }
    } else if (len == 4) {
        if (HOPStrEq(s, len, "enum")) {
            return HOPTok_ENUM;
        }
        if (HOPStrEq(s, len, "else")) {
            return HOPTok_ELSE;
        }
        if (HOPStrEq(s, len, "case")) {
            return HOPTok_CASE;
        }
        if (HOPStrEq(s, len, "true")) {
            return HOPTok_TRUE;
        }
        if (HOPStrEq(s, len, "null")) {
            return HOPTok_NULL;
        }
        if (HOPStrEq(s, len, "type")) {
            return HOPTok_TYPE;
        }
    } else if (len == 5) {
        if (HOPStrEq(s, len, "break")) {
            return HOPTok_BREAK;
        }
        if (HOPStrEq(s, len, "const")) {
            return HOPTok_CONST;
        }
        if (HOPStrEq(s, len, "defer")) {
            return HOPTok_DEFER;
        }
        if (HOPStrEq(s, len, "false")) {
            return HOPTok_FALSE;
        }
        if (HOPStrEq(s, len, "union")) {
            return HOPTok_UNION;
        }
    } else if (len == 6) {
        if (HOPStrEq(s, len, "import")) {
            return HOPTok_IMPORT;
        }
        if (HOPStrEq(s, len, "sizeof")) {
            return HOPTok_SIZEOF;
        }
        if (HOPStrEq(s, len, "return")) {
            return HOPTok_RETURN;
        }
        if (HOPStrEq(s, len, "switch")) {
            return HOPTok_SWITCH;
        }
        if (HOPStrEq(s, len, "struct")) {
            return HOPTok_STRUCT;
        }
        if (HOPStrEq(s, len, "assert")) {
            return HOPTok_ASSERT;
        }
    } else if (len == 7) {
        if (HOPStrEq(s, len, "anytype")) {
            return HOPTok_ANYTYPE;
        }
        if (HOPStrEq(s, len, "default")) {
            return HOPTok_DEFAULT;
        }
    } else if (len == 8) {
        if (HOPStrEq(s, len, "continue")) {
            return HOPTok_CONTINUE;
        }
    }
    return HOPTok_IDENT;
}

static int HOPTokenCanEndStmt(HOPTokenKind kind) {
    switch (kind) {
        case HOPTok_IDENT:
        case HOPTok_INT:
        case HOPTok_FLOAT:
        case HOPTok_STRING:
        case HOPTok_RUNE:
        case HOPTok_TRUE:
        case HOPTok_FALSE:
        case HOPTok_BREAK:
        case HOPTok_CONTINUE:
        case HOPTok_RETURN:
        case HOPTok_RPAREN:
        case HOPTok_RBRACK:
        case HOPTok_RBRACE:
        case HOPTok_NOT:
        case HOPTok_NULL:
        case HOPTok_TYPE:     return 1;
        default:              return 0;
    }
}

static int HOPPushToken(
    HOPTokenBuf* out, HOPDiag* diag, HOPTokenKind kind, uint32_t start, uint32_t end) {
    if (out->len >= out->cap) {
        HOPSetDiag(diag, HOPDiag_ARENA_OOM, start, end);
        return -1;
    }

    out->v[out->len].kind = kind;
    out->v[out->len].start = start;
    out->v[out->len].end = end;
    out->len++;
    return 0;
}

static int HOPSkipBlockComment(
    HOPStrView src, uint32_t* ioPos, int* ioSawNewline, uint32_t* ioNewlinePos) {
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

const char* HOPTokenKindName(HOPTokenKind kind) {
    switch (kind) {
        case HOPTok_INVALID:       return "INVALID";
        case HOPTok_EOF:           return "EOF";
        case HOPTok_IDENT:         return "IDENT";
        case HOPTok_INT:           return "INT";
        case HOPTok_FLOAT:         return "FLOAT";
        case HOPTok_STRING:        return "STRING";
        case HOPTok_RUNE:          return "RUNE";
        case HOPTok_IMPORT:        return "IMPORT";
        case HOPTok_PUB:           return "PUB";
        case HOPTok_STRUCT:        return "STRUCT";
        case HOPTok_UNION:         return "UNION";
        case HOPTok_ENUM:          return "ENUM";
        case HOPTok_FN:            return "FN";
        case HOPTok_VAR:           return "VAR";
        case HOPTok_CONST:         return "CONST";
        case HOPTok_TYPE:          return "TYPE";
        case HOPTok_MUT:           return "MUT";
        case HOPTok_IF:            return "IF";
        case HOPTok_ELSE:          return "ELSE";
        case HOPTok_FOR:           return "FOR";
        case HOPTok_SWITCH:        return "SWITCH";
        case HOPTok_CASE:          return "CASE";
        case HOPTok_DEFAULT:       return "DEFAULT";
        case HOPTok_BREAK:         return "BREAK";
        case HOPTok_CONTINUE:      return "CONTINUE";
        case HOPTok_RETURN:        return "RETURN";
        case HOPTok_DEFER:         return "DEFER";
        case HOPTok_ASSERT:        return "ASSERT";
        case HOPTok_SIZEOF:        return "SIZEOF";
        case HOPTok_NEW:           return "NEW";
        case HOPTok_DEL:           return "DEL";
        case HOPTok_TRUE:          return "TRUE";
        case HOPTok_FALSE:         return "FALSE";
        case HOPTok_IN:            return "IN";
        case HOPTok_AS:            return "AS";
        case HOPTok_CONTEXT:       return "CONTEXT";
        case HOPTok_ANYTYPE:       return "ANYTYPE";
        case HOPTok_LPAREN:        return "LPAREN";
        case HOPTok_RPAREN:        return "RPAREN";
        case HOPTok_LBRACE:        return "LBRACE";
        case HOPTok_RBRACE:        return "RBRACE";
        case HOPTok_LBRACK:        return "LBRACK";
        case HOPTok_RBRACK:        return "RBRACK";
        case HOPTok_COMMA:         return "COMMA";
        case HOPTok_DOT:           return "DOT";
        case HOPTok_ELLIPSIS:      return "ELLIPSIS";
        case HOPTok_SEMICOLON:     return "SEMICOLON";
        case HOPTok_COLON:         return "COLON";
        case HOPTok_AT:            return "AT";
        case HOPTok_SHORT_ASSIGN:  return "SHORT_ASSIGN";
        case HOPTok_ASSIGN:        return "ASSIGN";
        case HOPTok_ADD:           return "ADD";
        case HOPTok_SUB:           return "SUB";
        case HOPTok_MUL:           return "MUL";
        case HOPTok_DIV:           return "DIV";
        case HOPTok_MOD:           return "MOD";
        case HOPTok_AND:           return "AND";
        case HOPTok_OR:            return "OR";
        case HOPTok_XOR:           return "XOR";
        case HOPTok_NOT:           return "NOT";
        case HOPTok_LSHIFT:        return "LSHIFT";
        case HOPTok_RSHIFT:        return "RSHIFT";
        case HOPTok_EQ:            return "EQ";
        case HOPTok_NEQ:           return "NEQ";
        case HOPTok_LT:            return "LT";
        case HOPTok_GT:            return "GT";
        case HOPTok_LTE:           return "LTE";
        case HOPTok_GTE:           return "GTE";
        case HOPTok_LOGICAL_AND:   return "LOGICAL_AND";
        case HOPTok_LOGICAL_OR:    return "LOGICAL_OR";
        case HOPTok_ADD_ASSIGN:    return "ADD_ASSIGN";
        case HOPTok_SUB_ASSIGN:    return "SUB_ASSIGN";
        case HOPTok_MUL_ASSIGN:    return "MUL_ASSIGN";
        case HOPTok_DIV_ASSIGN:    return "DIV_ASSIGN";
        case HOPTok_MOD_ASSIGN:    return "MOD_ASSIGN";
        case HOPTok_AND_ASSIGN:    return "AND_ASSIGN";
        case HOPTok_OR_ASSIGN:     return "OR_ASSIGN";
        case HOPTok_XOR_ASSIGN:    return "XOR_ASSIGN";
        case HOPTok_LSHIFT_ASSIGN: return "LSHIFT_ASSIGN";
        case HOPTok_RSHIFT_ASSIGN: return "RSHIFT_ASSIGN";
        case HOPTok_QUESTION:      return "QUESTION";
        case HOPTok_NULL:          return "NULL";
    }
    return "UNKNOWN";
}

int HOPLex(HOPArena* arena, HOPStrView src, HOPTokenStream* out, HOPDiag* _Nullable diag) {
    HOPTokenBuf  tokbuf;
    uint32_t     pos = 0;
    int          insertedEOFSemicolon = 0;
    HOPTokenKind prevKind = HOPTok_INVALID;

    if (diag != NULL) {
        *diag = (HOPDiag){ 0 };
    }
    out->v = NULL;
    out->len = 0;

    tokbuf.len = 0;
    tokbuf.cap = src.len + 2;
    if (tokbuf.cap < 8) {
        tokbuf.cap = 8;
    }
    tokbuf.v = (HOPToken*)HOPArenaAlloc(
        arena, tokbuf.cap * (uint32_t)sizeof(HOPToken), (uint32_t)_Alignof(HOPToken));
    if (tokbuf.v == NULL) {
        HOPSetDiag(diag, HOPDiag_ARENA_OOM, 0, 0);
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
                if (HOPSkipBlockComment(src, &pos, &sawNewline, &newlinePos) != 0) {
                    HOPSetDiag(diag, HOPDiag_UNTERMINATED_BLOCK_COMMENT, commentStart, pos);
                    return -1;
                }
                continue;
            }
            break;
        }

        if (sawNewline && HOPTokenCanEndStmt(prevKind)) {
            if (HOPPushToken(&tokbuf, diag, HOPTok_SEMICOLON, newlinePos, newlinePos) != 0) {
                return -1;
            }
            prevKind = HOPTok_SEMICOLON;
            continue;
        }

        if (pos >= src.len) {
            if (!insertedEOFSemicolon && HOPTokenCanEndStmt(prevKind)) {
                if (HOPPushToken(&tokbuf, diag, HOPTok_SEMICOLON, src.len, src.len) != 0) {
                    return -1;
                }
                prevKind = HOPTok_SEMICOLON;
                insertedEOFSemicolon = 1;
                continue;
            }
            if (HOPPushToken(&tokbuf, diag, HOPTok_EOF, src.len, src.len) != 0) {
                return -1;
            }
            break;
        }

        {
            HOPTokenKind  kind = HOPTok_INVALID;
            uint32_t      start = pos;
            unsigned char c = (unsigned char)src.ptr[pos];

            if (HOPIsAlpha(c) || c == (unsigned char)'_') {
                pos++;
                while (pos < src.len) {
                    c = (unsigned char)src.ptr[pos];
                    if (!HOPIsAlnum(c) && c != (unsigned char)'_') {
                        break;
                    }
                    pos++;
                }
                kind = HOPKeywordKind(src.ptr + start, pos - start);
            } else if (HOPIsDigit(c)) {
                kind = HOPTok_INT;

                if (c == (unsigned char)'0' && pos + 1 < src.len
                    && ((unsigned char)src.ptr[pos + 1] == (unsigned char)'x'
                        || (unsigned char)src.ptr[pos + 1] == (unsigned char)'X'))
                {
                    uint32_t digitsStart;
                    pos += 2;
                    digitsStart = pos;
                    while (pos < src.len && HOPIsHexDigit((unsigned char)src.ptr[pos])) {
                        pos++;
                    }
                    if (pos == digitsStart) {
                        HOPSetDiag(diag, HOPDiag_INVALID_NUMBER, start, pos);
                        return -1;
                    }
                } else {
                    while (pos < src.len && HOPIsDigit((unsigned char)src.ptr[pos])) {
                        pos++;
                    }

                    if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'.') {
                        kind = HOPTok_FLOAT;
                        pos++;
                        while (pos < src.len && HOPIsDigit((unsigned char)src.ptr[pos])) {
                            pos++;
                        }
                    }

                    if (pos < src.len
                        && ((unsigned char)src.ptr[pos] == (unsigned char)'e'
                            || (unsigned char)src.ptr[pos] == (unsigned char)'E'))
                    {
                        uint32_t expStart;
                        kind = HOPTok_FLOAT;
                        pos++;
                        if (pos < src.len
                            && ((unsigned char)src.ptr[pos] == (unsigned char)'+'
                                || (unsigned char)src.ptr[pos] == (unsigned char)'-'))
                        {
                            pos++;
                        }
                        expStart = pos;
                        while (pos < src.len && HOPIsDigit((unsigned char)src.ptr[pos])) {
                            pos++;
                        }
                        if (pos == expStart) {
                            HOPSetDiag(diag, HOPDiag_INVALID_NUMBER, start, pos);
                            return -1;
                        }
                    }
                }
            } else if (c == (unsigned char)'\'') {
                kind = HOPTok_RUNE;
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
                            HOPSetDiag(diag, HOPDiag_UNTERMINATED_RUNE, start, pos);
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
                    HOPSetDiag(diag, HOPDiag_UNTERMINATED_RUNE, start, pos);
                    return -1;
                }
                {
                    HOPRuneLitErr runeErr = { 0 };
                    uint32_t      rune = 0;
                    if (HOPDecodeRuneLiteralValidate(src.ptr, start, pos, &rune, &runeErr) != 0) {
                        HOPSetDiag(
                            diag, HOPRuneLitErrDiagCode(runeErr.kind), runeErr.start, runeErr.end);
                        return -1;
                    }
                }
            } else if (c == (unsigned char)'"' || c == (unsigned char)'`') {
                unsigned char quote = c;
                kind = HOPTok_STRING;
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
                            HOPSetDiag(diag, HOPDiag_UNTERMINATED_STRING, start, pos);
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
                    HOPSetDiag(diag, HOPDiag_UNTERMINATED_STRING, start, pos);
                    return -1;
                }
                {
                    HOPStringLitErr litErr = { 0 };
                    if (HOPDecodeStringLiteralValidate(src.ptr, start, pos, &litErr) != 0) {
                        HOPSetDiag(
                            diag, HOPStringLitErrDiagCode(litErr.kind), litErr.start, litErr.end);
                        return -1;
                    }
                }
            } else {
                pos++;
                switch (c) {
                    case (unsigned char)'(': kind = HOPTok_LPAREN; break;
                    case (unsigned char)')': kind = HOPTok_RPAREN; break;
                    case (unsigned char)'{': kind = HOPTok_LBRACE; break;
                    case (unsigned char)'}': kind = HOPTok_RBRACE; break;
                    case (unsigned char)'[': kind = HOPTok_LBRACK; break;
                    case (unsigned char)']': kind = HOPTok_RBRACK; break;
                    case (unsigned char)',': kind = HOPTok_COMMA; break;
                    case (unsigned char)'.':
                        if (pos + 1u < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'.'
                            && (unsigned char)src.ptr[pos + 1u] == (unsigned char)'.')
                        {
                            pos += 2u;
                            kind = HOPTok_ELLIPSIS;
                        } else {
                            kind = HOPTok_DOT;
                        }
                        break;
                    case (unsigned char)';': kind = HOPTok_SEMICOLON; break;
                    case (unsigned char)':':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = HOPTok_SHORT_ASSIGN;
                        } else {
                            kind = HOPTok_COLON;
                        }
                        break;
                    case (unsigned char)'@': kind = HOPTok_AT; break;

                    case (unsigned char)'+':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = HOPTok_ADD_ASSIGN;
                        } else {
                            kind = HOPTok_ADD;
                        }
                        break;
                    case (unsigned char)'-':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = HOPTok_SUB_ASSIGN;
                        } else {
                            kind = HOPTok_SUB;
                        }
                        break;
                    case (unsigned char)'*':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = HOPTok_MUL_ASSIGN;
                        } else {
                            kind = HOPTok_MUL;
                        }
                        break;
                    case (unsigned char)'/':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = HOPTok_DIV_ASSIGN;
                        } else {
                            kind = HOPTok_DIV;
                        }
                        break;
                    case (unsigned char)'%':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = HOPTok_MOD_ASSIGN;
                        } else {
                            kind = HOPTok_MOD;
                        }
                        break;
                    case (unsigned char)'&':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'&') {
                            pos++;
                            kind = HOPTok_LOGICAL_AND;
                        } else if (
                            pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=')
                        {
                            pos++;
                            kind = HOPTok_AND_ASSIGN;
                        } else {
                            kind = HOPTok_AND;
                        }
                        break;
                    case (unsigned char)'|':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'|') {
                            pos++;
                            kind = HOPTok_LOGICAL_OR;
                        } else if (
                            pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=')
                        {
                            pos++;
                            kind = HOPTok_OR_ASSIGN;
                        } else {
                            kind = HOPTok_OR;
                        }
                        break;
                    case (unsigned char)'^':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = HOPTok_XOR_ASSIGN;
                        } else {
                            kind = HOPTok_XOR;
                        }
                        break;
                    case (unsigned char)'?': kind = HOPTok_QUESTION; break;
                    case (unsigned char)'!':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = HOPTok_NEQ;
                        } else {
                            kind = HOPTok_NOT;
                        }
                        break;
                    case (unsigned char)'=':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = HOPTok_EQ;
                        } else {
                            kind = HOPTok_ASSIGN;
                        }
                        break;
                    case (unsigned char)'<':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'<') {
                            pos++;
                            if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=')
                            {
                                pos++;
                                kind = HOPTok_LSHIFT_ASSIGN;
                            } else {
                                kind = HOPTok_LSHIFT;
                            }
                        } else if (
                            pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=')
                        {
                            pos++;
                            kind = HOPTok_LTE;
                        } else {
                            kind = HOPTok_LT;
                        }
                        break;
                    case (unsigned char)'>':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'>') {
                            pos++;
                            if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=')
                            {
                                pos++;
                                kind = HOPTok_RSHIFT_ASSIGN;
                            } else {
                                kind = HOPTok_RSHIFT;
                            }
                        } else if (
                            pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=')
                        {
                            pos++;
                            kind = HOPTok_GTE;
                        } else {
                            kind = HOPTok_GT;
                        }
                        break;

                    default: HOPSetDiag(diag, HOPDiag_UNEXPECTED_CHAR, start, pos); return -1;
                }
            }

            if (HOPPushToken(&tokbuf, diag, kind, start, pos) != 0) {
                return -1;
            }
            prevKind = kind;
        }
    }

    out->v = tokbuf.v;
    out->len = tokbuf.len;
    return 0;
}

static void HOPWWrite(HOPWriter* w, const char* s, uint32_t len) {
    w->write(w->ctx, s, len);
}

static void HOPWCStr(HOPWriter* w, const char* s) {
    uint32_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    HOPWWrite(w, s, n);
}

static void HOPWU32(HOPWriter* w, uint32_t v) {
    char     buf[16];
    uint32_t n = 0;
    if (v == 0) {
        HOPWWrite(w, "0", 1);
        return;
    }
    while (v > 0 && n < (uint32_t)sizeof(buf)) {
        buf[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0) {
        n--;
        HOPWWrite(w, &buf[n], 1);
    }
}

static void HOPWIndent(HOPWriter* w, uint32_t depth) {
    uint32_t i;
    for (i = 0; i < depth; i++) {
        HOPWWrite(w, "  ", 2);
    }
}

static void HOPWEscaped(HOPWriter* w, HOPStrView src, uint32_t start, uint32_t end) {
    uint32_t i;
    HOPWWrite(w, "\"", 1);
    for (i = start; i < end && i < src.len; i++) {
        unsigned char c = (unsigned char)src.ptr[i];
        switch (c) {
            case '\"': HOPWWrite(w, "\\\"", 2); break;
            case '\\': HOPWWrite(w, "\\\\", 2); break;
            case '\n': HOPWWrite(w, "\\n", 2); break;
            case '\r': HOPWWrite(w, "\\r", 2); break;
            case '\t': HOPWWrite(w, "\\t", 2); break;
            default:
                if (c >= 0x20 && c <= 0x7e) {
                    HOPWWrite(w, (const char*)&src.ptr[i], 1);
                } else {
                    char               hex[4];
                    static const char* digits = "0123456789abcdef";
                    hex[0] = '\\';
                    hex[1] = 'x';
                    hex[2] = digits[(c >> 4) & 0x0f];
                    hex[3] = digits[c & 0x0f];
                    HOPWWrite(w, hex, 4);
                }
                break;
        }
    }
    HOPWWrite(w, "\"", 1);
}

static int HOPAstDumpNode(
    const HOPAst* ast, int32_t idx, uint32_t depth, HOPStrView src, HOPWriter* w) {
    const HOPAstNode* n;
    int32_t           c;
    if (idx < 0 || (uint32_t)idx >= ast->len) {
        return -1;
    }
    n = &ast->nodes[idx];
    HOPWIndent(w, depth);
    HOPWCStr(w, HOPAstKindName(n->kind));

    if (n->op != 0) {
        HOPWCStr(w, " op=");
        HOPWCStr(w, HOPTokenKindName((HOPTokenKind)n->op));
    }
    if (n->flags != 0) {
        HOPWCStr(w, " flags=");
        HOPWU32(w, n->flags);
    }
    if (n->dataEnd > n->dataStart) {
        HOPWCStr(w, " ");
        HOPWEscaped(w, src, n->dataStart, n->dataEnd);
    }
    HOPWCStr(w, " [");
    HOPWU32(w, n->start);
    HOPWCStr(w, ",");
    HOPWU32(w, n->end);
    HOPWCStr(w, "]\n");

    c = n->firstChild;
    while (c >= 0) {
        if (HOPAstDumpNode(ast, c, depth + 1, src, w) != 0) {
            return -1;
        }
        c = ast->nodes[c].nextSibling;
    }
    return 0;
}

int HOPAstDump(const HOPAst* ast, HOPStrView src, HOPWriter* w, HOPDiag* _Nullable diag) {
    if (diag != NULL) {
        *diag = (HOPDiag){ 0 };
    }
    if (ast == NULL || w == NULL || w->write == NULL || ast->nodes == NULL || ast->root < 0) {
        HOPSetDiag(diag, HOPDiag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }
    return HOPAstDumpNode(ast, ast->root, 0, src, w);
}

HOP_API_END
