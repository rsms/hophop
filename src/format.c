#include "libsl-impl.h"

SL_API_BEGIN

typedef struct {
    SLArena* arena;
    char* _Nullable v;
    uint32_t len;
    uint32_t cap;
} SLFmtBuf;

typedef struct {
    const SLAst* ast;
    SLStrView    src;
    const SLComment* _Nullable comments;
    uint32_t commentLen;
    uint8_t* _Nullable commentUsed;
    uint32_t indent;
    uint32_t indentWidth;
    int      lineStart;
    SLFmtBuf out;
} SLFmtCtx;

enum {
    SLFmtFlag_DROP_REDUNDANT_LITERAL_CAST = 0x4000u,
};

typedef enum {
    SLFmtNumericType_INVALID = 0,
    SLFmtNumericType_I8,
    SLFmtNumericType_I16,
    SLFmtNumericType_I32,
    SLFmtNumericType_I64,
    SLFmtNumericType_INT,
    SLFmtNumericType_U8,
    SLFmtNumericType_U16,
    SLFmtNumericType_U32,
    SLFmtNumericType_U64,
    SLFmtNumericType_UINT,
    SLFmtNumericType_F32,
    SLFmtNumericType_F64,
} SLFmtNumericType;

static int SLFmtBufReserve(SLFmtBuf* b, uint32_t extra) {
    char*    p;
    uint32_t need;
    uint32_t cap;
    if (extra == 0) {
        return 0;
    }
    if (UINT32_MAX - b->len < extra) {
        return -1;
    }
    need = b->len + extra;
    if (need <= b->cap) {
        return 0;
    }
    cap = b->cap > 0 ? b->cap : 256u;
    while (cap < need) {
        if (cap > UINT32_MAX / 2u) {
            cap = need;
            break;
        }
        cap *= 2u;
    }
    p = (char*)SLArenaAlloc(b->arena, cap, (uint32_t)_Alignof(char));
    if (p == NULL) {
        return -1;
    }
    if (b->len > 0) {
        memcpy(p, b->v, b->len);
    }
    b->v = p;
    b->cap = cap;
    return 0;
}

static int SLFmtBufAppendChar(SLFmtBuf* b, char c) {
    if (SLFmtBufReserve(b, 1u) != 0) {
        return -1;
    }
    b->v[b->len++] = c;
    return 0;
}

static int     SLFmtWriteChar(SLFmtCtx* c, char ch);
static int32_t SLFmtFindEnclosingFnNode(const SLAst* ast, int32_t nodeId);
static int32_t SLFmtFindParentNode(const SLAst* ast, int32_t childNodeId);
static int     SLFmtEmitType(SLFmtCtx* c, int32_t nodeId);

static uint32_t SLFmtCStrLen(const char* s) {
    const char* p = s;
    while (*p != '\0') {
        p++;
    }
    return (uint32_t)(p - s);
}

static int SLFmtWriteIndent(SLFmtCtx* c) {
    uint32_t i;
    if (!c->lineStart) {
        return 0;
    }
    for (i = 0; i < c->indent; i++) {
        if (SLFmtBufAppendChar(&c->out, '\t') != 0) {
            return -1;
        }
    }
    c->lineStart = 0;
    return 0;
}

static int SLFmtWriteSpaces(SLFmtCtx* c, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (SLFmtWriteChar(c, ' ') != 0) {
            return -1;
        }
    }
    return 0;
}

static int SLFmtWriteChar(SLFmtCtx* c, char ch) {
    if (ch != '\n' && c->lineStart) {
        if (SLFmtWriteIndent(c) != 0) {
            return -1;
        }
    }
    if (SLFmtBufAppendChar(&c->out, ch) != 0) {
        return -1;
    }
    c->lineStart = (ch == '\n');
    return 0;
}

static int SLFmtWrite(SLFmtCtx* c, const char* s, uint32_t len) {
    uint32_t i;
    for (i = 0; i < len; i++) {
        if (SLFmtWriteChar(c, s[i]) != 0) {
            return -1;
        }
    }
    return 0;
}

static int SLFmtWriteCStr(SLFmtCtx* c, const char* s) {
    while (*s != '\0') {
        if (SLFmtWriteChar(c, *s++) != 0) {
            return -1;
        }
    }
    return 0;
}

static int SLFmtWriteSlice(SLFmtCtx* c, uint32_t start, uint32_t end) {
    if (end < start || end > c->src.len) {
        return -1;
    }
    return SLFmtWrite(c, c->src.ptr + start, end - start);
}

static int SLFmtWriteSliceLiteral(SLFmtCtx* c, uint32_t start, uint32_t end) {
    uint32_t i;
    if (end < start || end > c->src.len) {
        return -1;
    }
    if (c->lineStart && SLFmtWriteIndent(c) != 0) {
        return -1;
    }
    for (i = start; i < end; i++) {
        char ch = c->src.ptr[i];
        if (SLFmtBufAppendChar(&c->out, ch) != 0) {
            return -1;
        }
        c->lineStart = (ch == '\n');
    }
    return 0;
}

static int SLFmtNewline(SLFmtCtx* c) {
    return SLFmtWriteChar(c, '\n');
}

static int32_t SLFmtFirstChild(const SLAst* ast, int32_t nodeId) {
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    return ast->nodes[nodeId].firstChild;
}

static int32_t SLFmtNextSibling(const SLAst* ast, int32_t nodeId) {
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    return ast->nodes[nodeId].nextSibling;
}

static uint32_t SLFmtListCount(const SLAst* ast, int32_t listNodeId) {
    uint32_t count = 0;
    int32_t  cur;
    if (listNodeId < 0 || (uint32_t)listNodeId >= ast->len) {
        return 0;
    }
    cur = ast->nodes[listNodeId].firstChild;
    while (cur >= 0) {
        count++;
        cur = ast->nodes[cur].nextSibling;
    }
    return count;
}

static int32_t SLFmtListItemAt(const SLAst* ast, int32_t listNodeId, uint32_t index) {
    uint32_t i = 0;
    int32_t  cur;
    if (listNodeId < 0 || (uint32_t)listNodeId >= ast->len) {
        return -1;
    }
    cur = ast->nodes[listNodeId].firstChild;
    while (cur >= 0) {
        if (i == index) {
            return cur;
        }
        i++;
        cur = ast->nodes[cur].nextSibling;
    }
    return -1;
}

static int SLFmtSlicesEqual(
    SLStrView src, uint32_t aStart, uint32_t aEnd, uint32_t bStart, uint32_t bEnd) {
    uint32_t len;
    if (aEnd < aStart || bEnd < bStart || aEnd > src.len || bEnd > src.len) {
        return 0;
    }
    len = aEnd - aStart;
    if (len != bEnd - bStart) {
        return 0;
    }
    if (len == 0) {
        return 1;
    }
    return memcmp(src.ptr + aStart, src.ptr + bStart, len) == 0;
}

static int SLFmtSliceEqLiteral(SLStrView src, uint32_t start, uint32_t end, const char* lit) {
    uint32_t len = SLFmtCStrLen(lit);
    if (end < start || end > src.len || (end - start) != len) {
        return 0;
    }
    return len == 0 || memcmp(src.ptr + start, lit, len) == 0;
}

static int SLFmtSliceHasChar(SLStrView src, uint32_t start, uint32_t end, char ch) {
    uint32_t i;
    if (end < start || end > src.len) {
        return 0;
    }
    for (i = start; i < end; i++) {
        if (src.ptr[i] == ch) {
            return 1;
        }
    }
    return 0;
}

static int SLFmtExprIsPlainIdent(
    const SLAst* ast, int32_t exprNodeId, uint32_t* outStart, uint32_t* outEnd) {
    const SLAstNode* n;
    if (exprNodeId < 0 || (uint32_t)exprNodeId >= ast->len || outStart == NULL || outEnd == NULL) {
        return 0;
    }
    n = &ast->nodes[exprNodeId];
    if (n->kind != SLAst_IDENT || (n->flags & SLAstFlag_PAREN) != 0 || n->dataEnd <= n->dataStart) {
        return 0;
    }
    *outStart = n->dataStart;
    *outEnd = n->dataEnd;
    return 1;
}

static int SLFmtIsTypeNodeKindRaw(SLAstKind kind) {
    return kind == SLAst_TYPE_NAME || kind == SLAst_TYPE_PTR || kind == SLAst_TYPE_REF
        || kind == SLAst_TYPE_MUTREF || kind == SLAst_TYPE_ARRAY || kind == SLAst_TYPE_VARRAY
        || kind == SLAst_TYPE_SLICE || kind == SLAst_TYPE_MUTSLICE || kind == SLAst_TYPE_OPTIONAL
        || kind == SLAst_TYPE_FN || kind == SLAst_TYPE_ANON_STRUCT || kind == SLAst_TYPE_ANON_UNION
        || kind == SLAst_TYPE_TUPLE || kind == SLAst_TYPE_PARAM;
}

static SLFmtNumericType SLFmtNumericTypeFromName(SLStrView src, uint32_t start, uint32_t end) {
    if (SLFmtSliceEqLiteral(src, start, end, "i8")) {
        return SLFmtNumericType_I8;
    }
    if (SLFmtSliceEqLiteral(src, start, end, "i16")) {
        return SLFmtNumericType_I16;
    }
    if (SLFmtSliceEqLiteral(src, start, end, "i32")) {
        return SLFmtNumericType_I32;
    }
    if (SLFmtSliceEqLiteral(src, start, end, "i64")) {
        return SLFmtNumericType_I64;
    }
    if (SLFmtSliceEqLiteral(src, start, end, "int")) {
        return SLFmtNumericType_INT;
    }
    if (SLFmtSliceEqLiteral(src, start, end, "u8")) {
        return SLFmtNumericType_U8;
    }
    if (SLFmtSliceEqLiteral(src, start, end, "u16")) {
        return SLFmtNumericType_U16;
    }
    if (SLFmtSliceEqLiteral(src, start, end, "u32")) {
        return SLFmtNumericType_U32;
    }
    if (SLFmtSliceEqLiteral(src, start, end, "u64")) {
        return SLFmtNumericType_U64;
    }
    if (SLFmtSliceEqLiteral(src, start, end, "uint")) {
        return SLFmtNumericType_UINT;
    }
    if (SLFmtSliceEqLiteral(src, start, end, "f32")) {
        return SLFmtNumericType_F32;
    }
    if (SLFmtSliceEqLiteral(src, start, end, "f64")) {
        return SLFmtNumericType_F64;
    }
    return SLFmtNumericType_INVALID;
}

static SLFmtNumericType SLFmtNumericTypeFromTypeNode(
    const SLAst* ast, SLStrView src, int32_t nodeId) {
    const SLAstNode* n;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return SLFmtNumericType_INVALID;
    }
    n = &ast->nodes[nodeId];
    if (n->kind != SLAst_TYPE_NAME) {
        return SLFmtNumericType_INVALID;
    }
    return SLFmtNumericTypeFromName(src, n->dataStart, n->dataEnd);
}

static SLFmtNumericType SLFmtCastLiteralNumericType(
    const SLAst* ast, SLStrView src, int32_t exprNodeId, int32_t typeNodeId) {
    if (exprNodeId < 0 || typeNodeId < 0 || (uint32_t)exprNodeId >= ast->len
        || (uint32_t)typeNodeId >= ast->len)
    {
        return SLFmtNumericType_INVALID;
    }
    if (ast->nodes[exprNodeId].kind == SLAst_INT) {
        SLFmtNumericType t = SLFmtNumericTypeFromTypeNode(ast, src, typeNodeId);
        switch (t) {
            case SLFmtNumericType_I8:
            case SLFmtNumericType_I16:
            case SLFmtNumericType_I32:
            case SLFmtNumericType_I64:
            case SLFmtNumericType_INT:
            case SLFmtNumericType_U8:
            case SLFmtNumericType_U16:
            case SLFmtNumericType_U32:
            case SLFmtNumericType_U64:
            case SLFmtNumericType_UINT: return t;
            default:                    return SLFmtNumericType_INVALID;
        }
    }
    if (ast->nodes[exprNodeId].kind == SLAst_FLOAT) {
        SLFmtNumericType t = SLFmtNumericTypeFromTypeNode(ast, src, typeNodeId);
        return (t == SLFmtNumericType_F32 || t == SLFmtNumericType_F64)
                 ? t
                 : SLFmtNumericType_INVALID;
    }
    return SLFmtNumericType_INVALID;
}

static int SLFmtBinaryOpSharesOperandType(uint16_t op) {
    switch ((SLTokenKind)op) {
        case SLTok_ADD:
        case SLTok_SUB:
        case SLTok_MUL:
        case SLTok_DIV:
        case SLTok_MOD:
        case SLTok_AND:
        case SLTok_OR:
        case SLTok_XOR:
        case SLTok_EQ:
        case SLTok_NEQ:
        case SLTok_LT:
        case SLTok_GT:
        case SLTok_LTE:
        case SLTok_GTE: return 1;
        default:        return 0;
    }
}

static int SLFmtIsAssignmentOp(SLTokenKind kind);

static int SLFmtTypeNodesEqualBySource(
    const SLAst* ast, SLStrView src, int32_t aTypeNodeId, int32_t bTypeNodeId) {
    const SLAstNode* a;
    const SLAstNode* b;
    if (aTypeNodeId < 0 || bTypeNodeId < 0 || (uint32_t)aTypeNodeId >= ast->len
        || (uint32_t)bTypeNodeId >= ast->len)
    {
        return 0;
    }
    a = &ast->nodes[aTypeNodeId];
    b = &ast->nodes[bTypeNodeId];
    return SLFmtSlicesEqual(src, a->start, a->end, b->start, b->end);
}

typedef struct {
    uint32_t nameStart;
    uint32_t nameEnd;
    int32_t  concreteTypeNodeId;
} SLFmtTypeBinding;

typedef struct {
    SLFmtTypeBinding items[16];
    uint32_t         len;
} SLFmtTypeEnv;

typedef struct {
    int32_t  paramNodeId;
    int32_t  typeNodeId;
    uint16_t flags;
} SLFmtCallParam;

typedef struct {
    int32_t  argNodeId;
    int32_t  exprNodeId;
    uint32_t labelStart;
    uint32_t labelEnd;
    uint8_t  hasLabel;
    uint8_t  isSynthetic;
} SLFmtCallActual;

typedef struct {
    int32_t      typeNodeId;
    SLFmtTypeEnv env;
} SLFmtInferredType;

static void SLFmtTypeEnvInit(SLFmtTypeEnv* env) {
    if (env != NULL) {
        env->len = 0;
    }
}

static void SLFmtInferredTypeInit(SLFmtInferredType* inferred) {
    if (inferred != NULL) {
        inferred->typeNodeId = -1;
        SLFmtTypeEnvInit(&inferred->env);
    }
}

static int SLFmtInferredTypeSet(
    SLFmtInferredType* inferred, int32_t typeNodeId, const SLFmtTypeEnv* _Nullable env);
static int SLFmtInferredTypeMatchesNode(
    const SLAst* ast, SLStrView src, const SLFmtInferredType* inferred, int32_t typeNodeId);

static int32_t SLFmtTypeEnvFind(
    const SLFmtTypeEnv* env, SLStrView src, uint32_t nameStart, uint32_t nameEnd) {
    uint32_t i;
    if (env == NULL || nameEnd <= nameStart) {
        return -1;
    }
    for (i = 0; i < env->len; i++) {
        if (SLFmtSlicesEqual(
                src, env->items[i].nameStart, env->items[i].nameEnd, nameStart, nameEnd))
        {
            return (int32_t)i;
        }
    }
    return -1;
}

static int SLFmtTypeEnvAdd(
    SLFmtTypeEnv* env, uint32_t nameStart, uint32_t nameEnd, int32_t concreteTypeNodeId) {
    if (env == NULL || nameEnd <= nameStart || env->len >= 16u) {
        return 0;
    }
    env->items[env->len].nameStart = nameStart;
    env->items[env->len].nameEnd = nameEnd;
    env->items[env->len].concreteTypeNodeId = concreteTypeNodeId;
    env->len++;
    return 1;
}

static int SLFmtTypeEnvInitFromDeclTypeParams(
    const SLAst* ast, int32_t declNodeId, SLFmtTypeEnv* env) {
    int32_t child;
    SLFmtTypeEnvInit(env);
    if (declNodeId < 0 || (uint32_t)declNodeId >= ast->len || env == NULL) {
        return 0;
    }
    child = SLFmtFirstChild(ast, declNodeId);
    while (child >= 0 && ast->nodes[child].kind == SLAst_TYPE_PARAM) {
        if (!SLFmtTypeEnvAdd(env, ast->nodes[child].dataStart, ast->nodes[child].dataEnd, -1)) {
            return 0;
        }
        child = SLFmtNextSibling(ast, child);
    }
    return 1;
}

static int SLFmtTypeNameIsBoundParam(
    const SLAst* ast,
    SLStrView    src,
    int32_t      typeNodeId,
    const SLFmtTypeEnv* _Nullable env,
    int32_t* outBindingIndex) {
    const SLAstNode* n;
    int32_t          bindingIndex;
    if (typeNodeId < 0 || (uint32_t)typeNodeId >= ast->len) {
        return 0;
    }
    n = &ast->nodes[typeNodeId];
    if (n->kind != SLAst_TYPE_NAME || SLFmtFirstChild(ast, typeNodeId) >= 0) {
        return 0;
    }
    bindingIndex = SLFmtTypeEnvFind(env, src, n->dataStart, n->dataEnd);
    if (bindingIndex < 0) {
        return 0;
    }
    if (outBindingIndex != NULL) {
        *outBindingIndex = bindingIndex;
    }
    return 1;
}

static int SLFmtTypeCompatibleWithEnvs(
    const SLAst* ast,
    SLStrView    src,
    int32_t      wantNodeId,
    SLFmtTypeEnv* _Nullable wantEnv,
    int32_t gotNodeId,
    const SLFmtTypeEnv* _Nullable gotEnv);

static int SLFmtTypeCompatibleChildren(
    const SLAst* ast,
    SLStrView    src,
    int32_t      wantNodeId,
    SLFmtTypeEnv* _Nullable wantEnv,
    int32_t gotNodeId,
    const SLFmtTypeEnv* _Nullable gotEnv) {
    int32_t wantChild = SLFmtFirstChild(ast, wantNodeId);
    int32_t gotChild = SLFmtFirstChild(ast, gotNodeId);
    while (wantChild >= 0 && gotChild >= 0) {
        if (!SLFmtTypeCompatibleWithEnvs(ast, src, wantChild, wantEnv, gotChild, gotEnv)) {
            return 0;
        }
        wantChild = SLFmtNextSibling(ast, wantChild);
        gotChild = SLFmtNextSibling(ast, gotChild);
    }
    return wantChild < 0 && gotChild < 0;
}

static int SLFmtTypeCompatibleWithEnvs(
    const SLAst* ast,
    SLStrView    src,
    int32_t      wantNodeId,
    SLFmtTypeEnv* _Nullable wantEnv,
    int32_t gotNodeId,
    const SLFmtTypeEnv* _Nullable gotEnv) {
    const SLAstNode* want;
    const SLAstNode* got;
    int32_t          bindingIndex;
    if (wantNodeId < 0 || gotNodeId < 0 || (uint32_t)wantNodeId >= ast->len
        || (uint32_t)gotNodeId >= ast->len)
    {
        return 0;
    }
    if (SLFmtTypeNameIsBoundParam(ast, src, wantNodeId, wantEnv, &bindingIndex)) {
        int32_t boundNodeId = wantEnv->items[bindingIndex].concreteTypeNodeId;
        if (boundNodeId >= 0) {
            return SLFmtTypeCompatibleWithEnvs(ast, src, boundNodeId, NULL, gotNodeId, gotEnv);
        }
        if (gotEnv == NULL || !SLFmtTypeNameIsBoundParam(ast, src, gotNodeId, gotEnv, &bindingIndex)
            || gotEnv->items[bindingIndex].concreteTypeNodeId < 0)
        {
            wantEnv
                ->items[SLFmtTypeEnvFind(
                    wantEnv, src, ast->nodes[wantNodeId].dataStart, ast->nodes[wantNodeId].dataEnd)]
                .concreteTypeNodeId = gotNodeId;
            return 1;
        }
    }
    if (gotEnv != NULL && SLFmtTypeNameIsBoundParam(ast, src, gotNodeId, gotEnv, &bindingIndex)) {
        int32_t boundNodeId = gotEnv->items[bindingIndex].concreteTypeNodeId;
        if (boundNodeId < 0) {
            return 0;
        }
        return SLFmtTypeCompatibleWithEnvs(ast, src, wantNodeId, wantEnv, boundNodeId, NULL);
    }
    want = &ast->nodes[wantNodeId];
    got = &ast->nodes[gotNodeId];
    if (want->kind != got->kind) {
        return 0;
    }
    switch (want->kind) {
        case SLAst_TYPE_NAME:
            if (!SLFmtSlicesEqual(
                    src, want->dataStart, want->dataEnd, got->dataStart, got->dataEnd))
            {
                return 0;
            }
            return SLFmtTypeCompatibleChildren(ast, src, wantNodeId, wantEnv, gotNodeId, gotEnv);
        case SLAst_TYPE_PTR:
        case SLAst_TYPE_REF:
        case SLAst_TYPE_MUTREF:
        case SLAst_TYPE_SLICE:
        case SLAst_TYPE_MUTSLICE:
        case SLAst_TYPE_OPTIONAL:
        case SLAst_TYPE_ARRAY:
        case SLAst_TYPE_VARRAY:
        case SLAst_TYPE_TUPLE:
        case SLAst_TYPE_FN:
            return SLFmtTypeCompatibleChildren(ast, src, wantNodeId, wantEnv, gotNodeId, gotEnv);
        default: return SLFmtSlicesEqual(src, want->start, want->end, got->start, got->end);
    }
}

static int SLFmtInferLocalCallAgainstFn(
    const SLAst* ast,
    SLStrView    src,
    int32_t      callNodeId,
    int32_t      fnNodeId,
    uint32_t     beforePos,
    int32_t      targetArgNodeId,
    SLFmtInferredType* _Nullable outReturnType,
    SLFmtInferredType* _Nullable outTargetParamType);

static int SLFmtCanDropLiteralCastFromLocalCall(
    const SLAst* ast, SLStrView src, int32_t castNodeId, int32_t castTypeNodeId) {
    int32_t  parentNodeId;
    int32_t  argNodeId;
    int32_t  callNodeId;
    int32_t  calleeNodeId;
    uint32_t calleeNameStart;
    uint32_t calleeNameEnd;
    int32_t  cur;
    int      sawMappedCandidate = 0;
    if (ast == NULL || castNodeId < 0 || castTypeNodeId < 0 || (uint32_t)castNodeId >= ast->len) {
        return 0;
    }
    parentNodeId = SLFmtFindParentNode(ast, castNodeId);
    if (parentNodeId < 0) {
        return 0;
    }
    if (ast->nodes[parentNodeId].kind == SLAst_CALL_ARG) {
        argNodeId = parentNodeId;
        callNodeId = SLFmtFindParentNode(ast, parentNodeId);
    } else if (ast->nodes[parentNodeId].kind == SLAst_CALL) {
        argNodeId = castNodeId;
        callNodeId = parentNodeId;
    } else {
        return 0;
    }
    if (callNodeId < 0 || ast->nodes[callNodeId].kind != SLAst_CALL) {
        return 0;
    }
    calleeNodeId = SLFmtFirstChild(ast, callNodeId);
    if (calleeNodeId < 0 || (uint32_t)calleeNodeId >= ast->len) {
        return 0;
    }
    if (ast->nodes[calleeNodeId].kind == SLAst_IDENT) {
        calleeNameStart = ast->nodes[calleeNodeId].dataStart;
        calleeNameEnd = ast->nodes[calleeNodeId].dataEnd;
    } else if (ast->nodes[calleeNodeId].kind == SLAst_FIELD_EXPR) {
        calleeNameStart = ast->nodes[calleeNodeId].dataStart;
        calleeNameEnd = ast->nodes[calleeNodeId].dataEnd;
    } else {
        return 0;
    }
    cur = SLFmtFirstChild(ast, ast->root);
    while (cur >= 0) {
        if (ast->nodes[cur].kind == SLAst_FN
            && SLFmtSlicesEqual(
                src,
                ast->nodes[cur].dataStart,
                ast->nodes[cur].dataEnd,
                calleeNameStart,
                calleeNameEnd))
        {
            SLFmtInferredType targetType;
            SLFmtInferredTypeInit(&targetType);
            if (!SLFmtInferLocalCallAgainstFn(
                    ast,
                    src,
                    callNodeId,
                    cur,
                    ast->nodes[castNodeId].start,
                    argNodeId,
                    NULL,
                    &targetType))
            {
                cur = SLFmtNextSibling(ast, cur);
                continue;
            }
            sawMappedCandidate = 1;
            if (!SLFmtInferredTypeMatchesNode(ast, src, &targetType, castTypeNodeId)) {
                return 0;
            }
        }
        cur = SLFmtNextSibling(ast, cur);
    }
    return sawMappedCandidate;
}

static void SLFmtGetCastParts(
    const SLAst* ast, int32_t castNodeId, int32_t* outExprNodeId, int32_t* outTypeNodeId) {
    int32_t exprNodeId = -1;
    int32_t typeNodeId = -1;
    if (castNodeId >= 0 && (uint32_t)castNodeId < ast->len
        && ast->nodes[castNodeId].kind == SLAst_CAST)
    {
        exprNodeId = SLFmtFirstChild(ast, castNodeId);
        typeNodeId = exprNodeId >= 0 ? SLFmtNextSibling(ast, exprNodeId) : -1;
    }
    if (outExprNodeId != NULL) {
        *outExprNodeId = exprNodeId;
    }
    if (outTypeNodeId != NULL) {
        *outTypeNodeId = typeNodeId;
    }
}

static void SLFmtGetVarLikeTypeAndInit(
    const SLAst* ast, int32_t nodeId, int32_t* outTypeNodeId, int32_t* outInitNodeId) {
    int32_t firstChild = SLFmtFirstChild(ast, nodeId);
    int32_t typeNodeId = -1;
    int32_t initNodeId = -1;
    if (firstChild >= 0 && ast->nodes[firstChild].kind == SLAst_NAME_LIST) {
        int32_t afterNames = SLFmtNextSibling(ast, firstChild);
        if (afterNames >= 0 && SLFmtIsTypeNodeKindRaw(ast->nodes[afterNames].kind)) {
            typeNodeId = afterNames;
            initNodeId = SLFmtNextSibling(ast, afterNames);
        } else {
            initNodeId = afterNames;
        }
    } else if (firstChild >= 0 && SLFmtIsTypeNodeKindRaw(ast->nodes[firstChild].kind)) {
        typeNodeId = firstChild;
        initNodeId = SLFmtNextSibling(ast, firstChild);
    } else {
        initNodeId = firstChild;
    }
    if (outTypeNodeId != NULL) {
        *outTypeNodeId = typeNodeId;
    }
    if (outInitNodeId != NULL) {
        *outInitNodeId = initNodeId;
    }
}

static int32_t SLFmtFindFnReturnTypeNode(const SLAst* ast, int32_t fnNodeId) {
    int32_t child;
    if (fnNodeId < 0 || (uint32_t)fnNodeId >= ast->len || ast->nodes[fnNodeId].kind != SLAst_FN) {
        return -1;
    }
    child = SLFmtFirstChild(ast, fnNodeId);
    while (child >= 0) {
        const SLAstNode* n = &ast->nodes[child];
        if (SLFmtIsTypeNodeKindRaw(n->kind) && n->flags == 1) {
            return child;
        }
        child = SLFmtNextSibling(ast, child);
    }
    return -1;
}

static int32_t SLFmtFindParentNode(const SLAst* ast, int32_t childNodeId) {
    uint32_t i;
    if (childNodeId < 0 || (uint32_t)childNodeId >= ast->len) {
        return -1;
    }
    for (i = 0; i < ast->len; i++) {
        int32_t cur = ast->nodes[i].firstChild;
        while (cur >= 0) {
            if (cur == childNodeId) {
                return (int32_t)i;
            }
            cur = ast->nodes[cur].nextSibling;
        }
    }
    return -1;
}

static int SLFmtNodeDeclaresNameRange(
    const SLAst* ast, SLStrView src, int32_t nodeId, uint32_t nameStart, uint32_t nameEnd) {
    const SLAstNode* n;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len || nameEnd <= nameStart) {
        return 0;
    }
    n = &ast->nodes[nodeId];
    if ((n->kind == SLAst_PARAM || n->kind == SLAst_VAR || n->kind == SLAst_CONST)
        && SLFmtSlicesEqual(src, n->dataStart, n->dataEnd, nameStart, nameEnd))
    {
        return 1;
    }
    if (n->kind == SLAst_VAR || n->kind == SLAst_CONST) {
        int32_t child = SLFmtFirstChild(ast, nodeId);
        if (child >= 0 && (uint32_t)child < ast->len && ast->nodes[child].kind == SLAst_NAME_LIST) {
            int32_t nameNode = SLFmtFirstChild(ast, child);
            while (nameNode >= 0) {
                const SLAstNode* nn = &ast->nodes[nameNode];
                if (nn->kind == SLAst_IDENT
                    && SLFmtSlicesEqual(src, nn->dataStart, nn->dataEnd, nameStart, nameEnd))
                {
                    return 1;
                }
                nameNode = SLFmtNextSibling(ast, nameNode);
            }
        }
    }
    return 0;
}

static int SLFmtNodeDeclaresNameLiteral(
    const SLAst* ast, SLStrView src, int32_t nodeId, const char* nameLit) {
    const SLAstNode* n;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return 0;
    }
    n = &ast->nodes[nodeId];
    if ((n->kind == SLAst_PARAM || n->kind == SLAst_VAR || n->kind == SLAst_CONST)
        && SLFmtSliceEqLiteral(src, n->dataStart, n->dataEnd, nameLit))
    {
        return 1;
    }
    if (n->kind == SLAst_VAR || n->kind == SLAst_CONST) {
        int32_t child = SLFmtFirstChild(ast, nodeId);
        if (child >= 0 && (uint32_t)child < ast->len && ast->nodes[child].kind == SLAst_NAME_LIST) {
            int32_t nameNode = SLFmtFirstChild(ast, child);
            while (nameNode >= 0) {
                const SLAstNode* nn = &ast->nodes[nameNode];
                if (nn->kind == SLAst_IDENT
                    && SLFmtSliceEqLiteral(src, nn->dataStart, nn->dataEnd, nameLit))
                {
                    return 1;
                }
                nameNode = SLFmtNextSibling(ast, nameNode);
            }
        }
    }
    return 0;
}

static int SLFmtFindLocalBindingBefore(
    const SLAst* ast,
    SLStrView    src,
    int32_t      identNodeId,
    uint32_t     beforePos,
    int32_t* _Nullable outDeclNodeId,
    int32_t* _Nullable outTypeNodeId,
    int32_t* _Nullable outInitNodeId);

static int SLFmtFindFieldTypeOnLocalNamedType(
    const SLAst*             ast,
    SLStrView                src,
    const SLFmtInferredType* baseType,
    uint32_t                 fieldStart,
    uint32_t                 fieldEnd,
    SLFmtInferredType*       outType);

static int SLFmtInferLocalCallAgainstFn(
    const SLAst* ast,
    SLStrView    src,
    int32_t      callNodeId,
    int32_t      fnNodeId,
    uint32_t     beforePos,
    int32_t      targetArgNodeId,
    SLFmtInferredType* _Nullable outReturnType,
    SLFmtInferredType* _Nullable outTargetParamType);

static int SLFmtInferExprTypeEx(
    const SLAst*       ast,
    SLStrView          src,
    int32_t            exprNodeId,
    uint32_t           beforePos,
    uint32_t           depth,
    SLFmtInferredType* outType) {
    const SLAstNode* n;
    if (outType != NULL) {
        SLFmtInferredTypeInit(outType);
    }
    if (exprNodeId < 0 || (uint32_t)exprNodeId >= ast->len) {
        return 0;
    }
    if (depth >= 32u || outType == NULL) {
        return 0;
    }
    n = &ast->nodes[exprNodeId];
    if (n->kind == SLAst_CAST) {
        int32_t castExprNodeId = -1;
        int32_t castTypeNodeId = -1;
        SLFmtGetCastParts(ast, exprNodeId, &castExprNodeId, &castTypeNodeId);
        (void)castExprNodeId;
        return SLFmtInferredTypeSet(outType, castTypeNodeId, NULL);
    }
    if (n->kind == SLAst_IDENT) {
        int32_t declNodeId = -1;
        int32_t typeNodeId = -1;
        int32_t initNodeId = -1;
        if (!SLFmtFindLocalBindingBefore(
                ast, src, exprNodeId, beforePos, &declNodeId, &typeNodeId, &initNodeId))
        {
            return 0;
        }
        if (typeNodeId >= 0) {
            return SLFmtInferredTypeSet(outType, typeNodeId, NULL);
        }
        if (initNodeId >= 0) {
            return SLFmtInferExprTypeEx(
                ast, src, initNodeId, ast->nodes[declNodeId].start, depth + 1u, outType);
        }
        return 0;
    }
    if (n->kind == SLAst_FIELD_EXPR) {
        SLFmtInferredType baseType;
        int32_t           baseNodeId = SLFmtFirstChild(ast, exprNodeId);
        SLFmtInferredTypeInit(&baseType);
        return baseNodeId >= 0
            && SLFmtInferExprTypeEx(ast, src, baseNodeId, beforePos, depth + 1u, &baseType)
            && SLFmtFindFieldTypeOnLocalNamedType(
                   ast, src, &baseType, n->dataStart, n->dataEnd, outType);
    }
    if (n->kind == SLAst_UNARY && n->op == SLTok_MUL) {
        SLFmtInferredType targetType;
        int32_t           targetNodeId = SLFmtFirstChild(ast, exprNodeId);
        int32_t           targetTypeNodeId;
        const SLAstNode*  targetTypeNode;
        SLFmtInferredTypeInit(&targetType);
        if (targetNodeId < 0
            || !SLFmtInferExprTypeEx(ast, src, targetNodeId, beforePos, depth + 1u, &targetType))
        {
            return 0;
        }
        targetTypeNodeId = targetType.typeNodeId;
        if (targetTypeNodeId < 0 || (uint32_t)targetTypeNodeId >= ast->len) {
            return 0;
        }
        targetTypeNode = &ast->nodes[targetTypeNodeId];
        if (targetTypeNode->kind != SLAst_TYPE_PTR && targetTypeNode->kind != SLAst_TYPE_REF
            && targetTypeNode->kind != SLAst_TYPE_MUTREF)
        {
            return 0;
        }
        return SLFmtInferredTypeSet(
            outType, SLFmtFirstChild(ast, targetTypeNodeId), &targetType.env);
    }
    if (n->kind == SLAst_CALL) {
        int32_t  calleeNodeId = SLFmtFirstChild(ast, exprNodeId);
        uint32_t calleeNameStart;
        uint32_t calleeNameEnd;
        int32_t  cur;
        if (calleeNodeId < 0 || (uint32_t)calleeNodeId >= ast->len) {
            return 0;
        }
        if (ast->nodes[calleeNodeId].kind == SLAst_IDENT) {
            calleeNameStart = ast->nodes[calleeNodeId].dataStart;
            calleeNameEnd = ast->nodes[calleeNodeId].dataEnd;
        } else if (ast->nodes[calleeNodeId].kind == SLAst_FIELD_EXPR) {
            calleeNameStart = ast->nodes[calleeNodeId].dataStart;
            calleeNameEnd = ast->nodes[calleeNodeId].dataEnd;
        } else {
            return 0;
        }
        cur = SLFmtFirstChild(ast, ast->root);
        while (cur >= 0) {
            if (ast->nodes[cur].kind == SLAst_FN
                && SLFmtSlicesEqual(
                    src,
                    ast->nodes[cur].dataStart,
                    ast->nodes[cur].dataEnd,
                    calleeNameStart,
                    calleeNameEnd)
                && SLFmtInferLocalCallAgainstFn(
                    ast, src, exprNodeId, cur, beforePos, -1, outType, NULL))
            {
                return 1;
            }
            cur = SLFmtNextSibling(ast, cur);
        }
        return 0;
    }
    if (n->kind == SLAst_COMPOUND_LIT) {
        int32_t typeNodeId = SLFmtFirstChild(ast, exprNodeId);
        if (typeNodeId >= 0 && SLFmtIsTypeNodeKindRaw(ast->nodes[typeNodeId].kind)) {
            return SLFmtInferredTypeSet(outType, typeNodeId, NULL);
        }
    }
    return 0;
}

static int SLFmtCanDropRedundantLiteralCast(
    const SLAst* ast, SLStrView src, const SLFormatOptions* _Nullable options, int32_t castNodeId) {
    const SLAstNode* castNode;
    int32_t          castExprNodeId = -1;
    int32_t          castTypeNodeId = -1;
    int32_t          parentNodeId;
    if (castNodeId < 0 || (uint32_t)castNodeId >= ast->len) {
        return 0;
    }
    castNode = &ast->nodes[castNodeId];
    if (castNode->kind != SLAst_CAST) {
        return 0;
    }
    SLFmtGetCastParts(ast, castNodeId, &castExprNodeId, &castTypeNodeId);
    if (SLFmtCastLiteralNumericType(ast, src, castExprNodeId, castTypeNodeId)
        == SLFmtNumericType_INVALID)
    {
        return 0;
    }
    parentNodeId = SLFmtFindParentNode(ast, castNodeId);
    if (parentNodeId < 0) {
        return 0;
    }
    if (ast->nodes[parentNodeId].kind == SLAst_UNARY && ast->nodes[parentNodeId].op == SLTok_SUB) {
        int32_t           binaryNodeId = SLFmtFindParentNode(ast, parentNodeId);
        int32_t           lhsNodeId;
        int32_t           rhsNodeId;
        int32_t           otherNodeId = -1;
        SLFmtInferredType otherType;
        if (SLFmtFirstChild(ast, parentNodeId) != castNodeId || binaryNodeId < 0
            || ast->nodes[binaryNodeId].kind != SLAst_BINARY
            || !SLFmtBinaryOpSharesOperandType(ast->nodes[binaryNodeId].op))
        {
            return 0;
        }
        lhsNodeId = SLFmtFirstChild(ast, binaryNodeId);
        rhsNodeId = lhsNodeId >= 0 ? SLFmtNextSibling(ast, lhsNodeId) : -1;
        if (lhsNodeId == parentNodeId) {
            otherNodeId = rhsNodeId;
        } else if (rhsNodeId == parentNodeId) {
            otherNodeId = lhsNodeId;
        }
        if (otherNodeId < 0) {
            return 0;
        }
        SLFmtInferredTypeInit(&otherType);
        return SLFmtInferExprTypeEx(ast, src, otherNodeId, castNode->start, 0u, &otherType)
            && SLFmtInferredTypeMatchesNode(ast, src, &otherType, castTypeNodeId);
    }
    switch (ast->nodes[parentNodeId].kind) {
        case SLAst_VAR:
        case SLAst_CONST: {
            int32_t declTypeNodeId = -1;
            int32_t initNodeId = -1;
            SLFmtGetVarLikeTypeAndInit(ast, parentNodeId, &declTypeNodeId, &initNodeId);
            if (initNodeId == castNodeId
                && SLFmtTypeNodesEqualBySource(ast, src, castTypeNodeId, declTypeNodeId))
            {
                return 1;
            }
            return 0;
        }
        case SLAst_RETURN: {
            int32_t fnNodeId;
            int32_t retTypeNodeId;
            int32_t retExprNodeId = SLFmtFirstChild(ast, parentNodeId);
            if (retExprNodeId != castNodeId) {
                return 0;
            }
            fnNodeId = SLFmtFindEnclosingFnNode(ast, parentNodeId);
            retTypeNodeId = SLFmtFindFnReturnTypeNode(ast, fnNodeId);
            return SLFmtTypeNodesEqualBySource(ast, src, castTypeNodeId, retTypeNodeId);
        }
        case SLAst_BINARY: {
            int32_t           lhsNodeId = SLFmtFirstChild(ast, parentNodeId);
            int32_t           rhsNodeId = lhsNodeId >= 0 ? SLFmtNextSibling(ast, lhsNodeId) : -1;
            int32_t           otherNodeId = -1;
            SLFmtInferredType otherType;
            if (rhsNodeId == castNodeId
                && SLFmtIsAssignmentOp((SLTokenKind)ast->nodes[parentNodeId].op))
            {
                SLFmtInferredTypeInit(&otherType);
                return SLFmtInferExprTypeEx(ast, src, lhsNodeId, castNode->start, 0u, &otherType)
                    && SLFmtInferredTypeMatchesNode(ast, src, &otherType, castTypeNodeId);
            }
            if (!SLFmtBinaryOpSharesOperandType(ast->nodes[parentNodeId].op)) {
                return 0;
            }
            if (lhsNodeId == castNodeId) {
                otherNodeId = rhsNodeId;
            } else if (rhsNodeId == castNodeId) {
                otherNodeId = lhsNodeId;
            }
            if (otherNodeId < 0) {
                return 0;
            }
            SLFmtInferredTypeInit(&otherType);
            return SLFmtInferExprTypeEx(ast, src, otherNodeId, castNode->start, 0u, &otherType)
                && SLFmtInferredTypeMatchesNode(ast, src, &otherType, castTypeNodeId);
        }
        default:
            if (SLFmtCanDropLiteralCastFromLocalCall(ast, src, castNodeId, castTypeNodeId)) {
                return 1;
            }
            if (options != NULL && options->canDropLiteralCast != NULL) {
                return options->canDropLiteralCast(options->ctx, ast, src, castNodeId);
            }
            return 0;
    }
}

static int SLFmtKeywordIsVar(const char* kw) {
    return kw[0] == 'v' && kw[1] == 'a' && kw[2] == 'r' && kw[3] == '\0';
}

static void SLFmtRewriteVarTypeFromLiteralCast(
    const SLAst* ast,
    SLStrView    src,
    const char*  kw,
    uint32_t     nameCount,
    int32_t*     ioTypeNodeId,
    int32_t*     ioInitNodeId) {
    int32_t initNodeId;
    int32_t castExprNodeId = -1;
    int32_t castTypeNodeId = -1;
    if (ioTypeNodeId == NULL || ioInitNodeId == NULL || !SLFmtKeywordIsVar(kw) || nameCount != 1u) {
        return;
    }
    if (*ioTypeNodeId >= 0 || *ioInitNodeId < 0 || (uint32_t)*ioInitNodeId >= ast->len) {
        return;
    }
    initNodeId = *ioInitNodeId;
    if (ast->nodes[initNodeId].kind != SLAst_CAST) {
        return;
    }
    SLFmtGetCastParts(ast, initNodeId, &castExprNodeId, &castTypeNodeId);
    if (SLFmtCastLiteralNumericType(ast, src, castExprNodeId, castTypeNodeId)
        == SLFmtNumericType_INVALID)
    {
        return;
    }
    *ioTypeNodeId = castTypeNodeId;
    *ioInitNodeId = castExprNodeId;
}

static void SLFmtRewriteRedundantVarType(
    const SLAst* ast,
    SLStrView    src,
    const char*  kw,
    uint32_t     nameCount,
    uint32_t     declStart,
    int32_t*     ioTypeNodeId,
    int32_t*     ioInitNodeId) {
    SLFmtInferredType initType;
    int32_t           typeNodeId;
    int32_t           initNodeId;
    if (ioTypeNodeId == NULL || ioInitNodeId == NULL || !SLFmtKeywordIsVar(kw) || nameCount != 1u) {
        return;
    }
    typeNodeId = *ioTypeNodeId;
    initNodeId = *ioInitNodeId;
    if (typeNodeId < 0 || initNodeId < 0 || (uint32_t)typeNodeId >= ast->len
        || (uint32_t)initNodeId >= ast->len)
    {
        return;
    }
    if (ast->nodes[initNodeId].kind == SLAst_CAST || ast->nodes[initNodeId].kind == SLAst_CALL) {
        return;
    }
    SLFmtInferredTypeInit(&initType);
    if (!SLFmtInferExprTypeEx(ast, src, initNodeId, declStart, 0u, &initType)) {
        return;
    }
    if (SLFmtInferredTypeMatchesNode(ast, src, &initType, typeNodeId)) {
        *ioTypeNodeId = -1;
    }
}

static int32_t SLFmtFindEnclosingFnNode(const SLAst* ast, int32_t nodeId) {
    const SLAstNode* target;
    int32_t          best = -1;
    uint32_t         bestSpan = UINT32_MAX;
    uint32_t         i;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    target = &ast->nodes[nodeId];
    for (i = 0; i < ast->len; i++) {
        const SLAstNode* n = &ast->nodes[i];
        if (n->kind != SLAst_FN) {
            continue;
        }
        if (n->start <= target->start && target->end <= n->end) {
            uint32_t span = n->end - n->start;
            if (span < bestSpan) {
                best = (int32_t)i;
                bestSpan = span;
            }
        }
    }
    return best;
}

static int SLFmtFnHasImplicitContextLocal(const SLAst* ast, SLStrView src, int32_t fnNodeId) {
    const SLAstNode* fn;
    int32_t          child;
    if (fnNodeId < 0 || (uint32_t)fnNodeId >= ast->len) {
        return 0;
    }
    fn = &ast->nodes[fnNodeId];
    if (fn->kind != SLAst_FN) {
        return 0;
    }
    if (SLFmtSliceEqLiteral(src, fn->dataStart, fn->dataEnd, "main")) {
        return 1;
    }
    child = SLFmtFirstChild(ast, fnNodeId);
    while (child >= 0) {
        if (ast->nodes[child].kind == SLAst_CONTEXT_CLAUSE) {
            return 1;
        }
        child = SLFmtNextSibling(ast, child);
    }
    return 0;
}

static int SLFmtFnHasShadowingContextLocalBefore(
    const SLAst* ast, SLStrView src, int32_t fnNodeId, uint32_t beforePos) {
    const SLAstNode* fn;
    uint32_t         i;
    if (fnNodeId < 0 || (uint32_t)fnNodeId >= ast->len) {
        return 0;
    }
    fn = &ast->nodes[fnNodeId];
    if (fn->kind != SLAst_FN) {
        return 0;
    }
    for (i = 0; i < ast->len; i++) {
        const SLAstNode* n = &ast->nodes[i];
        if (n->start < fn->start || n->end > fn->end || n->start >= beforePos) {
            continue;
        }
        if (SLFmtNodeDeclaresNameLiteral(ast, src, (int32_t)i, "context")) {
            return 1;
        }
    }
    return 0;
}

static int SLFmtExprIsImplicitContextFieldForBind(
    const SLAst* ast, SLStrView src, const SLAstNode* bindNode, int32_t exprNodeId) {
    const SLAstNode* exprNode;
    int32_t          baseNodeId;
    uint32_t         baseStart;
    uint32_t         baseEnd;
    if (exprNodeId < 0 || (uint32_t)exprNodeId >= ast->len) {
        return 0;
    }
    exprNode = &ast->nodes[exprNodeId];
    if (exprNode->kind != SLAst_FIELD_EXPR) {
        return 0;
    }
    if (!SLFmtSlicesEqual(
            src, bindNode->dataStart, bindNode->dataEnd, exprNode->dataStart, exprNode->dataEnd))
    {
        return 0;
    }
    baseNodeId = SLFmtFirstChild(ast, exprNodeId);
    if (!SLFmtExprIsPlainIdent(ast, baseNodeId, &baseStart, &baseEnd)) {
        return 0;
    }
    return SLFmtSliceEqLiteral(src, baseStart, baseEnd, "context");
}

typedef int (*SLFmtExprRewriteRule)(const SLAst* ast, SLStrView src, int32_t* exprNodeId);

static int SLFmtRewriteExprIdentity(const SLAst* ast, SLStrView src, int32_t* exprNodeId) {
    (void)ast;
    (void)src;
    (void)exprNodeId;
    return 0;
}

static int SLFmtRewriteExpr(const SLAst* ast, SLStrView src, int32_t* exprNodeId) {
    static const SLFmtExprRewriteRule rules[] = {
        SLFmtRewriteExprIdentity,
    };
    uint32_t i;
    if (exprNodeId == NULL) {
        return -1;
    }
    for (i = 0; i < (uint32_t)(sizeof(rules) / sizeof(rules[0])); i++) {
        if (rules[i](ast, src, exprNodeId) != 0) {
            return -1;
        }
    }
    return 0;
}

static int SLFmtRewriteCallArgShorthand(const SLAst* ast, SLStrView src, int32_t nodeId) {
    const SLAstNode* node;
    SLAstNode*       mutNode;
    int32_t          exprNode;
    uint32_t         identStart;
    uint32_t         identEnd;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    node = &ast->nodes[nodeId];
    if (node->kind != SLAst_CALL_ARG || node->dataEnd <= node->dataStart) {
        return 0;
    }
    exprNode = SLFmtFirstChild(ast, nodeId);
    if (exprNode < 0) {
        return 0;
    }
    if (SLFmtRewriteExpr(ast, src, &exprNode) != 0) {
        return -1;
    }
    if (!SLFmtExprIsPlainIdent(ast, exprNode, &identStart, &identEnd)) {
        return 0;
    }
    if (!SLFmtSlicesEqual(src, node->dataStart, node->dataEnd, identStart, identEnd)) {
        return 0;
    }
    mutNode = (SLAstNode*)&ast->nodes[nodeId];
    mutNode->dataStart = 0;
    mutNode->dataEnd = 0;
    return 0;
}

static int SLFmtRewriteCompoundFieldShorthand(const SLAst* ast, SLStrView src, int32_t nodeId) {
    const SLAstNode* node;
    SLAstNode*       mutNode;
    int32_t          exprNode;
    uint32_t         identStart;
    uint32_t         identEnd;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    node = &ast->nodes[nodeId];
    if (node->kind != SLAst_COMPOUND_FIELD
        || (node->flags & SLAstFlag_COMPOUND_FIELD_SHORTHAND) != 0
        || node->dataEnd <= node->dataStart || node->dataEnd > src.len)
    {
        return 0;
    }
    if (SLFmtSliceHasChar(src, node->dataStart, node->dataEnd, '.')) {
        return 0;
    }
    exprNode = SLFmtFirstChild(ast, nodeId);
    if (exprNode < 0) {
        return 0;
    }
    if (SLFmtRewriteExpr(ast, src, &exprNode) != 0) {
        return -1;
    }
    if (!SLFmtExprIsPlainIdent(ast, exprNode, &identStart, &identEnd)) {
        return 0;
    }
    if (!SLFmtSlicesEqual(src, node->dataStart, node->dataEnd, identStart, identEnd)) {
        return 0;
    }
    mutNode = (SLAstNode*)&ast->nodes[nodeId];
    mutNode->flags |= SLAstFlag_COMPOUND_FIELD_SHORTHAND;
    return 0;
}

static int SLFmtRewriteContextBindShorthand(const SLAst* ast, SLStrView src, int32_t nodeId) {
    const SLAstNode* node;
    SLAstNode*       mutNode;
    int32_t          exprNode;
    int32_t          enclosingFn;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    node = &ast->nodes[nodeId];
    if (node->kind != SLAst_CONTEXT_BIND || node->dataEnd <= node->dataStart
        || (node->flags & SLAstFlag_CONTEXT_BIND_SHORTHAND) != 0)
    {
        return 0;
    }
    exprNode = SLFmtFirstChild(ast, nodeId);
    if (exprNode < 0) {
        return 0;
    }
    if (SLFmtRewriteExpr(ast, src, &exprNode) != 0) {
        return -1;
    }
    if (!SLFmtExprIsImplicitContextFieldForBind(ast, src, node, exprNode)) {
        return 0;
    }
    enclosingFn = SLFmtFindEnclosingFnNode(ast, nodeId);
    if (enclosingFn < 0 || !SLFmtFnHasImplicitContextLocal(ast, src, enclosingFn)
        || SLFmtFnHasShadowingContextLocalBefore(ast, src, enclosingFn, node->start))
    {
        return 0;
    }
    mutNode = (SLAstNode*)&ast->nodes[nodeId];
    mutNode->flags |= SLAstFlag_CONTEXT_BIND_SHORTHAND;
    return 0;
}

static int SLFmtRewriteDropRedundantCastParenFlag(const SLAst* ast, int32_t nodeId) {
    const SLAstNode* n;
    SLAstNode*       mutNode;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    n = &ast->nodes[nodeId];
    if (n->kind != SLAst_CAST || (n->flags & SLAstFlag_PAREN) == 0) {
        return 0;
    }
    mutNode = (SLAstNode*)&ast->nodes[nodeId];
    mutNode->flags &= ~SLAstFlag_PAREN;
    return 0;
}

static int SLFmtRewriteBinaryCastParens(const SLAst* ast, int32_t nodeId) {
    int32_t lhs;
    int32_t rhs;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    if (ast->nodes[nodeId].kind != SLAst_BINARY) {
        return 0;
    }
    lhs = SLFmtFirstChild(ast, nodeId);
    rhs = lhs >= 0 ? SLFmtNextSibling(ast, lhs) : -1;
    /* Cast is postfix and binds tighter than all infix binary operators. */
    if (SLFmtRewriteDropRedundantCastParenFlag(ast, lhs) != 0
        || SLFmtRewriteDropRedundantCastParenFlag(ast, rhs) != 0)
    {
        return -1;
    }
    return 0;
}

static int SLFmtRewriteRedundantLiteralCast(
    const SLAst* ast, SLStrView src, const SLFormatOptions* _Nullable options, int32_t nodeId) {
    const SLAstNode* node;
    SLAstNode*       mutNode;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    node = &ast->nodes[nodeId];
    if (node->kind != SLAst_CAST) {
        return 0;
    }
    if (!SLFmtCanDropRedundantLiteralCast(ast, src, options, nodeId)) {
        return 0;
    }
    mutNode = (SLAstNode*)&ast->nodes[nodeId];
    mutNode->flags |= SLFmtFlag_DROP_REDUNDANT_LITERAL_CAST;
    return 0;
}

static int SLFmtRewriteReturnParens(const SLAst* ast, int32_t nodeId) {
    int32_t          exprNodeId;
    const SLAstNode* exprNode;
    SLAstNode*       mutExprNode;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    if (ast->nodes[nodeId].kind != SLAst_RETURN) {
        return 0;
    }
    exprNodeId = SLFmtFirstChild(ast, nodeId);
    if (exprNodeId < 0 || (uint32_t)exprNodeId >= ast->len) {
        return 0;
    }
    exprNode = &ast->nodes[exprNodeId];
    mutExprNode = (SLAstNode*)exprNode;
    if ((exprNode->flags & SLAstFlag_PAREN) != 0) {
        mutExprNode->flags &= ~SLAstFlag_PAREN;
        exprNode = mutExprNode;
    }
    if (exprNode->kind == SLAst_TUPLE_EXPR && (exprNode->flags & SLAstFlag_PAREN) == 0) {
        mutExprNode->kind = SLAst_EXPR_LIST;
    }
    return 0;
}

static int SLFmtRewriteAst(
    const SLAst* ast, SLStrView src, const SLFormatOptions* _Nullable options) {
    uint32_t i;
    if (ast == NULL || ast->nodes == NULL) {
        return -1;
    }
    for (i = 0; i < ast->len; i++) {
        int32_t nodeId = (int32_t)i;
        if (SLFmtRewriteCallArgShorthand(ast, src, nodeId) != 0
            || SLFmtRewriteCompoundFieldShorthand(ast, src, nodeId) != 0
            || SLFmtRewriteContextBindShorthand(ast, src, nodeId) != 0
            || SLFmtRewriteRedundantLiteralCast(ast, src, options, nodeId) != 0
            || SLFmtRewriteBinaryCastParens(ast, nodeId) != 0
            || SLFmtRewriteReturnParens(ast, nodeId) != 0)
        {
            return -1;
        }
    }
    return 0;
}

static int SLFmtIsTypeNodeKind(SLAstKind kind) {
    return SLFmtIsTypeNodeKindRaw(kind);
}

static int SLFmtIsStmtNodeKind(SLAstKind kind) {
    switch (kind) {
        case SLAst_BLOCK:
        case SLAst_VAR:
        case SLAst_CONST:
        case SLAst_CONST_BLOCK:
        case SLAst_IF:
        case SLAst_FOR:
        case SLAst_SWITCH:
        case SLAst_RETURN:
        case SLAst_BREAK:
        case SLAst_CONTINUE:
        case SLAst_DEFER:
        case SLAst_ASSERT:
        case SLAst_MULTI_ASSIGN:
        case SLAst_EXPR_STMT:    return 1;
        default:                 return 0;
    }
}

static int SLFmtIsGroupedVarLike(const SLFmtCtx* c, int32_t nodeId) {
    int32_t firstChild;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return 0;
    }
    firstChild = SLFmtFirstChild(c->ast, nodeId);
    return firstChild >= 0 && c->ast->nodes[firstChild].kind == SLAst_NAME_LIST;
}

static int SLFmtIsAssignmentOp(SLTokenKind kind) {
    switch (kind) {
        case SLTok_ASSIGN:
        case SLTok_ADD_ASSIGN:
        case SLTok_SUB_ASSIGN:
        case SLTok_MUL_ASSIGN:
        case SLTok_DIV_ASSIGN:
        case SLTok_MOD_ASSIGN:
        case SLTok_AND_ASSIGN:
        case SLTok_OR_ASSIGN:
        case SLTok_XOR_ASSIGN:
        case SLTok_LSHIFT_ASSIGN:
        case SLTok_RSHIFT_ASSIGN: return 1;
        default:                  return 0;
    }
}

static int SLFmtBinPrec(SLTokenKind kind) {
    if (SLFmtIsAssignmentOp(kind)) {
        return 1;
    }
    switch (kind) {
        case SLTok_LOGICAL_OR:  return 2;
        case SLTok_LOGICAL_AND: return 3;
        case SLTok_EQ:
        case SLTok_NEQ:
        case SLTok_LT:
        case SLTok_GT:
        case SLTok_LTE:
        case SLTok_GTE:         return 4;
        case SLTok_OR:
        case SLTok_XOR:
        case SLTok_ADD:
        case SLTok_SUB:         return 5;
        case SLTok_AND:
        case SLTok_LSHIFT:
        case SLTok_RSHIFT:
        case SLTok_MUL:
        case SLTok_DIV:
        case SLTok_MOD:         return 6;
        default:                return 0;
    }
}

static const char* SLFmtTokenOpText(SLTokenKind kind) {
    switch (kind) {
        case SLTok_ASSIGN:        return "=";
        case SLTok_ADD:           return "+";
        case SLTok_SUB:           return "-";
        case SLTok_MUL:           return "*";
        case SLTok_DIV:           return "/";
        case SLTok_MOD:           return "%";
        case SLTok_AND:           return "&";
        case SLTok_OR:            return "|";
        case SLTok_XOR:           return "^";
        case SLTok_NOT:           return "!";
        case SLTok_LSHIFT:        return "<<";
        case SLTok_RSHIFT:        return ">>";
        case SLTok_EQ:            return "==";
        case SLTok_NEQ:           return "!=";
        case SLTok_LT:            return "<";
        case SLTok_GT:            return ">";
        case SLTok_LTE:           return "<=";
        case SLTok_GTE:           return ">=";
        case SLTok_LOGICAL_AND:   return "&&";
        case SLTok_LOGICAL_OR:    return "||";
        case SLTok_ADD_ASSIGN:    return "+=";
        case SLTok_SUB_ASSIGN:    return "-=";
        case SLTok_MUL_ASSIGN:    return "*=";
        case SLTok_DIV_ASSIGN:    return "/=";
        case SLTok_MOD_ASSIGN:    return "%=";
        case SLTok_AND_ASSIGN:    return "&=";
        case SLTok_OR_ASSIGN:     return "|=";
        case SLTok_XOR_ASSIGN:    return "^=";
        case SLTok_LSHIFT_ASSIGN: return "<<=";
        case SLTok_RSHIFT_ASSIGN: return ">>=";
        case SLTok_AS:            return "as";
        default:                  return "?";
    }
}

static int SLFmtContainsSemicolonInRange(SLStrView src, uint32_t start, uint32_t end) {
    uint32_t i;
    if (end < start || end > src.len) {
        return 0;
    }
    for (i = start; i < end; i++) {
        if (src.ptr[i] == ';') {
            return 1;
        }
    }
    return 0;
}

static int SLFmtRangeHasChar(SLStrView src, uint32_t start, uint32_t end, char ch) {
    uint32_t i;
    if (end < start || end > src.len) {
        return 0;
    }
    for (i = start; i < end; i++) {
        if (src.ptr[i] == ch) {
            return 1;
        }
    }
    return 0;
}

static int SLFmtFindCharForwardInRange(
    SLStrView src, uint32_t start, uint32_t end, char ch, uint32_t* outPos) {
    uint32_t i;
    if (end < start || end > src.len) {
        return 0;
    }
    for (i = start; i < end; i++) {
        if (src.ptr[i] == ch) {
            *outPos = i;
            return 1;
        }
    }
    return 0;
}

static int SLFmtFindCharBackwardInRange(
    SLStrView src, uint32_t start, uint32_t end, char ch, uint32_t* outPos) {
    uint32_t i;
    if (end < start || end > src.len) {
        return 0;
    }
    for (i = end; i > start; i--) {
        if (src.ptr[i - 1u] == ch) {
            *outPos = i - 1u;
            return 1;
        }
    }
    return 0;
}

static uint32_t SLFmtTrimSliceEnd(const char* s, uint32_t start, uint32_t end) {
    while (end > start) {
        char c = s[end - 1u];
        if (c == ' ' || c == '\t' || c == '\r') {
            end--;
            continue;
        }
        break;
    }
    return end;
}

static int SLFmtEmitCommentText(SLFmtCtx* c, const SLComment* cm) {
    uint32_t end = SLFmtTrimSliceEnd(c->src.ptr, cm->start, cm->end);
    if (end < cm->start) {
        end = cm->start;
    }
    return SLFmtWriteSlice(c, cm->start, end);
}

static int SLFmtIsLeadingCommentForNode(const SLFmtCtx* c, const SLComment* cm, int32_t nodeId) {
    const SLAstNode* node;
    const SLAstNode* anchor;
    if (c == NULL || cm == NULL || nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return 0;
    }
    if (cm->attachment != SLCommentAttachment_LEADING
        && cm->attachment != SLCommentAttachment_FLOATING)
    {
        return 0;
    }
    if (cm->anchorNode == nodeId) {
        return 1;
    }
    if (cm->anchorNode < 0 || (uint32_t)cm->anchorNode >= c->ast->len) {
        return 0;
    }
    node = &c->ast->nodes[nodeId];
    anchor = &c->ast->nodes[cm->anchorNode];
    if (!(anchor->start >= node->start && anchor->end <= node->end)) {
        return 0;
    }
    return cm->end <= node->start;
}

static int SLFmtEmitLeadingCommentsForNode(SLFmtCtx* c, int32_t nodeId) {
    uint32_t i;
    for (i = 0; i < c->commentLen; i++) {
        const SLComment* cm = &c->comments[i];
        if (c->commentUsed[i]) {
            continue;
        }
        if (!SLFmtIsLeadingCommentForNode(c, cm, nodeId)) {
            continue;
        }
        if (!c->lineStart) {
            if (SLFmtNewline(c) != 0) {
                return -1;
            }
        }
        if (SLFmtEmitCommentText(c, cm) != 0 || SLFmtNewline(c) != 0) {
            return -1;
        }
        c->commentUsed[i] = 1;
    }
    return 0;
}

static int SLFmtEmitTrailingCommentsForNode(SLFmtCtx* c, int32_t nodeId) {
    uint32_t i;
    int      first = 1;
    for (i = 0; i < c->commentLen; i++) {
        const SLComment* cm = &c->comments[i];
        if (c->commentUsed[i]) {
            continue;
        }
        if (cm->anchorNode != nodeId || cm->attachment != SLCommentAttachment_TRAILING) {
            continue;
        }
        if (first) {
            if (SLFmtWriteChar(c, ' ') != 0 || SLFmtEmitCommentText(c, cm) != 0) {
                return -1;
            }
            first = 0;
        } else {
            if (SLFmtNewline(c) != 0 || SLFmtEmitCommentText(c, cm) != 0) {
                return -1;
            }
        }
        c->commentUsed[i] = 1;
    }
    return 0;
}

static int SLFmtEmitRemainingComments(SLFmtCtx* c) {
    uint32_t i;
    for (i = 0; i < c->commentLen; i++) {
        if (c->commentUsed[i]) {
            continue;
        }
        if (!c->lineStart && SLFmtNewline(c) != 0) {
            return -1;
        }
        if (SLFmtEmitCommentText(c, &c->comments[i]) != 0 || SLFmtNewline(c) != 0) {
            return -1;
        }
        c->commentUsed[i] = 1;
    }
    return 0;
}

static uint32_t SLFmtCountNewlinesInRange(SLStrView src, uint32_t start, uint32_t end) {
    uint32_t i;
    uint32_t n = 0;
    if (end < start || end > src.len) {
        return 0;
    }
    for (i = start; i < end; i++) {
        if (src.ptr[i] == '\n') {
            n++;
        }
    }
    return n;
}

static int SLFmtGapHasIntentionalBlankLine(SLStrView src, uint32_t start, uint32_t end) {
    uint32_t i;
    int      lineHasContent = 1;
    if (end < start || end > src.len) {
        return 0;
    }
    for (i = start; i < end; i++) {
        char ch = src.ptr[i];
        if (ch == '\n') {
            if (!lineHasContent) {
                return 1;
            }
            lineHasContent = 0;
            continue;
        }
        if (ch != ' ' && ch != '\t' && ch != '\r') {
            lineHasContent = 1;
        }
    }
    return 0;
}

static int SLFmtNodeContainsAnchor(const SLAst* ast, int32_t nodeId, int32_t anchorNodeId) {
    const SLAstNode* n;
    const SLAstNode* a;
    if (nodeId < 0 || anchorNodeId < 0 || (uint32_t)nodeId >= ast->len
        || (uint32_t)anchorNodeId >= ast->len)
    {
        return 0;
    }
    n = &ast->nodes[nodeId];
    a = &ast->nodes[anchorNodeId];
    return a->start >= n->start && a->end <= n->end;
}

static int SLFmtCommentAnchoredToAnyNode(
    const SLAst* ast, const SLComment* cm, const int32_t* nodeIds, uint32_t nodeLen) {
    uint32_t i;
    for (i = 0; i < nodeLen; i++) {
        if (SLFmtNodeContainsAnchor(ast, nodeIds[i], cm->anchorNode)) {
            return 1;
        }
    }
    return 0;
}

static int SLFmtHasUnusedLeadingCommentsForNode(const SLFmtCtx* c, int32_t nodeId) {
    uint32_t i;
    for (i = 0; i < c->commentLen; i++) {
        const SLComment* cm = &c->comments[i];
        if (c->commentUsed[i]) {
            continue;
        }
        if (SLFmtIsLeadingCommentForNode(c, cm, nodeId)) {
            return 1;
        }
    }
    return 0;
}

static int SLFmtHasUnusedTrailingCommentsForNodes(
    const SLFmtCtx* c, const int32_t* nodeIds, uint32_t nodeLen) {
    uint32_t i;
    for (i = 0; i < c->commentLen; i++) {
        const SLComment* cm = &c->comments[i];
        if (c->commentUsed[i] || cm->attachment != SLCommentAttachment_TRAILING) {
            continue;
        }
        if (SLFmtCommentAnchoredToAnyNode(c->ast, cm, nodeIds, nodeLen)) {
            return 1;
        }
    }
    return 0;
}

static int SLFmtCommentWithinNodeRange(const SLAst* ast, const SLComment* cm, int32_t nodeId) {
    const SLAstNode* n;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return 0;
    }
    n = &ast->nodes[nodeId];
    return cm->start >= n->start && cm->start < n->end;
}

static int SLFmtHasUnusedTrailingCommentsInNodeRange(const SLFmtCtx* c, int32_t nodeId) {
    uint32_t i;
    for (i = 0; i < c->commentLen; i++) {
        const SLComment* cm = &c->comments[i];
        if (c->commentUsed[i] || cm->attachment != SLCommentAttachment_TRAILING) {
            continue;
        }
        if (SLFmtCommentWithinNodeRange(c->ast, cm, nodeId)) {
            return 1;
        }
    }
    return 0;
}

static int SLFmtEmitTrailingCommentsInNodeRange(SLFmtCtx* c, int32_t nodeId, uint32_t padBefore) {
    uint32_t i;
    int      first = 1;
    for (i = 0; i < c->commentLen; i++) {
        const SLComment* cm = &c->comments[i];
        if (c->commentUsed[i] || cm->attachment != SLCommentAttachment_TRAILING) {
            continue;
        }
        if (!SLFmtCommentWithinNodeRange(c->ast, cm, nodeId)) {
            continue;
        }
        if (first) {
            uint32_t pad = padBefore > 0 ? padBefore : 1u;
            if (SLFmtWriteSpaces(c, pad) != 0 || SLFmtEmitCommentText(c, cm) != 0) {
                return -1;
            }
            first = 0;
        } else {
            if (SLFmtNewline(c) != 0 || SLFmtEmitCommentText(c, cm) != 0) {
                return -1;
            }
        }
        c->commentUsed[i] = 1;
    }
    return 0;
}

static int SLFmtEmitTrailingCommentsForNodes(
    SLFmtCtx* c, const int32_t* nodeIds, uint32_t nodeLen, uint32_t padBefore) {
    uint32_t i;
    int      first = 1;
    for (i = 0; i < c->commentLen; i++) {
        const SLComment* cm = &c->comments[i];
        if (c->commentUsed[i] || cm->attachment != SLCommentAttachment_TRAILING) {
            continue;
        }
        if (!SLFmtCommentAnchoredToAnyNode(c->ast, cm, nodeIds, nodeLen)) {
            continue;
        }
        if (first) {
            uint32_t pad = padBefore > 0 ? padBefore : 1u;
            if (SLFmtWriteSpaces(c, pad) != 0 || SLFmtEmitCommentText(c, cm) != 0) {
                return -1;
            }
            first = 0;
        } else {
            if (SLFmtNewline(c) != 0 || SLFmtEmitCommentText(c, cm) != 0) {
                return -1;
            }
        }
        c->commentUsed[i] = 1;
    }
    return 0;
}

static int SLFmtFindSourceTrailingLineComment(
    const SLFmtCtx* c, int32_t nodeId, uint32_t* outStart, uint32_t* outEnd) {
    uint32_t i;
    uint32_t lineEnd;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len || outStart == NULL || outEnd == NULL) {
        return 0;
    }
    i = c->ast->nodes[nodeId].end;
    while (i < c->src.len && c->src.ptr[i] != '\n') {
        if (i + 1u < c->src.len && c->src.ptr[i] == '/' && c->src.ptr[i + 1u] == '/') {
            lineEnd = i;
            while (lineEnd < c->src.len && c->src.ptr[lineEnd] != '\n') {
                lineEnd++;
            }
            *outStart = i;
            *outEnd = SLFmtTrimSliceEnd(c->src.ptr, i, lineEnd);
            return 1;
        }
        if (c->src.ptr[i] != ' ' && c->src.ptr[i] != '\t' && c->src.ptr[i] != '\r') {
            return 0;
        }
        i++;
    }
    return 0;
}

static void SLFmtMarkCommentUsedAtStart(SLFmtCtx* c, uint32_t start) {
    uint32_t i;
    for (i = 0; i < c->commentLen; i++) {
        if (c->comments[i].start == start) {
            c->commentUsed[i] = 1;
            return;
        }
    }
}

static int SLFmtEmitType(SLFmtCtx* c, int32_t nodeId);
static int SLFmtEmitExpr(SLFmtCtx* c, int32_t nodeId, int forceParen);
static int SLFmtEmitBlock(SLFmtCtx* c, int32_t nodeId);
static int SLFmtEmitStmtInline(SLFmtCtx* c, int32_t nodeId);
static int SLFmtEmitDecl(SLFmtCtx* c, int32_t nodeId);
static int SLFmtEmitDirectiveGroup(SLFmtCtx* c, int32_t firstDirective, int32_t* outNext);
static int SLFmtEmitDirective(SLFmtCtx* c, int32_t nodeId);
static int SLFmtEmitAggregateFieldBody(SLFmtCtx* c, int32_t firstFieldNodeId);
static int SLFmtEmitExprList(SLFmtCtx* c, int32_t listNodeId);

static int SLFmtEmitCompoundFieldWithAlign(SLFmtCtx* c, int32_t nodeId, uint32_t maxKeyLen) {
    const SLAstNode* n;
    int32_t          exprNode;
    uint32_t         keyLen;
    uint32_t         pad;

    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return -1;
    }
    n = &c->ast->nodes[nodeId];
    if (n->kind != SLAst_COMPOUND_FIELD) {
        return -1;
    }
    exprNode = SLFmtFirstChild(c->ast, nodeId);
    if ((n->flags & SLAstFlag_COMPOUND_FIELD_SHORTHAND) != 0) {
        return SLFmtWriteSlice(c, n->dataStart, n->dataEnd);
    }
    keyLen = n->dataEnd - n->dataStart;
    if (SLFmtWriteSlice(c, n->dataStart, n->dataEnd) != 0 || SLFmtWriteChar(c, ':') != 0) {
        return -1;
    }
    if (maxKeyLen > keyLen) {
        pad = (maxKeyLen - keyLen) + 1u;
        if (SLFmtWriteSpaces(c, pad) != 0) {
            return -1;
        }
    } else if (SLFmtWriteChar(c, ' ') != 0) {
        return -1;
    }
    return exprNode >= 0 ? SLFmtEmitExpr(c, exprNode, 0) : 0;
}

static int SLFmtEmitTypeParamList(SLFmtCtx* c, int32_t* ioChild) {
    int32_t cur;
    int     first = 1;
    if (ioChild == NULL) {
        return -1;
    }
    cur = *ioChild;
    if (cur < 0 || c->ast->nodes[cur].kind != SLAst_TYPE_PARAM) {
        return 0;
    }
    if (SLFmtWriteChar(c, '[') != 0) {
        return -1;
    }
    while (cur >= 0 && c->ast->nodes[cur].kind == SLAst_TYPE_PARAM) {
        if (!first && SLFmtWriteCStr(c, ", ") != 0) {
            return -1;
        }
        if (SLFmtWriteSlice(c, c->ast->nodes[cur].dataStart, c->ast->nodes[cur].dataEnd) != 0) {
            return -1;
        }
        first = 0;
        cur = SLFmtNextSibling(c->ast, cur);
    }
    if (SLFmtWriteChar(c, ']') != 0) {
        return -1;
    }
    *ioChild = cur;
    return 0;
}

static int SLFmtEmitType(SLFmtCtx* c, int32_t nodeId) {
    const SLAstNode* n;
    int32_t          ch;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return -1;
    }
    n = &c->ast->nodes[nodeId];
    switch (n->kind) {
        case SLAst_TYPE_NAME:
            if (SLFmtWriteSlice(c, n->dataStart, n->dataEnd) != 0) {
                return -1;
            }
            ch = SLFmtFirstChild(c->ast, nodeId);
            if (ch >= 0) {
                int first = 1;
                if (SLFmtWriteChar(c, '[') != 0) {
                    return -1;
                }
                while (ch >= 0) {
                    if (!first && SLFmtWriteCStr(c, ", ") != 0) {
                        return -1;
                    }
                    if (SLFmtEmitType(c, ch) != 0) {
                        return -1;
                    }
                    first = 0;
                    ch = SLFmtNextSibling(c->ast, ch);
                }
                if (SLFmtWriteChar(c, ']') != 0) {
                    return -1;
                }
            }
            return 0;
        case SLAst_TYPE_PARAM: return SLFmtWriteSlice(c, n->dataStart, n->dataEnd);
        case SLAst_TYPE_OPTIONAL:
            ch = SLFmtFirstChild(c->ast, nodeId);
            if (SLFmtWriteChar(c, '?') != 0) {
                return -1;
            }
            return ch >= 0 ? SLFmtEmitType(c, ch) : 0;
        case SLAst_TYPE_PTR:
            ch = SLFmtFirstChild(c->ast, nodeId);
            if (SLFmtWriteChar(c, '*') != 0) {
                return -1;
            }
            return ch >= 0 ? SLFmtEmitType(c, ch) : 0;
        case SLAst_TYPE_REF:
            ch = SLFmtFirstChild(c->ast, nodeId);
            if (SLFmtWriteChar(c, '&') != 0) {
                return -1;
            }
            return ch >= 0 ? SLFmtEmitType(c, ch) : 0;
        case SLAst_TYPE_MUTREF:
            ch = SLFmtFirstChild(c->ast, nodeId);
            if (SLFmtWriteCStr(c, "&mut ") != 0) {
                return -1;
            }
            return ch >= 0 ? SLFmtEmitType(c, ch) : 0;
        case SLAst_TYPE_SLICE:
            ch = SLFmtFirstChild(c->ast, nodeId);
            if (SLFmtWriteChar(c, '[') != 0 || (ch >= 0 && SLFmtEmitType(c, ch) != 0)
                || SLFmtWriteChar(c, ']') != 0)
            {
                return -1;
            }
            return 0;
        case SLAst_TYPE_MUTSLICE:
            ch = SLFmtFirstChild(c->ast, nodeId);
            if (SLFmtWriteCStr(c, "[mut ") != 0 || (ch >= 0 && SLFmtEmitType(c, ch) != 0)
                || SLFmtWriteChar(c, ']') != 0)
            {
                return -1;
            }
            return 0;
        case SLAst_TYPE_ARRAY:
        case SLAst_TYPE_VARRAY:
            ch = SLFmtFirstChild(c->ast, nodeId);
            if (SLFmtWriteChar(c, '[') != 0 || (ch >= 0 && SLFmtEmitType(c, ch) != 0)
                || SLFmtWriteChar(c, ' ') != 0)
            {
                return -1;
            }
            if (n->kind == SLAst_TYPE_VARRAY && SLFmtWriteChar(c, '.') != 0) {
                return -1;
            }
            if (SLFmtWriteSlice(c, n->dataStart, n->dataEnd) != 0 || SLFmtWriteChar(c, ']') != 0) {
                return -1;
            }
            return 0;
        case SLAst_TYPE_FN: {
            int32_t retType = -1;
            int32_t cur = SLFmtFirstChild(c->ast, nodeId);
            int     first = 1;
            if (SLFmtWriteCStr(c, "fn(") != 0) {
                return -1;
            }
            while (cur >= 0) {
                const SLAstNode* chn = &c->ast->nodes[cur];
                if (chn->flags == 1 && SLFmtIsTypeNodeKind(chn->kind)) {
                    retType = cur;
                    break;
                }
                if (!first && SLFmtWriteCStr(c, ", ") != 0) {
                    return -1;
                }
                if ((chn->flags & SLAstFlag_PARAM_CONST) != 0 && SLFmtWriteCStr(c, "const ") != 0) {
                    return -1;
                }
                if ((chn->flags & SLAstFlag_PARAM_VARIADIC) != 0 && SLFmtWriteCStr(c, "...") != 0) {
                    return -1;
                }
                if (SLFmtEmitType(c, cur) != 0) {
                    return -1;
                }
                first = 0;
                cur = SLFmtNextSibling(c->ast, cur);
            }
            if (SLFmtWriteChar(c, ')') != 0) {
                return -1;
            }
            if (retType >= 0) {
                if (SLFmtWriteChar(c, ' ') != 0 || SLFmtEmitType(c, retType) != 0) {
                    return -1;
                }
            }
            return 0;
        }
        case SLAst_TYPE_ANON_STRUCT:
        case SLAst_TYPE_ANON_UNION:  {
            int32_t field = SLFmtFirstChild(c->ast, nodeId);
            if (SLFmtCountNewlinesInRange(c->src, n->start, n->end) == 0u) {
                return SLFmtWriteSlice(c, n->start, n->end);
            }
            if (n->kind == SLAst_TYPE_ANON_UNION) {
                if (SLFmtWriteCStr(c, "union ") != 0) {
                    return -1;
                }
            } else if (SLFmtWriteCStr(c, "struct ") != 0) {
                return -1;
            }
            if (SLFmtWriteChar(c, '{') != 0) {
                return -1;
            }
            if (field >= 0 && SLFmtEmitAggregateFieldBody(c, field) != 0) {
                return -1;
            }
            return SLFmtWriteChar(c, '}');
        }
        case SLAst_TYPE_TUPLE: {
            int32_t cur = SLFmtFirstChild(c->ast, nodeId);
            int     first = 1;
            if (SLFmtWriteChar(c, '(') != 0) {
                return -1;
            }
            while (cur >= 0) {
                if (!first && SLFmtWriteCStr(c, ", ") != 0) {
                    return -1;
                }
                if (SLFmtEmitType(c, cur) != 0) {
                    return -1;
                }
                first = 0;
                cur = SLFmtNextSibling(c->ast, cur);
            }
            return SLFmtWriteChar(c, ')');
        }
        default: return SLFmtWriteSlice(c, n->start, n->end);
    }
}

static int SLFmtEmitExprList(SLFmtCtx* c, int32_t listNodeId) {
    uint32_t i;
    uint32_t exprCount;
    if (listNodeId < 0 || (uint32_t)listNodeId >= c->ast->len
        || c->ast->nodes[listNodeId].kind != SLAst_EXPR_LIST)
    {
        return -1;
    }
    exprCount = SLFmtListCount(c->ast, listNodeId);
    for (i = 0; i < exprCount; i++) {
        int32_t exprNode = SLFmtListItemAt(c->ast, listNodeId, i);
        if (exprNode < 0) {
            return -1;
        }
        if (i > 0 && SLFmtWriteCStr(c, ", ") != 0) {
            return -1;
        }
        if (SLFmtEmitExpr(c, exprNode, 0) != 0) {
            return -1;
        }
    }
    return 0;
}

static int SLFmtExprNeedsParensForBinaryChild(
    const SLAst* ast, int32_t parentId, int32_t childId, int rightChild) {
    const SLAstNode* p;
    const SLAstNode* ch;
    int              pp;
    int              cp;
    int              rightAssoc;
    if (childId < 0 || (uint32_t)childId >= ast->len || parentId < 0
        || (uint32_t)parentId >= ast->len)
    {
        return 0;
    }
    p = &ast->nodes[parentId];
    ch = &ast->nodes[childId];
    if (ch->kind != SLAst_BINARY) {
        return 0;
    }
    pp = SLFmtBinPrec((SLTokenKind)p->op);
    cp = SLFmtBinPrec((SLTokenKind)ch->op);
    rightAssoc = SLFmtIsAssignmentOp((SLTokenKind)p->op);
    if (cp < pp) {
        return 1;
    }
    if (cp == pp) {
        if (rightChild && !rightAssoc) {
            return 1;
        }
        if (!rightChild && rightAssoc) {
            return 1;
        }
    }
    return 0;
}

static int SLFmtUseTightMulDivModSpacing(
    SLFmtCtx* c, int32_t nodeId, int32_t lhsNodeId, int32_t rhsNodeId) {
    const SLAstNode* n;
    int32_t          curNodeId;
    int              curPrec;
    if (nodeId < 0 || lhsNodeId < 0 || rhsNodeId < 0 || (uint32_t)nodeId >= c->ast->len
        || (uint32_t)lhsNodeId >= c->ast->len || (uint32_t)rhsNodeId >= c->ast->len)
    {
        return 0;
    }
    n = &c->ast->nodes[nodeId];
    if (n->op != SLTok_MUL && n->op != SLTok_DIV && n->op != SLTok_MOD) {
        return 0;
    }
    if ((n->flags & SLAstFlag_PAREN) != 0) {
        return 0;
    }

    curNodeId = nodeId;
    curPrec = SLFmtBinPrec((SLTokenKind)n->op);
    while (curNodeId >= 0) {
        int32_t          parentNodeId = SLFmtFindParentNode(c->ast, curNodeId);
        const SLAstNode* parentNode;
        int              parentPrec;
        if (parentNodeId < 0) {
            break;
        }
        parentNode = &c->ast->nodes[parentNodeId];
        if (parentNode->kind != SLAst_BINARY) {
            break;
        }
        if (SLFmtIsAssignmentOp((SLTokenKind)parentNode->op)) {
            break;
        }
        switch ((SLTokenKind)parentNode->op) {
            case SLTok_LOGICAL_OR:
            case SLTok_LOGICAL_AND:
            case SLTok_EQ:
            case SLTok_NEQ:
            case SLTok_LT:
            case SLTok_GT:
            case SLTok_LTE:
            case SLTok_GTE:         return 0;
            default:                break;
        }
        parentPrec = SLFmtBinPrec((SLTokenKind)parentNode->op);
        if (parentPrec > 0 && parentPrec < curPrec) {
            return 1;
        }
        if (parentPrec == 0 || parentPrec > curPrec) {
            break;
        }
        if ((parentNode->flags & SLAstFlag_PAREN) != 0) {
            break;
        }
        curNodeId = parentNodeId;
    }
    return 0;
}

static int SLFmtEmitExprCore(SLFmtCtx* c, int32_t nodeId) {
    const SLAstNode* n;
    int32_t          ch;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return -1;
    }
    n = &c->ast->nodes[nodeId];
    switch (n->kind) {
        case SLAst_IDENT:
        case SLAst_INT:
        case SLAst_FLOAT:
        case SLAst_BOOL:   return SLFmtWriteSlice(c, n->dataStart, n->dataEnd);
        case SLAst_STRING: return SLFmtWriteSliceLiteral(c, n->dataStart, n->dataEnd);
        case SLAst_RUNE:   return SLFmtWriteSliceLiteral(c, n->dataStart, n->dataEnd);
        case SLAst_NULL:   return SLFmtWriteCStr(c, "null");
        case SLAst_UNARY:  {
            const char* op = SLFmtTokenOpText((SLTokenKind)n->op);
            ch = SLFmtFirstChild(c->ast, nodeId);
            if (SLFmtWriteCStr(c, op) != 0) {
                return -1;
            }
            if (ch >= 0) {
                const SLAstNode* cn = &c->ast->nodes[ch];
                int              need = cn->kind == SLAst_BINARY;
                if (SLFmtEmitExpr(c, ch, need) != 0) {
                    return -1;
                }
            }
            return 0;
        }
        case SLAst_BINARY: {
            const char* op = SLFmtTokenOpText((SLTokenKind)n->op);
            int32_t     lhs = SLFmtFirstChild(c->ast, nodeId);
            int32_t     rhs = lhs >= 0 ? SLFmtNextSibling(c->ast, lhs) : -1;
            int         tightOp = SLFmtUseTightMulDivModSpacing(c, nodeId, lhs, rhs);
            if (lhs >= 0
                && SLFmtEmitExpr(c, lhs, SLFmtExprNeedsParensForBinaryChild(c->ast, nodeId, lhs, 0))
                       != 0)
            {
                return -1;
            }
            if (tightOp) {
                if (SLFmtWriteCStr(c, op) != 0) {
                    return -1;
                }
            } else if (
                SLFmtWriteChar(c, ' ') != 0 || SLFmtWriteCStr(c, op) != 0
                || SLFmtWriteChar(c, ' ') != 0)
            {
                return -1;
            }
            if (rhs >= 0
                && SLFmtEmitExpr(c, rhs, SLFmtExprNeedsParensForBinaryChild(c->ast, nodeId, rhs, 1))
                       != 0)
            {
                return -1;
            }
            return 0;
        }
        case SLAst_CALL: {
            int32_t arg;
            ch = SLFmtFirstChild(c->ast, nodeId);
            if (ch >= 0 && SLFmtEmitExpr(c, ch, 0) != 0) {
                return -1;
            }
            if (SLFmtWriteChar(c, '(') != 0) {
                return -1;
            }
            arg = ch >= 0 ? SLFmtNextSibling(c->ast, ch) : -1;
            while (arg >= 0) {
                int32_t next = SLFmtNextSibling(c->ast, arg);
                if (SLFmtEmitExpr(c, arg, 0) != 0) {
                    return -1;
                }
                if (next >= 0 && SLFmtWriteCStr(c, ", ") != 0) {
                    return -1;
                }
                arg = next;
            }
            return SLFmtWriteChar(c, ')');
        }
        case SLAst_CALL_ARG: {
            int32_t exprNode = SLFmtFirstChild(c->ast, nodeId);
            if (n->dataEnd > n->dataStart) {
                if (SLFmtWriteSlice(c, n->dataStart, n->dataEnd) != 0
                    || SLFmtWriteCStr(c, ": ") != 0)
                {
                    return -1;
                }
            }
            if (exprNode >= 0 && SLFmtEmitExpr(c, exprNode, 0) != 0) {
                return -1;
            }
            if ((n->flags & SLAstFlag_CALL_ARG_SPREAD) != 0) {
                return SLFmtWriteCStr(c, "...");
            }
            return 0;
        }
        case SLAst_TUPLE_EXPR: {
            int32_t cur = SLFmtFirstChild(c->ast, nodeId);
            int     first = 1;
            if (SLFmtWriteChar(c, '(') != 0) {
                return -1;
            }
            while (cur >= 0) {
                if (!first && SLFmtWriteCStr(c, ", ") != 0) {
                    return -1;
                }
                if (SLFmtEmitExpr(c, cur, 0) != 0) {
                    return -1;
                }
                first = 0;
                cur = SLFmtNextSibling(c->ast, cur);
            }
            return SLFmtWriteChar(c, ')');
        }
        case SLAst_CALL_WITH_CONTEXT: {
            int32_t callNode = SLFmtFirstChild(c->ast, nodeId);
            int32_t ctxNode = callNode >= 0 ? SLFmtNextSibling(c->ast, callNode) : -1;
            if (callNode >= 0 && SLFmtEmitExpr(c, callNode, 0) != 0) {
                return -1;
            }
            if (SLFmtWriteCStr(c, " with ") != 0) {
                return -1;
            }
            if ((n->flags & SLAstFlag_CALL_WITH_CONTEXT_PASSTHROUGH) != 0) {
                return SLFmtWriteCStr(c, "context");
            }
            return ctxNode >= 0 ? SLFmtEmitExpr(c, ctxNode, 0) : 0;
        }
        case SLAst_CONTEXT_OVERLAY: {
            int32_t bind = SLFmtFirstChild(c->ast, nodeId);
            if (SLFmtWriteChar(c, '{') != 0) {
                return -1;
            }
            if (bind < 0) {
                return SLFmtWriteChar(c, '}');
            }
            if (SLFmtWriteChar(c, ' ') != 0) {
                return -1;
            }
            while (bind >= 0) {
                int32_t next = SLFmtNextSibling(c->ast, bind);
                if (SLFmtEmitExpr(c, bind, 0) != 0) {
                    return -1;
                }
                if (next >= 0 && SLFmtWriteCStr(c, ", ") != 0) {
                    return -1;
                }
                bind = next;
            }
            return SLFmtWriteCStr(c, " }");
        }
        case SLAst_CONTEXT_BIND: {
            ch = SLFmtFirstChild(c->ast, nodeId);
            if (SLFmtWriteSlice(c, n->dataStart, n->dataEnd) != 0) {
                return -1;
            }
            if ((n->flags & SLAstFlag_CONTEXT_BIND_SHORTHAND) == 0 && ch >= 0) {
                if (SLFmtWriteCStr(c, ": ") != 0 || SLFmtEmitExpr(c, ch, 0) != 0) {
                    return -1;
                }
            }
            return 0;
        }
        case SLAst_INDEX: {
            int32_t base = SLFmtFirstChild(c->ast, nodeId);
            int32_t a = base >= 0 ? SLFmtNextSibling(c->ast, base) : -1;
            int32_t b = a >= 0 ? SLFmtNextSibling(c->ast, a) : -1;
            if (base >= 0 && SLFmtEmitExpr(c, base, 0) != 0) {
                return -1;
            }
            if (SLFmtWriteChar(c, '[') != 0) {
                return -1;
            }
            if ((n->flags & SLAstFlag_INDEX_SLICE) != 0) {
                if ((n->flags & SLAstFlag_INDEX_HAS_START) != 0 && a >= 0
                    && SLFmtEmitExpr(c, a, 0) != 0)
                {
                    return -1;
                }
                if (SLFmtWriteChar(c, ':') != 0) {
                    return -1;
                }
                if ((n->flags & SLAstFlag_INDEX_HAS_END) != 0) {
                    int32_t endNode = (n->flags & SLAstFlag_INDEX_HAS_START) != 0 ? b : a;
                    if (endNode >= 0 && SLFmtEmitExpr(c, endNode, 0) != 0) {
                        return -1;
                    }
                }
            } else if (a >= 0 && SLFmtEmitExpr(c, a, 0) != 0) {
                return -1;
            }
            return SLFmtWriteChar(c, ']');
        }
        case SLAst_FIELD_EXPR:
            ch = SLFmtFirstChild(c->ast, nodeId);
            if (ch >= 0 && SLFmtEmitExpr(c, ch, 0) != 0) {
                return -1;
            }
            if (SLFmtWriteChar(c, '.') != 0) {
                return -1;
            }
            return SLFmtWriteSlice(c, n->dataStart, n->dataEnd);
        case SLAst_CAST: {
            int32_t expr = SLFmtFirstChild(c->ast, nodeId);
            int32_t type = expr >= 0 ? SLFmtNextSibling(c->ast, expr) : -1;
            if ((n->flags & SLFmtFlag_DROP_REDUNDANT_LITERAL_CAST) != 0) {
                return expr >= 0 ? SLFmtEmitExpr(c, expr, 0) : 0;
            }
            if (expr >= 0 && SLFmtEmitExpr(c, expr, 0) != 0) {
                return -1;
            }
            if (SLFmtWriteCStr(c, " as ") != 0) {
                return -1;
            }
            return type >= 0 ? SLFmtEmitType(c, type) : 0;
        }
        case SLAst_SIZEOF:
            ch = SLFmtFirstChild(c->ast, nodeId);
            if (SLFmtWriteCStr(c, "sizeof(") != 0) {
                return -1;
            }
            if (ch >= 0) {
                if (n->flags == 1) {
                    if (SLFmtEmitType(c, ch) != 0) {
                        return -1;
                    }
                } else if (SLFmtEmitExpr(c, ch, 0) != 0) {
                    return -1;
                }
            }
            return SLFmtWriteChar(c, ')');
        case SLAst_NEW: {
            int32_t type = SLFmtFirstChild(c->ast, nodeId);
            int32_t next = type >= 0 ? SLFmtNextSibling(c->ast, type) : -1;
            int32_t count = -1;
            int32_t init = -1;
            int32_t alloc = -1;
            if ((n->flags & SLAstFlag_NEW_HAS_COUNT) != 0) {
                count = next;
                next = count >= 0 ? SLFmtNextSibling(c->ast, count) : -1;
            }
            if ((n->flags & SLAstFlag_NEW_HAS_INIT) != 0) {
                init = next;
                next = init >= 0 ? SLFmtNextSibling(c->ast, init) : -1;
            }
            if ((n->flags & SLAstFlag_NEW_HAS_ALLOC) != 0) {
                alloc = next;
            }
            if (SLFmtWriteCStr(c, "new ") != 0) {
                return -1;
            }
            if (count >= 0) {
                if (SLFmtWriteChar(c, '[') != 0 || (type >= 0 && SLFmtEmitType(c, type) != 0)
                    || SLFmtWriteChar(c, ' ') != 0 || SLFmtEmitExpr(c, count, 0) != 0
                    || SLFmtWriteChar(c, ']') != 0)
                {
                    return -1;
                }
            } else {
                if (type >= 0 && SLFmtEmitType(c, type) != 0) {
                    return -1;
                }
                if (init >= 0) {
                    int32_t initFirst = SLFmtFirstChild(c->ast, init);
                    int     initTight =
                        c->ast->nodes[init].kind == SLAst_COMPOUND_LIT
                        && (initFirst < 0 || !SLFmtIsTypeNodeKind(c->ast->nodes[initFirst].kind));
                    if (!initTight && SLFmtWriteChar(c, ' ') != 0) {
                        return -1;
                    }
                    if (SLFmtEmitExpr(c, init, 0) != 0) {
                        return -1;
                    }
                }
            }
            if (alloc >= 0) {
                if (SLFmtWriteCStr(c, " with ") != 0 || SLFmtEmitExpr(c, alloc, 0) != 0) {
                    return -1;
                }
            }
            return 0;
        }
        case SLAst_COMPOUND_LIT: {
            int32_t  cur = SLFmtFirstChild(c->ast, nodeId);
            int32_t  type = -1;
            int32_t  field;
            uint32_t maxKeyLen = 0;
            uint32_t lbPos;
            uint32_t rbPos;
            if (cur >= 0 && SLFmtIsTypeNodeKind(c->ast->nodes[cur].kind)) {
                type = cur;
                cur = SLFmtNextSibling(c->ast, cur);
            }
            if (type >= 0 && SLFmtEmitType(c, type) != 0) {
                return -1;
            }
            if (!SLFmtFindCharForwardInRange(c->src, n->start, n->end, '{', &lbPos)
                || !SLFmtFindCharBackwardInRange(c->src, n->start, n->end, '}', &rbPos))
            {
                return -1;
            }
            if (SLFmtWriteChar(c, '{') != 0) {
                return -1;
            }
            if (cur < 0) {
                return SLFmtWriteChar(c, '}');
            }
            if (!SLFmtRangeHasChar(c->src, lbPos + 1u, rbPos, '\n')) {
                if (SLFmtWriteChar(c, ' ') != 0) {
                    return -1;
                }
                field = cur;
                while (field >= 0) {
                    int32_t next = SLFmtNextSibling(c->ast, field);
                    if (SLFmtEmitExpr(c, field, 0) != 0) {
                        return -1;
                    }
                    if (next >= 0 && SLFmtWriteCStr(c, ", ") != 0) {
                        return -1;
                    }
                    field = next;
                }
                return SLFmtWriteCStr(c, " }");
            }
            c->indent++;
            for (field = cur; field >= 0; field = SLFmtNextSibling(c->ast, field)) {
                const SLAstNode* fn = &c->ast->nodes[field];
                uint32_t         keyLen = fn->dataEnd - fn->dataStart;
                if (fn->kind == SLAst_COMPOUND_FIELD && keyLen > maxKeyLen) {
                    maxKeyLen = keyLen;
                }
            }
            field = cur;
            while (field >= 0) {
                int32_t  next = SLFmtNextSibling(c->ast, field);
                uint32_t gapStart = c->ast->nodes[field].end;
                uint32_t gapEnd = next >= 0 ? c->ast->nodes[next].start : rbPos;
                int      hasComma = SLFmtRangeHasChar(c->src, gapStart, gapEnd, ',');
                int      hasNewline = SLFmtRangeHasChar(c->src, gapStart, gapEnd, '\n');
                if (field == cur) {
                    int firstHasNewline = SLFmtRangeHasChar(
                        c->src, lbPos + 1u, c->ast->nodes[field].start, '\n');
                    if (firstHasNewline) {
                        if (SLFmtNewline(c) != 0) {
                            return -1;
                        }
                    } else if (SLFmtWriteChar(c, ' ') != 0) {
                        return -1;
                    }
                }
                if (SLFmtEmitCompoundFieldWithAlign(c, field, maxKeyLen) != 0) {
                    return -1;
                }
                if (hasComma && SLFmtWriteChar(c, ',') != 0) {
                    return -1;
                }
                if (next >= 0) {
                    if (hasNewline) {
                        if (SLFmtNewline(c) != 0) {
                            return -1;
                        }
                    } else if (SLFmtWriteChar(c, ' ') != 0) {
                        return -1;
                    }
                } else {
                    c->indent--;
                    if (hasNewline) {
                        if (SLFmtNewline(c) != 0) {
                            return -1;
                        }
                    } else if (SLFmtWriteChar(c, ' ') != 0) {
                        return -1;
                    }
                    return SLFmtWriteChar(c, '}');
                }
                field = next;
            }
            c->indent--;
            return SLFmtWriteChar(c, '}');
        }
        case SLAst_COMPOUND_FIELD: return SLFmtEmitCompoundFieldWithAlign(c, nodeId, 0u);
        case SLAst_TYPE_VALUE:
            ch = SLFmtFirstChild(c->ast, nodeId);
            if (SLFmtWriteCStr(c, "type ") != 0) {
                return -1;
            }
            return ch >= 0 ? SLFmtEmitType(c, ch) : 0;
        case SLAst_UNWRAP:
            ch = SLFmtFirstChild(c->ast, nodeId);
            if (ch >= 0 && SLFmtEmitExpr(c, ch, 0) != 0) {
                return -1;
            }
            return SLFmtWriteChar(c, '!');
        default: return SLFmtWriteSlice(c, n->start, n->end);
    }
}

static int SLFmtEmitExpr(SLFmtCtx* c, int32_t nodeId, int forceParen) {
    const SLAstNode* n;
    int              needParen;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return -1;
    }
    n = &c->ast->nodes[nodeId];
    needParen = forceParen || ((n->flags & SLAstFlag_PAREN) != 0);
    if (needParen && SLFmtWriteChar(c, '(') != 0) {
        return -1;
    }
    if (SLFmtEmitExprCore(c, nodeId) != 0) {
        return -1;
    }
    if (needParen && SLFmtWriteChar(c, ')') != 0) {
        return -1;
    }
    return 0;
}

static int SLFmtMeasureTypeLen(SLFmtCtx* c, int32_t nodeId, uint32_t* outLen) {
    SLFmtCtx m = *c;
    m.lineStart = 0;
    m.indent = 0;
    m.out.v = NULL;
    m.out.len = 0;
    m.out.cap = 0;
    if (SLFmtEmitType(&m, nodeId) != 0) {
        return -1;
    }
    *outLen = m.out.len;
    return 0;
}

static int SLFmtMeasureExprLen(SLFmtCtx* c, int32_t nodeId, int forceParen, uint32_t* outLen) {
    SLFmtCtx m = *c;
    m.lineStart = 0;
    m.indent = 0;
    m.out.v = NULL;
    m.out.len = 0;
    m.out.cap = 0;
    if (SLFmtEmitExpr(&m, nodeId, forceParen) != 0) {
        return -1;
    }
    *outLen = m.out.len;
    return 0;
}

static int SLFmtNeedsBlankLineBeforeNode(SLFmtCtx* c, int32_t prevNodeId, int32_t nextNodeId) {
    SLAstKind nextKind;
    uint32_t  gapNl;
    if (prevNodeId < 0 || nextNodeId < 0) {
        return 0;
    }
    nextKind = c->ast->nodes[nextNodeId].kind;
    gapNl = SLFmtCountNewlinesInRange(
        c->src, c->ast->nodes[prevNodeId].end, c->ast->nodes[nextNodeId].start);
    if (gapNl <= 1u) {
        return 0;
    }
    if (SLFmtHasUnusedLeadingCommentsForNode(c, nextNodeId)) {
        if (!SLFmtIsStmtNodeKind(nextKind)) {
            return 0;
        }
        if (!SLFmtGapHasIntentionalBlankLine(
                c->src, c->ast->nodes[prevNodeId].end, c->ast->nodes[nextNodeId].start))
        {
            return 0;
        }
    }
    return 1;
}

static int SLFmtTypeEnvCopy(SLFmtTypeEnv* dst, const SLFmtTypeEnv* src) {
    uint32_t i;
    if (dst == NULL) {
        return 0;
    }
    dst->len = 0;
    if (src == NULL) {
        return 1;
    }
    if (src->len > (uint32_t)(sizeof(dst->items) / sizeof(dst->items[0]))) {
        return 0;
    }
    for (i = 0; i < src->len; i++) {
        dst->items[i] = src->items[i];
    }
    dst->len = src->len;
    return 1;
}

static int32_t SLFmtResolveTypeNodeThroughEnv(
    const SLAst* ast, SLStrView src, int32_t typeNodeId, const SLFmtTypeEnv* _Nullable env) {
    int32_t bindingIndex;
    if (env == NULL || typeNodeId < 0 || (uint32_t)typeNodeId >= ast->len) {
        return typeNodeId;
    }
    if (!SLFmtTypeNameIsBoundParam(ast, src, typeNodeId, env, &bindingIndex)) {
        return typeNodeId;
    }
    if (env->items[bindingIndex].concreteTypeNodeId < 0) {
        return typeNodeId;
    }
    return env->items[bindingIndex].concreteTypeNodeId;
}

static int SLFmtInferredTypeSet(
    SLFmtInferredType* inferred, int32_t typeNodeId, const SLFmtTypeEnv* _Nullable env) {
    if (inferred == NULL || typeNodeId < 0) {
        return 0;
    }
    inferred->typeNodeId = typeNodeId;
    return SLFmtTypeEnvCopy(&inferred->env, env);
}

static int SLFmtInferredTypeMatchesNode(
    const SLAst* ast, SLStrView src, const SLFmtInferredType* inferred, int32_t typeNodeId) {
    SLFmtTypeEnv        envCopy;
    const SLFmtTypeEnv* env;
    if (inferred == NULL || inferred->typeNodeId < 0 || typeNodeId < 0) {
        return 0;
    }
    env = inferred->env.len > 0 ? &inferred->env : NULL;
    if (env != NULL && !SLFmtTypeEnvCopy(&envCopy, env)) {
        return 0;
    }
    return SLFmtTypeCompatibleWithEnvs(
               ast, src, inferred->typeNodeId, env != NULL ? &envCopy : NULL, typeNodeId, NULL)
        && SLFmtTypeCompatibleWithEnvs(ast, src, typeNodeId, NULL, inferred->typeNodeId, env);
}

static int32_t SLFmtFindLocalNamedTypeDeclByRange(
    const SLAst* ast, SLStrView src, uint32_t nameStart, uint32_t nameEnd) {
    int32_t cur;
    if (nameEnd <= nameStart) {
        return -1;
    }
    cur = SLFmtFirstChild(ast, ast->root);
    while (cur >= 0) {
        const SLAstNode* n = &ast->nodes[cur];
        if ((n->kind == SLAst_STRUCT || n->kind == SLAst_UNION || n->kind == SLAst_ENUM)
            && SLFmtSlicesEqual(src, n->dataStart, n->dataEnd, nameStart, nameEnd))
        {
            return cur;
        }
        cur = SLFmtNextSibling(ast, cur);
    }
    return -1;
}

static int32_t SLFmtFindLocalNamedTypeDeclForTypeNode(
    const SLAst* ast, SLStrView src, int32_t typeNodeId, const SLFmtTypeEnv* _Nullable env) {
    int32_t resolvedTypeNodeId = SLFmtResolveTypeNodeThroughEnv(ast, src, typeNodeId, env);
    if (resolvedTypeNodeId < 0 || (uint32_t)resolvedTypeNodeId >= ast->len) {
        return -1;
    }
    if (ast->nodes[resolvedTypeNodeId].kind != SLAst_TYPE_NAME) {
        return -1;
    }
    return SLFmtFindLocalNamedTypeDeclByRange(
        ast, src, ast->nodes[resolvedTypeNodeId].dataStart, ast->nodes[resolvedTypeNodeId].dataEnd);
}

static int SLFmtBindStructTypeArgsFromInstance(
    const SLAst*        ast,
    SLStrView           src,
    int32_t             declNodeId,
    int32_t             instTypeNodeId,
    const SLFmtTypeEnv* instEnv,
    SLFmtTypeEnv*       outEnv) {
    int32_t  declParamNodeId;
    int32_t  argNodeId;
    uint32_t i;
    if (!SLFmtTypeEnvInitFromDeclTypeParams(ast, declNodeId, outEnv)) {
        return 0;
    }
    declParamNodeId = SLFmtFirstChild(ast, declNodeId);
    argNodeId = SLFmtFirstChild(ast, instTypeNodeId);
    for (i = 0; i < outEnv->len; i++) {
        int32_t resolvedArgNodeId;
        if (declParamNodeId < 0 || ast->nodes[declParamNodeId].kind != SLAst_TYPE_PARAM
            || argNodeId < 0)
        {
            return 0;
        }
        resolvedArgNodeId = SLFmtResolveTypeNodeThroughEnv(ast, src, argNodeId, instEnv);
        outEnv->items[i].concreteTypeNodeId = resolvedArgNodeId;
        declParamNodeId = SLFmtNextSibling(ast, declParamNodeId);
        argNodeId = SLFmtNextSibling(ast, argNodeId);
    }
    return argNodeId < 0;
}

static int SLFmtFindFieldTypeOnLocalNamedType(
    const SLAst*             ast,
    SLStrView                src,
    const SLFmtInferredType* baseType,
    uint32_t                 fieldStart,
    uint32_t                 fieldEnd,
    SLFmtInferredType*       outType) {
    int32_t      declNodeId;
    int32_t      instTypeNodeId;
    int32_t      child;
    SLFmtTypeEnv declEnv;
    if (outType != NULL) {
        SLFmtInferredTypeInit(outType);
    }
    if (baseType == NULL || outType == NULL || baseType->typeNodeId < 0 || fieldEnd <= fieldStart) {
        return 0;
    }
    instTypeNodeId = SLFmtResolveTypeNodeThroughEnv(ast, src, baseType->typeNodeId, &baseType->env);
    if (instTypeNodeId < 0 || (uint32_t)instTypeNodeId >= ast->len
        || ast->nodes[instTypeNodeId].kind != SLAst_TYPE_NAME)
    {
        return 0;
    }
    declNodeId = SLFmtFindLocalNamedTypeDeclForTypeNode(ast, src, instTypeNodeId, NULL);
    if (declNodeId < 0 || ast->nodes[declNodeId].kind != SLAst_STRUCT) {
        return 0;
    }
    if (!SLFmtBindStructTypeArgsFromInstance(
            ast, src, declNodeId, instTypeNodeId, &baseType->env, &declEnv))
    {
        return 0;
    }
    child = SLFmtFirstChild(ast, declNodeId);
    while (child >= 0 && ast->nodes[child].kind == SLAst_TYPE_PARAM) {
        child = SLFmtNextSibling(ast, child);
    }
    while (child >= 0) {
        if (ast->nodes[child].kind == SLAst_FIELD
            && SLFmtSlicesEqual(
                src, ast->nodes[child].dataStart, ast->nodes[child].dataEnd, fieldStart, fieldEnd))
        {
            int32_t fieldTypeNodeId = SLFmtFirstChild(ast, child);
            return fieldTypeNodeId >= 0 && SLFmtInferredTypeSet(outType, fieldTypeNodeId, &declEnv);
        }
        child = SLFmtNextSibling(ast, child);
    }
    return 0;
}

static int SLFmtFindLocalBindingBefore(
    const SLAst* ast,
    SLStrView    src,
    int32_t      identNodeId,
    uint32_t     beforePos,
    int32_t* _Nullable outDeclNodeId,
    int32_t* _Nullable outTypeNodeId,
    int32_t* _Nullable outInitNodeId) {
    const SLAstNode* identNode;
    int32_t          fnNodeId;
    uint32_t         bestStart = 0;
    int32_t          bestDeclNodeId = -1;
    int32_t          bestTypeNodeId = -1;
    int32_t          bestInitNodeId = -1;
    if (outDeclNodeId != NULL) {
        *outDeclNodeId = -1;
    }
    if (outTypeNodeId != NULL) {
        *outTypeNodeId = -1;
    }
    if (outInitNodeId != NULL) {
        *outInitNodeId = -1;
    }
    if (identNodeId < 0 || (uint32_t)identNodeId >= ast->len) {
        return 0;
    }
    identNode = &ast->nodes[identNodeId];
    if (identNode->kind != SLAst_IDENT || identNode->dataEnd <= identNode->dataStart) {
        return 0;
    }
    fnNodeId = SLFmtFindEnclosingFnNode(ast, identNodeId);
    if (fnNodeId < 0) {
        return 0;
    }

    {
        int32_t child = SLFmtFirstChild(ast, fnNodeId);
        while (child >= 0 && ast->nodes[child].kind == SLAst_TYPE_PARAM) {
            child = SLFmtNextSibling(ast, child);
        }
        while (child >= 0 && ast->nodes[child].kind == SLAst_PARAM) {
            int32_t paramTypeNodeId = SLFmtFirstChild(ast, child);
            if (paramTypeNodeId >= 0 && SLFmtIsTypeNodeKindRaw(ast->nodes[paramTypeNodeId].kind)
                && SLFmtNodeDeclaresNameRange(
                    ast, src, child, identNode->dataStart, identNode->dataEnd)
                && ast->nodes[child].start < beforePos)
            {
                bestStart = ast->nodes[child].start;
                bestDeclNodeId = child;
                bestTypeNodeId = paramTypeNodeId;
                bestInitNodeId = -1;
            }
            child = SLFmtNextSibling(ast, child);
        }
    }

    {
        const SLAstNode* fn = &ast->nodes[fnNodeId];
        uint32_t         i;
        for (i = 0; i < ast->len; i++) {
            const SLAstNode* n = &ast->nodes[i];
            int32_t          typeNodeId = -1;
            int32_t          initNodeId = -1;
            if (n->kind != SLAst_VAR && n->kind != SLAst_CONST) {
                continue;
            }
            if (n->start < fn->start || n->end > fn->end || n->end > beforePos) {
                continue;
            }
            SLFmtGetVarLikeTypeAndInit(ast, (int32_t)i, &typeNodeId, &initNodeId);
            if (!SLFmtNodeDeclaresNameRange(
                    ast, src, (int32_t)i, identNode->dataStart, identNode->dataEnd))
            {
                continue;
            }
            if (bestDeclNodeId < 0 || n->start >= bestStart) {
                bestStart = n->start;
                bestDeclNodeId = (int32_t)i;
                bestTypeNodeId = typeNodeId;
                bestInitNodeId = initNodeId;
            }
        }
    }

    if (bestDeclNodeId < 0) {
        return 0;
    }
    if (outDeclNodeId != NULL) {
        *outDeclNodeId = bestDeclNodeId;
    }
    if (outTypeNodeId != NULL) {
        *outTypeNodeId = bestTypeNodeId;
    }
    if (outInitNodeId != NULL) {
        *outInitNodeId = bestInitNodeId;
    }
    return 1;
}

static int SLFmtInferExprTypeEx(
    const SLAst*       ast,
    SLStrView          src,
    int32_t            exprNodeId,
    uint32_t           beforePos,
    uint32_t           depth,
    SLFmtInferredType* outType);

static int SLFmtLiteralExprMatchesConcreteType(
    const SLAst* ast, SLStrView src, int32_t exprNodeId, int32_t typeNodeId) {
    int32_t innerExprNodeId = exprNodeId;
    if (exprNodeId < 0 || typeNodeId < 0 || (uint32_t)exprNodeId >= ast->len) {
        return 0;
    }
    if (ast->nodes[exprNodeId].kind == SLAst_UNARY && ast->nodes[exprNodeId].op == SLTok_SUB) {
        innerExprNodeId = SLFmtFirstChild(ast, exprNodeId);
    }
    return innerExprNodeId >= 0
        && SLFmtCastLiteralNumericType(ast, src, innerExprNodeId, typeNodeId)
               != SLFmtNumericType_INVALID;
}

static int SLFmtMapCallActuals(
    const SLAst*     ast,
    int32_t          callNodeId,
    SLFmtCallActual* actuals,
    uint32_t*        outActualCount,
    uint32_t         actualCap) {
    int32_t  calleeNodeId;
    int32_t  cur;
    uint32_t actualCount = 0;
    if (outActualCount != NULL) {
        *outActualCount = 0;
    }
    if (callNodeId < 0 || (uint32_t)callNodeId >= ast->len || actuals == NULL || actualCap == 0u) {
        return 0;
    }
    calleeNodeId = SLFmtFirstChild(ast, callNodeId);
    if (calleeNodeId < 0) {
        return 0;
    }
    if (ast->nodes[calleeNodeId].kind == SLAst_FIELD_EXPR) {
        int32_t recvNodeId = SLFmtFirstChild(ast, calleeNodeId);
        if (recvNodeId < 0 || actualCount >= actualCap) {
            return 0;
        }
        actuals[actualCount].argNodeId = -1;
        actuals[actualCount].exprNodeId = recvNodeId;
        actuals[actualCount].labelStart = 0;
        actuals[actualCount].labelEnd = 0;
        actuals[actualCount].hasLabel = 0;
        actuals[actualCount].isSynthetic = 1;
        actualCount++;
    }
    cur = SLFmtNextSibling(ast, calleeNodeId);
    while (cur >= 0) {
        const SLAstNode* argNode = &ast->nodes[cur];
        int32_t          exprNodeId;
        if (actualCount >= actualCap || (argNode->flags & SLAstFlag_CALL_ARG_SPREAD) != 0) {
            return 0;
        }
        exprNodeId = argNode->kind == SLAst_CALL_ARG ? SLFmtFirstChild(ast, cur) : cur;
        if (exprNodeId < 0) {
            return 0;
        }
        actuals[actualCount].argNodeId = cur;
        actuals[actualCount].exprNodeId = exprNodeId;
        actuals[actualCount].labelStart = argNode->dataStart;
        actuals[actualCount].labelEnd = argNode->dataEnd;
        actuals[actualCount].hasLabel =
            (uint8_t)(argNode->kind == SLAst_CALL_ARG && argNode->dataEnd > argNode->dataStart);
        actuals[actualCount].isSynthetic = 0;
        actualCount++;
        cur = SLFmtNextSibling(ast, cur);
    }
    if (outActualCount != NULL) {
        *outActualCount = actualCount;
    }
    return 1;
}

static int SLFmtGetFnParamForActual(
    const SLAst*           ast,
    SLStrView              src,
    int32_t                fnNodeId,
    const SLFmtCallActual* actuals,
    uint32_t               actualCount,
    uint32_t               actualIndex,
    int32_t* _Nullable outParamNodeId,
    int32_t* _Nullable outParamTypeNodeId) {
    SLFmtCallParam params[128];
    uint8_t        assigned[128] = { 0 };
    uint32_t       paramCount = 0;
    uint32_t       fixedCount;
    uint32_t       i;
    int            hasVariadic = 0;
    int32_t        cur;
    if (outParamNodeId != NULL) {
        *outParamNodeId = -1;
    }
    if (outParamTypeNodeId != NULL) {
        *outParamTypeNodeId = -1;
    }
    if (fnNodeId < 0 || (uint32_t)fnNodeId >= ast->len || actualIndex >= actualCount) {
        return 0;
    }
    cur = SLFmtFirstChild(ast, fnNodeId);
    while (cur >= 0 && ast->nodes[cur].kind == SLAst_TYPE_PARAM) {
        cur = SLFmtNextSibling(ast, cur);
    }
    while (cur >= 0 && ast->nodes[cur].kind == SLAst_PARAM) {
        int32_t typeNodeId = SLFmtFirstChild(ast, cur);
        if (paramCount >= 128u || typeNodeId < 0) {
            return 0;
        }
        params[paramCount].paramNodeId = cur;
        params[paramCount].typeNodeId = typeNodeId;
        params[paramCount].flags = ast->nodes[cur].flags;
        if ((ast->nodes[cur].flags & SLAstFlag_PARAM_VARIADIC) != 0) {
            hasVariadic = 1;
        }
        paramCount++;
        cur = SLFmtNextSibling(ast, cur);
    }
    fixedCount = hasVariadic ? (paramCount > 0 ? paramCount - 1u : 0u) : paramCount;
    if ((!hasVariadic && actualCount != paramCount) || (hasVariadic && actualCount < fixedCount)) {
        return 0;
    }

    for (i = 0; i < actualCount; i++) {
        const SLFmtCallActual* actual = &actuals[i];
        uint32_t               paramIndex = UINT32_MAX;
        uint32_t               p;
        if (actual->hasLabel) {
            for (p = 0; p < fixedCount; p++) {
                if (!assigned[p]
                    && SLFmtSlicesEqual(
                        src,
                        actual->labelStart,
                        actual->labelEnd,
                        ast->nodes[params[p].paramNodeId].dataStart,
                        ast->nodes[params[p].paramNodeId].dataEnd))
                {
                    paramIndex = p;
                    break;
                }
            }
            if (paramIndex == UINT32_MAX) {
                return 0;
            }
        } else if (i < fixedCount) {
            paramIndex = i;
        } else if (hasVariadic) {
            paramIndex = fixedCount;
        } else {
            return 0;
        }

        if (paramIndex < paramCount && paramIndex != fixedCount) {
            assigned[paramIndex] = 1;
        }
        if (i == actualIndex) {
            if (outParamNodeId != NULL) {
                *outParamNodeId = params[paramIndex].paramNodeId;
            }
            if (outParamTypeNodeId != NULL) {
                *outParamTypeNodeId = params[paramIndex].typeNodeId;
            }
            return 1;
        }
    }
    return 0;
}

static int SLFmtInferLocalCallAgainstFn(
    const SLAst* ast,
    SLStrView    src,
    int32_t      callNodeId,
    int32_t      fnNodeId,
    uint32_t     beforePos,
    int32_t      targetArgNodeId,
    SLFmtInferredType* _Nullable outReturnType,
    SLFmtInferredType* _Nullable outTargetParamType) {
    SLFmtCallActual actuals[128];
    uint32_t        actualCount = 0;
    uint32_t        i;
    SLFmtTypeEnv    fnEnv;
    int32_t         retTypeNodeId;
    int32_t         targetActualIndex = -1;
    if (outReturnType != NULL) {
        SLFmtInferredTypeInit(outReturnType);
    }
    if (outTargetParamType != NULL) {
        SLFmtInferredTypeInit(outTargetParamType);
    }
    if (!SLFmtMapCallActuals(ast, callNodeId, actuals, &actualCount, 128u)
        || !SLFmtTypeEnvInitFromDeclTypeParams(ast, fnNodeId, &fnEnv))
    {
        return 0;
    }
    for (i = 0; i < actualCount; i++) {
        SLFmtInferredType actualType;
        int32_t           paramTypeNodeId = -1;
        int32_t           concreteParamTypeNodeId;
        int               isTargetActual = 0;
        if (!SLFmtGetFnParamForActual(
                ast, src, fnNodeId, actuals, actualCount, i, NULL, &paramTypeNodeId))
        {
            return 0;
        }
        if (targetArgNodeId >= 0 && actuals[i].argNodeId == targetArgNodeId) {
            targetActualIndex = (int32_t)i;
            isTargetActual = 1;
        }
        if (isTargetActual) {
            continue;
        }
        SLFmtInferredTypeInit(&actualType);
        if (!SLFmtInferExprTypeEx(ast, src, actuals[i].exprNodeId, beforePos, 0u, &actualType)) {
            concreteParamTypeNodeId = SLFmtResolveTypeNodeThroughEnv(
                ast, src, paramTypeNodeId, &fnEnv);
            if (!SLFmtLiteralExprMatchesConcreteType(
                    ast, src, actuals[i].exprNodeId, concreteParamTypeNodeId))
            {
                return 0;
            }
        } else if (!SLFmtTypeCompatibleWithEnvs(
                       ast,
                       src,
                       paramTypeNodeId,
                       &fnEnv,
                       actualType.typeNodeId,
                       actualType.env.len > 0 ? &actualType.env : NULL))
        {
            return 0;
        }
    }
    if (targetActualIndex >= 0 && outTargetParamType != NULL) {
        int32_t targetParamTypeNodeId = -1;
        if (!SLFmtGetFnParamForActual(
                ast,
                src,
                fnNodeId,
                actuals,
                actualCount,
                (uint32_t)targetActualIndex,
                NULL,
                &targetParamTypeNodeId)
            || !SLFmtInferredTypeSet(outTargetParamType, targetParamTypeNodeId, &fnEnv))
        {
            return 0;
        }
    }
    retTypeNodeId = SLFmtFindFnReturnTypeNode(ast, fnNodeId);
    if (outReturnType == NULL) {
        return 1;
    }
    return retTypeNodeId >= 0 && SLFmtInferredTypeSet(outReturnType, retTypeNodeId, &fnEnv);
}

static int SLFmtCanContinueAlignedGroup(SLFmtCtx* c, int32_t prevNodeId, int32_t nextNodeId) {
    uint32_t gapNl;
    if (prevNodeId < 0 || nextNodeId < 0) {
        return 0;
    }
    gapNl = SLFmtCountNewlinesInRange(
        c->src, c->ast->nodes[prevNodeId].end, c->ast->nodes[nextNodeId].start);
    if (gapNl != 1u) {
        return 0;
    }
    if (SLFmtHasUnusedLeadingCommentsForNode(c, nextNodeId)) {
        return 0;
    }
    return 1;
}

static int SLFmtEmitStmt(SLFmtCtx* c, int32_t nodeId);

typedef struct {
    int32_t  nodeId;
    int32_t  typeNode;
    int32_t  initNode;
    uint32_t nameLen;
    uint32_t typeLen;
    uint32_t initLen;
    uint32_t codeLen;
    uint8_t  hasType;
    uint8_t  hasInit;
    uint8_t  hasTrailingComment;
    uint8_t  _pad;
} SLFmtAlignedVarRow;

typedef struct {
    int32_t  nodeId;
    int32_t  lhsNode;
    int32_t  rhsNode;
    uint16_t op;
    uint16_t _pad0;
    uint32_t lhsLen;
    uint32_t rhsLen;
    uint32_t opLen;
    uint32_t codeLen;
    uint8_t  hasTrailingComment;
    uint8_t  _pad1[3];
} SLFmtAlignedAssignRow;

typedef struct {
    uint32_t minLhsLen;
    uint32_t baseNameStart;
    uint32_t baseNameEnd;
    uint8_t  valid;
    uint8_t  _pad[3];
} SLFmtAssignCarryHint;

typedef struct {
    int32_t  nodeId;
    int32_t  bodyNodeId;
    uint32_t headLen;
    uint32_t inlineBodyLen;
    uint32_t codeLen;
    uint8_t  isDefault;
    uint8_t  inlineBody;
    uint8_t  hasTrailingComment;
    uint8_t  _pad[1];
} SLFmtSwitchClauseRow;

static int SLFmtGetAssignStmtParts(
    SLFmtCtx* c, int32_t stmtNodeId, int32_t* outLhs, int32_t* outRhs, uint16_t* outOp) {
    int32_t          exprNodeId;
    const SLAstNode* exprNode;
    int32_t          lhsNodeId;
    int32_t          rhsNodeId;
    if (stmtNodeId < 0 || (uint32_t)stmtNodeId >= c->ast->len) {
        return 0;
    }
    if (c->ast->nodes[stmtNodeId].kind != SLAst_EXPR_STMT) {
        return 0;
    }
    exprNodeId = SLFmtFirstChild(c->ast, stmtNodeId);
    if (exprNodeId < 0 || (uint32_t)exprNodeId >= c->ast->len) {
        return 0;
    }
    exprNode = &c->ast->nodes[exprNodeId];
    if (exprNode->kind != SLAst_BINARY || !SLFmtIsAssignmentOp((SLTokenKind)exprNode->op)) {
        return 0;
    }
    lhsNodeId = SLFmtFirstChild(c->ast, exprNodeId);
    rhsNodeId = lhsNodeId >= 0 ? SLFmtNextSibling(c->ast, lhsNodeId) : -1;
    if (lhsNodeId < 0 || rhsNodeId < 0) {
        return 0;
    }
    *outLhs = lhsNodeId;
    *outRhs = rhsNodeId;
    *outOp = exprNode->op;
    return 1;
}

static int SLFmtHasUnusedCommentsInNodeRange(const SLFmtCtx* c, int32_t nodeId) {
    uint32_t i;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return 0;
    }
    for (i = 0; i < c->commentLen; i++) {
        const SLComment* cm = &c->comments[i];
        if (c->commentUsed[i]) {
            continue;
        }
        if (SLFmtCommentWithinNodeRange(c->ast, cm, nodeId)) {
            return 1;
        }
    }
    return 0;
}

static int SLFmtCanInlineSingleStmtBlock(SLFmtCtx* c, int32_t blockNodeId, int32_t* outStmtNodeId) {
    int32_t stmtNodeId;
    if (blockNodeId < 0 || (uint32_t)blockNodeId >= c->ast->len
        || c->ast->nodes[blockNodeId].kind != SLAst_BLOCK)
    {
        return 0;
    }
    if (SLFmtHasUnusedCommentsInNodeRange(c, blockNodeId)) {
        return 0;
    }
    stmtNodeId = SLFmtFirstChild(c->ast, blockNodeId);
    if (stmtNodeId < 0 || SLFmtNextSibling(c->ast, stmtNodeId) >= 0) {
        return 0;
    }
    *outStmtNodeId = stmtNodeId;
    return 1;
}

static int SLFmtEmitInlineSingleStmtBlock(SLFmtCtx* c, int32_t blockNodeId) {
    int32_t stmtNodeId;
    if (!SLFmtCanInlineSingleStmtBlock(c, blockNodeId, &stmtNodeId)) {
        return -1;
    }
    if (SLFmtWriteCStr(c, "{ ") != 0 || SLFmtEmitStmtInline(c, stmtNodeId) != 0) {
        return -1;
    }
    return SLFmtWriteCStr(c, " }");
}

static int SLFmtMeasureInlineSingleStmtBlockLen(
    SLFmtCtx* c, int32_t blockNodeId, uint32_t* outLen) {
    SLFmtCtx m = *c;
    m.lineStart = 0;
    m.indent = 0;
    m.out.v = NULL;
    m.out.len = 0;
    m.out.cap = 0;
    if (SLFmtEmitInlineSingleStmtBlock(&m, blockNodeId) != 0) {
        return -1;
    }
    *outLen = m.out.len;
    return 0;
}

static int SLFmtNodeSourceTextEqual(SLFmtCtx* c, int32_t aNodeId, int32_t bNodeId) {
    const SLAstNode* a;
    const SLAstNode* b;
    uint32_t         aLen;
    if (aNodeId < 0 || bNodeId < 0 || (uint32_t)aNodeId >= c->ast->len
        || (uint32_t)bNodeId >= c->ast->len)
    {
        return 0;
    }
    a = &c->ast->nodes[aNodeId];
    b = &c->ast->nodes[bNodeId];
    aLen = a->end - a->start;
    if (aLen != (b->end - b->start)) {
        return 0;
    }
    if (aLen == 0) {
        return 1;
    }
    return memcmp(c->src.ptr + a->start, c->src.ptr + b->start, aLen) == 0;
}

static int SLFmtIsInlineAnonAggregateType(SLFmtCtx* c, int32_t typeNodeId) {
    SLAstKind kind;
    if (typeNodeId < 0 || (uint32_t)typeNodeId >= c->ast->len) {
        return 0;
    }
    kind = c->ast->nodes[typeNodeId].kind;
    if (kind != SLAst_TYPE_ANON_STRUCT && kind != SLAst_TYPE_ANON_UNION) {
        return 0;
    }
    return SLFmtCountNewlinesInRange(
               c->src, c->ast->nodes[typeNodeId].start, c->ast->nodes[typeNodeId].end)
        == 0u;
}

static int SLFmtEmitAlignedVarOrConstGroup(
    SLFmtCtx*             c,
    int32_t               firstNodeId,
    const char*           kw,
    int32_t*              outLast,
    int32_t*              outNext,
    SLFmtAssignCarryHint* outCarryHint) {
    const SLAstKind     kind = c->ast->nodes[firstNodeId].kind;
    int32_t             cur = firstNodeId;
    int32_t             prev = -1;
    uint32_t            count = 0;
    uint32_t            i;
    uint32_t            kwLen = SLFmtCStrLen(kw);
    uint32_t            maxNameLenWithType = 0;
    uint32_t            maxNameLenNoType = 0;
    uint32_t            maxBeforeOpLen = 0;
    SLFmtAlignedVarRow* rows;
    uint32_t*           commentRunMaxLens;

    while (cur >= 0) {
        int32_t next = SLFmtNextSibling(c->ast, cur);
        if (c->ast->nodes[cur].kind != kind) {
            break;
        }
        if (SLFmtIsGroupedVarLike(c, cur)) {
            break;
        }
        if (prev >= 0 && !SLFmtCanContinueAlignedGroup(c, prev, cur)) {
            break;
        }
        count++;
        prev = cur;
        cur = next;
    }

    rows = (SLFmtAlignedVarRow*)SLArenaAlloc(
        c->out.arena,
        count * (uint32_t)sizeof(SLFmtAlignedVarRow),
        (uint32_t)_Alignof(SLFmtAlignedVarRow));
    if (rows == NULL) {
        return -1;
    }
    memset(rows, 0, count * (uint32_t)sizeof(SLFmtAlignedVarRow));

    cur = firstNodeId;
    for (i = 0; i < count; i++) {
        const SLAstNode* n = &c->ast->nodes[cur];
        int32_t          typeNode = SLFmtFirstChild(c->ast, cur);
        int32_t          initNode = -1;
        rows[i].nodeId = cur;
        rows[i].nameLen = n->dataEnd - n->dataStart;
        if (typeNode >= 0 && !SLFmtIsTypeNodeKind(c->ast->nodes[typeNode].kind)) {
            initNode = typeNode;
            typeNode = -1;
        } else if (typeNode >= 0) {
            initNode = SLFmtNextSibling(c->ast, typeNode);
        }
        SLFmtRewriteVarTypeFromLiteralCast(c->ast, c->src, kw, 1u, &typeNode, &initNode);
        SLFmtRewriteRedundantVarType(c->ast, c->src, kw, 1u, n->start, &typeNode, &initNode);
        rows[i].typeNode = typeNode;
        rows[i].initNode = initNode;
        rows[i].hasType = (uint8_t)(typeNode >= 0);
        rows[i].hasInit = (uint8_t)(initNode >= 0);
        if (rows[i].hasType) {
            if (rows[i].nameLen > maxNameLenWithType) {
                maxNameLenWithType = rows[i].nameLen;
            }
        } else if (rows[i].nameLen > maxNameLenNoType) {
            maxNameLenNoType = rows[i].nameLen;
        }
        if (typeNode >= 0 && SLFmtMeasureTypeLen(c, typeNode, &rows[i].typeLen) != 0) {
            return -1;
        }
        if (initNode >= 0 && SLFmtMeasureExprLen(c, initNode, 0, &rows[i].initLen) != 0) {
            return -1;
        }
        rows[i].hasTrailingComment = (uint8_t)SLFmtHasUnusedTrailingCommentsForNodes(c, &cur, 1u);
        cur = SLFmtNextSibling(c->ast, cur);
    }

    for (i = 0; i < count; i++) {
        if (rows[i].hasInit) {
            uint32_t nameColLen = rows[i].hasType ? maxNameLenWithType : maxNameLenNoType;
            uint32_t beforeOpLen =
                kwLen + 1u + nameColLen + (rows[i].hasType ? 1u : 0u)
                + (rows[i].hasType ? rows[i].typeLen : 0u);
            if (beforeOpLen > maxBeforeOpLen) {
                maxBeforeOpLen = beforeOpLen;
            }
        }
    }

    commentRunMaxLens = (uint32_t*)SLArenaAlloc(
        c->out.arena, count * (uint32_t)sizeof(uint32_t), (uint32_t)_Alignof(uint32_t));
    if (commentRunMaxLens == NULL) {
        return -1;
    }
    memset(commentRunMaxLens, 0, count * (uint32_t)sizeof(uint32_t));

    for (i = 0; i < count; i++) {
        uint32_t nameColLen = rows[i].hasType ? maxNameLenWithType : maxNameLenNoType;
        uint32_t codeLen =
            kwLen + 1u + nameColLen + (rows[i].hasType ? 1u : 0u)
            + (rows[i].hasType ? rows[i].typeLen : 0u);
        if (rows[i].hasInit) {
            uint32_t padBeforeOp = (maxBeforeOpLen + 1u) - codeLen;
            codeLen += padBeforeOp + 1u + 1u + rows[i].initLen;
        }
        rows[i].codeLen = codeLen;
    }

    for (i = 0; i < count;) {
        uint32_t j;
        uint32_t runMax = 0;
        if (!rows[i].hasTrailingComment) {
            i++;
            continue;
        }
        j = i;
        while (j < count && rows[j].hasTrailingComment) {
            if (rows[j].codeLen > runMax) {
                runMax = rows[j].codeLen;
            }
            j++;
        }
        while (i < j) {
            commentRunMaxLens[i] = runMax;
            i++;
        }
    }

    for (i = 0; i < count; i++) {
        const SLAstNode* n = &c->ast->nodes[rows[i].nodeId];
        uint32_t         nameColLen = rows[i].hasType ? maxNameLenWithType : maxNameLenNoType;
        uint32_t         lineLen;
        if (SLFmtEmitLeadingCommentsForNode(c, rows[i].nodeId) != 0) {
            return -1;
        }
        if (SLFmtWriteCStr(c, kw) != 0 || SLFmtWriteChar(c, ' ') != 0
            || SLFmtWriteSlice(c, n->dataStart, n->dataEnd) != 0)
        {
            return -1;
        }
        if (SLFmtWriteSpaces(c, nameColLen - rows[i].nameLen + (rows[i].hasType ? 1u : 0u)) != 0) {
            return -1;
        }
        lineLen = kwLen + 1u + nameColLen + (rows[i].hasType ? 1u : 0u);
        if (rows[i].hasType) {
            if (SLFmtEmitType(c, rows[i].typeNode) != 0) {
                return -1;
            }
            lineLen += rows[i].typeLen;
        }
        if (rows[i].hasInit) {
            uint32_t padBeforeOp = (maxBeforeOpLen + 1u) - lineLen;
            if (SLFmtWriteSpaces(c, padBeforeOp) != 0 || SLFmtWriteChar(c, '=') != 0
                || SLFmtWriteChar(c, ' ') != 0 || SLFmtEmitExpr(c, rows[i].initNode, 0) != 0)
            {
                return -1;
            }
            lineLen += padBeforeOp + 1u + 1u + rows[i].initLen;
        }
        if (rows[i].hasTrailingComment) {
            uint32_t padComment = (commentRunMaxLens[i] - lineLen) + 1u;
            int32_t  nodeId = rows[i].nodeId;
            if (SLFmtEmitTrailingCommentsForNodes(c, &nodeId, 1u, padComment) != 0) {
                return -1;
            }
        }
        if (i + 1u < count && SLFmtNewline(c) != 0) {
            return -1;
        }
    }

    outCarryHint->valid = 0;
    if (count == 1u && !rows[0].hasType && rows[0].hasInit) {
        const SLAstNode* n = &c->ast->nodes[rows[0].nodeId];
        outCarryHint->minLhsLen = maxBeforeOpLen;
        outCarryHint->baseNameStart = n->dataStart;
        outCarryHint->baseNameEnd = n->dataEnd;
        outCarryHint->valid = 1;
    }

    *outLast = rows[count - 1u].nodeId;
    *outNext = cur;
    return 0;
}

static int SLFmtEmitAlignedAssignGroup(
    SLFmtCtx* c, int32_t firstNodeId, int32_t* outLast, int32_t* outNext, uint32_t minLhsLen) {
    int32_t                cur = firstNodeId;
    int32_t                prev = -1;
    int32_t                prevLhs = -1;
    uint32_t               count = 0;
    uint32_t               i;
    uint32_t               maxLhsLen = 0;
    uint32_t               maxOpLen = 0;
    SLFmtAlignedAssignRow* rows;

    while (cur >= 0) {
        int32_t  lhsNode;
        int32_t  rhsNode;
        uint16_t op;
        int32_t  next = SLFmtNextSibling(c->ast, cur);
        if (!SLFmtGetAssignStmtParts(c, cur, &lhsNode, &rhsNode, &op)) {
            break;
        }
        if (prev >= 0 && !SLFmtCanContinueAlignedGroup(c, prev, cur)) {
            break;
        }
        if (prevLhs >= 0 && !SLFmtNodeSourceTextEqual(c, prevLhs, lhsNode)) {
            break;
        }
        count++;
        prev = cur;
        prevLhs = lhsNode;
        cur = next;
    }

    rows = (SLFmtAlignedAssignRow*)SLArenaAlloc(
        c->out.arena,
        count * (uint32_t)sizeof(SLFmtAlignedAssignRow),
        (uint32_t)_Alignof(SLFmtAlignedAssignRow));
    if (rows == NULL) {
        return -1;
    }
    memset(rows, 0, count * (uint32_t)sizeof(SLFmtAlignedAssignRow));

    cur = firstNodeId;
    for (i = 0; i < count; i++) {
        int32_t     lhsNode;
        int32_t     rhsNode;
        uint16_t    op;
        const char* opText;
        if (!SLFmtGetAssignStmtParts(c, cur, &lhsNode, &rhsNode, &op)) {
            return -1;
        }
        rows[i].nodeId = cur;
        rows[i].lhsNode = lhsNode;
        rows[i].rhsNode = rhsNode;
        rows[i].op = op;
        if (SLFmtMeasureExprLen(c, lhsNode, 0, &rows[i].lhsLen) != 0
            || SLFmtMeasureExprLen(c, rhsNode, 0, &rows[i].rhsLen) != 0)
        {
            return -1;
        }
        opText = SLFmtTokenOpText((SLTokenKind)op);
        rows[i].opLen = SLFmtCStrLen(opText);
        if (rows[i].lhsLen > maxLhsLen) {
            maxLhsLen = rows[i].lhsLen;
        }
        if (rows[i].opLen > maxOpLen) {
            maxOpLen = rows[i].opLen;
        }
        rows[i].hasTrailingComment = (uint8_t)SLFmtHasUnusedTrailingCommentsForNodes(c, &cur, 1u);
        cur = SLFmtNextSibling(c->ast, cur);
    }

    if (minLhsLen > maxLhsLen) {
        maxLhsLen = minLhsLen;
    }

    for (i = 0; i < count; i++) {
        uint32_t padBeforeOp = (maxLhsLen + 1u) - rows[i].lhsLen;
        uint32_t padAfterOp = (maxOpLen + 1u) - rows[i].opLen;
        rows[i].codeLen =
            rows[i].lhsLen + padBeforeOp + rows[i].opLen + padAfterOp + rows[i].rhsLen;
    }

    for (i = 0; i < count; i++) {
        uint32_t    padBeforeOp = (maxLhsLen + 1u) - rows[i].lhsLen;
        uint32_t    padAfterOp = (maxOpLen + 1u) - rows[i].opLen;
        const char* opText = SLFmtTokenOpText((SLTokenKind)rows[i].op);
        if (SLFmtEmitLeadingCommentsForNode(c, rows[i].nodeId) != 0) {
            return -1;
        }
        if (SLFmtEmitExpr(c, rows[i].lhsNode, 0) != 0 || SLFmtWriteSpaces(c, padBeforeOp) != 0
            || SLFmtWriteCStr(c, opText) != 0 || SLFmtWriteSpaces(c, padAfterOp) != 0
            || SLFmtEmitExpr(c, rows[i].rhsNode, 0) != 0)
        {
            return -1;
        }
        if (rows[i].hasTrailingComment) {
            uint32_t padComment = 1u;
            int32_t  nodeId = rows[i].nodeId;
            if (SLFmtEmitTrailingCommentsForNodes(c, &nodeId, 1u, padComment) != 0) {
                return -1;
            }
        }
        if (i + 1u < count && SLFmtNewline(c) != 0) {
            return -1;
        }
    }

    *outLast = rows[count - 1u].nodeId;
    *outNext = cur;
    return 0;
}

static int SLFmtIsIndexedAssignOnNamedBase(
    SLFmtCtx* c, int32_t lhsNodeId, uint32_t nameStart, uint32_t nameEnd) {
    const SLAstNode* lhsNode;
    int32_t          baseNodeId;
    uint32_t         identStart = 0;
    uint32_t         identEnd = 0;
    if (lhsNodeId < 0 || (uint32_t)lhsNodeId >= c->ast->len || nameEnd <= nameStart) {
        return 0;
    }
    lhsNode = &c->ast->nodes[lhsNodeId];
    if (lhsNode->kind != SLAst_INDEX || (lhsNode->flags & SLAstFlag_INDEX_SLICE) != 0) {
        return 0;
    }
    baseNodeId = SLFmtFirstChild(c->ast, lhsNodeId);
    if (!SLFmtExprIsPlainIdent(c->ast, baseNodeId, &identStart, &identEnd)) {
        return 0;
    }
    return SLFmtSlicesEqual(c->src, identStart, identEnd, nameStart, nameEnd);
}

static int SLFmtMeasureCaseHeadLen(
    SLFmtCtx* c, int32_t caseNodeId, uint32_t* outLen, int32_t* outBodyNodeId) {
    int32_t  k = SLFmtFirstChild(c->ast, caseNodeId);
    uint32_t len = 5u;
    int      first = 1;
    int32_t  bodyNodeId = -1;
    while (k >= 0) {
        int32_t next = SLFmtNextSibling(c->ast, k);
        if (next < 0) {
            bodyNodeId = k;
            break;
        }
        int32_t          exprNode = k;
        int32_t          aliasNode = -1;
        const SLAstNode* kn = &c->ast->nodes[k];
        if (kn->kind == SLAst_CASE_PATTERN) {
            exprNode = SLFmtFirstChild(c->ast, k);
            aliasNode = exprNode >= 0 ? SLFmtNextSibling(c->ast, exprNode) : -1;
            if (exprNode < 0) {
                return -1;
            }
        }
        if (!first) {
            len += 2u;
        }
        {
            uint32_t exprLen;
            if (SLFmtMeasureExprLen(c, exprNode, 0, &exprLen) != 0) {
                return -1;
            }
            len += exprLen;
        }
        if (aliasNode >= 0) {
            const SLAstNode* alias = &c->ast->nodes[aliasNode];
            len += 4u + (alias->dataEnd - alias->dataStart); /* " as " + alias */
        }
        first = 0;
        k = next;
    }
    if (bodyNodeId < 0 || c->ast->nodes[bodyNodeId].kind != SLAst_BLOCK) {
        return -1;
    }
    *outLen = len;
    *outBodyNodeId = bodyNodeId;
    return 0;
}

static int SLFmtEmitCaseHead(SLFmtCtx* c, int32_t caseNodeId) {
    int32_t k = SLFmtFirstChild(c->ast, caseNodeId);
    int     first = 1;
    if (SLFmtWriteCStr(c, "case ") != 0) {
        return -1;
    }
    while (k >= 0) {
        int32_t next = SLFmtNextSibling(c->ast, k);
        int32_t exprNode = k;
        int32_t aliasNode = -1;
        if (next < 0) {
            break;
        }
        if (c->ast->nodes[k].kind == SLAst_CASE_PATTERN) {
            exprNode = SLFmtFirstChild(c->ast, k);
            aliasNode = exprNode >= 0 ? SLFmtNextSibling(c->ast, exprNode) : -1;
            if (exprNode < 0) {
                return -1;
            }
        }
        if (!first && SLFmtWriteCStr(c, ", ") != 0) {
            return -1;
        }
        if (SLFmtEmitExpr(c, exprNode, 0) != 0) {
            return -1;
        }
        if (aliasNode >= 0) {
            const SLAstNode* alias = &c->ast->nodes[aliasNode];
            if (SLFmtWriteCStr(c, " as ") != 0
                || SLFmtWriteSlice(c, alias->dataStart, alias->dataEnd) != 0)
            {
                return -1;
            }
        }
        first = 0;
        k = next;
    }
    return 0;
}

static int SLFmtEmitSwitchClauseGroup(
    SLFmtCtx* c,
    int32_t   firstClauseNodeId,
    int32_t*  outLastClauseNodeId,
    int32_t*  outNextClauseNodeId) {
    int32_t               cur = firstClauseNodeId;
    int32_t               prev = -1;
    uint32_t              count = 0;
    uint32_t              i;
    SLFmtSwitchClauseRow* rows;
    uint32_t*             commentRunMaxLens;
    uint32_t*             inlineRunMaxHeadLens;

    while (cur >= 0) {
        int32_t next = SLFmtNextSibling(c->ast, cur);
        if (c->ast->nodes[cur].kind != SLAst_CASE && c->ast->nodes[cur].kind != SLAst_DEFAULT) {
            break;
        }
        if (prev >= 0 && !SLFmtCanContinueAlignedGroup(c, prev, cur)) {
            break;
        }
        count++;
        prev = cur;
        cur = next;
    }

    rows = (SLFmtSwitchClauseRow*)SLArenaAlloc(
        c->out.arena,
        count * (uint32_t)sizeof(SLFmtSwitchClauseRow),
        (uint32_t)_Alignof(SLFmtSwitchClauseRow));
    if (rows == NULL) {
        return -1;
    }
    memset(rows, 0, count * (uint32_t)sizeof(SLFmtSwitchClauseRow));

    inlineRunMaxHeadLens = (uint32_t*)SLArenaAlloc(
        c->out.arena, count * (uint32_t)sizeof(uint32_t), (uint32_t)_Alignof(uint32_t));
    if (inlineRunMaxHeadLens == NULL) {
        return -1;
    }
    memset(inlineRunMaxHeadLens, 0, count * (uint32_t)sizeof(uint32_t));

    commentRunMaxLens = (uint32_t*)SLArenaAlloc(
        c->out.arena, count * (uint32_t)sizeof(uint32_t), (uint32_t)_Alignof(uint32_t));
    if (commentRunMaxLens == NULL) {
        return -1;
    }
    memset(commentRunMaxLens, 0, count * (uint32_t)sizeof(uint32_t));

    cur = firstClauseNodeId;
    for (i = 0; i < count; i++) {
        SLFmtSwitchClauseRow* r = &rows[i];
        int32_t               next = SLFmtNextSibling(c->ast, cur);
        r->nodeId = cur;
        r->isDefault = (uint8_t)(c->ast->nodes[cur].kind == SLAst_DEFAULT);
        if (r->isDefault) {
            r->headLen = 7u;
            r->bodyNodeId = SLFmtFirstChild(c->ast, cur);
            if (r->bodyNodeId < 0 || c->ast->nodes[r->bodyNodeId].kind != SLAst_BLOCK) {
                return -1;
            }
        } else if (SLFmtMeasureCaseHeadLen(c, cur, &r->headLen, &r->bodyNodeId) != 0) {
            return -1;
        }
        {
            int32_t stmtNodeId = -1;
            if (SLFmtCanInlineSingleStmtBlock(c, r->bodyNodeId, &stmtNodeId)) {
                r->inlineBody = 1;
                if (SLFmtMeasureInlineSingleStmtBlockLen(c, r->bodyNodeId, &r->inlineBodyLen) != 0)
                {
                    return -1;
                }
            }
        }
        r->hasTrailingComment = (uint8_t)SLFmtHasUnusedTrailingCommentsForNodes(c, &cur, 1u);
        cur = next;
    }

    for (i = 0; i < count;) {
        uint32_t j;
        uint32_t runMax = 0;
        if (!rows[i].inlineBody) {
            i++;
            continue;
        }
        j = i;
        while (j < count && rows[j].inlineBody) {
            if (rows[j].headLen > runMax) {
                runMax = rows[j].headLen;
            }
            j++;
        }
        while (i < j) {
            inlineRunMaxHeadLens[i] = runMax;
            i++;
        }
    }

    for (i = 0; i < count; i++) {
        SLFmtSwitchClauseRow* r = &rows[i];
        if (r->inlineBody) {
            uint32_t padBeforeBody = (inlineRunMaxHeadLens[i] - r->headLen) + 1u;
            r->codeLen = r->headLen + padBeforeBody + r->inlineBodyLen;
        } else {
            r->codeLen = 0u;
        }
    }

    for (i = 0; i < count;) {
        uint32_t j;
        uint32_t runMax = 0;
        if (!rows[i].hasTrailingComment || !rows[i].inlineBody) {
            i++;
            continue;
        }
        j = i;
        while (j < count && rows[j].hasTrailingComment && rows[j].inlineBody) {
            if (rows[j].codeLen > runMax) {
                runMax = rows[j].codeLen;
            }
            j++;
        }
        while (i < j) {
            commentRunMaxLens[i] = runMax;
            i++;
        }
    }

    for (i = 0; i < count; i++) {
        SLFmtSwitchClauseRow* r = &rows[i];
        uint32_t              lineLen = 0;
        uint32_t              padBeforeBody = 1u;
        if (r->inlineBody) {
            padBeforeBody = (inlineRunMaxHeadLens[i] - r->headLen) + 1u;
        } else if (i > 0u && rows[i - 1u].inlineBody) {
            uint32_t prevRunMaxHeadLen = inlineRunMaxHeadLens[i - 1u];
            if (prevRunMaxHeadLen > r->headLen) {
                padBeforeBody = (prevRunMaxHeadLen - r->headLen) + 1u;
            }
        }
        if (SLFmtEmitLeadingCommentsForNode(c, r->nodeId) != 0) {
            return -1;
        }
        if (r->isDefault) {
            if (SLFmtWriteCStr(c, "default") != 0) {
                return -1;
            }
        } else if (SLFmtEmitCaseHead(c, r->nodeId) != 0) {
            return -1;
        }
        lineLen = r->headLen;
        if (SLFmtWriteSpaces(c, padBeforeBody) != 0) {
            return -1;
        }
        lineLen += padBeforeBody;
        if (r->inlineBody) {
            if (SLFmtEmitInlineSingleStmtBlock(c, r->bodyNodeId) != 0) {
                return -1;
            }
            lineLen += r->inlineBodyLen;
        } else if (SLFmtEmitBlock(c, r->bodyNodeId) != 0) {
            return -1;
        }
        if (r->hasTrailingComment) {
            int32_t  nodeId = r->nodeId;
            uint32_t padComment = 1u;
            if (r->inlineBody && commentRunMaxLens[i] > 0u) {
                padComment = (commentRunMaxLens[i] - lineLen) + 1u;
            }
            if (SLFmtEmitTrailingCommentsForNodes(c, &nodeId, 1u, padComment) != 0) {
                return -1;
            }
        }
        if (i + 1u < count && SLFmtNewline(c) != 0) {
            return -1;
        }
    }

    *outLastClauseNodeId = rows[count - 1u].nodeId;
    *outNextClauseNodeId = cur;
    return 0;
}

static int SLFmtEmitBlock(SLFmtCtx* c, int32_t nodeId) {
    int32_t              stmt;
    int32_t              prevEmitted = -1;
    SLFmtAssignCarryHint carryHint;
    memset(&carryHint, 0, sizeof(carryHint));
    if (SLFmtWriteChar(c, '{') != 0) {
        return -1;
    }
    stmt = SLFmtFirstChild(c->ast, nodeId);
    if (stmt >= 0) {
        if (SLFmtNewline(c) != 0) {
            return -1;
        }
        c->indent++;
        while (stmt >= 0) {
            int32_t  last = stmt;
            int32_t  next = SLFmtNextSibling(c->ast, stmt);
            int32_t  lhsNode = -1;
            int32_t  rhsNode = -1;
            uint16_t op = 0;
            if (prevEmitted >= 0) {
                if (!SLFmtCanContinueAlignedGroup(c, prevEmitted, stmt)) {
                    carryHint.valid = 0;
                }
                if (SLFmtNewline(c) != 0) {
                    return -1;
                }
                if (SLFmtNeedsBlankLineBeforeNode(c, prevEmitted, stmt) && SLFmtNewline(c) != 0) {
                    return -1;
                }
            }
            if (c->ast->nodes[stmt].kind == SLAst_VAR && !SLFmtIsGroupedVarLike(c, stmt)) {
                if (SLFmtEmitAlignedVarOrConstGroup(c, stmt, "var", &last, &next, &carryHint) != 0)
                {
                    return -1;
                }
            } else if (c->ast->nodes[stmt].kind == SLAst_CONST && !SLFmtIsGroupedVarLike(c, stmt)) {
                if (SLFmtEmitAlignedVarOrConstGroup(c, stmt, "const", &last, &next, &carryHint)
                    != 0)
                {
                    return -1;
                }
            } else if (SLFmtGetAssignStmtParts(c, stmt, &lhsNode, &rhsNode, &op)) {
                uint32_t minLhsLen = 0;
                if (carryHint.valid
                    && SLFmtIsIndexedAssignOnNamedBase(
                        c, lhsNode, carryHint.baseNameStart, carryHint.baseNameEnd))
                {
                    minLhsLen = carryHint.minLhsLen;
                }
                if (SLFmtEmitAlignedAssignGroup(c, stmt, &last, &next, minLhsLen) != 0) {
                    return -1;
                }
                carryHint.valid = 0;
            } else {
                if (SLFmtEmitStmt(c, stmt) != 0) {
                    return -1;
                }
                carryHint.valid = 0;
                last = stmt;
            }
            prevEmitted = last;
            stmt = next;
        }
        c->indent--;
        if (SLFmtNewline(c) != 0) {
            return -1;
        }
    }
    return SLFmtWriteChar(c, '}');
}

static int SLFmtEmitVarLike(SLFmtCtx* c, int32_t nodeId, const char* kw) {
    int32_t firstChild = SLFmtFirstChild(c->ast, nodeId);
    int32_t type = -1;
    int32_t init = -1;
    if (firstChild >= 0 && c->ast->nodes[firstChild].kind == SLAst_NAME_LIST) {
        uint32_t i;
        uint32_t nameCount = SLFmtListCount(c->ast, firstChild);
        int32_t  afterNames = SLFmtNextSibling(c->ast, firstChild);
        if (afterNames >= 0 && SLFmtIsTypeNodeKind(c->ast->nodes[afterNames].kind)) {
            type = afterNames;
            init = SLFmtNextSibling(c->ast, afterNames);
        } else {
            init = afterNames;
        }
        SLFmtRewriteVarTypeFromLiteralCast(c->ast, c->src, kw, nameCount, &type, &init);
        SLFmtRewriteRedundantVarType(
            c->ast, c->src, kw, nameCount, c->ast->nodes[nodeId].start, &type, &init);
        if (SLFmtWriteCStr(c, kw) != 0 || SLFmtWriteChar(c, ' ') != 0) {
            return -1;
        }
        for (i = 0; i < nameCount; i++) {
            int32_t nameNode = SLFmtListItemAt(c->ast, firstChild, i);
            if (nameNode < 0) {
                return -1;
            }
            if (i > 0 && SLFmtWriteCStr(c, ", ") != 0) {
                return -1;
            }
            if (SLFmtWriteSlice(
                    c, c->ast->nodes[nameNode].dataStart, c->ast->nodes[nameNode].dataEnd)
                != 0)
            {
                return -1;
            }
        }
        if (type >= 0) {
            if (SLFmtWriteChar(c, ' ') != 0 || SLFmtEmitType(c, type) != 0) {
                return -1;
            }
        }
        if (init >= 0) {
            if (SLFmtWriteCStr(c, " = ") != 0) {
                return -1;
            }
            if (c->ast->nodes[init].kind == SLAst_EXPR_LIST) {
                if (SLFmtEmitExprList(c, init) != 0) {
                    return -1;
                }
            } else if (SLFmtEmitExpr(c, init, 0) != 0) {
                return -1;
            }
        }
        return 0;
    }

    {
        const SLAstNode* n = &c->ast->nodes[nodeId];
        type = firstChild;
        if (type >= 0 && !SLFmtIsTypeNodeKind(c->ast->nodes[type].kind)) {
            init = type;
            type = -1;
        } else if (type >= 0) {
            init = SLFmtNextSibling(c->ast, type);
        }
        SLFmtRewriteVarTypeFromLiteralCast(c->ast, c->src, kw, 1u, &type, &init);
        SLFmtRewriteRedundantVarType(c->ast, c->src, kw, 1u, n->start, &type, &init);
        if (SLFmtWriteCStr(c, kw) != 0 || SLFmtWriteChar(c, ' ') != 0
            || SLFmtWriteSlice(c, n->dataStart, n->dataEnd) != 0)
        {
            return -1;
        }
        if (type >= 0) {
            if (SLFmtWriteChar(c, ' ') != 0 || SLFmtEmitType(c, type) != 0) {
                return -1;
            }
        }
        if (init >= 0) {
            if (SLFmtWriteCStr(c, " = ") != 0 || SLFmtEmitExpr(c, init, 0) != 0) {
                return -1;
            }
        }
        return 0;
    }
}

static int SLFmtEmitMultiAssign(SLFmtCtx* c, int32_t nodeId) {
    int32_t  lhsList = SLFmtFirstChild(c->ast, nodeId);
    int32_t  rhsList = lhsList >= 0 ? SLFmtNextSibling(c->ast, lhsList) : -1;
    uint32_t i;
    uint32_t lhsCount;
    if (lhsList < 0 || rhsList < 0 || c->ast->nodes[lhsList].kind != SLAst_EXPR_LIST
        || c->ast->nodes[rhsList].kind != SLAst_EXPR_LIST)
    {
        return -1;
    }
    lhsCount = SLFmtListCount(c->ast, lhsList);
    for (i = 0; i < lhsCount; i++) {
        int32_t lhsNode = SLFmtListItemAt(c->ast, lhsList, i);
        if (lhsNode < 0) {
            return -1;
        }
        if (i > 0 && SLFmtWriteCStr(c, ", ") != 0) {
            return -1;
        }
        if (SLFmtEmitExpr(c, lhsNode, 0) != 0) {
            return -1;
        }
    }
    if (SLFmtWriteCStr(c, " = ") != 0) {
        return -1;
    }
    if (SLFmtEmitExprList(c, rhsList) != 0) {
        return -1;
    }
    return 0;
}

static int SLFmtEmitForHeaderFromSource(
    SLFmtCtx* c, int32_t nodeId, int32_t bodyNode, int32_t* parts, uint32_t partLen) {
    const SLAstNode* n = &c->ast->nodes[nodeId];
    uint32_t         hdrStart = n->start;
    uint32_t         hdrEnd = c->ast->nodes[bodyNode].start;
    uint32_t         i;
    uint32_t         s1 = UINT32_MAX;
    uint32_t         s2 = UINT32_MAX;
    uint32_t         idx = 0;
    uint32_t         aStart;
    while (hdrStart < hdrEnd && c->src.ptr[hdrStart] != 'f') {
        hdrStart++;
    }
    aStart = hdrStart;
    while (aStart < hdrEnd && c->src.ptr[aStart] != ' ') {
        aStart++;
    }
    for (i = aStart; i < hdrEnd; i++) {
        if (c->src.ptr[i] == ';') {
            if (s1 == UINT32_MAX) {
                s1 = i;
            } else {
                s2 = i;
                break;
            }
        }
    }
    if (s1 == UINT32_MAX || s2 == UINT32_MAX) {
        for (i = 0; i < partLen; i++) {
            if (i > 0 && SLFmtWriteCStr(c, "; ") != 0) {
                return -1;
            }
            if (SLFmtEmitExpr(c, parts[i], 0) != 0) {
                return -1;
            }
        }
        while (i < 3u) {
            if (SLFmtWriteCStr(c, ";") != 0) {
                return -1;
            }
            if (i < 2u && SLFmtWriteChar(c, ' ') != 0) {
                return -1;
            }
            i++;
        }
        return 0;
    }

    {
        uint32_t seg0Has = 0;
        uint32_t seg1Has = 0;
        uint32_t seg2Has = 0;
        uint32_t p;
        for (p = aStart; p < s1; p++) {
            char ch = c->src.ptr[p];
            if (!(ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')) {
                seg0Has = 1;
                break;
            }
        }
        for (p = s1 + 1u; p < s2; p++) {
            char ch = c->src.ptr[p];
            if (!(ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')) {
                seg1Has = 1;
                break;
            }
        }
        for (p = s2 + 1u; p < hdrEnd; p++) {
            char ch = c->src.ptr[p];
            if (!(ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')) {
                seg2Has = 1;
                break;
            }
        }

        if (seg0Has && idx < partLen) {
            if (c->ast->nodes[parts[idx]].kind == SLAst_VAR) {
                if (SLFmtEmitVarLike(c, parts[idx], "var") != 0) {
                    return -1;
                }
            } else if (SLFmtEmitExpr(c, parts[idx], 0) != 0) {
                return -1;
            }
            idx++;
        }
        if (SLFmtWriteCStr(c, "; ") != 0) {
            return -1;
        }
        if (seg1Has && idx < partLen) {
            if (SLFmtEmitExpr(c, parts[idx], 0) != 0) {
                return -1;
            }
            idx++;
        }
        if (SLFmtWriteCStr(c, "; ") != 0) {
            return -1;
        }
        if (seg2Has && idx < partLen) {
            if (SLFmtEmitExpr(c, parts[idx], 0) != 0) {
                return -1;
            }
            idx++;
        }
    }
    return 0;
}

static int SLFmtEmitStmtInline(SLFmtCtx* c, int32_t nodeId) {
    const SLAstNode* n;
    int32_t          ch;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return -1;
    }
    n = &c->ast->nodes[nodeId];
    switch (n->kind) {
        case SLAst_BLOCK: return SLFmtEmitBlock(c, nodeId);
        case SLAst_VAR:   return SLFmtEmitVarLike(c, nodeId, "var");
        case SLAst_CONST: return SLFmtEmitVarLike(c, nodeId, "const");
        case SLAst_CONST_BLOCK:
            ch = SLFmtFirstChild(c->ast, nodeId);
            if (SLFmtWriteCStr(c, "const ") != 0) {
                return -1;
            }
            return ch >= 0 ? SLFmtEmitBlock(c, ch) : 0;
        case SLAst_MULTI_ASSIGN: return SLFmtEmitMultiAssign(c, nodeId);
        case SLAst_IF:           {
            int32_t cond = SLFmtFirstChild(c->ast, nodeId);
            int32_t thenNode = cond >= 0 ? SLFmtNextSibling(c->ast, cond) : -1;
            int32_t elseNode = thenNode >= 0 ? SLFmtNextSibling(c->ast, thenNode) : -1;
            if (SLFmtWriteCStr(c, "if ") != 0 || (cond >= 0 && SLFmtEmitExpr(c, cond, 0) != 0)
                || SLFmtWriteChar(c, ' ') != 0
                || (thenNode >= 0 && SLFmtEmitBlock(c, thenNode) != 0))
            {
                return -1;
            }
            if (elseNode >= 0) {
                if (SLFmtWriteCStr(c, " else ") != 0) {
                    return -1;
                }
                if (c->ast->nodes[elseNode].kind == SLAst_IF) {
                    if (SLFmtEmitStmtInline(c, elseNode) != 0) {
                        return -1;
                    }
                } else if (SLFmtEmitBlock(c, elseNode) != 0) {
                    return -1;
                }
            }
            return 0;
        }
        case SLAst_FOR: {
            int32_t  parts[4];
            uint32_t partLen = 0;
            int32_t  cur = SLFmtFirstChild(c->ast, nodeId);
            int32_t  bodyNode;
            while (cur >= 0 && partLen < 4u) {
                parts[partLen++] = cur;
                cur = SLFmtNextSibling(c->ast, cur);
            }
            if (partLen == 0) {
                return SLFmtWriteCStr(c, "for {}");
            }
            bodyNode = parts[partLen - 1u];
            if (SLFmtWriteCStr(c, "for") != 0) {
                return -1;
            }
            if ((n->flags & SLAstFlag_FOR_IN) != 0) {
                int     hasKey = (n->flags & SLAstFlag_FOR_IN_HAS_KEY) != 0;
                int32_t keyNode = -1;
                int32_t valueNode = -1;
                int32_t sourceNode = -1;
                if ((!hasKey && partLen != 3u) || (hasKey && partLen != 4u)) {
                    return -1;
                }
                if (hasKey) {
                    keyNode = parts[0];
                    valueNode = parts[1];
                    sourceNode = parts[2];
                    if (SLFmtWriteChar(c, ' ') != 0) {
                        return -1;
                    }
                    if ((n->flags & SLAstFlag_FOR_IN_KEY_REF) != 0 && SLFmtWriteChar(c, '&') != 0) {
                        return -1;
                    }
                    if (SLFmtEmitExpr(c, keyNode, 0) != 0 || SLFmtWriteCStr(c, ", ") != 0) {
                        return -1;
                    }
                } else {
                    valueNode = parts[0];
                    sourceNode = parts[1];
                    if (SLFmtWriteChar(c, ' ') != 0) {
                        return -1;
                    }
                }
                if ((n->flags & SLAstFlag_FOR_IN_VALUE_DISCARD) == 0) {
                    if ((n->flags & SLAstFlag_FOR_IN_VALUE_REF) != 0 && SLFmtWriteChar(c, '&') != 0)
                    {
                        return -1;
                    }
                }
                if (SLFmtEmitExpr(c, valueNode, 0) != 0 || SLFmtWriteCStr(c, " in ") != 0
                    || SLFmtEmitExpr(c, sourceNode, 0) != 0 || SLFmtWriteChar(c, ' ') != 0
                    || SLFmtEmitBlock(c, bodyNode) != 0)
                {
                    return -1;
                }
                return 0;
            }
            if (partLen == 1u && c->ast->nodes[bodyNode].kind == SLAst_BLOCK) {
                if (SLFmtWriteChar(c, ' ') != 0 || SLFmtEmitBlock(c, bodyNode) != 0) {
                    return -1;
                }
                return 0;
            }
            if (SLFmtContainsSemicolonInRange(c->src, n->start, c->ast->nodes[bodyNode].start)) {
                if (SLFmtWriteChar(c, ' ') != 0
                    || SLFmtEmitForHeaderFromSource(c, nodeId, bodyNode, parts, partLen - 1u) != 0
                    || SLFmtWriteChar(c, ' ') != 0 || SLFmtEmitBlock(c, bodyNode) != 0)
                {
                    return -1;
                }
                return 0;
            }
            if (SLFmtWriteChar(c, ' ') != 0 || SLFmtEmitExpr(c, parts[0], 0) != 0
                || SLFmtWriteChar(c, ' ') != 0 || SLFmtEmitBlock(c, bodyNode) != 0)
            {
                return -1;
            }
            return 0;
        }
        case SLAst_SWITCH: {
            int32_t cur = SLFmtFirstChild(c->ast, nodeId);
            int32_t prevClause = -1;
            if (SLFmtWriteCStr(c, "switch") != 0) {
                return -1;
            }
            if (n->flags == 1 && cur >= 0) {
                if (SLFmtWriteChar(c, ' ') != 0 || SLFmtEmitExpr(c, cur, 0) != 0) {
                    return -1;
                }
                cur = SLFmtNextSibling(c->ast, cur);
            }
            if (SLFmtWriteCStr(c, " {") != 0) {
                return -1;
            }
            if (cur >= 0) {
                if (SLFmtNewline(c) != 0) {
                    return -1;
                }
                c->indent++;
                while (cur >= 0) {
                    int32_t lastClause = cur;
                    int32_t nextClause = SLFmtNextSibling(c->ast, cur);
                    if (prevClause >= 0) {
                        if (SLFmtNewline(c) != 0) {
                            return -1;
                        }
                        if (SLFmtNeedsBlankLineBeforeNode(c, prevClause, cur)
                            && SLFmtNewline(c) != 0)
                        {
                            return -1;
                        }
                    }
                    if (SLFmtEmitSwitchClauseGroup(c, cur, &lastClause, &nextClause) != 0) {
                        return -1;
                    }
                    prevClause = lastClause;
                    cur = nextClause;
                }
                c->indent--;
                if (SLFmtNewline(c) != 0) {
                    return -1;
                }
            }
            return SLFmtWriteChar(c, '}');
        }
        case SLAst_RETURN:
            ch = SLFmtFirstChild(c->ast, nodeId);
            if (SLFmtWriteCStr(c, "return") != 0) {
                return -1;
            }
            if (ch >= 0) {
                if (SLFmtWriteChar(c, ' ') != 0) {
                    return -1;
                }
                if (c->ast->nodes[ch].kind == SLAst_EXPR_LIST) {
                    if (SLFmtEmitExprList(c, ch) != 0) {
                        return -1;
                    }
                } else if (SLFmtEmitExpr(c, ch, 0) != 0) {
                    return -1;
                }
            }
            return 0;
        case SLAst_BREAK:    return SLFmtWriteCStr(c, "break");
        case SLAst_CONTINUE: return SLFmtWriteCStr(c, "continue");
        case SLAst_DEFER:
            ch = SLFmtFirstChild(c->ast, nodeId);
            if (SLFmtWriteCStr(c, "defer ") != 0) {
                return -1;
            }
            if (ch >= 0 && c->ast->nodes[ch].kind == SLAst_BLOCK) {
                int32_t stmtNodeId;
                if (SLFmtCanInlineSingleStmtBlock(c, ch, &stmtNodeId)) {
                    return SLFmtEmitStmtInline(c, stmtNodeId);
                }
            }
            return ch >= 0 ? SLFmtEmitStmtInline(c, ch) : 0;
        case SLAst_ASSERT:
            ch = SLFmtFirstChild(c->ast, nodeId);
            if (SLFmtWriteCStr(c, "assert ") != 0) {
                return -1;
            }
            while (ch >= 0) {
                int32_t next = SLFmtNextSibling(c->ast, ch);
                if (SLFmtEmitExpr(c, ch, 0) != 0) {
                    return -1;
                }
                if (next >= 0 && SLFmtWriteCStr(c, ", ") != 0) {
                    return -1;
                }
                ch = next;
            }
            return 0;
        case SLAst_EXPR_STMT:
            ch = SLFmtFirstChild(c->ast, nodeId);
            return ch >= 0 ? SLFmtEmitExpr(c, ch, 0) : 0;
        default: return SLFmtWriteSlice(c, n->start, n->end);
    }
}

static int SLFmtEmitStmt(SLFmtCtx* c, int32_t nodeId) {
    if (SLFmtEmitLeadingCommentsForNode(c, nodeId) != 0) {
        return -1;
    }
    if (SLFmtEmitStmtInline(c, nodeId) != 0) {
        return -1;
    }
    return SLFmtEmitTrailingCommentsForNode(c, nodeId);
}

typedef struct {
    int32_t  nodeId;
    int32_t  aliasNodeId;
    int32_t  symStartNodeId;
    uint32_t pathLen;
    uint32_t aliasLen;
    uint32_t symbolsLen;
    uint32_t headLen;
    uint32_t codeLen;
    uint8_t  hasSymbols;
    uint8_t  hasTrailingComment;
    uint8_t  _pad[2];
} SLFmtImportRow;

static int SLFmtImportParseRow(SLFmtCtx* c, int32_t nodeId, SLFmtImportRow* outRow) {
    const SLAstNode* n;
    int32_t          child;
    int32_t          aliasNodeId = -1;
    int32_t          symStartNodeId = -1;
    uint32_t         symbolsLen = 0;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len || c->ast->nodes[nodeId].kind != SLAst_IMPORT)
    {
        return -1;
    }
    n = &c->ast->nodes[nodeId];
    child = SLFmtFirstChild(c->ast, nodeId);
    if (child >= 0 && c->ast->nodes[child].kind == SLAst_IDENT) {
        aliasNodeId = child;
        child = SLFmtNextSibling(c->ast, child);
    }
    symStartNodeId = child;

    if (symStartNodeId >= 0) {
        int32_t sym = symStartNodeId;
        symbolsLen = 4u;
        while (sym >= 0) {
            const SLAstNode* sn = &c->ast->nodes[sym];
            int32_t          salias = SLFmtFirstChild(c->ast, sym);
            int32_t          next = SLFmtNextSibling(c->ast, sym);
            symbolsLen += sn->dataEnd - sn->dataStart;
            if (salias >= 0) {
                const SLAstNode* an = &c->ast->nodes[salias];
                symbolsLen += 4u + (an->dataEnd - an->dataStart);
            }
            if (next >= 0) {
                symbolsLen += 2u;
            }
            sym = next;
        }
    }

    memset(outRow, 0, sizeof(*outRow));
    outRow->nodeId = nodeId;
    outRow->aliasNodeId = aliasNodeId;
    outRow->symStartNodeId = symStartNodeId;
    outRow->pathLen = n->dataEnd - n->dataStart;
    outRow->aliasLen = 0;
    if (aliasNodeId >= 0) {
        const SLAstNode* an = &c->ast->nodes[aliasNodeId];
        outRow->aliasLen = 4u + (an->dataEnd - an->dataStart);
    }
    outRow->symbolsLen = symbolsLen;
    outRow->hasSymbols = (uint8_t)(symStartNodeId >= 0);
    outRow->headLen = 7u + outRow->pathLen + outRow->aliasLen;
    outRow->hasTrailingComment = (uint8_t)SLFmtHasUnusedTrailingCommentsInNodeRange(c, nodeId);
    return 0;
}

static int SLFmtCompareNodePathText(SLFmtCtx* c, int32_t aNodeId, int32_t bNodeId) {
    const SLAstNode* a = &c->ast->nodes[aNodeId];
    const SLAstNode* b = &c->ast->nodes[bNodeId];
    uint32_t         ai = a->dataStart;
    uint32_t         bi = b->dataStart;
    uint32_t         aEnd = a->dataEnd;
    uint32_t         bEnd = b->dataEnd;
    while (ai < aEnd && bi < bEnd) {
        char ac = c->src.ptr[ai];
        char bc = c->src.ptr[bi];
        if (ac < bc) {
            return -1;
        }
        if (ac > bc) {
            return 1;
        }
        ai++;
        bi++;
    }
    if (ai == aEnd && bi == bEnd) {
        return 0;
    }
    return ai == aEnd ? -1 : 1;
}

static int SLFmtCompareImportRows(SLFmtCtx* c, const SLFmtImportRow* a, const SLFmtImportRow* b) {
    int cmp = SLFmtCompareNodePathText(c, a->nodeId, b->nodeId);
    if (cmp != 0) {
        return cmp;
    }
    if (a->aliasNodeId < 0 && b->aliasNodeId >= 0) {
        return -1;
    }
    if (a->aliasNodeId >= 0 && b->aliasNodeId < 0) {
        return 1;
    }
    if (a->aliasNodeId >= 0 && b->aliasNodeId >= 0) {
        cmp = SLFmtCompareNodePathText(c, a->aliasNodeId, b->aliasNodeId);
        if (cmp != 0) {
            return cmp;
        }
    }
    if (a->nodeId < b->nodeId) {
        return -1;
    }
    if (a->nodeId > b->nodeId) {
        return 1;
    }
    return 0;
}

static void SLFmtSortImportRows(SLFmtCtx* c, SLFmtImportRow* rows, uint32_t len) {
    uint32_t i;
    for (i = 1; i < len; i++) {
        SLFmtImportRow key = rows[i];
        uint32_t       j = i;
        while (j > 0 && SLFmtCompareImportRows(c, &key, &rows[j - 1u]) < 0) {
            rows[j] = rows[j - 1u];
            j--;
        }
        rows[j] = key;
    }
}

static int SLFmtEmitImportSymbolsInline(SLFmtCtx* c, int32_t symStartNodeId) {
    int32_t sym = symStartNodeId;
    if (SLFmtWriteCStr(c, "{ ") != 0) {
        return -1;
    }
    while (sym >= 0) {
        const SLAstNode* sn = &c->ast->nodes[sym];
        int32_t          salias = SLFmtFirstChild(c->ast, sym);
        int32_t          next = SLFmtNextSibling(c->ast, sym);
        if (SLFmtWriteSlice(c, sn->dataStart, sn->dataEnd) != 0) {
            return -1;
        }
        if (salias >= 0) {
            const SLAstNode* an = &c->ast->nodes[salias];
            if (SLFmtWriteCStr(c, " as ") != 0
                || SLFmtWriteSlice(c, an->dataStart, an->dataEnd) != 0)
            {
                return -1;
            }
        }
        if (next >= 0 && SLFmtWriteCStr(c, ", ") != 0) {
            return -1;
        }
        sym = next;
    }
    return SLFmtWriteCStr(c, " }");
}

static int SLFmtCanContinueImportGroup(SLFmtCtx* c, int32_t prevNodeId, int32_t nextNodeId) {
    uint32_t gapNl;
    if (prevNodeId < 0 || nextNodeId < 0 || c->ast->nodes[nextNodeId].kind != SLAst_IMPORT) {
        return 0;
    }
    gapNl = SLFmtCountNewlinesInRange(
        c->src, c->ast->nodes[prevNodeId].end, c->ast->nodes[nextNodeId].start);
    if (gapNl > 1u) {
        return 0;
    }
    if (SLFmtHasUnusedLeadingCommentsForNode(c, nextNodeId)) {
        return 0;
    }
    return 1;
}

static int SLFmtEmitImportGroup(
    SLFmtCtx* c, int32_t firstNodeId, int32_t* outLastSourceNodeId, int32_t* outNextNodeId) {
    int32_t         cur = firstNodeId;
    int32_t         prev = -1;
    uint32_t        count = 0;
    uint32_t        i;
    SLFmtImportRow* rows;
    uint32_t*       commentRunMaxLens;
    uint32_t*       braceRunMaxHeadLens;

    while (cur >= 0 && c->ast->nodes[cur].kind == SLAst_IMPORT) {
        int32_t next = SLFmtNextSibling(c->ast, cur);
        if (prev >= 0 && !SLFmtCanContinueImportGroup(c, prev, cur)) {
            break;
        }
        count++;
        prev = cur;
        cur = next;
    }

    rows = (SLFmtImportRow*)SLArenaAlloc(
        c->out.arena, count * (uint32_t)sizeof(SLFmtImportRow), (uint32_t)_Alignof(SLFmtImportRow));
    if (rows == NULL) {
        return -1;
    }
    cur = firstNodeId;
    for (i = 0; i < count; i++) {
        int32_t next = SLFmtNextSibling(c->ast, cur);
        if (SLFmtImportParseRow(c, cur, &rows[i]) != 0) {
            return -1;
        }
        cur = next;
    }

    SLFmtSortImportRows(c, rows, count);

    braceRunMaxHeadLens = (uint32_t*)SLArenaAlloc(
        c->out.arena, count * (uint32_t)sizeof(uint32_t), (uint32_t)_Alignof(uint32_t));
    if (braceRunMaxHeadLens == NULL) {
        return -1;
    }
    memset(braceRunMaxHeadLens, 0, count * (uint32_t)sizeof(uint32_t));

    commentRunMaxLens = (uint32_t*)SLArenaAlloc(
        c->out.arena, count * (uint32_t)sizeof(uint32_t), (uint32_t)_Alignof(uint32_t));
    if (commentRunMaxLens == NULL) {
        return -1;
    }
    memset(commentRunMaxLens, 0, count * (uint32_t)sizeof(uint32_t));

    for (i = 0; i < count;) {
        uint32_t j;
        uint32_t runMax = 0;
        if (!rows[i].hasSymbols) {
            i++;
            continue;
        }
        j = i;
        while (j < count && rows[j].hasSymbols) {
            if (rows[j].headLen > runMax) {
                runMax = rows[j].headLen;
            }
            j++;
        }
        while (i < j) {
            braceRunMaxHeadLens[i] = runMax;
            i++;
        }
    }

    for (i = 0; i < count; i++) {
        rows[i].codeLen = rows[i].headLen;
        if (rows[i].hasSymbols) {
            uint32_t padBeforeBrace = (braceRunMaxHeadLens[i] - rows[i].headLen) + 1u;
            rows[i].codeLen += padBeforeBrace + rows[i].symbolsLen;
        }
    }

    for (i = 0; i < count;) {
        uint32_t j;
        uint32_t runMax = 0;
        if (!rows[i].hasTrailingComment) {
            i++;
            continue;
        }
        j = i;
        while (j < count && rows[j].hasTrailingComment) {
            if (rows[j].codeLen > runMax) {
                runMax = rows[j].codeLen;
            }
            j++;
        }
        while (i < j) {
            commentRunMaxLens[i] = runMax;
            i++;
        }
    }

    for (i = 0; i < count; i++) {
        const SLAstNode* n = &c->ast->nodes[rows[i].nodeId];
        uint32_t         lineLen = rows[i].headLen;
        int32_t          nodeId = rows[i].nodeId;
        if (SLFmtEmitLeadingCommentsForNode(c, rows[i].nodeId) != 0) {
            return -1;
        }
        if (SLFmtWriteCStr(c, "import ") != 0 || SLFmtWriteSlice(c, n->dataStart, n->dataEnd) != 0)
        {
            return -1;
        }
        if (rows[i].aliasNodeId >= 0) {
            const SLAstNode* an = &c->ast->nodes[rows[i].aliasNodeId];
            if (SLFmtWriteCStr(c, " as ") != 0
                || SLFmtWriteSlice(c, an->dataStart, an->dataEnd) != 0)
            {
                return -1;
            }
        }
        if (rows[i].hasSymbols) {
            uint32_t padBeforeBrace = (braceRunMaxHeadLens[i] - rows[i].headLen) + 1u;
            if (SLFmtWriteSpaces(c, padBeforeBrace) != 0
                || SLFmtEmitImportSymbolsInline(c, rows[i].symStartNodeId) != 0)
            {
                return -1;
            }
            lineLen += padBeforeBrace + rows[i].symbolsLen;
        }
        if (rows[i].hasTrailingComment) {
            uint32_t padComment = (commentRunMaxLens[i] - lineLen) + 1u;
            if (SLFmtEmitTrailingCommentsInNodeRange(c, nodeId, padComment) != 0) {
                return -1;
            }
        }
        if (i + 1u < count && SLFmtNewline(c) != 0) {
            return -1;
        }
    }

    *outLastSourceNodeId = prev;
    *outNextNodeId = cur;
    return 0;
}

static int SLFmtEmitImport(SLFmtCtx* c, int32_t nodeId) {
    SLFmtImportRow   row;
    const SLAstNode* n;
    uint32_t         lineLen;
    int32_t          id = nodeId;
    if (SLFmtImportParseRow(c, nodeId, &row) != 0) {
        return -1;
    }
    n = &c->ast->nodes[nodeId];
    lineLen = row.headLen;
    if (SLFmtWriteCStr(c, "import ") != 0 || SLFmtWriteSlice(c, n->dataStart, n->dataEnd) != 0) {
        return -1;
    }
    if (row.aliasNodeId >= 0) {
        const SLAstNode* an = &c->ast->nodes[row.aliasNodeId];
        if (SLFmtWriteCStr(c, " as ") != 0 || SLFmtWriteSlice(c, an->dataStart, an->dataEnd) != 0) {
            return -1;
        }
    }
    if (row.hasSymbols) {
        if (SLFmtWriteChar(c, ' ') != 0 || SLFmtEmitImportSymbolsInline(c, row.symStartNodeId) != 0)
        {
            return -1;
        }
        lineLen += 1u + row.symbolsLen;
    }
    if (row.hasTrailingComment) {
        if (SLFmtEmitTrailingCommentsInNodeRange(c, id, 1u) != 0) {
            return -1;
        }
    }
    (void)lineLen;
    return 0;
}

typedef struct {
    int32_t  firstNodeId;
    int32_t  lastNodeId;
    int32_t  typeNodeId;
    int32_t  defaultNodeId;
    uint32_t nameLen;
    uint32_t typeLen;
    uint32_t defaultLen;
    uint32_t codeLen;
    uint8_t  hasDefault;
    uint8_t  hasTrailingComment;
    uint8_t  noTypeAlign;
    uint8_t  _pad;
} SLFmtAlignedFieldRow;

typedef struct {
    int32_t  nodeId;
    int32_t  valueNodeId;
    uint32_t nameLen;
    uint32_t valueLen;
    uint32_t codeLen;
    uint8_t  hasValue;
    uint8_t  hasTrailingComment;
    uint8_t  _pad[2];
} SLFmtAlignedEnumRow;

static int SLFmtFieldTypesMatch(SLFmtCtx* c, int32_t aTypeNodeId, int32_t bTypeNodeId) {
    const SLAstNode* a;
    const SLAstNode* b;
    uint32_t         aLen;
    if (aTypeNodeId < 0 || bTypeNodeId < 0 || (uint32_t)aTypeNodeId >= c->ast->len
        || (uint32_t)bTypeNodeId >= c->ast->len)
    {
        return 0;
    }
    a = &c->ast->nodes[aTypeNodeId];
    b = &c->ast->nodes[bTypeNodeId];
    aLen = a->end - a->start;
    if (aLen != (b->end - b->start)) {
        return 0;
    }
    if (aLen == 0) {
        return 1;
    }
    return memcmp(c->src.ptr + a->start, c->src.ptr + b->start, aLen) == 0;
}

static int SLFmtIsAnonAggregateTypeNode(SLFmtCtx* c, int32_t typeNodeId) {
    SLAstKind kind;
    if (typeNodeId < 0 || (uint32_t)typeNodeId >= c->ast->len) {
        return 0;
    }
    kind = c->ast->nodes[typeNodeId].kind;
    return kind == SLAst_TYPE_ANON_STRUCT || kind == SLAst_TYPE_ANON_UNION;
}

static int SLFmtCanMergeFieldNames(SLFmtCtx* c, int32_t leftFieldNodeId, int32_t rightFieldNodeId) {
    const SLAstNode* left = &c->ast->nodes[leftFieldNodeId];
    const SLAstNode* right = &c->ast->nodes[rightFieldNodeId];
    uint32_t         i;
    int              sawComma = 0;
    if (right->dataStart < left->dataEnd || right->dataStart > c->src.len) {
        return 0;
    }
    for (i = left->dataEnd; i < right->dataStart; i++) {
        char ch = c->src.ptr[i];
        if (ch == ',') {
            if (sawComma) {
                return 0;
            }
            sawComma = 1;
            continue;
        }
        if (ch == ' ' || ch == '\t' || ch == '\r') {
            continue;
        }
        return 0;
    }
    return sawComma;
}

static uint32_t SLFmtMergedFieldNameLen(
    SLFmtCtx* c, int32_t firstFieldNodeId, int32_t lastFieldNodeId) {
    uint32_t len = 0;
    int32_t  cur = firstFieldNodeId;
    while (cur >= 0) {
        const SLAstNode* n = &c->ast->nodes[cur];
        int32_t          next = SLFmtNextSibling(c->ast, cur);
        len += n->dataEnd - n->dataStart;
        if (cur != lastFieldNodeId) {
            len += 2u;
        } else {
            break;
        }
        cur = next;
    }
    return len;
}

static int SLFmtEmitMergedFieldNames(
    SLFmtCtx* c, int32_t firstFieldNodeId, int32_t lastFieldNodeId) {
    int32_t cur = firstFieldNodeId;
    while (cur >= 0) {
        const SLAstNode* n = &c->ast->nodes[cur];
        int32_t          next = SLFmtNextSibling(c->ast, cur);
        if (SLFmtWriteSlice(c, n->dataStart, n->dataEnd) != 0) {
            return -1;
        }
        if (cur == lastFieldNodeId) {
            break;
        }
        if (SLFmtWriteCStr(c, ", ") != 0) {
            return -1;
        }
        cur = next;
    }
    return 0;
}

static int SLFmtBuildFieldRow(
    SLFmtCtx* c, int32_t startFieldNodeId, SLFmtAlignedFieldRow* outRow, int32_t* outNextNodeId) {
    int32_t          typeNodeId;
    int32_t          defaultNodeId;
    const SLAstNode* startNode;
    int32_t          lastNodeId;

    if (startFieldNodeId < 0 || (uint32_t)startFieldNodeId >= c->ast->len) {
        return -1;
    }
    startNode = &c->ast->nodes[startFieldNodeId];
    if (startNode->kind != SLAst_FIELD) {
        return -1;
    }

    typeNodeId = SLFmtFirstChild(c->ast, startFieldNodeId);
    defaultNodeId = typeNodeId >= 0 ? SLFmtNextSibling(c->ast, typeNodeId) : -1;
    lastNodeId = startFieldNodeId;

    if ((startNode->flags & SLAstFlag_FIELD_EMBEDDED) == 0 && defaultNodeId < 0) {
        int32_t next = SLFmtNextSibling(c->ast, lastNodeId);
        while (next >= 0 && c->ast->nodes[next].kind == SLAst_FIELD) {
            const SLAstNode* nn = &c->ast->nodes[next];
            int32_t          nextType = SLFmtFirstChild(c->ast, next);
            int32_t          nextDefault = nextType >= 0 ? SLFmtNextSibling(c->ast, nextType) : -1;
            if ((nn->flags & SLAstFlag_FIELD_EMBEDDED) != 0 || nextDefault >= 0) {
                break;
            }
            if (!SLFmtFieldTypesMatch(c, typeNodeId, nextType)) {
                break;
            }
            if (!SLFmtCanMergeFieldNames(c, lastNodeId, next)) {
                break;
            }
            lastNodeId = next;
            next = SLFmtNextSibling(c->ast, next);
        }
    }

    memset(outRow, 0, sizeof(*outRow));
    outRow->firstNodeId = startFieldNodeId;
    outRow->lastNodeId = lastNodeId;
    outRow->typeNodeId = typeNodeId;
    outRow->defaultNodeId = defaultNodeId;
    outRow->hasDefault = (uint8_t)(defaultNodeId >= 0);
    outRow->nameLen = SLFmtMergedFieldNameLen(c, startFieldNodeId, lastNodeId);
    if (typeNodeId >= 0 && SLFmtMeasureTypeLen(c, typeNodeId, &outRow->typeLen) != 0) {
        return -1;
    }
    if (defaultNodeId >= 0 && SLFmtMeasureExprLen(c, defaultNodeId, 0, &outRow->defaultLen) != 0) {
        return -1;
    }
    outRow->hasTrailingComment = (uint8_t)SLFmtHasUnusedTrailingCommentsForNodes(
        c, &startFieldNodeId, 1u);
    *outNextNodeId = SLFmtNextSibling(c->ast, lastNodeId);
    return 0;
}

static int SLFmtEmitSimpleFieldDecl(SLFmtCtx* c, int32_t fieldNodeId) {
    const SLAstNode* fn = &c->ast->nodes[fieldNodeId];
    int32_t          typeNode = SLFmtFirstChild(c->ast, fieldNodeId);
    int32_t          defaultNode = typeNode >= 0 ? SLFmtNextSibling(c->ast, typeNode) : -1;
    int32_t          nodeIds[1];
    int              hasTrailing;
    if ((fn->flags & SLAstFlag_FIELD_EMBEDDED) != 0) {
        if (typeNode >= 0 && SLFmtEmitType(c, typeNode) != 0) {
            return -1;
        }
    } else if (
        SLFmtWriteSlice(c, fn->dataStart, fn->dataEnd) != 0 || SLFmtWriteChar(c, ' ') != 0
        || (typeNode >= 0 && SLFmtEmitType(c, typeNode) != 0))
    {
        return -1;
    }
    if (defaultNode >= 0) {
        if (SLFmtWriteCStr(c, " = ") != 0 || SLFmtEmitExpr(c, defaultNode, 0) != 0) {
            return -1;
        }
    }
    nodeIds[0] = fieldNodeId;
    hasTrailing = SLFmtHasUnusedTrailingCommentsForNodes(c, nodeIds, 1u);
    if (hasTrailing) {
        return SLFmtEmitTrailingCommentsForNodes(c, nodeIds, 1u, 1u);
    }
    {
        uint32_t cmStart;
        uint32_t cmEnd;
        if (SLFmtFindSourceTrailingLineComment(c, fieldNodeId, &cmStart, &cmEnd)) {
            if (SLFmtWriteChar(c, ' ') != 0 || SLFmtWriteSlice(c, cmStart, cmEnd) != 0) {
                return -1;
            }
            SLFmtMarkCommentUsedAtStart(c, cmStart);
        }
    }
    return 0;
}

static int SLFmtEmitAlignedFieldGroup(
    SLFmtCtx* c, int32_t firstFieldNodeId, int32_t* outLastNodeId, int32_t* outNextNodeId) {
    int32_t               cur = firstFieldNodeId;
    int32_t               prev = -1;
    int                   prevInlineAnon = 0;
    uint32_t              count = 0;
    uint32_t              i;
    uint32_t              maxNameLen = 0;
    uint32_t              maxBeforeOpLen = 0;
    SLFmtAlignedFieldRow* rows;
    uint32_t*             commentRunMaxLens;

    while (cur >= 0) {
        SLFmtAlignedFieldRow row;
        int32_t              next;
        int                  curInlineAnon;
        if (c->ast->nodes[cur].kind != SLAst_FIELD) {
            break;
        }
        if ((c->ast->nodes[cur].flags & SLAstFlag_FIELD_EMBEDDED) != 0) {
            break;
        }
        if (SLFmtBuildFieldRow(c, cur, &row, &next) != 0) {
            return -1;
        }
        curInlineAnon = SLFmtIsInlineAnonAggregateType(c, row.typeNodeId);
        if (prev >= 0) {
            if (!SLFmtCanContinueAlignedGroup(c, prev, row.firstNodeId)) {
                break;
            }
            if (prevInlineAnon != curInlineAnon) {
                break;
            }
        }
        count++;
        prev = row.lastNodeId;
        prevInlineAnon = curInlineAnon;
        cur = next;
    }

    if (count == 0) {
        *outLastNodeId = firstFieldNodeId;
        *outNextNodeId = SLFmtNextSibling(c->ast, firstFieldNodeId);
        return 0;
    }

    rows = (SLFmtAlignedFieldRow*)SLArenaAlloc(
        c->out.arena,
        count * (uint32_t)sizeof(SLFmtAlignedFieldRow),
        (uint32_t)_Alignof(SLFmtAlignedFieldRow));
    if (rows == NULL) {
        return -1;
    }

    cur = firstFieldNodeId;
    for (i = 0; i < count; i++) {
        if (SLFmtBuildFieldRow(c, cur, &rows[i], &cur) != 0) {
            return -1;
        }
        if (rows[i].nameLen > maxNameLen) {
            maxNameLen = rows[i].nameLen;
        }
    }

    for (i = 0; i < count; i++) {
        int isAnon = SLFmtIsAnonAggregateTypeNode(c, rows[i].typeNodeId);
        int prevAnon = i > 0u && SLFmtIsAnonAggregateTypeNode(c, rows[i - 1u].typeNodeId);
        int nextAnon = i + 1u < count && SLFmtIsAnonAggregateTypeNode(c, rows[i + 1u].typeNodeId);
        rows[i].noTypeAlign = (uint8_t)(isAnon && (prevAnon || nextAnon));
    }

    for (i = 0; i < count; i++) {
        if (rows[i].hasDefault) {
            uint32_t beforeOpLen;
            if (rows[i].noTypeAlign) {
                continue;
            }
            beforeOpLen = maxNameLen + 1u + rows[i].typeLen;
            if (beforeOpLen > maxBeforeOpLen) {
                maxBeforeOpLen = beforeOpLen;
            }
        }
    }

    for (i = 0; i < count; i++) {
        uint32_t codeLen =
            rows[i].noTypeAlign
                ? (rows[i].nameLen + 1u + rows[i].typeLen)
                : (maxNameLen + 1u + rows[i].typeLen);
        if (rows[i].hasDefault) {
            if (rows[i].noTypeAlign) {
                codeLen += 3u + rows[i].defaultLen;
            } else {
                uint32_t padBeforeOp = (maxBeforeOpLen + 1u) - codeLen;
                codeLen += padBeforeOp + 1u + 1u + rows[i].defaultLen;
            }
        }
        rows[i].codeLen = codeLen;
    }

    commentRunMaxLens = (uint32_t*)SLArenaAlloc(
        c->out.arena, count * (uint32_t)sizeof(uint32_t), (uint32_t)_Alignof(uint32_t));
    if (commentRunMaxLens == NULL) {
        return -1;
    }
    memset(commentRunMaxLens, 0, count * (uint32_t)sizeof(uint32_t));

    for (i = 0; i < count;) {
        uint32_t j;
        uint32_t runMax = 0;
        if (!rows[i].hasTrailingComment) {
            i++;
            continue;
        }
        j = i;
        while (j < count && rows[j].hasTrailingComment) {
            if (rows[j].codeLen > runMax) {
                runMax = rows[j].codeLen;
            }
            j++;
        }
        while (i < j) {
            commentRunMaxLens[i] = runMax;
            i++;
        }
    }

    for (i = 0; i < count; i++) {
        uint32_t lineLen;
        int32_t  nodeIds[1];
        if (SLFmtEmitLeadingCommentsForNode(c, rows[i].firstNodeId) != 0) {
            return -1;
        }
        if (SLFmtEmitMergedFieldNames(c, rows[i].firstNodeId, rows[i].lastNodeId) != 0) {
            return -1;
        }
        lineLen = rows[i].nameLen;
        if (rows[i].noTypeAlign) {
            if (SLFmtWriteChar(c, ' ') != 0) {
                return -1;
            }
        } else {
            if (SLFmtWriteSpaces(c, maxNameLen - rows[i].nameLen + 1u) != 0) {
                return -1;
            }
            lineLen = maxNameLen + 1u;
        }
        if (rows[i].typeNodeId >= 0 && SLFmtEmitType(c, rows[i].typeNodeId) != 0) {
            return -1;
        }
        lineLen += rows[i].typeLen;
        if (rows[i].hasDefault) {
            if (rows[i].noTypeAlign) {
                if (SLFmtWriteCStr(c, " = ") != 0
                    || SLFmtEmitExpr(c, rows[i].defaultNodeId, 0) != 0)
                {
                    return -1;
                }
                lineLen += 3u + rows[i].defaultLen;
            } else {
                uint32_t padBeforeOp = (maxBeforeOpLen + 1u) - lineLen;
                if (SLFmtWriteSpaces(c, padBeforeOp) != 0 || SLFmtWriteChar(c, '=') != 0
                    || SLFmtWriteChar(c, ' ') != 0
                    || SLFmtEmitExpr(c, rows[i].defaultNodeId, 0) != 0)
                {
                    return -1;
                }
                lineLen += padBeforeOp + 1u + 1u + rows[i].defaultLen;
            }
        }
        if (rows[i].hasTrailingComment) {
            uint32_t padComment = (commentRunMaxLens[i] - lineLen) + 1u;
            nodeIds[0] = rows[i].firstNodeId;
            if (SLFmtEmitTrailingCommentsForNodes(c, nodeIds, 1u, padComment) != 0) {
                return -1;
            }
        } else {
            uint32_t cmStart;
            uint32_t cmEnd;
            if (SLFmtFindSourceTrailingLineComment(c, rows[i].lastNodeId, &cmStart, &cmEnd)) {
                if (SLFmtWriteChar(c, ' ') != 0 || SLFmtWriteSlice(c, cmStart, cmEnd) != 0) {
                    return -1;
                }
                SLFmtMarkCommentUsedAtStart(c, cmStart);
            }
        }
        if (i + 1u < count && SLFmtNewline(c) != 0) {
            return -1;
        }
    }

    *outLastNodeId = rows[count - 1u].lastNodeId;
    *outNextNodeId = cur;
    return 0;
}

static int SLFmtEmitAlignedEnumGroup(
    SLFmtCtx* c, int32_t firstFieldNodeId, int32_t* outLastNodeId, int32_t* outNextNodeId) {
    int32_t              cur = firstFieldNodeId;
    int32_t              prev = -1;
    uint32_t             count = 0;
    uint32_t             i;
    uint32_t             maxNameLenForValues = 0;
    SLFmtAlignedEnumRow* rows;
    uint32_t*            commentRunMaxLens;

    while (cur >= 0) {
        int32_t next = SLFmtNextSibling(c->ast, cur);
        if (c->ast->nodes[cur].kind != SLAst_FIELD) {
            break;
        }
        if (prev >= 0 && !SLFmtCanContinueAlignedGroup(c, prev, cur)) {
            break;
        }
        count++;
        prev = cur;
        cur = next;
    }

    rows = (SLFmtAlignedEnumRow*)SLArenaAlloc(
        c->out.arena,
        count * (uint32_t)sizeof(SLFmtAlignedEnumRow),
        (uint32_t)_Alignof(SLFmtAlignedEnumRow));
    if (rows == NULL) {
        return -1;
    }
    memset(rows, 0, count * (uint32_t)sizeof(SLFmtAlignedEnumRow));

    cur = firstFieldNodeId;
    for (i = 0; i < count; i++) {
        const SLAstNode* n = &c->ast->nodes[cur];
        int32_t          valueNode = SLFmtFirstChild(c->ast, cur);
        rows[i].nodeId = cur;
        rows[i].nameLen = n->dataEnd - n->dataStart;
        while (valueNode >= 0 && c->ast->nodes[valueNode].kind == SLAst_FIELD) {
            valueNode = SLFmtNextSibling(c->ast, valueNode);
        }
        rows[i].valueNodeId = valueNode;
        rows[i].hasValue = (uint8_t)(rows[i].valueNodeId >= 0);
        if (rows[i].hasValue
            && SLFmtMeasureExprLen(c, rows[i].valueNodeId, 0, &rows[i].valueLen) != 0)
        {
            return -1;
        }
        rows[i].hasTrailingComment = (uint8_t)SLFmtHasUnusedTrailingCommentsForNodes(c, &cur, 1u);
        if (rows[i].hasValue && rows[i].nameLen > maxNameLenForValues) {
            maxNameLenForValues = rows[i].nameLen;
        }
        cur = SLFmtNextSibling(c->ast, cur);
    }

    commentRunMaxLens = (uint32_t*)SLArenaAlloc(
        c->out.arena, count * (uint32_t)sizeof(uint32_t), (uint32_t)_Alignof(uint32_t));
    if (commentRunMaxLens == NULL) {
        return -1;
    }
    memset(commentRunMaxLens, 0, count * (uint32_t)sizeof(uint32_t));

    for (i = 0; i < count; i++) {
        uint32_t codeLen = rows[i].nameLen;
        if (rows[i].hasValue) {
            uint32_t padBeforeOp = (maxNameLenForValues + 1u) - rows[i].nameLen;
            codeLen += padBeforeOp + 1u + 1u + rows[i].valueLen;
        }
        rows[i].codeLen = codeLen;
    }

    for (i = 0; i < count;) {
        uint32_t j;
        uint32_t runMax = 0;
        if (!rows[i].hasTrailingComment) {
            i++;
            continue;
        }
        j = i;
        while (j < count && rows[j].hasTrailingComment) {
            if (rows[j].codeLen > runMax) {
                runMax = rows[j].codeLen;
            }
            j++;
        }
        while (i < j) {
            commentRunMaxLens[i] = runMax;
            i++;
        }
    }

    for (i = 0; i < count; i++) {
        uint32_t         lineLen = rows[i].nameLen;
        int32_t          nodeIds[1];
        const SLAstNode* n = &c->ast->nodes[rows[i].nodeId];
        if (SLFmtEmitLeadingCommentsForNode(c, rows[i].nodeId) != 0) {
            return -1;
        }
        if (SLFmtWriteSlice(c, n->dataStart, n->dataEnd) != 0) {
            return -1;
        }
        if (rows[i].hasValue) {
            uint32_t padBeforeOp = (maxNameLenForValues + 1u) - rows[i].nameLen;
            if (SLFmtWriteSpaces(c, padBeforeOp) != 0 || SLFmtWriteChar(c, '=') != 0
                || SLFmtWriteChar(c, ' ') != 0 || SLFmtEmitExpr(c, rows[i].valueNodeId, 0) != 0)
            {
                return -1;
            }
            lineLen += padBeforeOp + 1u + 1u + rows[i].valueLen;
        }
        if (rows[i].hasTrailingComment) {
            uint32_t padComment = (commentRunMaxLens[i] - lineLen) + 1u;
            nodeIds[0] = rows[i].nodeId;
            if (SLFmtEmitTrailingCommentsForNodes(c, nodeIds, 1u, padComment) != 0) {
                return -1;
            }
        }
        if (i + 1u < count && SLFmtNewline(c) != 0) {
            return -1;
        }
    }

    *outLastNodeId = rows[count - 1u].nodeId;
    *outNextNodeId = cur;
    return 0;
}

static int SLFmtIsNestedTypeDeclNodeKind(SLAstKind kind) {
    return kind == SLAst_STRUCT || kind == SLAst_UNION || kind == SLAst_ENUM
        || kind == SLAst_TYPE_ALIAS;
}

static int SLFmtEmitAggregateFieldBody(SLFmtCtx* c, int32_t firstFieldNodeId) {
    int32_t child = firstFieldNodeId;
    int32_t prevEmitted = -1;

    if (child < 0) {
        return 0;
    }
    if (SLFmtNewline(c) != 0) {
        return -1;
    }
    c->indent++;
    while (child >= 0) {
        SLAstKind kind = c->ast->nodes[child].kind;
        if (kind != SLAst_FIELD && !SLFmtIsNestedTypeDeclNodeKind(kind)) {
            break;
        }
        if (prevEmitted >= 0) {
            if (SLFmtNewline(c) != 0) {
                return -1;
            }
            if (SLFmtNeedsBlankLineBeforeNode(c, prevEmitted, child) && SLFmtNewline(c) != 0) {
                return -1;
            }
        }
        if (kind == SLAst_FIELD) {
            int32_t next = SLFmtNextSibling(c->ast, child);
            int32_t last = child;
            if ((c->ast->nodes[child].flags & SLAstFlag_FIELD_EMBEDDED) != 0) {
                if (SLFmtEmitLeadingCommentsForNode(c, child) != 0
                    || SLFmtEmitSimpleFieldDecl(c, child) != 0)
                {
                    return -1;
                }
            } else if (SLFmtEmitAlignedFieldGroup(c, child, &last, &next) != 0) {
                return -1;
            }
            prevEmitted = last;
            child = next;
            continue;
        }
        if (SLFmtEmitDecl(c, child) != 0) {
            return -1;
        }
        prevEmitted = child;
        child = SLFmtNextSibling(c->ast, child);
    }
    c->indent--;
    if (SLFmtNewline(c) != 0) {
        return -1;
    }
    return 0;
}

static int SLFmtEmitAggregateDecl(SLFmtCtx* c, int32_t nodeId, const char* kw) {
    const SLAstNode* n = &c->ast->nodes[nodeId];
    int32_t          child = SLFmtFirstChild(c->ast, nodeId);
    int32_t          underType = -1;
    int32_t          prevEmitted = -1;
    while (child >= 0 && c->ast->nodes[child].kind == SLAst_TYPE_PARAM) {
        child = SLFmtNextSibling(c->ast, child);
    }
    if (n->kind == SLAst_ENUM && child >= 0 && SLFmtIsTypeNodeKind(c->ast->nodes[child].kind)) {
        underType = child;
        child = SLFmtNextSibling(c->ast, child);
    }

    if ((n->flags & SLAstFlag_PUB) != 0 && SLFmtWriteCStr(c, "pub ") != 0) {
        return -1;
    }
    if (SLFmtWriteCStr(c, kw) != 0 || SLFmtWriteChar(c, ' ') != 0
        || SLFmtWriteSlice(c, n->dataStart, n->dataEnd) != 0)
    {
        return -1;
    }
    child = SLFmtFirstChild(c->ast, nodeId);
    if (SLFmtEmitTypeParamList(c, &child) != 0) {
        return -1;
    }
    if (underType >= 0) {
        if (SLFmtWriteChar(c, ' ') != 0 || SLFmtEmitType(c, underType) != 0) {
            return -1;
        }
        child = SLFmtNextSibling(c->ast, underType);
    }
    if (SLFmtWriteChar(c, ' ') != 0 || SLFmtWriteChar(c, '{') != 0) {
        return -1;
    }

    if (child >= 0) {
        if (n->kind == SLAst_ENUM) {
            int     enumHasPayload = 0;
            int32_t scanItem = child;
            while (scanItem >= 0) {
                int32_t vch = SLFmtFirstChild(c->ast, scanItem);
                while (vch >= 0) {
                    if (c->ast->nodes[vch].kind == SLAst_FIELD) {
                        enumHasPayload = 1;
                        break;
                    }
                    vch = SLFmtNextSibling(c->ast, vch);
                }
                if (enumHasPayload) {
                    break;
                }
                scanItem = SLFmtNextSibling(c->ast, scanItem);
            }
            if (SLFmtNewline(c) != 0) {
                return -1;
            }
            c->indent++;
            if (!enumHasPayload) {
                while (child >= 0) {
                    int32_t next = SLFmtNextSibling(c->ast, child);
                    int32_t last = child;
                    if (prevEmitted >= 0) {
                        if (SLFmtNewline(c) != 0) {
                            return -1;
                        }
                        if (SLFmtNeedsBlankLineBeforeNode(c, prevEmitted, child)
                            && SLFmtNewline(c) != 0)
                        {
                            return -1;
                        }
                    }
                    if (SLFmtEmitAlignedEnumGroup(c, child, &last, &next) != 0) {
                        return -1;
                    }
                    prevEmitted = last;
                    child = next;
                }
            } else {
                while (child >= 0) {
                    int32_t          next = SLFmtNextSibling(c->ast, child);
                    const SLAstNode* item = &c->ast->nodes[child];
                    int32_t          payloadFirst = -1;
                    int32_t          tagExpr = -1;
                    int32_t          ch = SLFmtFirstChild(c->ast, child);
                    if (prevEmitted >= 0) {
                        if (SLFmtNewline(c) != 0) {
                            return -1;
                        }
                        if (SLFmtNeedsBlankLineBeforeNode(c, prevEmitted, child)
                            && SLFmtNewline(c) != 0)
                        {
                            return -1;
                        }
                    }
                    if (SLFmtEmitLeadingCommentsForNode(c, child) != 0
                        || SLFmtWriteSlice(c, item->dataStart, item->dataEnd) != 0)
                    {
                        return -1;
                    }
                    while (ch >= 0) {
                        if (c->ast->nodes[ch].kind == SLAst_FIELD) {
                            if (payloadFirst < 0) {
                                payloadFirst = ch;
                            }
                        } else if (tagExpr < 0) {
                            tagExpr = ch;
                        }
                        ch = SLFmtNextSibling(c->ast, ch);
                    }
                    if (payloadFirst >= 0) {
                        if (SLFmtWriteChar(c, '{') != 0
                            || SLFmtEmitAggregateFieldBody(c, payloadFirst) != 0
                            || SLFmtWriteChar(c, '}') != 0)
                        {
                            return -1;
                        }
                    }
                    if (tagExpr >= 0) {
                        if (SLFmtWriteCStr(c, " = ") != 0 || SLFmtEmitExpr(c, tagExpr, 0) != 0) {
                            return -1;
                        }
                    }
                    if (SLFmtEmitTrailingCommentsForNode(c, child) != 0) {
                        return -1;
                    }
                    prevEmitted = child;
                    child = next;
                }
            }
            c->indent--;
            if (SLFmtNewline(c) != 0) {
                return -1;
            }
        } else if (SLFmtEmitAggregateFieldBody(c, child) != 0) {
            return -1;
        }
    }

    if (SLFmtWriteChar(c, '}') != 0) {
        return -1;
    }
    return SLFmtEmitTrailingCommentsForNode(c, nodeId);
}

static int SLFmtEmitFnDecl(SLFmtCtx* c, int32_t nodeId) {
    const SLAstNode* n = &c->ast->nodes[nodeId];
    int32_t          child;
    int              firstParam = 1;
    int32_t          retType = -1;
    int32_t          ctxClause = -1;
    int32_t          body = -1;

    if ((n->flags & SLAstFlag_PUB) != 0 && SLFmtWriteCStr(c, "pub ") != 0) {
        return -1;
    }
    if (SLFmtWriteCStr(c, "fn ") != 0 || SLFmtWriteSlice(c, n->dataStart, n->dataEnd) != 0) {
        return -1;
    }

    child = SLFmtFirstChild(c->ast, nodeId);
    if (SLFmtEmitTypeParamList(c, &child) != 0) {
        return -1;
    }
    if (SLFmtWriteChar(c, '(') != 0) {
        return -1;
    }
    while (child >= 0) {
        const SLAstNode* ch = &c->ast->nodes[child];
        int32_t          runFirst = child;
        int32_t          runLast = child;
        int32_t          runType = -1;
        uint16_t         runFlags;
        int32_t          nextAfterRun;
        int              firstNameInRun = 1;
        if (ch->kind != SLAst_PARAM) {
            break;
        }
        runType = SLFmtFirstChild(c->ast, child);
        runFlags = (uint16_t)(ch->flags & (SLAstFlag_PARAM_CONST | SLAstFlag_PARAM_VARIADIC));
        nextAfterRun = SLFmtNextSibling(c->ast, child);
        while (nextAfterRun >= 0) {
            const SLAstNode* nextParam = &c->ast->nodes[nextAfterRun];
            int32_t          nextType;
            uint16_t         nextFlags;
            if (nextParam->kind != SLAst_PARAM) {
                break;
            }
            nextType = SLFmtFirstChild(c->ast, nextAfterRun);
            nextFlags =
                (uint16_t)(nextParam->flags & (SLAstFlag_PARAM_CONST | SLAstFlag_PARAM_VARIADIC));
            if (nextFlags != runFlags
                || !SLFmtTypeNodesEqualBySource(c->ast, c->src, runType, nextType))
            {
                break;
            }
            runLast = nextAfterRun;
            nextAfterRun = SLFmtNextSibling(c->ast, nextAfterRun);
        }
        if (!firstParam && SLFmtWriteCStr(c, ", ") != 0) {
            return -1;
        }
        if ((runFlags & SLAstFlag_PARAM_CONST) != 0 && SLFmtWriteCStr(c, "const ") != 0) {
            return -1;
        }
        while (runFirst >= 0) {
            const SLAstNode* p = &c->ast->nodes[runFirst];
            if (!firstNameInRun && SLFmtWriteCStr(c, ", ") != 0) {
                return -1;
            }
            if (SLFmtWriteSlice(c, p->dataStart, p->dataEnd) != 0) {
                return -1;
            }
            firstNameInRun = 0;
            if (runFirst == runLast) {
                break;
            }
            runFirst = SLFmtNextSibling(c->ast, runFirst);
        }
        if (SLFmtWriteChar(c, ' ') != 0) {
            return -1;
        }
        if ((runFlags & SLAstFlag_PARAM_VARIADIC) != 0 && SLFmtWriteCStr(c, "...") != 0) {
            return -1;
        }
        if (runType >= 0 && SLFmtEmitType(c, runType) != 0) {
            return -1;
        }
        firstParam = 0;
        child = nextAfterRun;
    }

    if (SLFmtWriteChar(c, ')') != 0) {
        return -1;
    }

    while (child >= 0) {
        const SLAstNode* ch = &c->ast->nodes[child];
        if (ch->kind == SLAst_CONTEXT_CLAUSE) {
            ctxClause = child;
        } else if (ch->kind == SLAst_BLOCK) {
            body = child;
        } else if (SLFmtIsTypeNodeKind(ch->kind) && ch->flags == 1) {
            retType = child;
        }
        child = SLFmtNextSibling(c->ast, child);
    }

    if (retType >= 0) {
        if (SLFmtWriteChar(c, ' ') != 0 || SLFmtEmitType(c, retType) != 0) {
            return -1;
        }
    }
    if (ctxClause >= 0) {
        int32_t ctype = SLFmtFirstChild(c->ast, ctxClause);
        if (SLFmtWriteCStr(c, " context ") != 0) {
            return -1;
        }
        if (ctype >= 0 && SLFmtEmitType(c, ctype) != 0) {
            return -1;
        }
    }
    if (body >= 0) {
        if (SLFmtWriteChar(c, ' ') != 0 || SLFmtEmitBlock(c, body) != 0) {
            return -1;
        }
    }
    return SLFmtEmitTrailingCommentsForNode(c, nodeId);
}

static int SLFmtEmitDirective(SLFmtCtx* c, int32_t nodeId) {
    const SLAstNode* n = &c->ast->nodes[nodeId];
    int32_t          child = SLFmtFirstChild(c->ast, nodeId);
    int              first = 1;
    if (SLFmtWriteChar(c, '@') != 0 || SLFmtWriteSlice(c, n->dataStart, n->dataEnd) != 0) {
        return -1;
    }
    if (child >= 0 || SLFmtSliceHasChar(c->src, n->dataEnd, n->end, '(')) {
        if (SLFmtWriteChar(c, '(') != 0) {
            return -1;
        }
        while (child >= 0) {
            int32_t next = SLFmtNextSibling(c->ast, child);
            if (!first && SLFmtWriteCStr(c, ", ") != 0) {
                return -1;
            }
            if (SLFmtEmitExpr(c, child, 0) != 0) {
                return -1;
            }
            first = 0;
            child = next;
        }
        if (SLFmtWriteChar(c, ')') != 0) {
            return -1;
        }
    }
    return SLFmtEmitTrailingCommentsForNode(c, nodeId);
}

static int SLFmtEmitDecl(SLFmtCtx* c, int32_t nodeId) {
    const SLAstNode* n = &c->ast->nodes[nodeId];
    if (SLFmtEmitLeadingCommentsForNode(c, nodeId) != 0) {
        return -1;
    }
    switch (n->kind) {
        case SLAst_IMPORT:     return SLFmtEmitImport(c, nodeId);
        case SLAst_DIRECTIVE:  return SLFmtEmitDirective(c, nodeId);
        case SLAst_STRUCT:     return SLFmtEmitAggregateDecl(c, nodeId, "struct");
        case SLAst_UNION:      return SLFmtEmitAggregateDecl(c, nodeId, "union");
        case SLAst_ENUM:       return SLFmtEmitAggregateDecl(c, nodeId, "enum");
        case SLAst_TYPE_ALIAS: {
            int32_t type = SLFmtFirstChild(c->ast, nodeId);
            if ((n->flags & SLAstFlag_PUB) != 0 && SLFmtWriteCStr(c, "pub ") != 0) {
                return -1;
            }
            if (SLFmtWriteCStr(c, "type ") != 0
                || SLFmtWriteSlice(c, n->dataStart, n->dataEnd) != 0)
            {
                return -1;
            }
            if (SLFmtEmitTypeParamList(c, &type) != 0) {
                return -1;
            }
            if (SLFmtWriteChar(c, ' ') != 0 || (type >= 0 && SLFmtEmitType(c, type) != 0)) {
                return -1;
            }
            return SLFmtEmitTrailingCommentsForNode(c, nodeId);
        }
        case SLAst_FN: return SLFmtEmitFnDecl(c, nodeId);
        case SLAst_VAR:
            if ((n->flags & SLAstFlag_PUB) != 0 && SLFmtWriteCStr(c, "pub ") != 0) {
                return -1;
            }
            if (SLFmtEmitVarLike(c, nodeId, "var") != 0) {
                return -1;
            }
            return SLFmtEmitTrailingCommentsForNode(c, nodeId);
        case SLAst_CONST:
            if ((n->flags & SLAstFlag_PUB) != 0 && SLFmtWriteCStr(c, "pub ") != 0) {
                return -1;
            }
            if (SLFmtEmitVarLike(c, nodeId, "const") != 0) {
                return -1;
            }
            return SLFmtEmitTrailingCommentsForNode(c, nodeId);
        default:
            if (SLFmtWriteSlice(c, n->start, n->end) != 0) {
                return -1;
            }
            return SLFmtEmitTrailingCommentsForNode(c, nodeId);
    }
}

static int SLFmtEmitDirectiveGroup(SLFmtCtx* c, int32_t firstDirective, int32_t* outNext) {
    int32_t child = firstDirective;
    int32_t next = -1;
    int     first = 1;
    while (child >= 0 && c->ast->nodes[child].kind == SLAst_DIRECTIVE) {
        next = SLFmtNextSibling(c->ast, child);
        if (!first && SLFmtNewline(c) != 0) {
            return -1;
        }
        if (SLFmtEmitDecl(c, child) != 0) {
            return -1;
        }
        first = 0;
        child = next;
    }
    if (child >= 0) {
        if (!first && SLFmtNewline(c) != 0) {
            return -1;
        }
        if (SLFmtEmitDecl(c, child) != 0) {
            return -1;
        }
        next = SLFmtNextSibling(c->ast, child);
    }
    *outNext = next;
    return 0;
}

static int SLFmtEmitFile(SLFmtCtx* c) {
    int32_t child = SLFmtFirstChild(c->ast, c->ast->root);
    int     first = 1;
    while (child >= 0) {
        int32_t next;
        if (!first) {
            if (SLFmtNewline(c) != 0 || SLFmtNewline(c) != 0) {
                return -1;
            }
        }
        next = SLFmtNextSibling(c->ast, child);
        if (c->ast->nodes[child].kind == SLAst_IMPORT) {
            int32_t lastSourceNode = child;
            if (SLFmtEmitImportGroup(c, child, &lastSourceNode, &next) != 0) {
                return -1;
            }
        } else if (c->ast->nodes[child].kind == SLAst_DIRECTIVE) {
            if (SLFmtEmitDirectiveGroup(c, child, &next) != 0) {
                return -1;
            }
        } else if (SLFmtEmitDecl(c, child) != 0) {
            return -1;
        }
        first = 0;
        child = next;
    }
    if (!first && SLFmtNewline(c) != 0) {
        return -1;
    }
    if (SLFmtEmitRemainingComments(c) != 0) {
        return -1;
    }
    if (c->out.len == 0 || c->out.v[c->out.len - 1u] != '\n') {
        if (SLFmtNewline(c) != 0) {
            return -1;
        }
    }
    while (
        c->out.len > 1u && c->out.v[c->out.len - 1u] == '\n' && c->out.v[c->out.len - 2u] == '\n')
    {
        c->out.len--;
    }
    return 0;
}

int SLFormat(
    SLArena*  arena,
    SLStrView src,
    const SLFormatOptions* _Nullable options,
    SLStrView* out,
    SLDiag* _Nullable diag) {
    SLAst          ast;
    SLParseExtras  extras;
    SLParseOptions parseOptions;
    SLFmtCtx       c;
    uint32_t       indentWidth = 4u;

    if (diag != NULL) {
        *diag = (SLDiag){ 0 };
    }
    if (out == NULL) {
        return -1;
    }
    out->ptr = NULL;
    out->len = 0;

    if (options != NULL && options->indentWidth != 0) {
        indentWidth = options->indentWidth;
    }

    parseOptions.flags = SLParseFlag_COLLECT_FORMATTING;
    if (SLParse(arena, src, &parseOptions, &ast, &extras, diag) != 0) {
        return -1;
    }
    if (SLFmtRewriteAst(&ast, src, options) != 0) {
        if (diag != NULL) {
            diag->code = SLDiag_ARENA_OOM;
            diag->type = SLDiagTypeOfCode(SLDiag_ARENA_OOM);
            diag->start = 0;
            diag->end = 0;
            diag->argStart = 0;
            diag->argEnd = 0;
            diag->detail = NULL;
            diag->hintOverride = NULL;
        }
        return -1;
    }

    c.ast = &ast;
    c.src = src;
    c.comments = extras.comments;
    c.commentLen = extras.commentLen;
    c.commentUsed = NULL;
    c.indent = 0;
    c.indentWidth = indentWidth;
    c.lineStart = 1;
    c.out.arena = arena;
    c.out.v = NULL;
    c.out.len = 0;
    c.out.cap = 0;

    if (c.commentLen > 0) {
        c.commentUsed = (uint8_t*)SLArenaAlloc(
            arena, c.commentLen * (uint32_t)sizeof(uint8_t), (uint32_t)_Alignof(uint8_t));
        if (c.commentUsed == NULL) {
            if (diag != NULL) {
                diag->code = SLDiag_ARENA_OOM;
                diag->type = SLDiagTypeOfCode(SLDiag_ARENA_OOM);
                diag->start = 0;
                diag->end = 0;
                diag->argStart = 0;
                diag->argEnd = 0;
                diag->detail = NULL;
                diag->hintOverride = NULL;
            }
            return -1;
        }
        memset(c.commentUsed, 0, c.commentLen);
    }

    if (SLFmtEmitFile(&c) != 0) {
        if (diag != NULL && diag->code == SLDiag_NONE) {
            diag->code = SLDiag_ARENA_OOM;
            diag->type = SLDiagTypeOfCode(SLDiag_ARENA_OOM);
            diag->start = 0;
            diag->end = 0;
            diag->argStart = 0;
            diag->argEnd = 0;
            diag->detail = NULL;
            diag->hintOverride = NULL;
        }
        return -1;
    }

    if (SLFmtBufAppendChar(&c.out, '\0') != 0) {
        if (diag != NULL) {
            diag->code = SLDiag_ARENA_OOM;
            diag->type = SLDiagTypeOfCode(SLDiag_ARENA_OOM);
            diag->start = 0;
            diag->end = 0;
            diag->argStart = 0;
            diag->argEnd = 0;
            diag->detail = NULL;
            diag->hintOverride = NULL;
        }
        return -1;
    }

    out->ptr = c.out.v;
    out->len = c.out.len - 1u;
    return 0;
}

SL_API_END
