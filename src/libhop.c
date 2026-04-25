#include "libhop-impl.h"
H2_API_BEGIN

typedef struct {
    H2Token* v;
    uint32_t len;
    uint32_t cap;
} H2TokenBuf;

static void H2DiagResetExtras(H2Diag* diag) {
    diag->phase = H2DiagPhase_UNKNOWN;
    diag->groupId = 0;
    diag->isPrimary = 1;
    diag->_reserved[0] = 0;
    diag->_reserved[1] = 0;
    diag->_reserved[2] = 0;
    diag->notes = NULL;
    diag->notesLen = 0;
    diag->fixIts = NULL;
    diag->fixItsLen = 0;
    diag->expectations = NULL;
    diag->expectationsLen = 0;
}

static void H2SetDiag(H2Diag* diag, H2DiagCode code, uint32_t start, uint32_t end) {
    if (diag == NULL) {
        return;
    }
    diag->code = code;
    diag->type = H2DiagTypeOfCode(code);
    diag->start = start;
    diag->end = end;
    diag->argStart = 0;
    diag->argEnd = 0;
    diag->relatedStart = 0;
    diag->relatedEnd = 0;
    diag->detail = NULL;
    diag->hintOverride = NULL;
    H2DiagResetExtras(diag);
}

void H2DiagClear(H2Diag* _Nullable diag) {
    if (diag == NULL) {
        return;
    }
    diag->code = H2Diag_NONE;
    diag->type = H2DiagType_ERROR;
    diag->start = 0;
    diag->end = 0;
    diag->argStart = 0;
    diag->argEnd = 0;
    diag->relatedStart = 0;
    diag->relatedEnd = 0;
    diag->detail = NULL;
    diag->hintOverride = NULL;
    H2DiagResetExtras(diag);
}

static int H2DiagAppendItems(
    H2Arena*         arena,
    void* _Nullable* dstItems,
    uint32_t*        dstLen,
    uint32_t         itemSize,
    const void*      item) {
    uint32_t need;
    void*    out;
    if (dstItems == NULL || dstLen == NULL || item == NULL) {
        return -1;
    }
    if (arena == NULL) {
        return -1;
    }
    if (*dstLen == UINT32_MAX) {
        return -1;
    }
    need = *dstLen + 1u;
    out = H2ArenaAlloc(arena, need * itemSize, (uint32_t)_Alignof(max_align_t));
    if (out == NULL) {
        return -1;
    }
    if (*dstLen > 0 && *dstItems != NULL) {
        memcpy(out, *dstItems, (*dstLen) * itemSize);
    }
    memcpy((uint8_t*)out + (*dstLen) * itemSize, item, itemSize);
    *dstItems = out;
    *dstLen = need;
    return 0;
}

int H2DiagAddNote(
    H2Arena* _Nullable arena,
    H2Diag* _Nullable diag,
    H2DiagNoteKind kind,
    uint32_t       start,
    uint32_t       end,
    const char* _Nullable message) {
    return H2DiagAddNoteEx(arena, diag, kind, start, end, message, NULL, NULL);
}

int H2DiagAddNoteEx(
    H2Arena* _Nullable arena,
    H2Diag* _Nullable diag,
    H2DiagNoteKind kind,
    uint32_t       start,
    uint32_t       end,
    const char* _Nullable message,
    const char* _Nullable path,
    const char* _Nullable source) {
    H2DiagNote note;
    if (diag == NULL) {
        return 0;
    }
    note.kind = kind;
    note.start = start;
    note.end = end;
    note.message = message;
    note.path = path;
    note.source = source;
    if (H2DiagAppendItems(
            arena,
            (void* _Nullable*)&diag->notes,
            &diag->notesLen,
            (uint32_t)sizeof(H2DiagNote),
            &note)
        != 0)
    {
        return -1;
    }
    if (diag->relatedEnd <= diag->relatedStart) {
        diag->relatedStart = start;
        diag->relatedEnd = end;
    }
    return 0;
}

int H2DiagAddFixIt(
    H2Arena* _Nullable arena,
    H2Diag* _Nullable diag,
    H2DiagFixItKind kind,
    uint32_t        start,
    uint32_t        end,
    const char* _Nullable text) {
    H2DiagFixIt fixIt;
    if (diag == NULL) {
        return 0;
    }
    fixIt.kind = kind;
    fixIt.start = start;
    fixIt.end = end;
    fixIt.text = text;
    return H2DiagAppendItems(
        arena,
        (void* _Nullable*)&diag->fixIts,
        &diag->fixItsLen,
        (uint32_t)sizeof(H2DiagFixIt),
        &fixIt);
}

int H2DiagAddExpectation(
    H2Arena* _Nullable arena,
    H2Diag* _Nullable diag,
    H2DiagExpectationKind kind,
    const char* _Nullable text) {
    H2DiagExpectation expectation;
    if (diag == NULL) {
        return 0;
    }
    expectation.kind = kind;
    expectation.text = text;
    return H2DiagAppendItems(
        arena,
        (void* _Nullable*)&diag->expectations,
        &diag->expectationsLen,
        (uint32_t)sizeof(H2DiagExpectation),
        &expectation);
}

static uint32_t H2ArenaAlignUpU32(uint32_t value, uint32_t align) {
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

static uint32_t H2ArenaGrowHeaderSize(void) {
    uint32_t align = (uint32_t)_Alignof(max_align_t);
    return H2ArenaAlignUpU32((uint32_t)sizeof(H2ArenaBlock), align);
}

static void H2ArenaInitBlock(
    H2ArenaBlock* block,
    uint8_t* _Nullable mem,
    uint32_t cap,
    uint32_t allocSize,
    uint8_t  owned,
    H2ArenaBlock* _Nullable next) {
    block->mem = mem;
    block->cap = cap;
    block->len = 0;
    block->allocSize = allocSize;
    block->next = next;
    block->owned = owned;
}

static void* _Nullable H2ArenaTryAllocInBlock(H2ArenaBlock* block, uint32_t size, uint32_t align) {
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

void H2ArenaInit(H2Arena* arena, void* storage, uint32_t storageSize) {
    H2ArenaInitEx(arena, storage, storageSize, NULL, NULL, NULL);
}

void H2ArenaInitEx(
    H2Arena* arena,
    void* _Nullable storage,
    uint32_t storageSize,
    void* _Nullable allocatorCtx,
    H2ArenaGrowFn _Nullable growFn,
    H2ArenaFreeFn _Nullable freeFn) {
    arena->allocatorCtx = allocatorCtx;
    arena->grow = growFn;
    arena->free = freeFn;
    H2ArenaInitBlock(&arena->inlineBlock, (uint8_t*)storage, storageSize, storageSize, 0, NULL);
    arena->first = &arena->inlineBlock;
    arena->current = &arena->inlineBlock;
}

void H2ArenaSetAllocator(
    H2Arena* arena,
    void* _Nullable allocatorCtx,
    H2ArenaGrowFn _Nullable growFn,
    H2ArenaFreeFn _Nullable freeFn) {
    arena->allocatorCtx = allocatorCtx;
    arena->grow = growFn;
    arena->free = freeFn;
}

void H2ArenaReset(H2Arena* arena) {
    H2ArenaBlock* block = arena->first;
    while (block != NULL) {
        block->len = 0;
        block = block->next;
    }
    arena->current = arena->first;
}

void H2ArenaDispose(H2Arena* arena) {
    H2ArenaBlock* block;

    if (arena->first == NULL) {
        return;
    }

    block = arena->first->next;
    while (block != NULL) {
        H2ArenaBlock* next = block->next;
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

void* _Nullable H2ArenaAlloc(H2Arena* arena, uint32_t size, uint32_t align) {
    H2ArenaBlock* block;

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
        void* p = H2ArenaTryAllocInBlock(block, size, align);
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
        uint32_t headerSize = H2ArenaGrowHeaderSize();
        uint32_t minPayload;
        uint32_t targetPayload;
        uint64_t requestedSize64;
        uint32_t requestedSize;
        uint32_t allocSize = 0;
        void* _Nullable allocMem;
        H2ArenaBlock* newBlock;
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

        newBlock = (H2ArenaBlock*)allocMem;
        H2ArenaInitBlock(
            newBlock, (uint8_t*)allocMem + headerSize, allocSize - headerSize, allocSize, 1, NULL);
        block->next = newBlock;
        arena->current = newBlock;
        p = H2ArenaTryAllocInBlock(newBlock, size, align);
        if (p != NULL) {
            return p;
        }
    }

    return NULL;
}

static int H2IsAlpha(unsigned char c) {
    return (c >= (unsigned char)'A' && c <= (unsigned char)'Z')
        || (c >= (unsigned char)'a' && c <= (unsigned char)'z');
}

static int H2IsDigit(unsigned char c) {
    return c >= (unsigned char)'0' && c <= (unsigned char)'9';
}

static int H2IsAlnum(unsigned char c) {
    return H2IsAlpha(c) || H2IsDigit(c);
}

static int H2IsAsciiSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static int H2IsHexDigit(unsigned char c) {
    return H2IsDigit(c) || (c >= (unsigned char)'a' && c <= (unsigned char)'f')
        || (c >= (unsigned char)'A' && c <= (unsigned char)'F');
}

static int H2StrEq(const char* a, uint32_t aLen, const char* b) {
    uint32_t i = 0;
    while (i < aLen) {
        if (b[i] == '\0' || a[i] != b[i]) {
            return 0;
        }
        i++;
    }
    return b[i] == '\0';
}

static int H2IsValidImportPathChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'
        || c == '.' || c == '/' || c == '-';
}

int H2NormalizeImportPath(
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
    if (H2IsAsciiSpace(importPath[0]) || H2IsAsciiSpace(importPath[len - 1u])) {
        if (outErrReason != NULL) {
            *outErrReason = "leading/trailing whitespace";
        }
        return -1;
    }

    for (i = 0; i < len; i++) {
        if (!H2IsValidImportPathChar(importPath[i])) {
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

static H2TokenKind H2KeywordKind(const char* s, uint32_t len) {
    if (len == 2) {
        if (H2StrEq(s, len, "as")) {
            return H2Tok_AS;
        }
        if (H2StrEq(s, len, "in")) {
            return H2Tok_IN;
        }
        if (H2StrEq(s, len, "fn")) {
            return H2Tok_FN;
        }
        if (H2StrEq(s, len, "if")) {
            return H2Tok_IF;
        }
    } else if (len == 3) {
        if (H2StrEq(s, len, "for")) {
            return H2Tok_FOR;
        }
        if (H2StrEq(s, len, "new")) {
            return H2Tok_NEW;
        }
        if (H2StrEq(s, len, "del")) {
            return H2Tok_DEL;
        }
        if (H2StrEq(s, len, "var")) {
            return H2Tok_VAR;
        }
        if (H2StrEq(s, len, "mut")) {
            return H2Tok_MUT;
        }
        if (H2StrEq(s, len, "pub")) {
            return H2Tok_PUB;
        }
    } else if (len == 4) {
        if (H2StrEq(s, len, "enum")) {
            return H2Tok_ENUM;
        }
        if (H2StrEq(s, len, "else")) {
            return H2Tok_ELSE;
        }
        if (H2StrEq(s, len, "case")) {
            return H2Tok_CASE;
        }
        if (H2StrEq(s, len, "true")) {
            return H2Tok_TRUE;
        }
        if (H2StrEq(s, len, "null")) {
            return H2Tok_NULL;
        }
        if (H2StrEq(s, len, "type")) {
            return H2Tok_TYPE;
        }
    } else if (len == 5) {
        if (H2StrEq(s, len, "break")) {
            return H2Tok_BREAK;
        }
        if (H2StrEq(s, len, "const")) {
            return H2Tok_CONST;
        }
        if (H2StrEq(s, len, "defer")) {
            return H2Tok_DEFER;
        }
        if (H2StrEq(s, len, "false")) {
            return H2Tok_FALSE;
        }
        if (H2StrEq(s, len, "union")) {
            return H2Tok_UNION;
        }
    } else if (len == 6) {
        if (H2StrEq(s, len, "import")) {
            return H2Tok_IMPORT;
        }
        if (H2StrEq(s, len, "sizeof")) {
            return H2Tok_SIZEOF;
        }
        if (H2StrEq(s, len, "return")) {
            return H2Tok_RETURN;
        }
        if (H2StrEq(s, len, "switch")) {
            return H2Tok_SWITCH;
        }
        if (H2StrEq(s, len, "struct")) {
            return H2Tok_STRUCT;
        }
        if (H2StrEq(s, len, "assert")) {
            return H2Tok_ASSERT;
        }
    } else if (len == 7) {
        if (H2StrEq(s, len, "anytype")) {
            return H2Tok_ANYTYPE;
        }
        if (H2StrEq(s, len, "default")) {
            return H2Tok_DEFAULT;
        }
    } else if (len == 8) {
        if (H2StrEq(s, len, "continue")) {
            return H2Tok_CONTINUE;
        }
    }
    return H2Tok_IDENT;
}

static int H2TokenCanEndStmt(H2TokenKind kind) {
    switch (kind) {
        case H2Tok_IDENT:
        case H2Tok_INT:
        case H2Tok_FLOAT:
        case H2Tok_STRING:
        case H2Tok_RUNE:
        case H2Tok_TRUE:
        case H2Tok_FALSE:
        case H2Tok_BREAK:
        case H2Tok_CONTINUE:
        case H2Tok_RETURN:
        case H2Tok_RPAREN:
        case H2Tok_RBRACK:
        case H2Tok_RBRACE:
        case H2Tok_NOT:
        case H2Tok_NULL:
        case H2Tok_TYPE:     return 1;
        default:             return 0;
    }
}

static int H2PushToken(
    H2TokenBuf* out, H2Diag* diag, H2TokenKind kind, uint32_t start, uint32_t end) {
    if (out->len >= out->cap) {
        H2SetDiag(diag, H2Diag_ARENA_OOM, start, end);
        return -1;
    }

    out->v[out->len].kind = kind;
    out->v[out->len].start = start;
    out->v[out->len].end = end;
    out->len++;
    return 0;
}

static int H2SkipBlockComment(
    H2StrView src, uint32_t* ioPos, int* ioSawNewline, uint32_t* ioNewlinePos) {
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

const char* H2TokenKindName(H2TokenKind kind) {
    switch (kind) {
        case H2Tok_INVALID:       return "INVALID";
        case H2Tok_EOF:           return "EOF";
        case H2Tok_IDENT:         return "IDENT";
        case H2Tok_INT:           return "INT";
        case H2Tok_FLOAT:         return "FLOAT";
        case H2Tok_STRING:        return "STRING";
        case H2Tok_RUNE:          return "RUNE";
        case H2Tok_IMPORT:        return "IMPORT";
        case H2Tok_PUB:           return "PUB";
        case H2Tok_STRUCT:        return "STRUCT";
        case H2Tok_UNION:         return "UNION";
        case H2Tok_ENUM:          return "ENUM";
        case H2Tok_FN:            return "FN";
        case H2Tok_VAR:           return "VAR";
        case H2Tok_CONST:         return "CONST";
        case H2Tok_TYPE:          return "TYPE";
        case H2Tok_MUT:           return "MUT";
        case H2Tok_IF:            return "IF";
        case H2Tok_ELSE:          return "ELSE";
        case H2Tok_FOR:           return "FOR";
        case H2Tok_SWITCH:        return "SWITCH";
        case H2Tok_CASE:          return "CASE";
        case H2Tok_DEFAULT:       return "DEFAULT";
        case H2Tok_BREAK:         return "BREAK";
        case H2Tok_CONTINUE:      return "CONTINUE";
        case H2Tok_RETURN:        return "RETURN";
        case H2Tok_DEFER:         return "DEFER";
        case H2Tok_ASSERT:        return "ASSERT";
        case H2Tok_SIZEOF:        return "SIZEOF";
        case H2Tok_NEW:           return "NEW";
        case H2Tok_DEL:           return "DEL";
        case H2Tok_TRUE:          return "TRUE";
        case H2Tok_FALSE:         return "FALSE";
        case H2Tok_IN:            return "IN";
        case H2Tok_AS:            return "AS";
        case H2Tok_CONTEXT:       return "CONTEXT";
        case H2Tok_ANYTYPE:       return "ANYTYPE";
        case H2Tok_LPAREN:        return "LPAREN";
        case H2Tok_RPAREN:        return "RPAREN";
        case H2Tok_LBRACE:        return "LBRACE";
        case H2Tok_RBRACE:        return "RBRACE";
        case H2Tok_LBRACK:        return "LBRACK";
        case H2Tok_RBRACK:        return "RBRACK";
        case H2Tok_COMMA:         return "COMMA";
        case H2Tok_DOT:           return "DOT";
        case H2Tok_ELLIPSIS:      return "ELLIPSIS";
        case H2Tok_SEMICOLON:     return "SEMICOLON";
        case H2Tok_COLON:         return "COLON";
        case H2Tok_AT:            return "AT";
        case H2Tok_SHORT_ASSIGN:  return "SHORT_ASSIGN";
        case H2Tok_ASSIGN:        return "ASSIGN";
        case H2Tok_ADD:           return "ADD";
        case H2Tok_SUB:           return "SUB";
        case H2Tok_MUL:           return "MUL";
        case H2Tok_DIV:           return "DIV";
        case H2Tok_MOD:           return "MOD";
        case H2Tok_AND:           return "AND";
        case H2Tok_OR:            return "OR";
        case H2Tok_XOR:           return "XOR";
        case H2Tok_NOT:           return "NOT";
        case H2Tok_LSHIFT:        return "LSHIFT";
        case H2Tok_RSHIFT:        return "RSHIFT";
        case H2Tok_EQ:            return "EQ";
        case H2Tok_NEQ:           return "NEQ";
        case H2Tok_LT:            return "LT";
        case H2Tok_GT:            return "GT";
        case H2Tok_LTE:           return "LTE";
        case H2Tok_GTE:           return "GTE";
        case H2Tok_LOGICAL_AND:   return "LOGICAL_AND";
        case H2Tok_LOGICAL_OR:    return "LOGICAL_OR";
        case H2Tok_ADD_ASSIGN:    return "ADD_ASSIGN";
        case H2Tok_SUB_ASSIGN:    return "SUB_ASSIGN";
        case H2Tok_MUL_ASSIGN:    return "MUL_ASSIGN";
        case H2Tok_DIV_ASSIGN:    return "DIV_ASSIGN";
        case H2Tok_MOD_ASSIGN:    return "MOD_ASSIGN";
        case H2Tok_AND_ASSIGN:    return "AND_ASSIGN";
        case H2Tok_OR_ASSIGN:     return "OR_ASSIGN";
        case H2Tok_XOR_ASSIGN:    return "XOR_ASSIGN";
        case H2Tok_LSHIFT_ASSIGN: return "LSHIFT_ASSIGN";
        case H2Tok_RSHIFT_ASSIGN: return "RSHIFT_ASSIGN";
        case H2Tok_QUESTION:      return "QUESTION";
        case H2Tok_NULL:          return "NULL";
    }
    return "UNKNOWN";
}

int H2Lex(H2Arena* arena, H2StrView src, H2TokenStream* out, H2Diag* _Nullable diag) {
    H2TokenBuf  tokbuf;
    uint32_t    pos = 0;
    int         insertedEOFSemicolon = 0;
    H2TokenKind prevKind = H2Tok_INVALID;

    if (diag != NULL) {
        *diag = (H2Diag){ 0 };
        diag->phase = H2DiagPhase_LEX;
    }
    out->v = NULL;
    out->len = 0;

    tokbuf.len = 0;
    tokbuf.cap = src.len + 2;
    if (tokbuf.cap < 8) {
        tokbuf.cap = 8;
    }
    tokbuf.v = (H2Token*)H2ArenaAlloc(
        arena, tokbuf.cap * (uint32_t)sizeof(H2Token), (uint32_t)_Alignof(H2Token));
    if (tokbuf.v == NULL) {
        H2SetDiag(diag, H2Diag_ARENA_OOM, 0, 0);
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
                if (H2SkipBlockComment(src, &pos, &sawNewline, &newlinePos) != 0) {
                    H2SetDiag(diag, H2Diag_UNTERMINATED_BLOCK_COMMENT, commentStart, pos);
                    return -1;
                }
                continue;
            }
            break;
        }

        if (sawNewline && H2TokenCanEndStmt(prevKind)) {
            if (H2PushToken(&tokbuf, diag, H2Tok_SEMICOLON, newlinePos, newlinePos) != 0) {
                return -1;
            }
            prevKind = H2Tok_SEMICOLON;
            continue;
        }

        if (pos >= src.len) {
            if (!insertedEOFSemicolon && H2TokenCanEndStmt(prevKind)) {
                if (H2PushToken(&tokbuf, diag, H2Tok_SEMICOLON, src.len, src.len) != 0) {
                    return -1;
                }
                prevKind = H2Tok_SEMICOLON;
                insertedEOFSemicolon = 1;
                continue;
            }
            if (H2PushToken(&tokbuf, diag, H2Tok_EOF, src.len, src.len) != 0) {
                return -1;
            }
            break;
        }

        {
            H2TokenKind   kind = H2Tok_INVALID;
            uint32_t      start = pos;
            unsigned char c = (unsigned char)src.ptr[pos];

            if (H2IsAlpha(c) || c == (unsigned char)'_') {
                pos++;
                while (pos < src.len) {
                    c = (unsigned char)src.ptr[pos];
                    if (!H2IsAlnum(c) && c != (unsigned char)'_') {
                        break;
                    }
                    pos++;
                }
                kind = H2KeywordKind(src.ptr + start, pos - start);
            } else if (H2IsDigit(c)) {
                kind = H2Tok_INT;

                if (c == (unsigned char)'0' && pos + 1 < src.len
                    && ((unsigned char)src.ptr[pos + 1] == (unsigned char)'x'
                        || (unsigned char)src.ptr[pos + 1] == (unsigned char)'X'))
                {
                    uint32_t digitsStart;
                    pos += 2;
                    digitsStart = pos;
                    while (pos < src.len && H2IsHexDigit((unsigned char)src.ptr[pos])) {
                        pos++;
                    }
                    if (pos == digitsStart) {
                        H2SetDiag(diag, H2Diag_INVALID_NUMBER, start, pos);
                        return -1;
                    }
                } else {
                    while (pos < src.len && H2IsDigit((unsigned char)src.ptr[pos])) {
                        pos++;
                    }

                    if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'.') {
                        kind = H2Tok_FLOAT;
                        pos++;
                        while (pos < src.len && H2IsDigit((unsigned char)src.ptr[pos])) {
                            pos++;
                        }
                    }

                    if (pos < src.len
                        && ((unsigned char)src.ptr[pos] == (unsigned char)'e'
                            || (unsigned char)src.ptr[pos] == (unsigned char)'E'))
                    {
                        uint32_t expStart;
                        kind = H2Tok_FLOAT;
                        pos++;
                        if (pos < src.len
                            && ((unsigned char)src.ptr[pos] == (unsigned char)'+'
                                || (unsigned char)src.ptr[pos] == (unsigned char)'-'))
                        {
                            pos++;
                        }
                        expStart = pos;
                        while (pos < src.len && H2IsDigit((unsigned char)src.ptr[pos])) {
                            pos++;
                        }
                        if (pos == expStart) {
                            H2SetDiag(diag, H2Diag_INVALID_NUMBER, start, pos);
                            return -1;
                        }
                    }
                }
            } else if (c == (unsigned char)'\'') {
                kind = H2Tok_RUNE;
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
                            H2SetDiag(diag, H2Diag_UNTERMINATED_RUNE, start, pos);
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
                    H2SetDiag(diag, H2Diag_UNTERMINATED_RUNE, start, pos);
                    return -1;
                }
                {
                    H2RuneLitErr runeErr = { 0 };
                    uint32_t     rune = 0;
                    if (H2DecodeRuneLiteralValidate(src.ptr, start, pos, &rune, &runeErr) != 0) {
                        H2SetDiag(
                            diag, H2RuneLitErrDiagCode(runeErr.kind), runeErr.start, runeErr.end);
                        return -1;
                    }
                }
            } else if (c == (unsigned char)'"' || c == (unsigned char)'`') {
                unsigned char quote = c;
                kind = H2Tok_STRING;
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
                            H2SetDiag(diag, H2Diag_UNTERMINATED_STRING, start, pos);
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
                    H2SetDiag(diag, H2Diag_UNTERMINATED_STRING, start, pos);
                    return -1;
                }
                {
                    H2StringLitErr litErr = { 0 };
                    if (H2DecodeStringLiteralValidate(src.ptr, start, pos, &litErr) != 0) {
                        H2SetDiag(
                            diag, H2StringLitErrDiagCode(litErr.kind), litErr.start, litErr.end);
                        return -1;
                    }
                }
            } else {
                pos++;
                switch (c) {
                    case (unsigned char)'(': kind = H2Tok_LPAREN; break;
                    case (unsigned char)')': kind = H2Tok_RPAREN; break;
                    case (unsigned char)'{': kind = H2Tok_LBRACE; break;
                    case (unsigned char)'}': kind = H2Tok_RBRACE; break;
                    case (unsigned char)'[': kind = H2Tok_LBRACK; break;
                    case (unsigned char)']': kind = H2Tok_RBRACK; break;
                    case (unsigned char)',': kind = H2Tok_COMMA; break;
                    case (unsigned char)'.':
                        if (pos + 1u < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'.'
                            && (unsigned char)src.ptr[pos + 1u] == (unsigned char)'.')
                        {
                            pos += 2u;
                            kind = H2Tok_ELLIPSIS;
                        } else {
                            kind = H2Tok_DOT;
                        }
                        break;
                    case (unsigned char)';': kind = H2Tok_SEMICOLON; break;
                    case (unsigned char)':':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = H2Tok_SHORT_ASSIGN;
                        } else {
                            kind = H2Tok_COLON;
                        }
                        break;
                    case (unsigned char)'@': kind = H2Tok_AT; break;

                    case (unsigned char)'+':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = H2Tok_ADD_ASSIGN;
                        } else {
                            kind = H2Tok_ADD;
                        }
                        break;
                    case (unsigned char)'-':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = H2Tok_SUB_ASSIGN;
                        } else {
                            kind = H2Tok_SUB;
                        }
                        break;
                    case (unsigned char)'*':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = H2Tok_MUL_ASSIGN;
                        } else {
                            kind = H2Tok_MUL;
                        }
                        break;
                    case (unsigned char)'/':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = H2Tok_DIV_ASSIGN;
                        } else {
                            kind = H2Tok_DIV;
                        }
                        break;
                    case (unsigned char)'%':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = H2Tok_MOD_ASSIGN;
                        } else {
                            kind = H2Tok_MOD;
                        }
                        break;
                    case (unsigned char)'&':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'&') {
                            pos++;
                            kind = H2Tok_LOGICAL_AND;
                        } else if (
                            pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=')
                        {
                            pos++;
                            kind = H2Tok_AND_ASSIGN;
                        } else {
                            kind = H2Tok_AND;
                        }
                        break;
                    case (unsigned char)'|':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'|') {
                            pos++;
                            kind = H2Tok_LOGICAL_OR;
                        } else if (
                            pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=')
                        {
                            pos++;
                            kind = H2Tok_OR_ASSIGN;
                        } else {
                            kind = H2Tok_OR;
                        }
                        break;
                    case (unsigned char)'^':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = H2Tok_XOR_ASSIGN;
                        } else {
                            kind = H2Tok_XOR;
                        }
                        break;
                    case (unsigned char)'?': kind = H2Tok_QUESTION; break;
                    case (unsigned char)'!':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = H2Tok_NEQ;
                        } else {
                            kind = H2Tok_NOT;
                        }
                        break;
                    case (unsigned char)'=':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = H2Tok_EQ;
                        } else {
                            kind = H2Tok_ASSIGN;
                        }
                        break;
                    case (unsigned char)'<':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'<') {
                            pos++;
                            if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=')
                            {
                                pos++;
                                kind = H2Tok_LSHIFT_ASSIGN;
                            } else {
                                kind = H2Tok_LSHIFT;
                            }
                        } else if (
                            pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=')
                        {
                            pos++;
                            kind = H2Tok_LTE;
                        } else {
                            kind = H2Tok_LT;
                        }
                        break;
                    case (unsigned char)'>':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'>') {
                            pos++;
                            if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=')
                            {
                                pos++;
                                kind = H2Tok_RSHIFT_ASSIGN;
                            } else {
                                kind = H2Tok_RSHIFT;
                            }
                        } else if (
                            pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=')
                        {
                            pos++;
                            kind = H2Tok_GTE;
                        } else {
                            kind = H2Tok_GT;
                        }
                        break;

                    default: H2SetDiag(diag, H2Diag_UNEXPECTED_CHAR, start, pos); return -1;
                }
            }

            if (H2PushToken(&tokbuf, diag, kind, start, pos) != 0) {
                return -1;
            }
            prevKind = kind;
        }
    }

    out->v = tokbuf.v;
    out->len = tokbuf.len;
    return 0;
}

static void H2WWrite(H2Writer* w, const char* s, uint32_t len) {
    w->write(w->ctx, s, len);
}

static void H2WCStr(H2Writer* w, const char* s) {
    uint32_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    H2WWrite(w, s, n);
}

static void H2WU32(H2Writer* w, uint32_t v) {
    char     buf[16];
    uint32_t n = 0;
    if (v == 0) {
        H2WWrite(w, "0", 1);
        return;
    }
    while (v > 0 && n < (uint32_t)sizeof(buf)) {
        buf[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0) {
        n--;
        H2WWrite(w, &buf[n], 1);
    }
}

static void H2WIndent(H2Writer* w, uint32_t depth) {
    uint32_t i;
    for (i = 0; i < depth; i++) {
        H2WWrite(w, "  ", 2);
    }
}

static void H2WEscaped(H2Writer* w, H2StrView src, uint32_t start, uint32_t end) {
    uint32_t i;
    H2WWrite(w, "\"", 1);
    for (i = start; i < end && i < src.len; i++) {
        unsigned char c = (unsigned char)src.ptr[i];
        switch (c) {
            case '\"': H2WWrite(w, "\\\"", 2); break;
            case '\\': H2WWrite(w, "\\\\", 2); break;
            case '\n': H2WWrite(w, "\\n", 2); break;
            case '\r': H2WWrite(w, "\\r", 2); break;
            case '\t': H2WWrite(w, "\\t", 2); break;
            default:
                if (c >= 0x20 && c <= 0x7e) {
                    H2WWrite(w, (const char*)&src.ptr[i], 1);
                } else {
                    char               hex[4];
                    static const char* digits = "0123456789abcdef";
                    hex[0] = '\\';
                    hex[1] = 'x';
                    hex[2] = digits[(c >> 4) & 0x0f];
                    hex[3] = digits[c & 0x0f];
                    H2WWrite(w, hex, 4);
                }
                break;
        }
    }
    H2WWrite(w, "\"", 1);
}

static int H2AstDumpNode(
    const H2Ast* ast, int32_t idx, uint32_t depth, H2StrView src, H2Writer* w) {
    const H2AstNode* n;
    int32_t          c;
    if (idx < 0 || (uint32_t)idx >= ast->len) {
        return -1;
    }
    n = &ast->nodes[idx];
    H2WIndent(w, depth);
    H2WCStr(w, H2AstKindName(n->kind));

    if (n->op != 0) {
        H2WCStr(w, " op=");
        H2WCStr(w, H2TokenKindName((H2TokenKind)n->op));
    }
    if (n->flags != 0) {
        H2WCStr(w, " flags=");
        H2WU32(w, n->flags);
    }
    if (n->dataEnd > n->dataStart) {
        H2WCStr(w, " ");
        H2WEscaped(w, src, n->dataStart, n->dataEnd);
    }
    H2WCStr(w, " [");
    H2WU32(w, n->start);
    H2WCStr(w, ",");
    H2WU32(w, n->end);
    H2WCStr(w, "]\n");

    c = n->firstChild;
    while (c >= 0) {
        if (H2AstDumpNode(ast, c, depth + 1, src, w) != 0) {
            return -1;
        }
        c = ast->nodes[c].nextSibling;
    }
    return 0;
}

int H2AstDump(const H2Ast* ast, H2StrView src, H2Writer* w, H2Diag* _Nullable diag) {
    if (diag != NULL) {
        *diag = (H2Diag){ 0 };
        diag->phase = H2DiagPhase_COMPILER;
    }
    if (ast == NULL || w == NULL || w->write == NULL || ast->nodes == NULL || ast->root < 0) {
        H2SetDiag(diag, H2Diag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }
    return H2AstDumpNode(ast, ast->root, 0, src, w);
}

H2_API_END
