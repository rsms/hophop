#include "libhop-impl.h"

HOP_API_BEGIN

typedef struct {
    HOPArena* arena;
    char* _Nullable v;
    uint32_t len;
    uint32_t cap;
} HOPFmtBuf;

typedef struct {
    const HOPAst* ast;
    HOPStrView    src;
    const HOPComment* _Nullable comments;
    uint32_t commentLen;
    uint8_t* _Nullable commentUsed;
    uint32_t  indent;
    uint32_t  indentWidth;
    int       lineStart;
    HOPFmtBuf out;
} HOPFmtCtx;

enum {
    HOPFmtFlag_DROP_REDUNDANT_LITERAL_CAST = 0x4000u,
};

typedef enum {
    HOPFmtNumericType_INVALID = 0,
    HOPFmtNumericType_I8,
    HOPFmtNumericType_I16,
    HOPFmtNumericType_I32,
    HOPFmtNumericType_I64,
    HOPFmtNumericType_INT,
    HOPFmtNumericType_U8,
    HOPFmtNumericType_U16,
    HOPFmtNumericType_U32,
    HOPFmtNumericType_U64,
    HOPFmtNumericType_UINT,
    HOPFmtNumericType_F32,
    HOPFmtNumericType_F64,
} HOPFmtNumericType;

static int HOPFmtBufReserve(HOPFmtBuf* b, uint32_t extra) {
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
    p = (char*)HOPArenaAlloc(b->arena, cap, (uint32_t)_Alignof(char));
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

static int HOPFmtBufAppendChar(HOPFmtBuf* b, char c) {
    if (HOPFmtBufReserve(b, 1u) != 0) {
        return -1;
    }
    b->v[b->len++] = c;
    return 0;
}

static int     HOPFmtWriteChar(HOPFmtCtx* c, char ch);
static int32_t HOPFmtFindEnclosingFnNode(const HOPAst* ast, int32_t nodeId);
static int32_t HOPFmtFindParentNode(const HOPAst* ast, int32_t childNodeId);
static int     HOPFmtEmitType(HOPFmtCtx* c, int32_t nodeId);

static uint32_t HOPFmtCStrLen(const char* s) {
    const char* p = s;
    while (*p != '\0') {
        p++;
    }
    return (uint32_t)(p - s);
}

static int HOPFmtWriteIndent(HOPFmtCtx* c) {
    uint32_t i;
    if (!c->lineStart) {
        return 0;
    }
    for (i = 0; i < c->indent; i++) {
        if (HOPFmtBufAppendChar(&c->out, '\t') != 0) {
            return -1;
        }
    }
    c->lineStart = 0;
    return 0;
}

static int HOPFmtWriteSpaces(HOPFmtCtx* c, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (HOPFmtWriteChar(c, ' ') != 0) {
            return -1;
        }
    }
    return 0;
}

static int HOPFmtWriteChar(HOPFmtCtx* c, char ch) {
    if (ch != '\n' && c->lineStart) {
        if (HOPFmtWriteIndent(c) != 0) {
            return -1;
        }
    }
    if (HOPFmtBufAppendChar(&c->out, ch) != 0) {
        return -1;
    }
    c->lineStart = (ch == '\n');
    return 0;
}

static int HOPFmtWrite(HOPFmtCtx* c, const char* s, uint32_t len) {
    uint32_t i;
    for (i = 0; i < len; i++) {
        if (HOPFmtWriteChar(c, s[i]) != 0) {
            return -1;
        }
    }
    return 0;
}

static int HOPFmtWriteCStr(HOPFmtCtx* c, const char* s) {
    while (*s != '\0') {
        if (HOPFmtWriteChar(c, *s++) != 0) {
            return -1;
        }
    }
    return 0;
}

static int HOPFmtWriteSlice(HOPFmtCtx* c, uint32_t start, uint32_t end) {
    if (end < start || end > c->src.len) {
        return -1;
    }
    return HOPFmtWrite(c, c->src.ptr + start, end - start);
}

static int HOPFmtWriteSliceLiteral(HOPFmtCtx* c, uint32_t start, uint32_t end) {
    uint32_t i;
    if (end < start || end > c->src.len) {
        return -1;
    }
    if (c->lineStart && HOPFmtWriteIndent(c) != 0) {
        return -1;
    }
    for (i = start; i < end; i++) {
        char ch = c->src.ptr[i];
        if (HOPFmtBufAppendChar(&c->out, ch) != 0) {
            return -1;
        }
        c->lineStart = (ch == '\n');
    }
    return 0;
}

static int HOPFmtNewline(HOPFmtCtx* c) {
    return HOPFmtWriteChar(c, '\n');
}

static int32_t HOPFmtFirstChild(const HOPAst* ast, int32_t nodeId) {
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    return ast->nodes[nodeId].firstChild;
}

static int32_t HOPFmtNextSibling(const HOPAst* ast, int32_t nodeId) {
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    return ast->nodes[nodeId].nextSibling;
}

static uint32_t HOPFmtListCount(const HOPAst* ast, int32_t listNodeId) {
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

static int32_t HOPFmtListItemAt(const HOPAst* ast, int32_t listNodeId, uint32_t index) {
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

static int HOPFmtSlicesEqual(
    HOPStrView src, uint32_t aStart, uint32_t aEnd, uint32_t bStart, uint32_t bEnd) {
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

static int HOPFmtSliceEqLiteral(HOPStrView src, uint32_t start, uint32_t end, const char* lit) {
    uint32_t len = HOPFmtCStrLen(lit);
    if (end < start || end > src.len || (end - start) != len) {
        return 0;
    }
    return len == 0 || memcmp(src.ptr + start, lit, len) == 0;
}

static int HOPFmtSliceHasChar(HOPStrView src, uint32_t start, uint32_t end, char ch) {
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

static int HOPFmtExprIsPlainIdent(
    const HOPAst* ast, int32_t exprNodeId, uint32_t* outStart, uint32_t* outEnd) {
    const HOPAstNode* n;
    if (exprNodeId < 0 || (uint32_t)exprNodeId >= ast->len || outStart == NULL || outEnd == NULL) {
        return 0;
    }
    n = &ast->nodes[exprNodeId];
    if (n->kind != HOPAst_IDENT || (n->flags & HOPAstFlag_PAREN) != 0 || n->dataEnd <= n->dataStart)
    {
        return 0;
    }
    *outStart = n->dataStart;
    *outEnd = n->dataEnd;
    return 1;
}

static int HOPFmtIsTypeNodeKindRaw(HOPAstKind kind) {
    return kind == HOPAst_TYPE_NAME || kind == HOPAst_TYPE_PTR || kind == HOPAst_TYPE_REF
        || kind == HOPAst_TYPE_MUTREF || kind == HOPAst_TYPE_ARRAY || kind == HOPAst_TYPE_VARRAY
        || kind == HOPAst_TYPE_SLICE || kind == HOPAst_TYPE_MUTSLICE || kind == HOPAst_TYPE_OPTIONAL
        || kind == HOPAst_TYPE_FN || kind == HOPAst_TYPE_ANON_STRUCT
        || kind == HOPAst_TYPE_ANON_UNION || kind == HOPAst_TYPE_TUPLE || kind == HOPAst_TYPE_PARAM;
}

static HOPFmtNumericType HOPFmtNumericTypeFromName(HOPStrView src, uint32_t start, uint32_t end) {
    if (HOPFmtSliceEqLiteral(src, start, end, "i8")) {
        return HOPFmtNumericType_I8;
    }
    if (HOPFmtSliceEqLiteral(src, start, end, "i16")) {
        return HOPFmtNumericType_I16;
    }
    if (HOPFmtSliceEqLiteral(src, start, end, "i32")) {
        return HOPFmtNumericType_I32;
    }
    if (HOPFmtSliceEqLiteral(src, start, end, "i64")) {
        return HOPFmtNumericType_I64;
    }
    if (HOPFmtSliceEqLiteral(src, start, end, "int")) {
        return HOPFmtNumericType_INT;
    }
    if (HOPFmtSliceEqLiteral(src, start, end, "u8")) {
        return HOPFmtNumericType_U8;
    }
    if (HOPFmtSliceEqLiteral(src, start, end, "u16")) {
        return HOPFmtNumericType_U16;
    }
    if (HOPFmtSliceEqLiteral(src, start, end, "u32")) {
        return HOPFmtNumericType_U32;
    }
    if (HOPFmtSliceEqLiteral(src, start, end, "u64")) {
        return HOPFmtNumericType_U64;
    }
    if (HOPFmtSliceEqLiteral(src, start, end, "uint")) {
        return HOPFmtNumericType_UINT;
    }
    if (HOPFmtSliceEqLiteral(src, start, end, "f32")) {
        return HOPFmtNumericType_F32;
    }
    if (HOPFmtSliceEqLiteral(src, start, end, "f64")) {
        return HOPFmtNumericType_F64;
    }
    return HOPFmtNumericType_INVALID;
}

static HOPFmtNumericType HOPFmtNumericTypeFromTypeNode(
    const HOPAst* ast, HOPStrView src, int32_t nodeId) {
    const HOPAstNode* n;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return HOPFmtNumericType_INVALID;
    }
    n = &ast->nodes[nodeId];
    if (n->kind != HOPAst_TYPE_NAME) {
        return HOPFmtNumericType_INVALID;
    }
    return HOPFmtNumericTypeFromName(src, n->dataStart, n->dataEnd);
}

static HOPFmtNumericType HOPFmtCastLiteralNumericType(
    const HOPAst* ast, HOPStrView src, int32_t exprNodeId, int32_t typeNodeId) {
    if (exprNodeId < 0 || typeNodeId < 0 || (uint32_t)exprNodeId >= ast->len
        || (uint32_t)typeNodeId >= ast->len)
    {
        return HOPFmtNumericType_INVALID;
    }
    if (ast->nodes[exprNodeId].kind == HOPAst_INT) {
        HOPFmtNumericType t = HOPFmtNumericTypeFromTypeNode(ast, src, typeNodeId);
        switch (t) {
            case HOPFmtNumericType_I8:
            case HOPFmtNumericType_I16:
            case HOPFmtNumericType_I32:
            case HOPFmtNumericType_I64:
            case HOPFmtNumericType_INT:
            case HOPFmtNumericType_U8:
            case HOPFmtNumericType_U16:
            case HOPFmtNumericType_U32:
            case HOPFmtNumericType_U64:
            case HOPFmtNumericType_UINT: return t;
            default:                     return HOPFmtNumericType_INVALID;
        }
    }
    if (ast->nodes[exprNodeId].kind == HOPAst_FLOAT) {
        HOPFmtNumericType t = HOPFmtNumericTypeFromTypeNode(ast, src, typeNodeId);
        return (t == HOPFmtNumericType_F32 || t == HOPFmtNumericType_F64)
                 ? t
                 : HOPFmtNumericType_INVALID;
    }
    return HOPFmtNumericType_INVALID;
}

static int HOPFmtBinaryOpSharesOperandType(uint16_t op) {
    switch ((HOPTokenKind)op) {
        case HOPTok_ADD:
        case HOPTok_SUB:
        case HOPTok_MUL:
        case HOPTok_DIV:
        case HOPTok_MOD:
        case HOPTok_AND:
        case HOPTok_OR:
        case HOPTok_XOR:
        case HOPTok_EQ:
        case HOPTok_NEQ:
        case HOPTok_LT:
        case HOPTok_GT:
        case HOPTok_LTE:
        case HOPTok_GTE: return 1;
        default:         return 0;
    }
}

static int HOPFmtIsAssignmentOp(HOPTokenKind kind);

static int HOPFmtTypeNodesEqualBySource(
    const HOPAst* ast, HOPStrView src, int32_t aTypeNodeId, int32_t bTypeNodeId) {
    const HOPAstNode* a;
    const HOPAstNode* b;
    if (aTypeNodeId < 0 || bTypeNodeId < 0 || (uint32_t)aTypeNodeId >= ast->len
        || (uint32_t)bTypeNodeId >= ast->len)
    {
        return 0;
    }
    a = &ast->nodes[aTypeNodeId];
    b = &ast->nodes[bTypeNodeId];
    return HOPFmtSlicesEqual(src, a->start, a->end, b->start, b->end);
}

typedef struct {
    uint32_t nameStart;
    uint32_t nameEnd;
    int32_t  concreteTypeNodeId;
} HOPFmtTypeBinding;

typedef struct {
    HOPFmtTypeBinding items[16];
    uint32_t          len;
} HOPFmtTypeEnv;

typedef struct {
    int32_t  paramNodeId;
    int32_t  typeNodeId;
    uint16_t flags;
} HOPFmtCallParam;

typedef struct {
    int32_t  argNodeId;
    int32_t  exprNodeId;
    uint32_t labelStart;
    uint32_t labelEnd;
    uint8_t  hasLabel;
    uint8_t  isSynthetic;
} HOPFmtCallActual;

typedef struct {
    int32_t       typeNodeId;
    HOPFmtTypeEnv env;
} HOPFmtInferredType;

static void HOPFmtTypeEnvInit(HOPFmtTypeEnv* env) {
    if (env != NULL) {
        env->len = 0;
    }
}

static void HOPFmtInferredTypeInit(HOPFmtInferredType* inferred) {
    if (inferred != NULL) {
        inferred->typeNodeId = -1;
        HOPFmtTypeEnvInit(&inferred->env);
    }
}

static int HOPFmtInferredTypeSet(
    HOPFmtInferredType* inferred, int32_t typeNodeId, const HOPFmtTypeEnv* _Nullable env);
static int HOPFmtInferredTypeMatchesNode(
    const HOPAst* ast, HOPStrView src, const HOPFmtInferredType* inferred, int32_t typeNodeId);

static int32_t HOPFmtTypeEnvFind(
    const HOPFmtTypeEnv* env, HOPStrView src, uint32_t nameStart, uint32_t nameEnd) {
    uint32_t i;
    if (env == NULL || nameEnd <= nameStart) {
        return -1;
    }
    for (i = 0; i < env->len; i++) {
        if (HOPFmtSlicesEqual(
                src, env->items[i].nameStart, env->items[i].nameEnd, nameStart, nameEnd))
        {
            return (int32_t)i;
        }
    }
    return -1;
}

static int HOPFmtTypeEnvAdd(
    HOPFmtTypeEnv* env, uint32_t nameStart, uint32_t nameEnd, int32_t concreteTypeNodeId) {
    if (env == NULL || nameEnd <= nameStart || env->len >= 16u) {
        return 0;
    }
    env->items[env->len].nameStart = nameStart;
    env->items[env->len].nameEnd = nameEnd;
    env->items[env->len].concreteTypeNodeId = concreteTypeNodeId;
    env->len++;
    return 1;
}

static int HOPFmtTypeEnvInitFromDeclTypeParams(
    const HOPAst* ast, int32_t declNodeId, HOPFmtTypeEnv* env) {
    int32_t child;
    HOPFmtTypeEnvInit(env);
    if (declNodeId < 0 || (uint32_t)declNodeId >= ast->len || env == NULL) {
        return 0;
    }
    child = HOPFmtFirstChild(ast, declNodeId);
    while (child >= 0 && ast->nodes[child].kind == HOPAst_TYPE_PARAM) {
        if (!HOPFmtTypeEnvAdd(env, ast->nodes[child].dataStart, ast->nodes[child].dataEnd, -1)) {
            return 0;
        }
        child = HOPFmtNextSibling(ast, child);
    }
    return 1;
}

static int HOPFmtTypeNameIsBoundParam(
    const HOPAst* ast,
    HOPStrView    src,
    int32_t       typeNodeId,
    const HOPFmtTypeEnv* _Nullable env,
    int32_t* outBindingIndex) {
    const HOPAstNode* n;
    int32_t           bindingIndex;
    if (typeNodeId < 0 || (uint32_t)typeNodeId >= ast->len) {
        return 0;
    }
    n = &ast->nodes[typeNodeId];
    if (n->kind != HOPAst_TYPE_NAME || HOPFmtFirstChild(ast, typeNodeId) >= 0) {
        return 0;
    }
    bindingIndex = HOPFmtTypeEnvFind(env, src, n->dataStart, n->dataEnd);
    if (bindingIndex < 0) {
        return 0;
    }
    if (outBindingIndex != NULL) {
        *outBindingIndex = bindingIndex;
    }
    return 1;
}

static int HOPFmtTypeCompatibleWithEnvs(
    const HOPAst* ast,
    HOPStrView    src,
    int32_t       wantNodeId,
    HOPFmtTypeEnv* _Nullable wantEnv,
    int32_t gotNodeId,
    const HOPFmtTypeEnv* _Nullable gotEnv);

static int HOPFmtTypeCompatibleChildren(
    const HOPAst* ast,
    HOPStrView    src,
    int32_t       wantNodeId,
    HOPFmtTypeEnv* _Nullable wantEnv,
    int32_t gotNodeId,
    const HOPFmtTypeEnv* _Nullable gotEnv) {
    int32_t wantChild = HOPFmtFirstChild(ast, wantNodeId);
    int32_t gotChild = HOPFmtFirstChild(ast, gotNodeId);
    while (wantChild >= 0 && gotChild >= 0) {
        if (!HOPFmtTypeCompatibleWithEnvs(ast, src, wantChild, wantEnv, gotChild, gotEnv)) {
            return 0;
        }
        wantChild = HOPFmtNextSibling(ast, wantChild);
        gotChild = HOPFmtNextSibling(ast, gotChild);
    }
    return wantChild < 0 && gotChild < 0;
}

static int HOPFmtTypeCompatibleWithEnvs(
    const HOPAst* ast,
    HOPStrView    src,
    int32_t       wantNodeId,
    HOPFmtTypeEnv* _Nullable wantEnv,
    int32_t gotNodeId,
    const HOPFmtTypeEnv* _Nullable gotEnv) {
    const HOPAstNode* want;
    const HOPAstNode* got;
    int32_t           bindingIndex;
    if (wantNodeId < 0 || gotNodeId < 0 || (uint32_t)wantNodeId >= ast->len
        || (uint32_t)gotNodeId >= ast->len)
    {
        return 0;
    }
    if (HOPFmtTypeNameIsBoundParam(ast, src, wantNodeId, wantEnv, &bindingIndex)) {
        int32_t boundNodeId = wantEnv->items[bindingIndex].concreteTypeNodeId;
        if (boundNodeId >= 0) {
            return HOPFmtTypeCompatibleWithEnvs(ast, src, boundNodeId, NULL, gotNodeId, gotEnv);
        }
        if (gotEnv == NULL
            || !HOPFmtTypeNameIsBoundParam(ast, src, gotNodeId, gotEnv, &bindingIndex)
            || gotEnv->items[bindingIndex].concreteTypeNodeId < 0)
        {
            wantEnv
                ->items[HOPFmtTypeEnvFind(
                    wantEnv, src, ast->nodes[wantNodeId].dataStart, ast->nodes[wantNodeId].dataEnd)]
                .concreteTypeNodeId = gotNodeId;
            return 1;
        }
    }
    if (gotEnv != NULL && HOPFmtTypeNameIsBoundParam(ast, src, gotNodeId, gotEnv, &bindingIndex)) {
        int32_t boundNodeId = gotEnv->items[bindingIndex].concreteTypeNodeId;
        if (boundNodeId < 0) {
            return 0;
        }
        return HOPFmtTypeCompatibleWithEnvs(ast, src, wantNodeId, wantEnv, boundNodeId, NULL);
    }
    want = &ast->nodes[wantNodeId];
    got = &ast->nodes[gotNodeId];
    if (want->kind != got->kind) {
        return 0;
    }
    switch (want->kind) {
        case HOPAst_TYPE_NAME:
            if (!HOPFmtSlicesEqual(
                    src, want->dataStart, want->dataEnd, got->dataStart, got->dataEnd))
            {
                return 0;
            }
            return HOPFmtTypeCompatibleChildren(ast, src, wantNodeId, wantEnv, gotNodeId, gotEnv);
        case HOPAst_TYPE_PTR:
        case HOPAst_TYPE_REF:
        case HOPAst_TYPE_MUTREF:
        case HOPAst_TYPE_SLICE:
        case HOPAst_TYPE_MUTSLICE:
        case HOPAst_TYPE_OPTIONAL:
        case HOPAst_TYPE_ARRAY:
        case HOPAst_TYPE_VARRAY:
        case HOPAst_TYPE_TUPLE:
        case HOPAst_TYPE_FN:
            return HOPFmtTypeCompatibleChildren(ast, src, wantNodeId, wantEnv, gotNodeId, gotEnv);
        default: return HOPFmtSlicesEqual(src, want->start, want->end, got->start, got->end);
    }
}

static int HOPFmtInferLocalCallAgainstFn(
    const HOPAst* ast,
    HOPStrView    src,
    int32_t       callNodeId,
    int32_t       fnNodeId,
    uint32_t      beforePos,
    int32_t       targetArgNodeId,
    HOPFmtInferredType* _Nullable outReturnType,
    HOPFmtInferredType* _Nullable outTargetParamType);

static int HOPFmtCanDropLiteralCastFromLocalCall(
    const HOPAst* ast, HOPStrView src, int32_t castNodeId, int32_t castTypeNodeId) {
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
    parentNodeId = HOPFmtFindParentNode(ast, castNodeId);
    if (parentNodeId < 0) {
        return 0;
    }
    if (ast->nodes[parentNodeId].kind == HOPAst_CALL_ARG) {
        argNodeId = parentNodeId;
        callNodeId = HOPFmtFindParentNode(ast, parentNodeId);
    } else if (ast->nodes[parentNodeId].kind == HOPAst_CALL) {
        argNodeId = castNodeId;
        callNodeId = parentNodeId;
    } else {
        return 0;
    }
    if (callNodeId < 0 || ast->nodes[callNodeId].kind != HOPAst_CALL) {
        return 0;
    }
    calleeNodeId = HOPFmtFirstChild(ast, callNodeId);
    if (calleeNodeId < 0 || (uint32_t)calleeNodeId >= ast->len) {
        return 0;
    }
    if (ast->nodes[calleeNodeId].kind == HOPAst_IDENT) {
        calleeNameStart = ast->nodes[calleeNodeId].dataStart;
        calleeNameEnd = ast->nodes[calleeNodeId].dataEnd;
    } else if (ast->nodes[calleeNodeId].kind == HOPAst_FIELD_EXPR) {
        calleeNameStart = ast->nodes[calleeNodeId].dataStart;
        calleeNameEnd = ast->nodes[calleeNodeId].dataEnd;
    } else {
        return 0;
    }
    cur = HOPFmtFirstChild(ast, ast->root);
    while (cur >= 0) {
        if (ast->nodes[cur].kind == HOPAst_FN
            && HOPFmtSlicesEqual(
                src,
                ast->nodes[cur].dataStart,
                ast->nodes[cur].dataEnd,
                calleeNameStart,
                calleeNameEnd))
        {
            HOPFmtInferredType targetType;
            HOPFmtInferredTypeInit(&targetType);
            if (!HOPFmtInferLocalCallAgainstFn(
                    ast,
                    src,
                    callNodeId,
                    cur,
                    ast->nodes[castNodeId].start,
                    argNodeId,
                    NULL,
                    &targetType))
            {
                cur = HOPFmtNextSibling(ast, cur);
                continue;
            }
            sawMappedCandidate = 1;
            if (!HOPFmtInferredTypeMatchesNode(ast, src, &targetType, castTypeNodeId)) {
                return 0;
            }
        }
        cur = HOPFmtNextSibling(ast, cur);
    }
    return sawMappedCandidate;
}

static void HOPFmtGetCastParts(
    const HOPAst* ast, int32_t castNodeId, int32_t* outExprNodeId, int32_t* outTypeNodeId) {
    int32_t exprNodeId = -1;
    int32_t typeNodeId = -1;
    if (castNodeId >= 0 && (uint32_t)castNodeId < ast->len
        && ast->nodes[castNodeId].kind == HOPAst_CAST)
    {
        exprNodeId = HOPFmtFirstChild(ast, castNodeId);
        typeNodeId = exprNodeId >= 0 ? HOPFmtNextSibling(ast, exprNodeId) : -1;
    }
    if (outExprNodeId != NULL) {
        *outExprNodeId = exprNodeId;
    }
    if (outTypeNodeId != NULL) {
        *outTypeNodeId = typeNodeId;
    }
}

static void HOPFmtGetVarLikeTypeAndInit(
    const HOPAst* ast, int32_t nodeId, int32_t* outTypeNodeId, int32_t* outInitNodeId) {
    int32_t firstChild = HOPFmtFirstChild(ast, nodeId);
    int32_t typeNodeId = -1;
    int32_t initNodeId = -1;
    if (firstChild >= 0 && ast->nodes[firstChild].kind == HOPAst_NAME_LIST) {
        int32_t afterNames = HOPFmtNextSibling(ast, firstChild);
        if (afterNames >= 0 && HOPFmtIsTypeNodeKindRaw(ast->nodes[afterNames].kind)) {
            typeNodeId = afterNames;
            initNodeId = HOPFmtNextSibling(ast, afterNames);
        } else {
            initNodeId = afterNames;
        }
    } else if (firstChild >= 0 && HOPFmtIsTypeNodeKindRaw(ast->nodes[firstChild].kind)) {
        typeNodeId = firstChild;
        initNodeId = HOPFmtNextSibling(ast, firstChild);
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

static int32_t HOPFmtFindFnReturnTypeNode(const HOPAst* ast, int32_t fnNodeId) {
    int32_t child;
    if (fnNodeId < 0 || (uint32_t)fnNodeId >= ast->len || ast->nodes[fnNodeId].kind != HOPAst_FN) {
        return -1;
    }
    child = HOPFmtFirstChild(ast, fnNodeId);
    while (child >= 0) {
        const HOPAstNode* n = &ast->nodes[child];
        if (HOPFmtIsTypeNodeKindRaw(n->kind) && n->flags == 1) {
            return child;
        }
        child = HOPFmtNextSibling(ast, child);
    }
    return -1;
}

static int HOPFmtFnReturnTypeIsGenericParam(const HOPAst* ast, HOPStrView src, int32_t fnNodeId) {
    HOPFmtTypeEnv env;
    int32_t       retTypeNodeId = HOPFmtFindFnReturnTypeNode(ast, fnNodeId);
    int32_t       bindingIndex = -1;
    if (retTypeNodeId < 0 || !HOPFmtTypeEnvInitFromDeclTypeParams(ast, fnNodeId, &env)) {
        return 0;
    }
    return HOPFmtTypeNameIsBoundParam(ast, src, retTypeNodeId, &env, &bindingIndex);
}

static int32_t HOPFmtFindParentNode(const HOPAst* ast, int32_t childNodeId) {
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

static int HOPFmtNodeDeclaresNameRange(
    const HOPAst* ast, HOPStrView src, int32_t nodeId, uint32_t nameStart, uint32_t nameEnd) {
    const HOPAstNode* n;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len || nameEnd <= nameStart) {
        return 0;
    }
    n = &ast->nodes[nodeId];
    if ((n->kind == HOPAst_PARAM || n->kind == HOPAst_VAR || n->kind == HOPAst_CONST)
        && HOPFmtSlicesEqual(src, n->dataStart, n->dataEnd, nameStart, nameEnd))
    {
        return 1;
    }
    if (n->kind == HOPAst_VAR || n->kind == HOPAst_CONST) {
        int32_t child = HOPFmtFirstChild(ast, nodeId);
        if (child >= 0 && (uint32_t)child < ast->len && ast->nodes[child].kind == HOPAst_NAME_LIST)
        {
            int32_t nameNode = HOPFmtFirstChild(ast, child);
            while (nameNode >= 0) {
                const HOPAstNode* nn = &ast->nodes[nameNode];
                if (nn->kind == HOPAst_IDENT
                    && HOPFmtSlicesEqual(src, nn->dataStart, nn->dataEnd, nameStart, nameEnd))
                {
                    return 1;
                }
                nameNode = HOPFmtNextSibling(ast, nameNode);
            }
        }
    }
    return 0;
}

static int HOPFmtNodeDeclaresNameLiteral(
    const HOPAst* ast, HOPStrView src, int32_t nodeId, const char* nameLit) {
    const HOPAstNode* n;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return 0;
    }
    n = &ast->nodes[nodeId];
    if ((n->kind == HOPAst_PARAM || n->kind == HOPAst_VAR || n->kind == HOPAst_CONST)
        && HOPFmtSliceEqLiteral(src, n->dataStart, n->dataEnd, nameLit))
    {
        return 1;
    }
    if (n->kind == HOPAst_VAR || n->kind == HOPAst_CONST) {
        int32_t child = HOPFmtFirstChild(ast, nodeId);
        if (child >= 0 && (uint32_t)child < ast->len && ast->nodes[child].kind == HOPAst_NAME_LIST)
        {
            int32_t nameNode = HOPFmtFirstChild(ast, child);
            while (nameNode >= 0) {
                const HOPAstNode* nn = &ast->nodes[nameNode];
                if (nn->kind == HOPAst_IDENT
                    && HOPFmtSliceEqLiteral(src, nn->dataStart, nn->dataEnd, nameLit))
                {
                    return 1;
                }
                nameNode = HOPFmtNextSibling(ast, nameNode);
            }
        }
    }
    return 0;
}

static int HOPFmtFindLocalBindingBefore(
    const HOPAst* ast,
    HOPStrView    src,
    int32_t       identNodeId,
    uint32_t      beforePos,
    int32_t* _Nullable outDeclNodeId,
    int32_t* _Nullable outTypeNodeId,
    int32_t* _Nullable outInitNodeId);

static int HOPFmtFindFieldTypeOnLocalNamedType(
    const HOPAst*             ast,
    HOPStrView                src,
    const HOPFmtInferredType* baseType,
    uint32_t                  fieldStart,
    uint32_t                  fieldEnd,
    HOPFmtInferredType*       outType);

static int HOPFmtInferLocalCallAgainstFn(
    const HOPAst* ast,
    HOPStrView    src,
    int32_t       callNodeId,
    int32_t       fnNodeId,
    uint32_t      beforePos,
    int32_t       targetArgNodeId,
    HOPFmtInferredType* _Nullable outReturnType,
    HOPFmtInferredType* _Nullable outTargetParamType);

static int HOPFmtInferExprTypeEx(
    const HOPAst*       ast,
    HOPStrView          src,
    int32_t             exprNodeId,
    uint32_t            beforePos,
    uint32_t            depth,
    HOPFmtInferredType* outType) {
    const HOPAstNode* n;
    if (outType != NULL) {
        HOPFmtInferredTypeInit(outType);
    }
    if (exprNodeId < 0 || (uint32_t)exprNodeId >= ast->len) {
        return 0;
    }
    if (depth >= 32u || outType == NULL) {
        return 0;
    }
    n = &ast->nodes[exprNodeId];
    if (n->kind == HOPAst_CAST) {
        int32_t castExprNodeId = -1;
        int32_t castTypeNodeId = -1;
        HOPFmtGetCastParts(ast, exprNodeId, &castExprNodeId, &castTypeNodeId);
        (void)castExprNodeId;
        return HOPFmtInferredTypeSet(outType, castTypeNodeId, NULL);
    }
    if (n->kind == HOPAst_IDENT) {
        int32_t declNodeId = -1;
        int32_t typeNodeId = -1;
        int32_t initNodeId = -1;
        if (!HOPFmtFindLocalBindingBefore(
                ast, src, exprNodeId, beforePos, &declNodeId, &typeNodeId, &initNodeId))
        {
            return 0;
        }
        if (typeNodeId >= 0) {
            return HOPFmtInferredTypeSet(outType, typeNodeId, NULL);
        }
        if (initNodeId >= 0) {
            return HOPFmtInferExprTypeEx(
                ast, src, initNodeId, ast->nodes[declNodeId].start, depth + 1u, outType);
        }
        return 0;
    }
    if (n->kind == HOPAst_FIELD_EXPR) {
        HOPFmtInferredType baseType;
        int32_t            baseNodeId = HOPFmtFirstChild(ast, exprNodeId);
        HOPFmtInferredTypeInit(&baseType);
        return baseNodeId >= 0
            && HOPFmtInferExprTypeEx(ast, src, baseNodeId, beforePos, depth + 1u, &baseType)
            && HOPFmtFindFieldTypeOnLocalNamedType(
                   ast, src, &baseType, n->dataStart, n->dataEnd, outType);
    }
    if (n->kind == HOPAst_UNARY && n->op == HOPTok_MUL) {
        HOPFmtInferredType targetType;
        int32_t            targetNodeId = HOPFmtFirstChild(ast, exprNodeId);
        int32_t            targetTypeNodeId;
        const HOPAstNode*  targetTypeNode;
        HOPFmtInferredTypeInit(&targetType);
        if (targetNodeId < 0
            || !HOPFmtInferExprTypeEx(ast, src, targetNodeId, beforePos, depth + 1u, &targetType))
        {
            return 0;
        }
        targetTypeNodeId = targetType.typeNodeId;
        if (targetTypeNodeId < 0 || (uint32_t)targetTypeNodeId >= ast->len) {
            return 0;
        }
        targetTypeNode = &ast->nodes[targetTypeNodeId];
        if (targetTypeNode->kind != HOPAst_TYPE_PTR && targetTypeNode->kind != HOPAst_TYPE_REF
            && targetTypeNode->kind != HOPAst_TYPE_MUTREF)
        {
            return 0;
        }
        return HOPFmtInferredTypeSet(
            outType, HOPFmtFirstChild(ast, targetTypeNodeId), &targetType.env);
    }
    if (n->kind == HOPAst_CALL) {
        int32_t  calleeNodeId = HOPFmtFirstChild(ast, exprNodeId);
        uint32_t calleeNameStart;
        uint32_t calleeNameEnd;
        int32_t  cur;
        if (calleeNodeId < 0 || (uint32_t)calleeNodeId >= ast->len) {
            return 0;
        }
        if (ast->nodes[calleeNodeId].kind == HOPAst_IDENT) {
            calleeNameStart = ast->nodes[calleeNodeId].dataStart;
            calleeNameEnd = ast->nodes[calleeNodeId].dataEnd;
        } else if (ast->nodes[calleeNodeId].kind == HOPAst_FIELD_EXPR) {
            calleeNameStart = ast->nodes[calleeNodeId].dataStart;
            calleeNameEnd = ast->nodes[calleeNodeId].dataEnd;
        } else {
            return 0;
        }
        cur = HOPFmtFirstChild(ast, ast->root);
        while (cur >= 0) {
            if (ast->nodes[cur].kind == HOPAst_FN
                && HOPFmtSlicesEqual(
                    src,
                    ast->nodes[cur].dataStart,
                    ast->nodes[cur].dataEnd,
                    calleeNameStart,
                    calleeNameEnd)
                && HOPFmtInferLocalCallAgainstFn(
                    ast, src, exprNodeId, cur, beforePos, -1, outType, NULL))
            {
                return 1;
            }
            cur = HOPFmtNextSibling(ast, cur);
        }
        return 0;
    }
    if (n->kind == HOPAst_COMPOUND_LIT) {
        int32_t typeNodeId = HOPFmtFirstChild(ast, exprNodeId);
        if (typeNodeId >= 0 && HOPFmtIsTypeNodeKindRaw(ast->nodes[typeNodeId].kind)) {
            return HOPFmtInferredTypeSet(outType, typeNodeId, NULL);
        }
    }
    return 0;
}

static int HOPFmtCanDropRedundantLiteralCast(
    const HOPAst* ast,
    HOPStrView    src,
    const HOPFormatOptions* _Nullable options,
    int32_t castNodeId) {
    const HOPAstNode* castNode;
    int32_t           castExprNodeId = -1;
    int32_t           castTypeNodeId = -1;
    int32_t           parentNodeId;
    if (castNodeId < 0 || (uint32_t)castNodeId >= ast->len) {
        return 0;
    }
    castNode = &ast->nodes[castNodeId];
    if (castNode->kind != HOPAst_CAST) {
        return 0;
    }
    HOPFmtGetCastParts(ast, castNodeId, &castExprNodeId, &castTypeNodeId);
    if (HOPFmtCastLiteralNumericType(ast, src, castExprNodeId, castTypeNodeId)
        == HOPFmtNumericType_INVALID)
    {
        return 0;
    }
    parentNodeId = HOPFmtFindParentNode(ast, castNodeId);
    if (parentNodeId < 0) {
        return 0;
    }
    if (ast->nodes[parentNodeId].kind == HOPAst_UNARY && ast->nodes[parentNodeId].op == HOPTok_SUB)
    {
        int32_t            binaryNodeId = HOPFmtFindParentNode(ast, parentNodeId);
        int32_t            lhsNodeId;
        int32_t            rhsNodeId;
        int32_t            otherNodeId = -1;
        HOPFmtInferredType otherType;
        if (HOPFmtFirstChild(ast, parentNodeId) != castNodeId || binaryNodeId < 0
            || ast->nodes[binaryNodeId].kind != HOPAst_BINARY
            || !HOPFmtBinaryOpSharesOperandType(ast->nodes[binaryNodeId].op))
        {
            return 0;
        }
        lhsNodeId = HOPFmtFirstChild(ast, binaryNodeId);
        rhsNodeId = lhsNodeId >= 0 ? HOPFmtNextSibling(ast, lhsNodeId) : -1;
        if (lhsNodeId == parentNodeId) {
            otherNodeId = rhsNodeId;
        } else if (rhsNodeId == parentNodeId) {
            otherNodeId = lhsNodeId;
        }
        if (otherNodeId < 0) {
            return 0;
        }
        HOPFmtInferredTypeInit(&otherType);
        return HOPFmtInferExprTypeEx(ast, src, otherNodeId, castNode->start, 0u, &otherType)
            && HOPFmtInferredTypeMatchesNode(ast, src, &otherType, castTypeNodeId);
    }
    switch (ast->nodes[parentNodeId].kind) {
        case HOPAst_VAR:
        case HOPAst_CONST: {
            int32_t declTypeNodeId = -1;
            int32_t initNodeId = -1;
            HOPFmtGetVarLikeTypeAndInit(ast, parentNodeId, &declTypeNodeId, &initNodeId);
            if (initNodeId == castNodeId
                && HOPFmtTypeNodesEqualBySource(ast, src, castTypeNodeId, declTypeNodeId))
            {
                return 1;
            }
            return 0;
        }
        case HOPAst_RETURN: {
            int32_t fnNodeId;
            int32_t retTypeNodeId;
            int32_t retExprNodeId = HOPFmtFirstChild(ast, parentNodeId);
            if (retExprNodeId != castNodeId) {
                return 0;
            }
            fnNodeId = HOPFmtFindEnclosingFnNode(ast, parentNodeId);
            retTypeNodeId = HOPFmtFindFnReturnTypeNode(ast, fnNodeId);
            return HOPFmtTypeNodesEqualBySource(ast, src, castTypeNodeId, retTypeNodeId)
                || HOPFmtFnReturnTypeIsGenericParam(ast, src, fnNodeId);
        }
        case HOPAst_BINARY: {
            int32_t            lhsNodeId = HOPFmtFirstChild(ast, parentNodeId);
            int32_t            rhsNodeId = lhsNodeId >= 0 ? HOPFmtNextSibling(ast, lhsNodeId) : -1;
            int32_t            otherNodeId = -1;
            HOPFmtInferredType otherType;
            if (rhsNodeId == castNodeId
                && HOPFmtIsAssignmentOp((HOPTokenKind)ast->nodes[parentNodeId].op))
            {
                HOPFmtInferredTypeInit(&otherType);
                return HOPFmtInferExprTypeEx(ast, src, lhsNodeId, castNode->start, 0u, &otherType)
                    && HOPFmtInferredTypeMatchesNode(ast, src, &otherType, castTypeNodeId);
            }
            if (!HOPFmtBinaryOpSharesOperandType(ast->nodes[parentNodeId].op)) {
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
            HOPFmtInferredTypeInit(&otherType);
            return HOPFmtInferExprTypeEx(ast, src, otherNodeId, castNode->start, 0u, &otherType)
                && HOPFmtInferredTypeMatchesNode(ast, src, &otherType, castTypeNodeId);
        }
        default:
            if (HOPFmtCanDropLiteralCastFromLocalCall(ast, src, castNodeId, castTypeNodeId)) {
                return 1;
            }
            if (options != NULL && options->canDropLiteralCast != NULL) {
                return options->canDropLiteralCast(options->ctx, ast, src, castNodeId);
            }
            return 0;
    }
}

static int HOPFmtKeywordIsVar(const char* kw) {
    return kw[0] == 'v' && kw[1] == 'a' && kw[2] == 'r' && kw[3] == '\0';
}

static void HOPFmtRewriteVarTypeFromLiteralCast(
    const HOPAst* ast,
    HOPStrView    src,
    const char*   kw,
    uint32_t      nameCount,
    int32_t*      ioTypeNodeId,
    int32_t*      ioInitNodeId) {
    int32_t initNodeId;
    int32_t castExprNodeId = -1;
    int32_t castTypeNodeId = -1;
    if (ioTypeNodeId == NULL || ioInitNodeId == NULL || !HOPFmtKeywordIsVar(kw) || nameCount != 1u)
    {
        return;
    }
    if (*ioTypeNodeId >= 0 || *ioInitNodeId < 0 || (uint32_t)*ioInitNodeId >= ast->len) {
        return;
    }
    initNodeId = *ioInitNodeId;
    if (ast->nodes[initNodeId].kind != HOPAst_CAST) {
        return;
    }
    HOPFmtGetCastParts(ast, initNodeId, &castExprNodeId, &castTypeNodeId);
    if (HOPFmtCastLiteralNumericType(ast, src, castExprNodeId, castTypeNodeId)
        == HOPFmtNumericType_INVALID)
    {
        return;
    }
    *ioTypeNodeId = castTypeNodeId;
    *ioInitNodeId = castExprNodeId;
}

static void HOPFmtRewriteRedundantVarType(
    const HOPAst* ast,
    HOPStrView    src,
    const char*   kw,
    uint32_t      nameCount,
    uint32_t      declStart,
    int32_t*      ioTypeNodeId,
    int32_t*      ioInitNodeId) {
    HOPFmtInferredType initType;
    int32_t            typeNodeId;
    int32_t            initNodeId;
    if (ioTypeNodeId == NULL || ioInitNodeId == NULL || !HOPFmtKeywordIsVar(kw) || nameCount != 1u)
    {
        return;
    }
    typeNodeId = *ioTypeNodeId;
    initNodeId = *ioInitNodeId;
    if (typeNodeId < 0 || initNodeId < 0 || (uint32_t)typeNodeId >= ast->len
        || (uint32_t)initNodeId >= ast->len)
    {
        return;
    }
    if (ast->nodes[initNodeId].kind == HOPAst_CAST || ast->nodes[initNodeId].kind == HOPAst_CALL) {
        return;
    }
    HOPFmtInferredTypeInit(&initType);
    if (!HOPFmtInferExprTypeEx(ast, src, initNodeId, declStart, 0u, &initType)) {
        return;
    }
    if (HOPFmtInferredTypeMatchesNode(ast, src, &initType, typeNodeId)) {
        *ioTypeNodeId = -1;
    }
}

static int32_t HOPFmtFindEnclosingFnNode(const HOPAst* ast, int32_t nodeId) {
    const HOPAstNode* target;
    int32_t           best = -1;
    uint32_t          bestSpan = UINT32_MAX;
    uint32_t          i;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    target = &ast->nodes[nodeId];
    for (i = 0; i < ast->len; i++) {
        const HOPAstNode* n = &ast->nodes[i];
        if (n->kind != HOPAst_FN) {
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

static int HOPFmtFnHasImplicitContextLocal(const HOPAst* ast, HOPStrView src, int32_t fnNodeId) {
    const HOPAstNode* fn;
    int32_t           child;
    if (fnNodeId < 0 || (uint32_t)fnNodeId >= ast->len) {
        return 0;
    }
    fn = &ast->nodes[fnNodeId];
    if (fn->kind != HOPAst_FN) {
        return 0;
    }
    if (HOPFmtSliceEqLiteral(src, fn->dataStart, fn->dataEnd, "main")) {
        return 1;
    }
    child = HOPFmtFirstChild(ast, fnNodeId);
    while (child >= 0) {
        if (ast->nodes[child].kind == HOPAst_CONTEXT_CLAUSE) {
            return 1;
        }
        child = HOPFmtNextSibling(ast, child);
    }
    return 0;
}

static int HOPFmtFnHasShadowingContextLocalBefore(
    const HOPAst* ast, HOPStrView src, int32_t fnNodeId, uint32_t beforePos) {
    const HOPAstNode* fn;
    uint32_t          i;
    if (fnNodeId < 0 || (uint32_t)fnNodeId >= ast->len) {
        return 0;
    }
    fn = &ast->nodes[fnNodeId];
    if (fn->kind != HOPAst_FN) {
        return 0;
    }
    for (i = 0; i < ast->len; i++) {
        const HOPAstNode* n = &ast->nodes[i];
        if (n->start < fn->start || n->end > fn->end || n->start >= beforePos) {
            continue;
        }
        if (HOPFmtNodeDeclaresNameLiteral(ast, src, (int32_t)i, "context")) {
            return 1;
        }
    }
    return 0;
}

static int HOPFmtExprIsImplicitContextFieldForBind(
    const HOPAst* ast, HOPStrView src, const HOPAstNode* bindNode, int32_t exprNodeId) {
    const HOPAstNode* exprNode;
    int32_t           baseNodeId;
    uint32_t          baseStart;
    uint32_t          baseEnd;
    if (exprNodeId < 0 || (uint32_t)exprNodeId >= ast->len) {
        return 0;
    }
    exprNode = &ast->nodes[exprNodeId];
    if (exprNode->kind != HOPAst_FIELD_EXPR) {
        return 0;
    }
    if (!HOPFmtSlicesEqual(
            src, bindNode->dataStart, bindNode->dataEnd, exprNode->dataStart, exprNode->dataEnd))
    {
        return 0;
    }
    baseNodeId = HOPFmtFirstChild(ast, exprNodeId);
    if (!HOPFmtExprIsPlainIdent(ast, baseNodeId, &baseStart, &baseEnd)) {
        return 0;
    }
    return HOPFmtSliceEqLiteral(src, baseStart, baseEnd, "context");
}

typedef int (*HOPFmtExprRewriteRule)(const HOPAst* ast, HOPStrView src, int32_t* exprNodeId);

static int HOPFmtRewriteExprIdentity(const HOPAst* ast, HOPStrView src, int32_t* exprNodeId) {
    (void)ast;
    (void)src;
    (void)exprNodeId;
    return 0;
}

static int HOPFmtRewriteExpr(const HOPAst* ast, HOPStrView src, int32_t* exprNodeId) {
    static const HOPFmtExprRewriteRule rules[] = {
        HOPFmtRewriteExprIdentity,
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

static int HOPFmtRewriteCallArgShorthand(const HOPAst* ast, HOPStrView src, int32_t nodeId) {
    const HOPAstNode* node;
    HOPAstNode*       mutNode;
    int32_t           exprNode;
    uint32_t          identStart;
    uint32_t          identEnd;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    node = &ast->nodes[nodeId];
    if (node->kind != HOPAst_CALL_ARG || node->dataEnd <= node->dataStart) {
        return 0;
    }
    exprNode = HOPFmtFirstChild(ast, nodeId);
    if (exprNode < 0) {
        return 0;
    }
    if (HOPFmtRewriteExpr(ast, src, &exprNode) != 0) {
        return -1;
    }
    if (!HOPFmtExprIsPlainIdent(ast, exprNode, &identStart, &identEnd)) {
        return 0;
    }
    if (!HOPFmtSlicesEqual(src, node->dataStart, node->dataEnd, identStart, identEnd)) {
        return 0;
    }
    mutNode = (HOPAstNode*)&ast->nodes[nodeId];
    mutNode->dataStart = 0;
    mutNode->dataEnd = 0;
    return 0;
}

static int HOPFmtRewriteCompoundFieldShorthand(const HOPAst* ast, HOPStrView src, int32_t nodeId) {
    const HOPAstNode* node;
    HOPAstNode*       mutNode;
    int32_t           exprNode;
    uint32_t          identStart;
    uint32_t          identEnd;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    node = &ast->nodes[nodeId];
    if (node->kind != HOPAst_COMPOUND_FIELD
        || (node->flags & HOPAstFlag_COMPOUND_FIELD_SHORTHAND) != 0
        || node->dataEnd <= node->dataStart || node->dataEnd > src.len)
    {
        return 0;
    }
    if (HOPFmtSliceHasChar(src, node->dataStart, node->dataEnd, '.')) {
        return 0;
    }
    exprNode = HOPFmtFirstChild(ast, nodeId);
    if (exprNode < 0) {
        return 0;
    }
    if (HOPFmtRewriteExpr(ast, src, &exprNode) != 0) {
        return -1;
    }
    if (!HOPFmtExprIsPlainIdent(ast, exprNode, &identStart, &identEnd)) {
        return 0;
    }
    if (!HOPFmtSlicesEqual(src, node->dataStart, node->dataEnd, identStart, identEnd)) {
        return 0;
    }
    mutNode = (HOPAstNode*)&ast->nodes[nodeId];
    mutNode->flags |= HOPAstFlag_COMPOUND_FIELD_SHORTHAND;
    return 0;
}

static int HOPFmtRewriteContextBindShorthand(const HOPAst* ast, HOPStrView src, int32_t nodeId) {
    const HOPAstNode* node;
    HOPAstNode*       mutNode;
    int32_t           exprNode;
    int32_t           enclosingFn;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    node = &ast->nodes[nodeId];
    if (node->kind != HOPAst_CONTEXT_BIND || node->dataEnd <= node->dataStart
        || (node->flags & HOPAstFlag_CONTEXT_BIND_SHORTHAND) != 0)
    {
        return 0;
    }
    exprNode = HOPFmtFirstChild(ast, nodeId);
    if (exprNode < 0) {
        return 0;
    }
    if (HOPFmtRewriteExpr(ast, src, &exprNode) != 0) {
        return -1;
    }
    if (!HOPFmtExprIsImplicitContextFieldForBind(ast, src, node, exprNode)) {
        return 0;
    }
    enclosingFn = HOPFmtFindEnclosingFnNode(ast, nodeId);
    if (enclosingFn < 0 || !HOPFmtFnHasImplicitContextLocal(ast, src, enclosingFn)
        || HOPFmtFnHasShadowingContextLocalBefore(ast, src, enclosingFn, node->start))
    {
        return 0;
    }
    mutNode = (HOPAstNode*)&ast->nodes[nodeId];
    mutNode->flags |= HOPAstFlag_CONTEXT_BIND_SHORTHAND;
    return 0;
}

static int HOPFmtRewriteDropRedundantCastParenFlag(const HOPAst* ast, int32_t nodeId) {
    const HOPAstNode* n;
    HOPAstNode*       mutNode;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    n = &ast->nodes[nodeId];
    if (n->kind != HOPAst_CAST || (n->flags & HOPAstFlag_PAREN) == 0) {
        return 0;
    }
    mutNode = (HOPAstNode*)&ast->nodes[nodeId];
    mutNode->flags &= ~HOPAstFlag_PAREN;
    return 0;
}

static int HOPFmtRewriteBinaryCastParens(const HOPAst* ast, int32_t nodeId) {
    int32_t lhs;
    int32_t rhs;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    if (ast->nodes[nodeId].kind != HOPAst_BINARY) {
        return 0;
    }
    lhs = HOPFmtFirstChild(ast, nodeId);
    rhs = lhs >= 0 ? HOPFmtNextSibling(ast, lhs) : -1;
    /* Cast is postfix and binds tighter than all infix binary operators. */
    if (HOPFmtRewriteDropRedundantCastParenFlag(ast, lhs) != 0
        || HOPFmtRewriteDropRedundantCastParenFlag(ast, rhs) != 0)
    {
        return -1;
    }
    return 0;
}

static int HOPFmtRewriteRedundantLiteralCast(
    const HOPAst* ast, HOPStrView src, const HOPFormatOptions* _Nullable options, int32_t nodeId) {
    const HOPAstNode* node;
    HOPAstNode*       mutNode;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    node = &ast->nodes[nodeId];
    if (node->kind != HOPAst_CAST) {
        return 0;
    }
    if (!HOPFmtCanDropRedundantLiteralCast(ast, src, options, nodeId)) {
        return 0;
    }
    mutNode = (HOPAstNode*)&ast->nodes[nodeId];
    mutNode->flags |= HOPFmtFlag_DROP_REDUNDANT_LITERAL_CAST;
    return 0;
}

static int HOPFmtRewriteReturnParens(const HOPAst* ast, int32_t nodeId) {
    int32_t           exprNodeId;
    const HOPAstNode* exprNode;
    HOPAstNode*       mutExprNode;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    if (ast->nodes[nodeId].kind != HOPAst_RETURN) {
        return 0;
    }
    exprNodeId = HOPFmtFirstChild(ast, nodeId);
    if (exprNodeId < 0 || (uint32_t)exprNodeId >= ast->len) {
        return 0;
    }
    exprNode = &ast->nodes[exprNodeId];
    mutExprNode = (HOPAstNode*)exprNode;
    if ((exprNode->flags & HOPAstFlag_PAREN) != 0) {
        mutExprNode->flags &= ~HOPAstFlag_PAREN;
        exprNode = mutExprNode;
    }
    if (exprNode->kind == HOPAst_TUPLE_EXPR && (exprNode->flags & HOPAstFlag_PAREN) == 0) {
        mutExprNode->kind = HOPAst_EXPR_LIST;
    }
    return 0;
}

static int HOPFmtRewriteAst(
    const HOPAst* ast, HOPStrView src, const HOPFormatOptions* _Nullable options) {
    uint32_t i;
    if (ast == NULL || ast->nodes == NULL) {
        return -1;
    }
    for (i = 0; i < ast->len; i++) {
        int32_t nodeId = (int32_t)i;
        if (HOPFmtRewriteCallArgShorthand(ast, src, nodeId) != 0
            || HOPFmtRewriteCompoundFieldShorthand(ast, src, nodeId) != 0
            || HOPFmtRewriteContextBindShorthand(ast, src, nodeId) != 0
            || HOPFmtRewriteRedundantLiteralCast(ast, src, options, nodeId) != 0
            || HOPFmtRewriteBinaryCastParens(ast, nodeId) != 0
            || HOPFmtRewriteReturnParens(ast, nodeId) != 0)
        {
            return -1;
        }
    }
    return 0;
}

static int HOPFmtIsTypeNodeKind(HOPAstKind kind) {
    return HOPFmtIsTypeNodeKindRaw(kind);
}

static int HOPFmtIsStmtNodeKind(HOPAstKind kind) {
    switch (kind) {
        case HOPAst_BLOCK:
        case HOPAst_VAR:
        case HOPAst_CONST:
        case HOPAst_CONST_BLOCK:
        case HOPAst_IF:
        case HOPAst_FOR:
        case HOPAst_SWITCH:
        case HOPAst_RETURN:
        case HOPAst_BREAK:
        case HOPAst_CONTINUE:
        case HOPAst_DEFER:
        case HOPAst_ASSERT:
        case HOPAst_DEL:
        case HOPAst_MULTI_ASSIGN:
        case HOPAst_SHORT_ASSIGN:
        case HOPAst_EXPR_STMT:    return 1;
        default:                  return 0;
    }
}

static int HOPFmtIsGroupedVarLike(const HOPFmtCtx* c, int32_t nodeId) {
    int32_t firstChild;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return 0;
    }
    firstChild = HOPFmtFirstChild(c->ast, nodeId);
    return firstChild >= 0 && c->ast->nodes[firstChild].kind == HOPAst_NAME_LIST;
}

static int HOPFmtIsAssignmentOp(HOPTokenKind kind) {
    switch (kind) {
        case HOPTok_ASSIGN:
        case HOPTok_ADD_ASSIGN:
        case HOPTok_SUB_ASSIGN:
        case HOPTok_MUL_ASSIGN:
        case HOPTok_DIV_ASSIGN:
        case HOPTok_MOD_ASSIGN:
        case HOPTok_AND_ASSIGN:
        case HOPTok_OR_ASSIGN:
        case HOPTok_XOR_ASSIGN:
        case HOPTok_LSHIFT_ASSIGN:
        case HOPTok_RSHIFT_ASSIGN: return 1;
        default:                   return 0;
    }
}

static int HOPFmtBinPrec(HOPTokenKind kind) {
    if (HOPFmtIsAssignmentOp(kind)) {
        return 1;
    }
    switch (kind) {
        case HOPTok_LOGICAL_OR:  return 2;
        case HOPTok_LOGICAL_AND: return 3;
        case HOPTok_EQ:
        case HOPTok_NEQ:
        case HOPTok_LT:
        case HOPTok_GT:
        case HOPTok_LTE:
        case HOPTok_GTE:         return 4;
        case HOPTok_OR:
        case HOPTok_XOR:
        case HOPTok_ADD:
        case HOPTok_SUB:         return 5;
        case HOPTok_AND:
        case HOPTok_LSHIFT:
        case HOPTok_RSHIFT:
        case HOPTok_MUL:
        case HOPTok_DIV:
        case HOPTok_MOD:         return 6;
        default:                 return 0;
    }
}

static const char* HOPFmtTokenOpText(HOPTokenKind kind) {
    switch (kind) {
        case HOPTok_ASSIGN:        return "=";
        case HOPTok_ADD:           return "+";
        case HOPTok_SUB:           return "-";
        case HOPTok_MUL:           return "*";
        case HOPTok_DIV:           return "/";
        case HOPTok_MOD:           return "%";
        case HOPTok_AND:           return "&";
        case HOPTok_OR:            return "|";
        case HOPTok_XOR:           return "^";
        case HOPTok_NOT:           return "!";
        case HOPTok_LSHIFT:        return "<<";
        case HOPTok_RSHIFT:        return ">>";
        case HOPTok_EQ:            return "==";
        case HOPTok_NEQ:           return "!=";
        case HOPTok_LT:            return "<";
        case HOPTok_GT:            return ">";
        case HOPTok_LTE:           return "<=";
        case HOPTok_GTE:           return ">=";
        case HOPTok_LOGICAL_AND:   return "&&";
        case HOPTok_LOGICAL_OR:    return "||";
        case HOPTok_SHORT_ASSIGN:  return ":=";
        case HOPTok_ADD_ASSIGN:    return "+=";
        case HOPTok_SUB_ASSIGN:    return "-=";
        case HOPTok_MUL_ASSIGN:    return "*=";
        case HOPTok_DIV_ASSIGN:    return "/=";
        case HOPTok_MOD_ASSIGN:    return "%=";
        case HOPTok_AND_ASSIGN:    return "&=";
        case HOPTok_OR_ASSIGN:     return "|=";
        case HOPTok_XOR_ASSIGN:    return "^=";
        case HOPTok_LSHIFT_ASSIGN: return "<<=";
        case HOPTok_RSHIFT_ASSIGN: return ">>=";
        case HOPTok_AS:            return "as";
        default:                   return "?";
    }
}

static int HOPFmtContainsSemicolonInRange(HOPStrView src, uint32_t start, uint32_t end) {
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

static int HOPFmtRangeHasChar(HOPStrView src, uint32_t start, uint32_t end, char ch) {
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

static int HOPFmtFindCharForwardInRange(
    HOPStrView src, uint32_t start, uint32_t end, char ch, uint32_t* outPos) {
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

static int HOPFmtFindCharBackwardInRange(
    HOPStrView src, uint32_t start, uint32_t end, char ch, uint32_t* outPos) {
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

static uint32_t HOPFmtTrimSliceEnd(const char* s, uint32_t start, uint32_t end) {
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

static int HOPFmtEmitCommentText(HOPFmtCtx* c, const HOPComment* cm) {
    uint32_t end = HOPFmtTrimSliceEnd(c->src.ptr, cm->start, cm->end);
    if (end < cm->start) {
        end = cm->start;
    }
    return HOPFmtWriteSlice(c, cm->start, end);
}

static int HOPFmtIsLeadingCommentForNode(const HOPFmtCtx* c, const HOPComment* cm, int32_t nodeId) {
    const HOPAstNode* node;
    const HOPAstNode* anchor;
    if (c == NULL || cm == NULL || nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return 0;
    }
    if (cm->attachment != HOPCommentAttachment_LEADING
        && cm->attachment != HOPCommentAttachment_FLOATING)
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

static int HOPFmtEmitLeadingCommentsForNode(HOPFmtCtx* c, int32_t nodeId) {
    uint32_t i;
    for (i = 0; i < c->commentLen; i++) {
        const HOPComment* cm = &c->comments[i];
        if (c->commentUsed[i]) {
            continue;
        }
        if (!HOPFmtIsLeadingCommentForNode(c, cm, nodeId)) {
            continue;
        }
        if (!c->lineStart) {
            if (HOPFmtNewline(c) != 0) {
                return -1;
            }
        }
        if (HOPFmtEmitCommentText(c, cm) != 0 || HOPFmtNewline(c) != 0) {
            return -1;
        }
        c->commentUsed[i] = 1;
    }
    return 0;
}

static int HOPFmtEmitTrailingCommentsForNode(HOPFmtCtx* c, int32_t nodeId) {
    uint32_t i;
    int      first = 1;
    for (i = 0; i < c->commentLen; i++) {
        const HOPComment* cm = &c->comments[i];
        if (c->commentUsed[i]) {
            continue;
        }
        if (cm->anchorNode != nodeId || cm->attachment != HOPCommentAttachment_TRAILING) {
            continue;
        }
        if (first) {
            if (HOPFmtWriteChar(c, ' ') != 0 || HOPFmtEmitCommentText(c, cm) != 0) {
                return -1;
            }
            first = 0;
        } else {
            if (HOPFmtNewline(c) != 0 || HOPFmtEmitCommentText(c, cm) != 0) {
                return -1;
            }
        }
        c->commentUsed[i] = 1;
    }
    return 0;
}

static int HOPFmtEmitRemainingComments(HOPFmtCtx* c) {
    uint32_t i;
    for (i = 0; i < c->commentLen; i++) {
        if (c->commentUsed[i]) {
            continue;
        }
        if (!c->lineStart && HOPFmtNewline(c) != 0) {
            return -1;
        }
        if (HOPFmtEmitCommentText(c, &c->comments[i]) != 0 || HOPFmtNewline(c) != 0) {
            return -1;
        }
        c->commentUsed[i] = 1;
    }
    return 0;
}

static uint32_t HOPFmtCountNewlinesInRange(HOPStrView src, uint32_t start, uint32_t end) {
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

static int HOPFmtGapHasIntentionalBlankLine(HOPStrView src, uint32_t start, uint32_t end) {
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

static int HOPFmtNodeContainsAnchor(const HOPAst* ast, int32_t nodeId, int32_t anchorNodeId) {
    const HOPAstNode* n;
    const HOPAstNode* a;
    if (nodeId < 0 || anchorNodeId < 0 || (uint32_t)nodeId >= ast->len
        || (uint32_t)anchorNodeId >= ast->len)
    {
        return 0;
    }
    n = &ast->nodes[nodeId];
    a = &ast->nodes[anchorNodeId];
    return a->start >= n->start && a->end <= n->end;
}

static int HOPFmtCommentAnchoredToAnyNode(
    const HOPAst* ast, const HOPComment* cm, const int32_t* nodeIds, uint32_t nodeLen) {
    uint32_t i;
    for (i = 0; i < nodeLen; i++) {
        if (HOPFmtNodeContainsAnchor(ast, nodeIds[i], cm->anchorNode)) {
            return 1;
        }
    }
    return 0;
}

static int HOPFmtHasUnusedLeadingCommentsForNode(const HOPFmtCtx* c, int32_t nodeId) {
    uint32_t i;
    for (i = 0; i < c->commentLen; i++) {
        const HOPComment* cm = &c->comments[i];
        if (c->commentUsed[i]) {
            continue;
        }
        if (HOPFmtIsLeadingCommentForNode(c, cm, nodeId)) {
            return 1;
        }
    }
    return 0;
}

static int HOPFmtHasUnusedTrailingCommentsForNodes(
    const HOPFmtCtx* c, const int32_t* nodeIds, uint32_t nodeLen) {
    uint32_t i;
    for (i = 0; i < c->commentLen; i++) {
        const HOPComment* cm = &c->comments[i];
        if (c->commentUsed[i] || cm->attachment != HOPCommentAttachment_TRAILING) {
            continue;
        }
        if (HOPFmtCommentAnchoredToAnyNode(c->ast, cm, nodeIds, nodeLen)) {
            return 1;
        }
    }
    return 0;
}

static int HOPFmtCommentWithinNodeRange(const HOPAst* ast, const HOPComment* cm, int32_t nodeId) {
    const HOPAstNode* n;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return 0;
    }
    n = &ast->nodes[nodeId];
    return cm->start >= n->start && cm->start < n->end;
}

static int HOPFmtHasUnusedTrailingCommentsInNodeRange(const HOPFmtCtx* c, int32_t nodeId) {
    uint32_t i;
    for (i = 0; i < c->commentLen; i++) {
        const HOPComment* cm = &c->comments[i];
        if (c->commentUsed[i] || cm->attachment != HOPCommentAttachment_TRAILING) {
            continue;
        }
        if (HOPFmtCommentWithinNodeRange(c->ast, cm, nodeId)) {
            return 1;
        }
    }
    return 0;
}

static int HOPFmtEmitTrailingCommentsInNodeRange(HOPFmtCtx* c, int32_t nodeId, uint32_t padBefore) {
    uint32_t i;
    int      first = 1;
    for (i = 0; i < c->commentLen; i++) {
        const HOPComment* cm = &c->comments[i];
        if (c->commentUsed[i] || cm->attachment != HOPCommentAttachment_TRAILING) {
            continue;
        }
        if (!HOPFmtCommentWithinNodeRange(c->ast, cm, nodeId)) {
            continue;
        }
        if (first) {
            uint32_t pad = padBefore > 0 ? padBefore : 1u;
            if (HOPFmtWriteSpaces(c, pad) != 0 || HOPFmtEmitCommentText(c, cm) != 0) {
                return -1;
            }
            first = 0;
        } else {
            if (HOPFmtNewline(c) != 0 || HOPFmtEmitCommentText(c, cm) != 0) {
                return -1;
            }
        }
        c->commentUsed[i] = 1;
    }
    return 0;
}

static int HOPFmtEmitTrailingCommentsForNodes(
    HOPFmtCtx* c, const int32_t* nodeIds, uint32_t nodeLen, uint32_t padBefore) {
    uint32_t i;
    int      first = 1;
    for (i = 0; i < c->commentLen; i++) {
        const HOPComment* cm = &c->comments[i];
        if (c->commentUsed[i] || cm->attachment != HOPCommentAttachment_TRAILING) {
            continue;
        }
        if (!HOPFmtCommentAnchoredToAnyNode(c->ast, cm, nodeIds, nodeLen)) {
            continue;
        }
        if (first) {
            uint32_t pad = padBefore > 0 ? padBefore : 1u;
            if (HOPFmtWriteSpaces(c, pad) != 0 || HOPFmtEmitCommentText(c, cm) != 0) {
                return -1;
            }
            first = 0;
        } else {
            if (HOPFmtNewline(c) != 0 || HOPFmtEmitCommentText(c, cm) != 0) {
                return -1;
            }
        }
        c->commentUsed[i] = 1;
    }
    return 0;
}

static int HOPFmtFindSourceTrailingLineComment(
    const HOPFmtCtx* c, int32_t nodeId, uint32_t* outStart, uint32_t* outEnd) {
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
            *outEnd = HOPFmtTrimSliceEnd(c->src.ptr, i, lineEnd);
            return 1;
        }
        if (c->src.ptr[i] != ' ' && c->src.ptr[i] != '\t' && c->src.ptr[i] != '\r') {
            return 0;
        }
        i++;
    }
    return 0;
}

static void HOPFmtMarkCommentUsedAtStart(HOPFmtCtx* c, uint32_t start) {
    uint32_t i;
    for (i = 0; i < c->commentLen; i++) {
        if (c->comments[i].start == start) {
            c->commentUsed[i] = 1;
            return;
        }
    }
}

static int HOPFmtEmitType(HOPFmtCtx* c, int32_t nodeId);
static int HOPFmtEmitExpr(HOPFmtCtx* c, int32_t nodeId, int forceParen);
static int HOPFmtEmitBlock(HOPFmtCtx* c, int32_t nodeId);
static int HOPFmtEmitStmtInline(HOPFmtCtx* c, int32_t nodeId);
static int HOPFmtEmitDecl(HOPFmtCtx* c, int32_t nodeId);
static int HOPFmtEmitDirectiveGroup(HOPFmtCtx* c, int32_t firstDirective, int32_t* outNext);
static int HOPFmtEmitDirective(HOPFmtCtx* c, int32_t nodeId);
static int HOPFmtEmitAggregateFieldBody(HOPFmtCtx* c, int32_t firstFieldNodeId);
static int HOPFmtEmitExprList(HOPFmtCtx* c, int32_t listNodeId);

static int HOPFmtEmitCompoundFieldWithAlign(HOPFmtCtx* c, int32_t nodeId, uint32_t maxKeyLen) {
    const HOPAstNode* n;
    int32_t           exprNode;
    uint32_t          keyLen;
    uint32_t          pad;

    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return -1;
    }
    n = &c->ast->nodes[nodeId];
    if (n->kind != HOPAst_COMPOUND_FIELD) {
        return -1;
    }
    exprNode = HOPFmtFirstChild(c->ast, nodeId);
    if ((n->flags & HOPAstFlag_COMPOUND_FIELD_SHORTHAND) != 0) {
        return HOPFmtWriteSlice(c, n->dataStart, n->dataEnd);
    }
    keyLen = n->dataEnd - n->dataStart;
    if (HOPFmtWriteSlice(c, n->dataStart, n->dataEnd) != 0 || HOPFmtWriteChar(c, ':') != 0) {
        return -1;
    }
    if (maxKeyLen > keyLen) {
        pad = (maxKeyLen - keyLen) + 1u;
        if (HOPFmtWriteSpaces(c, pad) != 0) {
            return -1;
        }
    } else if (HOPFmtWriteChar(c, ' ') != 0) {
        return -1;
    }
    return exprNode >= 0 ? HOPFmtEmitExpr(c, exprNode, 0) : 0;
}

static int HOPFmtEmitTypeParamList(HOPFmtCtx* c, int32_t* ioChild) {
    int32_t cur;
    int     first = 1;
    if (ioChild == NULL) {
        return -1;
    }
    cur = *ioChild;
    if (cur < 0 || c->ast->nodes[cur].kind != HOPAst_TYPE_PARAM) {
        return 0;
    }
    if (HOPFmtWriteChar(c, '[') != 0) {
        return -1;
    }
    while (cur >= 0 && c->ast->nodes[cur].kind == HOPAst_TYPE_PARAM) {
        if (!first && HOPFmtWriteCStr(c, ", ") != 0) {
            return -1;
        }
        if (HOPFmtWriteSlice(c, c->ast->nodes[cur].dataStart, c->ast->nodes[cur].dataEnd) != 0) {
            return -1;
        }
        first = 0;
        cur = HOPFmtNextSibling(c->ast, cur);
    }
    if (HOPFmtWriteChar(c, ']') != 0) {
        return -1;
    }
    *ioChild = cur;
    return 0;
}

static int HOPFmtEmitType(HOPFmtCtx* c, int32_t nodeId) {
    const HOPAstNode* n;
    int32_t           ch;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return -1;
    }
    n = &c->ast->nodes[nodeId];
    switch (n->kind) {
        case HOPAst_TYPE_NAME:
            if (HOPFmtWriteSlice(c, n->dataStart, n->dataEnd) != 0) {
                return -1;
            }
            ch = HOPFmtFirstChild(c->ast, nodeId);
            if (ch >= 0) {
                int first = 1;
                if (HOPFmtWriteChar(c, '[') != 0) {
                    return -1;
                }
                while (ch >= 0) {
                    if (!first && HOPFmtWriteCStr(c, ", ") != 0) {
                        return -1;
                    }
                    if (HOPFmtEmitType(c, ch) != 0) {
                        return -1;
                    }
                    first = 0;
                    ch = HOPFmtNextSibling(c->ast, ch);
                }
                if (HOPFmtWriteChar(c, ']') != 0) {
                    return -1;
                }
            }
            return 0;
        case HOPAst_TYPE_PARAM: return HOPFmtWriteSlice(c, n->dataStart, n->dataEnd);
        case HOPAst_TYPE_OPTIONAL:
            ch = HOPFmtFirstChild(c->ast, nodeId);
            if (HOPFmtWriteChar(c, '?') != 0) {
                return -1;
            }
            return ch >= 0 ? HOPFmtEmitType(c, ch) : 0;
        case HOPAst_TYPE_PTR:
            ch = HOPFmtFirstChild(c->ast, nodeId);
            if (HOPFmtWriteChar(c, '*') != 0) {
                return -1;
            }
            return ch >= 0 ? HOPFmtEmitType(c, ch) : 0;
        case HOPAst_TYPE_REF:
            ch = HOPFmtFirstChild(c->ast, nodeId);
            if (HOPFmtWriteChar(c, '&') != 0) {
                return -1;
            }
            return ch >= 0 ? HOPFmtEmitType(c, ch) : 0;
        case HOPAst_TYPE_MUTREF:
            ch = HOPFmtFirstChild(c->ast, nodeId);
            if (HOPFmtWriteCStr(c, "&mut ") != 0) {
                return -1;
            }
            return ch >= 0 ? HOPFmtEmitType(c, ch) : 0;
        case HOPAst_TYPE_SLICE:
            ch = HOPFmtFirstChild(c->ast, nodeId);
            if (HOPFmtWriteChar(c, '[') != 0 || (ch >= 0 && HOPFmtEmitType(c, ch) != 0)
                || HOPFmtWriteChar(c, ']') != 0)
            {
                return -1;
            }
            return 0;
        case HOPAst_TYPE_MUTSLICE:
            ch = HOPFmtFirstChild(c->ast, nodeId);
            if (HOPFmtWriteCStr(c, "[mut ") != 0 || (ch >= 0 && HOPFmtEmitType(c, ch) != 0)
                || HOPFmtWriteChar(c, ']') != 0)
            {
                return -1;
            }
            return 0;
        case HOPAst_TYPE_ARRAY:
        case HOPAst_TYPE_VARRAY:
            ch = HOPFmtFirstChild(c->ast, nodeId);
            if (HOPFmtWriteChar(c, '[') != 0 || (ch >= 0 && HOPFmtEmitType(c, ch) != 0)
                || HOPFmtWriteChar(c, ' ') != 0)
            {
                return -1;
            }
            if (n->kind == HOPAst_TYPE_VARRAY && HOPFmtWriteChar(c, '.') != 0) {
                return -1;
            }
            if (HOPFmtWriteSlice(c, n->dataStart, n->dataEnd) != 0 || HOPFmtWriteChar(c, ']') != 0)
            {
                return -1;
            }
            return 0;
        case HOPAst_TYPE_FN: {
            int32_t retType = -1;
            int32_t cur = HOPFmtFirstChild(c->ast, nodeId);
            int     first = 1;
            if (HOPFmtWriteCStr(c, "fn(") != 0) {
                return -1;
            }
            while (cur >= 0) {
                const HOPAstNode* chn = &c->ast->nodes[cur];
                if (chn->flags == 1 && HOPFmtIsTypeNodeKind(chn->kind)) {
                    retType = cur;
                    break;
                }
                if (!first && HOPFmtWriteCStr(c, ", ") != 0) {
                    return -1;
                }
                if ((chn->flags & HOPAstFlag_PARAM_CONST) != 0 && HOPFmtWriteCStr(c, "const ") != 0)
                {
                    return -1;
                }
                if ((chn->flags & HOPAstFlag_PARAM_VARIADIC) != 0 && HOPFmtWriteCStr(c, "...") != 0)
                {
                    return -1;
                }
                if (HOPFmtEmitType(c, cur) != 0) {
                    return -1;
                }
                first = 0;
                cur = HOPFmtNextSibling(c->ast, cur);
            }
            if (HOPFmtWriteChar(c, ')') != 0) {
                return -1;
            }
            if (retType >= 0) {
                if (HOPFmtWriteChar(c, ' ') != 0 || HOPFmtEmitType(c, retType) != 0) {
                    return -1;
                }
            }
            return 0;
        }
        case HOPAst_TYPE_ANON_STRUCT:
        case HOPAst_TYPE_ANON_UNION:  {
            int32_t field = HOPFmtFirstChild(c->ast, nodeId);
            if (HOPFmtCountNewlinesInRange(c->src, n->start, n->end) == 0u) {
                return HOPFmtWriteSlice(c, n->start, n->end);
            }
            if (n->kind == HOPAst_TYPE_ANON_UNION) {
                if (HOPFmtWriteCStr(c, "union ") != 0) {
                    return -1;
                }
            } else if (HOPFmtWriteCStr(c, "struct ") != 0) {
                return -1;
            }
            if (HOPFmtWriteChar(c, '{') != 0) {
                return -1;
            }
            if (field >= 0 && HOPFmtEmitAggregateFieldBody(c, field) != 0) {
                return -1;
            }
            return HOPFmtWriteChar(c, '}');
        }
        case HOPAst_TYPE_TUPLE: {
            int32_t cur = HOPFmtFirstChild(c->ast, nodeId);
            int     first = 1;
            if (HOPFmtWriteChar(c, '(') != 0) {
                return -1;
            }
            while (cur >= 0) {
                if (!first && HOPFmtWriteCStr(c, ", ") != 0) {
                    return -1;
                }
                if (HOPFmtEmitType(c, cur) != 0) {
                    return -1;
                }
                first = 0;
                cur = HOPFmtNextSibling(c->ast, cur);
            }
            return HOPFmtWriteChar(c, ')');
        }
        default: return HOPFmtWriteSlice(c, n->start, n->end);
    }
}

static int HOPFmtEmitExprList(HOPFmtCtx* c, int32_t listNodeId) {
    uint32_t i;
    uint32_t exprCount;
    if (listNodeId < 0 || (uint32_t)listNodeId >= c->ast->len
        || c->ast->nodes[listNodeId].kind != HOPAst_EXPR_LIST)
    {
        return -1;
    }
    exprCount = HOPFmtListCount(c->ast, listNodeId);
    for (i = 0; i < exprCount; i++) {
        int32_t exprNode = HOPFmtListItemAt(c->ast, listNodeId, i);
        if (exprNode < 0) {
            return -1;
        }
        if (i > 0 && HOPFmtWriteCStr(c, ", ") != 0) {
            return -1;
        }
        if (HOPFmtEmitExpr(c, exprNode, 0) != 0) {
            return -1;
        }
    }
    return 0;
}

static int HOPFmtExprNeedsParensForBinaryChild(
    const HOPAst* ast, int32_t parentId, int32_t childId, int rightChild) {
    const HOPAstNode* p;
    const HOPAstNode* ch;
    int               pp;
    int               cp;
    int               rightAssoc;
    if (childId < 0 || (uint32_t)childId >= ast->len || parentId < 0
        || (uint32_t)parentId >= ast->len)
    {
        return 0;
    }
    p = &ast->nodes[parentId];
    ch = &ast->nodes[childId];
    if (ch->kind != HOPAst_BINARY) {
        return 0;
    }
    pp = HOPFmtBinPrec((HOPTokenKind)p->op);
    cp = HOPFmtBinPrec((HOPTokenKind)ch->op);
    rightAssoc = HOPFmtIsAssignmentOp((HOPTokenKind)p->op);
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

static int HOPFmtUseTightMulDivModSpacing(
    HOPFmtCtx* c, int32_t nodeId, int32_t lhsNodeId, int32_t rhsNodeId) {
    const HOPAstNode* n;
    int32_t           curNodeId;
    int               curPrec;
    if (nodeId < 0 || lhsNodeId < 0 || rhsNodeId < 0 || (uint32_t)nodeId >= c->ast->len
        || (uint32_t)lhsNodeId >= c->ast->len || (uint32_t)rhsNodeId >= c->ast->len)
    {
        return 0;
    }
    n = &c->ast->nodes[nodeId];
    if (n->op != HOPTok_MUL && n->op != HOPTok_DIV && n->op != HOPTok_MOD) {
        return 0;
    }
    if ((n->flags & HOPAstFlag_PAREN) != 0) {
        return 0;
    }

    curNodeId = nodeId;
    curPrec = HOPFmtBinPrec((HOPTokenKind)n->op);
    while (curNodeId >= 0) {
        int32_t           parentNodeId = HOPFmtFindParentNode(c->ast, curNodeId);
        const HOPAstNode* parentNode;
        int               parentPrec;
        if (parentNodeId < 0) {
            break;
        }
        parentNode = &c->ast->nodes[parentNodeId];
        if (parentNode->kind != HOPAst_BINARY) {
            break;
        }
        if (HOPFmtIsAssignmentOp((HOPTokenKind)parentNode->op)) {
            break;
        }
        switch ((HOPTokenKind)parentNode->op) {
            case HOPTok_LOGICAL_OR:
            case HOPTok_LOGICAL_AND:
            case HOPTok_EQ:
            case HOPTok_NEQ:
            case HOPTok_LT:
            case HOPTok_GT:
            case HOPTok_LTE:
            case HOPTok_GTE:         return 0;
            default:                 break;
        }
        parentPrec = HOPFmtBinPrec((HOPTokenKind)parentNode->op);
        if (parentPrec > 0 && parentPrec < curPrec) {
            return 1;
        }
        if (parentPrec == 0 || parentPrec > curPrec) {
            break;
        }
        if ((parentNode->flags & HOPAstFlag_PAREN) != 0) {
            break;
        }
        curNodeId = parentNodeId;
    }
    return 0;
}

static int HOPFmtEmitExprCore(HOPFmtCtx* c, int32_t nodeId) {
    const HOPAstNode* n;
    int32_t           ch;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return -1;
    }
    n = &c->ast->nodes[nodeId];
    switch (n->kind) {
        case HOPAst_IDENT:
        case HOPAst_INT:
        case HOPAst_FLOAT:
        case HOPAst_BOOL:   return HOPFmtWriteSlice(c, n->dataStart, n->dataEnd);
        case HOPAst_STRING: return HOPFmtWriteSliceLiteral(c, n->dataStart, n->dataEnd);
        case HOPAst_RUNE:   return HOPFmtWriteSliceLiteral(c, n->dataStart, n->dataEnd);
        case HOPAst_NULL:   return HOPFmtWriteCStr(c, "null");
        case HOPAst_UNARY:  {
            const char* op = HOPFmtTokenOpText((HOPTokenKind)n->op);
            ch = HOPFmtFirstChild(c->ast, nodeId);
            if (HOPFmtWriteCStr(c, op) != 0) {
                return -1;
            }
            if (ch >= 0) {
                const HOPAstNode* cn = &c->ast->nodes[ch];
                int               need = cn->kind == HOPAst_BINARY;
                if (HOPFmtEmitExpr(c, ch, need) != 0) {
                    return -1;
                }
            }
            return 0;
        }
        case HOPAst_BINARY: {
            const char* op = HOPFmtTokenOpText((HOPTokenKind)n->op);
            int32_t     lhs = HOPFmtFirstChild(c->ast, nodeId);
            int32_t     rhs = lhs >= 0 ? HOPFmtNextSibling(c->ast, lhs) : -1;
            int         tightOp = HOPFmtUseTightMulDivModSpacing(c, nodeId, lhs, rhs);
            if (lhs >= 0
                && HOPFmtEmitExpr(
                       c, lhs, HOPFmtExprNeedsParensForBinaryChild(c->ast, nodeId, lhs, 0))
                       != 0)
            {
                return -1;
            }
            if (tightOp) {
                if (HOPFmtWriteCStr(c, op) != 0) {
                    return -1;
                }
            } else if (
                HOPFmtWriteChar(c, ' ') != 0 || HOPFmtWriteCStr(c, op) != 0
                || HOPFmtWriteChar(c, ' ') != 0)
            {
                return -1;
            }
            if (rhs >= 0
                && HOPFmtEmitExpr(
                       c, rhs, HOPFmtExprNeedsParensForBinaryChild(c->ast, nodeId, rhs, 1))
                       != 0)
            {
                return -1;
            }
            return 0;
        }
        case HOPAst_CALL: {
            int32_t arg;
            ch = HOPFmtFirstChild(c->ast, nodeId);
            if (ch >= 0 && HOPFmtEmitExpr(c, ch, 0) != 0) {
                return -1;
            }
            if (HOPFmtWriteChar(c, '(') != 0) {
                return -1;
            }
            arg = ch >= 0 ? HOPFmtNextSibling(c->ast, ch) : -1;
            while (arg >= 0) {
                int32_t next = HOPFmtNextSibling(c->ast, arg);
                if (HOPFmtEmitExpr(c, arg, 0) != 0) {
                    return -1;
                }
                if (next >= 0 && HOPFmtWriteCStr(c, ", ") != 0) {
                    return -1;
                }
                arg = next;
            }
            return HOPFmtWriteChar(c, ')');
        }
        case HOPAst_CALL_ARG: {
            int32_t exprNode = HOPFmtFirstChild(c->ast, nodeId);
            if (n->dataEnd > n->dataStart) {
                if (HOPFmtWriteSlice(c, n->dataStart, n->dataEnd) != 0
                    || HOPFmtWriteCStr(c, ": ") != 0)
                {
                    return -1;
                }
            }
            if (exprNode >= 0 && HOPFmtEmitExpr(c, exprNode, 0) != 0) {
                return -1;
            }
            if ((n->flags & HOPAstFlag_CALL_ARG_SPREAD) != 0) {
                return HOPFmtWriteCStr(c, "...");
            }
            return 0;
        }
        case HOPAst_TUPLE_EXPR: {
            int32_t cur = HOPFmtFirstChild(c->ast, nodeId);
            int     first = 1;
            if (HOPFmtWriteChar(c, '(') != 0) {
                return -1;
            }
            while (cur >= 0) {
                if (!first && HOPFmtWriteCStr(c, ", ") != 0) {
                    return -1;
                }
                if (HOPFmtEmitExpr(c, cur, 0) != 0) {
                    return -1;
                }
                first = 0;
                cur = HOPFmtNextSibling(c->ast, cur);
            }
            return HOPFmtWriteChar(c, ')');
        }
        case HOPAst_CALL_WITH_CONTEXT: {
            int32_t callNode = HOPFmtFirstChild(c->ast, nodeId);
            int32_t ctxNode = callNode >= 0 ? HOPFmtNextSibling(c->ast, callNode) : -1;
            if (callNode >= 0 && HOPFmtEmitExpr(c, callNode, 0) != 0) {
                return -1;
            }
            if (HOPFmtWriteCStr(c, " context ") != 0) {
                return -1;
            }
            if ((n->flags & HOPAstFlag_CALL_WITH_CONTEXT_PASSTHROUGH) != 0) {
                return HOPFmtWriteCStr(c, "context");
            }
            return ctxNode >= 0 ? HOPFmtEmitExpr(c, ctxNode, 0) : 0;
        }
        case HOPAst_CONTEXT_OVERLAY: {
            int32_t bind = HOPFmtFirstChild(c->ast, nodeId);
            if (HOPFmtWriteChar(c, '{') != 0) {
                return -1;
            }
            if (bind < 0) {
                return HOPFmtWriteChar(c, '}');
            }
            if (HOPFmtWriteChar(c, ' ') != 0) {
                return -1;
            }
            while (bind >= 0) {
                int32_t next = HOPFmtNextSibling(c->ast, bind);
                if (HOPFmtEmitExpr(c, bind, 0) != 0) {
                    return -1;
                }
                if (next >= 0 && HOPFmtWriteCStr(c, ", ") != 0) {
                    return -1;
                }
                bind = next;
            }
            return HOPFmtWriteCStr(c, " }");
        }
        case HOPAst_CONTEXT_BIND: {
            ch = HOPFmtFirstChild(c->ast, nodeId);
            if (HOPFmtWriteSlice(c, n->dataStart, n->dataEnd) != 0) {
                return -1;
            }
            if ((n->flags & HOPAstFlag_CONTEXT_BIND_SHORTHAND) == 0 && ch >= 0) {
                if (HOPFmtWriteCStr(c, ": ") != 0 || HOPFmtEmitExpr(c, ch, 0) != 0) {
                    return -1;
                }
            }
            return 0;
        }
        case HOPAst_INDEX: {
            int32_t base = HOPFmtFirstChild(c->ast, nodeId);
            int32_t a = base >= 0 ? HOPFmtNextSibling(c->ast, base) : -1;
            int32_t b = a >= 0 ? HOPFmtNextSibling(c->ast, a) : -1;
            if (base >= 0 && HOPFmtEmitExpr(c, base, 0) != 0) {
                return -1;
            }
            if (HOPFmtWriteChar(c, '[') != 0) {
                return -1;
            }
            if ((n->flags & HOPAstFlag_INDEX_SLICE) != 0) {
                if ((n->flags & HOPAstFlag_INDEX_HAS_START) != 0 && a >= 0
                    && HOPFmtEmitExpr(c, a, 0) != 0)
                {
                    return -1;
                }
                if (HOPFmtWriteChar(c, ':') != 0) {
                    return -1;
                }
                if ((n->flags & HOPAstFlag_INDEX_HAS_END) != 0) {
                    int32_t endNode = (n->flags & HOPAstFlag_INDEX_HAS_START) != 0 ? b : a;
                    if (endNode >= 0 && HOPFmtEmitExpr(c, endNode, 0) != 0) {
                        return -1;
                    }
                }
            } else if (a >= 0 && HOPFmtEmitExpr(c, a, 0) != 0) {
                return -1;
            }
            return HOPFmtWriteChar(c, ']');
        }
        case HOPAst_FIELD_EXPR:
            ch = HOPFmtFirstChild(c->ast, nodeId);
            if (ch >= 0 && HOPFmtEmitExpr(c, ch, 0) != 0) {
                return -1;
            }
            if (HOPFmtWriteChar(c, '.') != 0) {
                return -1;
            }
            return HOPFmtWriteSlice(c, n->dataStart, n->dataEnd);
        case HOPAst_CAST: {
            int32_t expr = HOPFmtFirstChild(c->ast, nodeId);
            int32_t type = expr >= 0 ? HOPFmtNextSibling(c->ast, expr) : -1;
            if ((n->flags & HOPFmtFlag_DROP_REDUNDANT_LITERAL_CAST) != 0) {
                return expr >= 0 ? HOPFmtEmitExpr(c, expr, 0) : 0;
            }
            if (expr >= 0 && HOPFmtEmitExpr(c, expr, 0) != 0) {
                return -1;
            }
            if (HOPFmtWriteCStr(c, " as ") != 0) {
                return -1;
            }
            return type >= 0 ? HOPFmtEmitType(c, type) : 0;
        }
        case HOPAst_SIZEOF:
            ch = HOPFmtFirstChild(c->ast, nodeId);
            if (HOPFmtWriteCStr(c, "sizeof(") != 0) {
                return -1;
            }
            if (ch >= 0) {
                if (n->flags == 1) {
                    if (HOPFmtEmitType(c, ch) != 0) {
                        return -1;
                    }
                } else if (HOPFmtEmitExpr(c, ch, 0) != 0) {
                    return -1;
                }
            }
            return HOPFmtWriteChar(c, ')');
        case HOPAst_NEW: {
            int32_t type = HOPFmtFirstChild(c->ast, nodeId);
            int32_t next = type >= 0 ? HOPFmtNextSibling(c->ast, type) : -1;
            int32_t count = -1;
            int32_t init = -1;
            int32_t alloc = -1;
            if ((n->flags & HOPAstFlag_NEW_HAS_COUNT) != 0) {
                count = next;
                next = count >= 0 ? HOPFmtNextSibling(c->ast, count) : -1;
            }
            if ((n->flags & HOPAstFlag_NEW_HAS_INIT) != 0) {
                init = next;
                next = init >= 0 ? HOPFmtNextSibling(c->ast, init) : -1;
            }
            if ((n->flags & HOPAstFlag_NEW_HAS_ALLOC) != 0) {
                alloc = next;
            }
            if (HOPFmtWriteCStr(c, "new ") != 0) {
                return -1;
            }
            if (count >= 0) {
                if (HOPFmtWriteChar(c, '[') != 0 || (type >= 0 && HOPFmtEmitType(c, type) != 0)
                    || HOPFmtWriteChar(c, ' ') != 0 || HOPFmtEmitExpr(c, count, 0) != 0
                    || HOPFmtWriteChar(c, ']') != 0)
                {
                    return -1;
                }
            } else {
                if (type >= 0 && HOPFmtEmitType(c, type) != 0) {
                    return -1;
                }
                if (init >= 0) {
                    int32_t initFirst = HOPFmtFirstChild(c->ast, init);
                    int     initTight =
                        c->ast->nodes[init].kind == HOPAst_COMPOUND_LIT
                        && (initFirst < 0 || !HOPFmtIsTypeNodeKind(c->ast->nodes[initFirst].kind));
                    if (!initTight && HOPFmtWriteChar(c, ' ') != 0) {
                        return -1;
                    }
                    if (HOPFmtEmitExpr(c, init, 0) != 0) {
                        return -1;
                    }
                }
            }
            if (alloc >= 0) {
                if (HOPFmtWriteCStr(c, " in ") != 0 || HOPFmtEmitExpr(c, alloc, 0) != 0) {
                    return -1;
                }
            }
            return 0;
        }
        case HOPAst_COMPOUND_LIT: {
            int32_t  cur = HOPFmtFirstChild(c->ast, nodeId);
            int32_t  type = -1;
            int32_t  field;
            uint32_t maxKeyLen = 0;
            uint32_t lbPos;
            uint32_t rbPos;
            if (cur >= 0 && HOPFmtIsTypeNodeKind(c->ast->nodes[cur].kind)) {
                type = cur;
                cur = HOPFmtNextSibling(c->ast, cur);
            }
            if (type >= 0 && HOPFmtEmitType(c, type) != 0) {
                return -1;
            }
            if (!HOPFmtFindCharForwardInRange(c->src, n->start, n->end, '{', &lbPos)
                || !HOPFmtFindCharBackwardInRange(c->src, n->start, n->end, '}', &rbPos))
            {
                return -1;
            }
            if (HOPFmtWriteChar(c, '{') != 0) {
                return -1;
            }
            if (cur < 0) {
                return HOPFmtWriteChar(c, '}');
            }
            if (!HOPFmtRangeHasChar(c->src, lbPos + 1u, rbPos, '\n')) {
                if (HOPFmtWriteChar(c, ' ') != 0) {
                    return -1;
                }
                field = cur;
                while (field >= 0) {
                    int32_t next = HOPFmtNextSibling(c->ast, field);
                    if (HOPFmtEmitExpr(c, field, 0) != 0) {
                        return -1;
                    }
                    if (next >= 0 && HOPFmtWriteCStr(c, ", ") != 0) {
                        return -1;
                    }
                    field = next;
                }
                return HOPFmtWriteCStr(c, " }");
            }
            c->indent++;
            for (field = cur; field >= 0; field = HOPFmtNextSibling(c->ast, field)) {
                const HOPAstNode* fn = &c->ast->nodes[field];
                uint32_t          keyLen = fn->dataEnd - fn->dataStart;
                if (fn->kind == HOPAst_COMPOUND_FIELD && keyLen > maxKeyLen) {
                    maxKeyLen = keyLen;
                }
            }
            field = cur;
            while (field >= 0) {
                int32_t  next = HOPFmtNextSibling(c->ast, field);
                uint32_t gapStart = c->ast->nodes[field].end;
                uint32_t gapEnd = next >= 0 ? c->ast->nodes[next].start : rbPos;
                int      hasComma = HOPFmtRangeHasChar(c->src, gapStart, gapEnd, ',');
                int      hasNewline = HOPFmtRangeHasChar(c->src, gapStart, gapEnd, '\n');
                if (field == cur) {
                    int firstHasNewline = HOPFmtRangeHasChar(
                        c->src, lbPos + 1u, c->ast->nodes[field].start, '\n');
                    if (firstHasNewline) {
                        if (HOPFmtNewline(c) != 0) {
                            return -1;
                        }
                    } else if (HOPFmtWriteChar(c, ' ') != 0) {
                        return -1;
                    }
                }
                if (HOPFmtEmitCompoundFieldWithAlign(c, field, maxKeyLen) != 0) {
                    return -1;
                }
                if (hasComma && HOPFmtWriteChar(c, ',') != 0) {
                    return -1;
                }
                if (next >= 0) {
                    if (hasNewline) {
                        if (HOPFmtNewline(c) != 0) {
                            return -1;
                        }
                    } else if (HOPFmtWriteChar(c, ' ') != 0) {
                        return -1;
                    }
                } else {
                    c->indent--;
                    if (hasNewline) {
                        if (HOPFmtNewline(c) != 0) {
                            return -1;
                        }
                    } else if (HOPFmtWriteChar(c, ' ') != 0) {
                        return -1;
                    }
                    return HOPFmtWriteChar(c, '}');
                }
                field = next;
            }
            c->indent--;
            return HOPFmtWriteChar(c, '}');
        }
        case HOPAst_COMPOUND_FIELD: return HOPFmtEmitCompoundFieldWithAlign(c, nodeId, 0u);
        case HOPAst_TYPE_VALUE:
            ch = HOPFmtFirstChild(c->ast, nodeId);
            if (HOPFmtWriteCStr(c, "type ") != 0) {
                return -1;
            }
            return ch >= 0 ? HOPFmtEmitType(c, ch) : 0;
        case HOPAst_UNWRAP:
            ch = HOPFmtFirstChild(c->ast, nodeId);
            if (ch >= 0 && HOPFmtEmitExpr(c, ch, 0) != 0) {
                return -1;
            }
            return HOPFmtWriteChar(c, '!');
        default: return HOPFmtWriteSlice(c, n->start, n->end);
    }
}

static int HOPFmtEmitExpr(HOPFmtCtx* c, int32_t nodeId, int forceParen) {
    const HOPAstNode* n;
    int               needParen;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return -1;
    }
    n = &c->ast->nodes[nodeId];
    needParen = forceParen || ((n->flags & HOPAstFlag_PAREN) != 0);
    if (needParen && HOPFmtWriteChar(c, '(') != 0) {
        return -1;
    }
    if (HOPFmtEmitExprCore(c, nodeId) != 0) {
        return -1;
    }
    if (needParen && HOPFmtWriteChar(c, ')') != 0) {
        return -1;
    }
    return 0;
}

static int HOPFmtMeasureTypeLen(HOPFmtCtx* c, int32_t nodeId, uint32_t* outLen) {
    HOPFmtCtx m = *c;
    m.lineStart = 0;
    m.indent = 0;
    m.out.v = NULL;
    m.out.len = 0;
    m.out.cap = 0;
    if (HOPFmtEmitType(&m, nodeId) != 0) {
        return -1;
    }
    *outLen = m.out.len;
    return 0;
}

static int HOPFmtMeasureExprLen(HOPFmtCtx* c, int32_t nodeId, int forceParen, uint32_t* outLen) {
    HOPFmtCtx m = *c;
    m.lineStart = 0;
    m.indent = 0;
    m.out.v = NULL;
    m.out.len = 0;
    m.out.cap = 0;
    if (HOPFmtEmitExpr(&m, nodeId, forceParen) != 0) {
        return -1;
    }
    *outLen = m.out.len;
    return 0;
}

static int HOPFmtNeedsBlankLineBeforeNode(HOPFmtCtx* c, int32_t prevNodeId, int32_t nextNodeId) {
    HOPAstKind nextKind;
    uint32_t   gapNl;
    if (prevNodeId < 0 || nextNodeId < 0) {
        return 0;
    }
    nextKind = c->ast->nodes[nextNodeId].kind;
    gapNl = HOPFmtCountNewlinesInRange(
        c->src, c->ast->nodes[prevNodeId].end, c->ast->nodes[nextNodeId].start);
    if (gapNl <= 1u) {
        return 0;
    }
    if (HOPFmtHasUnusedLeadingCommentsForNode(c, nextNodeId)) {
        if (!HOPFmtIsStmtNodeKind(nextKind)) {
            return 0;
        }
        if (!HOPFmtGapHasIntentionalBlankLine(
                c->src, c->ast->nodes[prevNodeId].end, c->ast->nodes[nextNodeId].start))
        {
            return 0;
        }
    }
    return 1;
}

static int HOPFmtTypeEnvCopy(HOPFmtTypeEnv* dst, const HOPFmtTypeEnv* src) {
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

static int32_t HOPFmtResolveTypeNodeThroughEnv(
    const HOPAst* ast, HOPStrView src, int32_t typeNodeId, const HOPFmtTypeEnv* _Nullable env) {
    int32_t bindingIndex;
    if (env == NULL || typeNodeId < 0 || (uint32_t)typeNodeId >= ast->len) {
        return typeNodeId;
    }
    if (!HOPFmtTypeNameIsBoundParam(ast, src, typeNodeId, env, &bindingIndex)) {
        return typeNodeId;
    }
    if (env->items[bindingIndex].concreteTypeNodeId < 0) {
        return typeNodeId;
    }
    return env->items[bindingIndex].concreteTypeNodeId;
}

static int HOPFmtInferredTypeSet(
    HOPFmtInferredType* inferred, int32_t typeNodeId, const HOPFmtTypeEnv* _Nullable env) {
    if (inferred == NULL || typeNodeId < 0) {
        return 0;
    }
    inferred->typeNodeId = typeNodeId;
    return HOPFmtTypeEnvCopy(&inferred->env, env);
}

static int HOPFmtInferredTypeMatchesNode(
    const HOPAst* ast, HOPStrView src, const HOPFmtInferredType* inferred, int32_t typeNodeId) {
    HOPFmtTypeEnv        envCopy;
    const HOPFmtTypeEnv* env;
    if (inferred == NULL || inferred->typeNodeId < 0 || typeNodeId < 0) {
        return 0;
    }
    env = inferred->env.len > 0 ? &inferred->env : NULL;
    if (env != NULL && !HOPFmtTypeEnvCopy(&envCopy, env)) {
        return 0;
    }
    return HOPFmtTypeCompatibleWithEnvs(
               ast, src, inferred->typeNodeId, env != NULL ? &envCopy : NULL, typeNodeId, NULL)
        && HOPFmtTypeCompatibleWithEnvs(ast, src, typeNodeId, NULL, inferred->typeNodeId, env);
}

static int32_t HOPFmtFindLocalNamedTypeDeclByRange(
    const HOPAst* ast, HOPStrView src, uint32_t nameStart, uint32_t nameEnd) {
    int32_t cur;
    if (nameEnd <= nameStart) {
        return -1;
    }
    cur = HOPFmtFirstChild(ast, ast->root);
    while (cur >= 0) {
        const HOPAstNode* n = &ast->nodes[cur];
        if ((n->kind == HOPAst_STRUCT || n->kind == HOPAst_UNION || n->kind == HOPAst_ENUM)
            && HOPFmtSlicesEqual(src, n->dataStart, n->dataEnd, nameStart, nameEnd))
        {
            return cur;
        }
        cur = HOPFmtNextSibling(ast, cur);
    }
    return -1;
}

static int32_t HOPFmtFindLocalNamedTypeDeclForTypeNode(
    const HOPAst* ast, HOPStrView src, int32_t typeNodeId, const HOPFmtTypeEnv* _Nullable env) {
    int32_t resolvedTypeNodeId = HOPFmtResolveTypeNodeThroughEnv(ast, src, typeNodeId, env);
    if (resolvedTypeNodeId < 0 || (uint32_t)resolvedTypeNodeId >= ast->len) {
        return -1;
    }
    if (ast->nodes[resolvedTypeNodeId].kind != HOPAst_TYPE_NAME) {
        return -1;
    }
    return HOPFmtFindLocalNamedTypeDeclByRange(
        ast, src, ast->nodes[resolvedTypeNodeId].dataStart, ast->nodes[resolvedTypeNodeId].dataEnd);
}

static int HOPFmtBindStructTypeArgsFromInstance(
    const HOPAst*        ast,
    HOPStrView           src,
    int32_t              declNodeId,
    int32_t              instTypeNodeId,
    const HOPFmtTypeEnv* instEnv,
    HOPFmtTypeEnv*       outEnv) {
    int32_t  declParamNodeId;
    int32_t  argNodeId;
    uint32_t i;
    if (!HOPFmtTypeEnvInitFromDeclTypeParams(ast, declNodeId, outEnv)) {
        return 0;
    }
    declParamNodeId = HOPFmtFirstChild(ast, declNodeId);
    argNodeId = HOPFmtFirstChild(ast, instTypeNodeId);
    for (i = 0; i < outEnv->len; i++) {
        int32_t resolvedArgNodeId;
        if (declParamNodeId < 0 || ast->nodes[declParamNodeId].kind != HOPAst_TYPE_PARAM
            || argNodeId < 0)
        {
            return 0;
        }
        resolvedArgNodeId = HOPFmtResolveTypeNodeThroughEnv(ast, src, argNodeId, instEnv);
        outEnv->items[i].concreteTypeNodeId = resolvedArgNodeId;
        declParamNodeId = HOPFmtNextSibling(ast, declParamNodeId);
        argNodeId = HOPFmtNextSibling(ast, argNodeId);
    }
    return argNodeId < 0;
}

static int HOPFmtFindFieldTypeOnLocalNamedType(
    const HOPAst*             ast,
    HOPStrView                src,
    const HOPFmtInferredType* baseType,
    uint32_t                  fieldStart,
    uint32_t                  fieldEnd,
    HOPFmtInferredType*       outType) {
    int32_t       declNodeId;
    int32_t       instTypeNodeId;
    int32_t       child;
    HOPFmtTypeEnv declEnv;
    if (outType != NULL) {
        HOPFmtInferredTypeInit(outType);
    }
    if (baseType == NULL || outType == NULL || baseType->typeNodeId < 0 || fieldEnd <= fieldStart) {
        return 0;
    }
    instTypeNodeId = HOPFmtResolveTypeNodeThroughEnv(
        ast, src, baseType->typeNodeId, &baseType->env);
    if (instTypeNodeId < 0 || (uint32_t)instTypeNodeId >= ast->len
        || ast->nodes[instTypeNodeId].kind != HOPAst_TYPE_NAME)
    {
        return 0;
    }
    declNodeId = HOPFmtFindLocalNamedTypeDeclForTypeNode(ast, src, instTypeNodeId, NULL);
    if (declNodeId < 0 || ast->nodes[declNodeId].kind != HOPAst_STRUCT) {
        return 0;
    }
    if (!HOPFmtBindStructTypeArgsFromInstance(
            ast, src, declNodeId, instTypeNodeId, &baseType->env, &declEnv))
    {
        return 0;
    }
    child = HOPFmtFirstChild(ast, declNodeId);
    while (child >= 0 && ast->nodes[child].kind == HOPAst_TYPE_PARAM) {
        child = HOPFmtNextSibling(ast, child);
    }
    while (child >= 0) {
        if (ast->nodes[child].kind == HOPAst_FIELD
            && HOPFmtSlicesEqual(
                src, ast->nodes[child].dataStart, ast->nodes[child].dataEnd, fieldStart, fieldEnd))
        {
            int32_t fieldTypeNodeId = HOPFmtFirstChild(ast, child);
            return fieldTypeNodeId >= 0
                && HOPFmtInferredTypeSet(outType, fieldTypeNodeId, &declEnv);
        }
        child = HOPFmtNextSibling(ast, child);
    }
    return 0;
}

static int HOPFmtFindLocalBindingBefore(
    const HOPAst* ast,
    HOPStrView    src,
    int32_t       identNodeId,
    uint32_t      beforePos,
    int32_t* _Nullable outDeclNodeId,
    int32_t* _Nullable outTypeNodeId,
    int32_t* _Nullable outInitNodeId) {
    const HOPAstNode* identNode;
    int32_t           fnNodeId;
    uint32_t          bestStart = 0;
    int32_t           bestDeclNodeId = -1;
    int32_t           bestTypeNodeId = -1;
    int32_t           bestInitNodeId = -1;
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
    if (identNode->kind != HOPAst_IDENT || identNode->dataEnd <= identNode->dataStart) {
        return 0;
    }
    fnNodeId = HOPFmtFindEnclosingFnNode(ast, identNodeId);
    if (fnNodeId < 0) {
        return 0;
    }

    {
        int32_t child = HOPFmtFirstChild(ast, fnNodeId);
        while (child >= 0 && ast->nodes[child].kind == HOPAst_TYPE_PARAM) {
            child = HOPFmtNextSibling(ast, child);
        }
        while (child >= 0 && ast->nodes[child].kind == HOPAst_PARAM) {
            int32_t paramTypeNodeId = HOPFmtFirstChild(ast, child);
            if (paramTypeNodeId >= 0 && HOPFmtIsTypeNodeKindRaw(ast->nodes[paramTypeNodeId].kind)
                && HOPFmtNodeDeclaresNameRange(
                    ast, src, child, identNode->dataStart, identNode->dataEnd)
                && ast->nodes[child].start < beforePos)
            {
                bestStart = ast->nodes[child].start;
                bestDeclNodeId = child;
                bestTypeNodeId = paramTypeNodeId;
                bestInitNodeId = -1;
            }
            child = HOPFmtNextSibling(ast, child);
        }
    }

    {
        const HOPAstNode* fn = &ast->nodes[fnNodeId];
        uint32_t          i;
        for (i = 0; i < ast->len; i++) {
            const HOPAstNode* n = &ast->nodes[i];
            int32_t           typeNodeId = -1;
            int32_t           initNodeId = -1;
            if (n->kind != HOPAst_VAR && n->kind != HOPAst_CONST) {
                continue;
            }
            if (n->start < fn->start || n->end > fn->end || n->end > beforePos) {
                continue;
            }
            HOPFmtGetVarLikeTypeAndInit(ast, (int32_t)i, &typeNodeId, &initNodeId);
            if (!HOPFmtNodeDeclaresNameRange(
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

static int HOPFmtInferExprTypeEx(
    const HOPAst*       ast,
    HOPStrView          src,
    int32_t             exprNodeId,
    uint32_t            beforePos,
    uint32_t            depth,
    HOPFmtInferredType* outType);

static int HOPFmtLiteralExprMatchesConcreteType(
    const HOPAst* ast, HOPStrView src, int32_t exprNodeId, int32_t typeNodeId) {
    int32_t innerExprNodeId = exprNodeId;
    if (exprNodeId < 0 || typeNodeId < 0 || (uint32_t)exprNodeId >= ast->len) {
        return 0;
    }
    if (ast->nodes[exprNodeId].kind == HOPAst_UNARY && ast->nodes[exprNodeId].op == HOPTok_SUB) {
        innerExprNodeId = HOPFmtFirstChild(ast, exprNodeId);
    }
    return innerExprNodeId >= 0
        && HOPFmtCastLiteralNumericType(ast, src, innerExprNodeId, typeNodeId)
               != HOPFmtNumericType_INVALID;
}

static int HOPFmtMapCallActuals(
    const HOPAst*     ast,
    int32_t           callNodeId,
    HOPFmtCallActual* actuals,
    uint32_t*         outActualCount,
    uint32_t          actualCap) {
    int32_t  calleeNodeId;
    int32_t  cur;
    uint32_t actualCount = 0;
    if (outActualCount != NULL) {
        *outActualCount = 0;
    }
    if (callNodeId < 0 || (uint32_t)callNodeId >= ast->len || actuals == NULL || actualCap == 0u) {
        return 0;
    }
    calleeNodeId = HOPFmtFirstChild(ast, callNodeId);
    if (calleeNodeId < 0) {
        return 0;
    }
    if (ast->nodes[calleeNodeId].kind == HOPAst_FIELD_EXPR) {
        int32_t recvNodeId = HOPFmtFirstChild(ast, calleeNodeId);
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
    cur = HOPFmtNextSibling(ast, calleeNodeId);
    while (cur >= 0) {
        const HOPAstNode* argNode = &ast->nodes[cur];
        int32_t           exprNodeId;
        if (actualCount >= actualCap || (argNode->flags & HOPAstFlag_CALL_ARG_SPREAD) != 0) {
            return 0;
        }
        exprNodeId = argNode->kind == HOPAst_CALL_ARG ? HOPFmtFirstChild(ast, cur) : cur;
        if (exprNodeId < 0) {
            return 0;
        }
        actuals[actualCount].argNodeId = cur;
        actuals[actualCount].exprNodeId = exprNodeId;
        actuals[actualCount].labelStart = argNode->dataStart;
        actuals[actualCount].labelEnd = argNode->dataEnd;
        actuals[actualCount].hasLabel =
            (uint8_t)(argNode->kind == HOPAst_CALL_ARG && argNode->dataEnd > argNode->dataStart);
        actuals[actualCount].isSynthetic = 0;
        actualCount++;
        cur = HOPFmtNextSibling(ast, cur);
    }
    if (outActualCount != NULL) {
        *outActualCount = actualCount;
    }
    return 1;
}

static int HOPFmtGetFnParamForActual(
    const HOPAst*           ast,
    HOPStrView              src,
    int32_t                 fnNodeId,
    const HOPFmtCallActual* actuals,
    uint32_t                actualCount,
    uint32_t                actualIndex,
    int32_t* _Nullable outParamNodeId,
    int32_t* _Nullable outParamTypeNodeId) {
    HOPFmtCallParam params[128];
    uint8_t         assigned[128] = { 0 };
    uint32_t        paramCount = 0;
    uint32_t        fixedCount;
    uint32_t        i;
    int             hasVariadic = 0;
    int32_t         cur;
    if (outParamNodeId != NULL) {
        *outParamNodeId = -1;
    }
    if (outParamTypeNodeId != NULL) {
        *outParamTypeNodeId = -1;
    }
    if (fnNodeId < 0 || (uint32_t)fnNodeId >= ast->len || actualIndex >= actualCount) {
        return 0;
    }
    cur = HOPFmtFirstChild(ast, fnNodeId);
    while (cur >= 0 && ast->nodes[cur].kind == HOPAst_TYPE_PARAM) {
        cur = HOPFmtNextSibling(ast, cur);
    }
    while (cur >= 0 && ast->nodes[cur].kind == HOPAst_PARAM) {
        int32_t typeNodeId = HOPFmtFirstChild(ast, cur);
        if (paramCount >= 128u || typeNodeId < 0) {
            return 0;
        }
        params[paramCount].paramNodeId = cur;
        params[paramCount].typeNodeId = typeNodeId;
        params[paramCount].flags = ast->nodes[cur].flags;
        if ((ast->nodes[cur].flags & HOPAstFlag_PARAM_VARIADIC) != 0) {
            hasVariadic = 1;
        }
        paramCount++;
        cur = HOPFmtNextSibling(ast, cur);
    }
    fixedCount = hasVariadic ? (paramCount > 0 ? paramCount - 1u : 0u) : paramCount;
    if ((!hasVariadic && actualCount != paramCount) || (hasVariadic && actualCount < fixedCount)) {
        return 0;
    }

    for (i = 0; i < actualCount; i++) {
        const HOPFmtCallActual* actual = &actuals[i];
        uint32_t                paramIndex = UINT32_MAX;
        uint32_t                p;
        if (actual->hasLabel) {
            for (p = 0; p < fixedCount; p++) {
                if (!assigned[p]
                    && HOPFmtSlicesEqual(
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

static int HOPFmtInferLocalCallAgainstFn(
    const HOPAst* ast,
    HOPStrView    src,
    int32_t       callNodeId,
    int32_t       fnNodeId,
    uint32_t      beforePos,
    int32_t       targetArgNodeId,
    HOPFmtInferredType* _Nullable outReturnType,
    HOPFmtInferredType* _Nullable outTargetParamType) {
    HOPFmtCallActual actuals[128];
    uint32_t         actualCount = 0;
    uint32_t         i;
    HOPFmtTypeEnv    fnEnv;
    int32_t          retTypeNodeId;
    int32_t          targetActualIndex = -1;
    if (outReturnType != NULL) {
        HOPFmtInferredTypeInit(outReturnType);
    }
    if (outTargetParamType != NULL) {
        HOPFmtInferredTypeInit(outTargetParamType);
    }
    if (!HOPFmtMapCallActuals(ast, callNodeId, actuals, &actualCount, 128u)
        || !HOPFmtTypeEnvInitFromDeclTypeParams(ast, fnNodeId, &fnEnv))
    {
        return 0;
    }
    for (i = 0; i < actualCount; i++) {
        HOPFmtInferredType actualType;
        int32_t            paramTypeNodeId = -1;
        int32_t            concreteParamTypeNodeId;
        int                isTargetActual = 0;
        if (!HOPFmtGetFnParamForActual(
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
        HOPFmtInferredTypeInit(&actualType);
        if (!HOPFmtInferExprTypeEx(ast, src, actuals[i].exprNodeId, beforePos, 0u, &actualType)) {
            concreteParamTypeNodeId = HOPFmtResolveTypeNodeThroughEnv(
                ast, src, paramTypeNodeId, &fnEnv);
            if (!HOPFmtLiteralExprMatchesConcreteType(
                    ast, src, actuals[i].exprNodeId, concreteParamTypeNodeId))
            {
                return 0;
            }
        } else if (!HOPFmtTypeCompatibleWithEnvs(
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
        if (!HOPFmtGetFnParamForActual(
                ast,
                src,
                fnNodeId,
                actuals,
                actualCount,
                (uint32_t)targetActualIndex,
                NULL,
                &targetParamTypeNodeId)
            || !HOPFmtInferredTypeSet(outTargetParamType, targetParamTypeNodeId, &fnEnv))
        {
            return 0;
        }
    }
    retTypeNodeId = HOPFmtFindFnReturnTypeNode(ast, fnNodeId);
    if (outReturnType == NULL) {
        return 1;
    }
    return retTypeNodeId >= 0 && HOPFmtInferredTypeSet(outReturnType, retTypeNodeId, &fnEnv);
}

static int HOPFmtCanContinueAlignedGroup(HOPFmtCtx* c, int32_t prevNodeId, int32_t nextNodeId) {
    uint32_t gapNl;
    if (prevNodeId < 0 || nextNodeId < 0) {
        return 0;
    }
    gapNl = HOPFmtCountNewlinesInRange(
        c->src, c->ast->nodes[prevNodeId].end, c->ast->nodes[nextNodeId].start);
    if (gapNl != 1u) {
        return 0;
    }
    if (HOPFmtHasUnusedLeadingCommentsForNode(c, nextNodeId)) {
        return 0;
    }
    return 1;
}

static int HOPFmtEmitStmt(HOPFmtCtx* c, int32_t nodeId);

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
} HOPFmtAlignedVarRow;

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
} HOPFmtAlignedAssignRow;

typedef struct {
    uint32_t minLhsLen;
    uint32_t baseNameStart;
    uint32_t baseNameEnd;
    uint8_t  valid;
    uint8_t  _pad[3];
} HOPFmtAssignCarryHint;

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
} HOPFmtSwitchClauseRow;

static int HOPFmtGetAssignStmtParts(
    HOPFmtCtx* c, int32_t stmtNodeId, int32_t* outLhs, int32_t* outRhs, uint16_t* outOp) {
    int32_t           exprNodeId;
    const HOPAstNode* exprNode;
    int32_t           lhsNodeId;
    int32_t           rhsNodeId;
    if (stmtNodeId < 0 || (uint32_t)stmtNodeId >= c->ast->len) {
        return 0;
    }
    if (c->ast->nodes[stmtNodeId].kind != HOPAst_EXPR_STMT) {
        return 0;
    }
    exprNodeId = HOPFmtFirstChild(c->ast, stmtNodeId);
    if (exprNodeId < 0 || (uint32_t)exprNodeId >= c->ast->len) {
        return 0;
    }
    exprNode = &c->ast->nodes[exprNodeId];
    if (exprNode->kind != HOPAst_BINARY || !HOPFmtIsAssignmentOp((HOPTokenKind)exprNode->op)) {
        return 0;
    }
    lhsNodeId = HOPFmtFirstChild(c->ast, exprNodeId);
    rhsNodeId = lhsNodeId >= 0 ? HOPFmtNextSibling(c->ast, lhsNodeId) : -1;
    if (lhsNodeId < 0 || rhsNodeId < 0) {
        return 0;
    }
    *outLhs = lhsNodeId;
    *outRhs = rhsNodeId;
    *outOp = exprNode->op;
    return 1;
}

static int HOPFmtHasUnusedCommentsInNodeRange(const HOPFmtCtx* c, int32_t nodeId) {
    uint32_t i;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return 0;
    }
    for (i = 0; i < c->commentLen; i++) {
        const HOPComment* cm = &c->comments[i];
        if (c->commentUsed[i]) {
            continue;
        }
        if (HOPFmtCommentWithinNodeRange(c->ast, cm, nodeId)) {
            return 1;
        }
    }
    return 0;
}

static int HOPFmtCanInlineSingleStmtBlock(
    HOPFmtCtx* c, int32_t blockNodeId, int32_t* outStmtNodeId) {
    int32_t stmtNodeId;
    if (blockNodeId < 0 || (uint32_t)blockNodeId >= c->ast->len
        || c->ast->nodes[blockNodeId].kind != HOPAst_BLOCK)
    {
        return 0;
    }
    if (HOPFmtHasUnusedCommentsInNodeRange(c, blockNodeId)) {
        return 0;
    }
    stmtNodeId = HOPFmtFirstChild(c->ast, blockNodeId);
    if (stmtNodeId < 0 || HOPFmtNextSibling(c->ast, stmtNodeId) >= 0) {
        return 0;
    }
    *outStmtNodeId = stmtNodeId;
    return 1;
}

static int HOPFmtEmitInlineSingleStmtBlock(HOPFmtCtx* c, int32_t blockNodeId) {
    int32_t stmtNodeId;
    if (!HOPFmtCanInlineSingleStmtBlock(c, blockNodeId, &stmtNodeId)) {
        return -1;
    }
    if (HOPFmtWriteCStr(c, "{ ") != 0 || HOPFmtEmitStmtInline(c, stmtNodeId) != 0) {
        return -1;
    }
    return HOPFmtWriteCStr(c, " }");
}

static int HOPFmtMeasureInlineSingleStmtBlockLen(
    HOPFmtCtx* c, int32_t blockNodeId, uint32_t* outLen) {
    HOPFmtCtx m = *c;
    m.lineStart = 0;
    m.indent = 0;
    m.out.v = NULL;
    m.out.len = 0;
    m.out.cap = 0;
    if (HOPFmtEmitInlineSingleStmtBlock(&m, blockNodeId) != 0) {
        return -1;
    }
    *outLen = m.out.len;
    return 0;
}

static int HOPFmtNodeSourceTextEqual(HOPFmtCtx* c, int32_t aNodeId, int32_t bNodeId) {
    const HOPAstNode* a;
    const HOPAstNode* b;
    uint32_t          aLen;
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

static int HOPFmtIsInlineAnonAggregateType(HOPFmtCtx* c, int32_t typeNodeId) {
    HOPAstKind kind;
    if (typeNodeId < 0 || (uint32_t)typeNodeId >= c->ast->len) {
        return 0;
    }
    kind = c->ast->nodes[typeNodeId].kind;
    if (kind != HOPAst_TYPE_ANON_STRUCT && kind != HOPAst_TYPE_ANON_UNION) {
        return 0;
    }
    return HOPFmtCountNewlinesInRange(
               c->src, c->ast->nodes[typeNodeId].start, c->ast->nodes[typeNodeId].end)
        == 0u;
}

static int HOPFmtEmitAlignedVarOrConstGroup(
    HOPFmtCtx*             c,
    int32_t                firstNodeId,
    const char*            kw,
    int32_t*               outLast,
    int32_t*               outNext,
    HOPFmtAssignCarryHint* outCarryHint) {
    const HOPAstKind     kind = c->ast->nodes[firstNodeId].kind;
    int32_t              cur = firstNodeId;
    int32_t              prev = -1;
    uint32_t             count = 0;
    uint32_t             i;
    uint32_t             kwLen = HOPFmtCStrLen(kw);
    uint32_t             maxNameLenWithType = 0;
    uint32_t             maxNameLenNoType = 0;
    uint32_t             maxBeforeOpLen = 0;
    HOPFmtAlignedVarRow* rows;
    uint32_t*            commentRunMaxLens;

    while (cur >= 0) {
        int32_t next = HOPFmtNextSibling(c->ast, cur);
        if (c->ast->nodes[cur].kind != kind) {
            break;
        }
        if (HOPFmtIsGroupedVarLike(c, cur)) {
            break;
        }
        if (prev >= 0 && !HOPFmtCanContinueAlignedGroup(c, prev, cur)) {
            break;
        }
        count++;
        prev = cur;
        cur = next;
    }

    rows = (HOPFmtAlignedVarRow*)HOPArenaAlloc(
        c->out.arena,
        count * (uint32_t)sizeof(HOPFmtAlignedVarRow),
        (uint32_t)_Alignof(HOPFmtAlignedVarRow));
    if (rows == NULL) {
        return -1;
    }
    memset(rows, 0, count * (uint32_t)sizeof(HOPFmtAlignedVarRow));

    cur = firstNodeId;
    for (i = 0; i < count; i++) {
        const HOPAstNode* n = &c->ast->nodes[cur];
        int32_t           typeNode = HOPFmtFirstChild(c->ast, cur);
        int32_t           initNode = -1;
        rows[i].nodeId = cur;
        rows[i].nameLen = n->dataEnd - n->dataStart;
        if (typeNode >= 0 && !HOPFmtIsTypeNodeKind(c->ast->nodes[typeNode].kind)) {
            initNode = typeNode;
            typeNode = -1;
        } else if (typeNode >= 0) {
            initNode = HOPFmtNextSibling(c->ast, typeNode);
        }
        HOPFmtRewriteVarTypeFromLiteralCast(c->ast, c->src, kw, 1u, &typeNode, &initNode);
        HOPFmtRewriteRedundantVarType(c->ast, c->src, kw, 1u, n->start, &typeNode, &initNode);
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
        if (typeNode >= 0 && HOPFmtMeasureTypeLen(c, typeNode, &rows[i].typeLen) != 0) {
            return -1;
        }
        if (initNode >= 0 && HOPFmtMeasureExprLen(c, initNode, 0, &rows[i].initLen) != 0) {
            return -1;
        }
        rows[i].hasTrailingComment = (uint8_t)HOPFmtHasUnusedTrailingCommentsForNodes(c, &cur, 1u);
        cur = HOPFmtNextSibling(c->ast, cur);
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

    commentRunMaxLens = (uint32_t*)HOPArenaAlloc(
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
        const HOPAstNode* n = &c->ast->nodes[rows[i].nodeId];
        uint32_t          nameColLen = rows[i].hasType ? maxNameLenWithType : maxNameLenNoType;
        uint32_t          lineLen;
        if (HOPFmtEmitLeadingCommentsForNode(c, rows[i].nodeId) != 0) {
            return -1;
        }
        if (HOPFmtWriteCStr(c, kw) != 0 || HOPFmtWriteChar(c, ' ') != 0
            || HOPFmtWriteSlice(c, n->dataStart, n->dataEnd) != 0)
        {
            return -1;
        }
        if (HOPFmtWriteSpaces(c, nameColLen - rows[i].nameLen + (rows[i].hasType ? 1u : 0u)) != 0) {
            return -1;
        }
        lineLen = kwLen + 1u + nameColLen + (rows[i].hasType ? 1u : 0u);
        if (rows[i].hasType) {
            if (HOPFmtEmitType(c, rows[i].typeNode) != 0) {
                return -1;
            }
            lineLen += rows[i].typeLen;
        }
        if (rows[i].hasInit) {
            uint32_t padBeforeOp = (maxBeforeOpLen + 1u) - lineLen;
            if (HOPFmtWriteSpaces(c, padBeforeOp) != 0 || HOPFmtWriteChar(c, '=') != 0
                || HOPFmtWriteChar(c, ' ') != 0 || HOPFmtEmitExpr(c, rows[i].initNode, 0) != 0)
            {
                return -1;
            }
            lineLen += padBeforeOp + 1u + 1u + rows[i].initLen;
        }
        if (rows[i].hasTrailingComment) {
            uint32_t padComment = (commentRunMaxLens[i] - lineLen) + 1u;
            int32_t  nodeId = rows[i].nodeId;
            if (HOPFmtEmitTrailingCommentsForNodes(c, &nodeId, 1u, padComment) != 0) {
                return -1;
            }
        }
        if (i + 1u < count && HOPFmtNewline(c) != 0) {
            return -1;
        }
    }

    outCarryHint->valid = 0;
    if (count == 1u && !rows[0].hasType && rows[0].hasInit) {
        const HOPAstNode* n = &c->ast->nodes[rows[0].nodeId];
        outCarryHint->minLhsLen = maxBeforeOpLen;
        outCarryHint->baseNameStart = n->dataStart;
        outCarryHint->baseNameEnd = n->dataEnd;
        outCarryHint->valid = 1;
    }

    *outLast = rows[count - 1u].nodeId;
    *outNext = cur;
    return 0;
}

static int HOPFmtEmitAlignedAssignGroup(
    HOPFmtCtx* c, int32_t firstNodeId, int32_t* outLast, int32_t* outNext, uint32_t minLhsLen) {
    int32_t                 cur = firstNodeId;
    int32_t                 prev = -1;
    int32_t                 prevLhs = -1;
    uint32_t                count = 0;
    uint32_t                i;
    uint32_t                maxLhsLen = 0;
    uint32_t                maxOpLen = 0;
    HOPFmtAlignedAssignRow* rows;

    while (cur >= 0) {
        int32_t  lhsNode;
        int32_t  rhsNode;
        uint16_t op;
        int32_t  next = HOPFmtNextSibling(c->ast, cur);
        if (!HOPFmtGetAssignStmtParts(c, cur, &lhsNode, &rhsNode, &op)) {
            break;
        }
        if (prev >= 0 && !HOPFmtCanContinueAlignedGroup(c, prev, cur)) {
            break;
        }
        if (prevLhs >= 0 && !HOPFmtNodeSourceTextEqual(c, prevLhs, lhsNode)) {
            break;
        }
        count++;
        prev = cur;
        prevLhs = lhsNode;
        cur = next;
    }

    rows = (HOPFmtAlignedAssignRow*)HOPArenaAlloc(
        c->out.arena,
        count * (uint32_t)sizeof(HOPFmtAlignedAssignRow),
        (uint32_t)_Alignof(HOPFmtAlignedAssignRow));
    if (rows == NULL) {
        return -1;
    }
    memset(rows, 0, count * (uint32_t)sizeof(HOPFmtAlignedAssignRow));

    cur = firstNodeId;
    for (i = 0; i < count; i++) {
        int32_t     lhsNode;
        int32_t     rhsNode;
        uint16_t    op;
        const char* opText;
        if (!HOPFmtGetAssignStmtParts(c, cur, &lhsNode, &rhsNode, &op)) {
            return -1;
        }
        rows[i].nodeId = cur;
        rows[i].lhsNode = lhsNode;
        rows[i].rhsNode = rhsNode;
        rows[i].op = op;
        if (HOPFmtMeasureExprLen(c, lhsNode, 0, &rows[i].lhsLen) != 0
            || HOPFmtMeasureExprLen(c, rhsNode, 0, &rows[i].rhsLen) != 0)
        {
            return -1;
        }
        opText = HOPFmtTokenOpText((HOPTokenKind)op);
        rows[i].opLen = HOPFmtCStrLen(opText);
        if (rows[i].lhsLen > maxLhsLen) {
            maxLhsLen = rows[i].lhsLen;
        }
        if (rows[i].opLen > maxOpLen) {
            maxOpLen = rows[i].opLen;
        }
        rows[i].hasTrailingComment = (uint8_t)HOPFmtHasUnusedTrailingCommentsForNodes(c, &cur, 1u);
        cur = HOPFmtNextSibling(c->ast, cur);
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
        const char* opText = HOPFmtTokenOpText((HOPTokenKind)rows[i].op);
        if (HOPFmtEmitLeadingCommentsForNode(c, rows[i].nodeId) != 0) {
            return -1;
        }
        if (HOPFmtEmitExpr(c, rows[i].lhsNode, 0) != 0 || HOPFmtWriteSpaces(c, padBeforeOp) != 0
            || HOPFmtWriteCStr(c, opText) != 0 || HOPFmtWriteSpaces(c, padAfterOp) != 0
            || HOPFmtEmitExpr(c, rows[i].rhsNode, 0) != 0)
        {
            return -1;
        }
        if (rows[i].hasTrailingComment) {
            uint32_t padComment = 1u;
            int32_t  nodeId = rows[i].nodeId;
            if (HOPFmtEmitTrailingCommentsForNodes(c, &nodeId, 1u, padComment) != 0) {
                return -1;
            }
        }
        if (i + 1u < count && HOPFmtNewline(c) != 0) {
            return -1;
        }
    }

    *outLast = rows[count - 1u].nodeId;
    *outNext = cur;
    return 0;
}

static int HOPFmtIsIndexedAssignOnNamedBase(
    HOPFmtCtx* c, int32_t lhsNodeId, uint32_t nameStart, uint32_t nameEnd) {
    const HOPAstNode* lhsNode;
    int32_t           baseNodeId;
    uint32_t          identStart = 0;
    uint32_t          identEnd = 0;
    if (lhsNodeId < 0 || (uint32_t)lhsNodeId >= c->ast->len || nameEnd <= nameStart) {
        return 0;
    }
    lhsNode = &c->ast->nodes[lhsNodeId];
    if (lhsNode->kind != HOPAst_INDEX || (lhsNode->flags & HOPAstFlag_INDEX_SLICE) != 0) {
        return 0;
    }
    baseNodeId = HOPFmtFirstChild(c->ast, lhsNodeId);
    if (!HOPFmtExprIsPlainIdent(c->ast, baseNodeId, &identStart, &identEnd)) {
        return 0;
    }
    return HOPFmtSlicesEqual(c->src, identStart, identEnd, nameStart, nameEnd);
}

static int HOPFmtMeasureCaseHeadLen(
    HOPFmtCtx* c, int32_t caseNodeId, uint32_t* outLen, int32_t* outBodyNodeId) {
    int32_t  k = HOPFmtFirstChild(c->ast, caseNodeId);
    uint32_t len = 5u;
    int      first = 1;
    int32_t  bodyNodeId = -1;
    while (k >= 0) {
        int32_t next = HOPFmtNextSibling(c->ast, k);
        if (next < 0) {
            bodyNodeId = k;
            break;
        }
        int32_t           exprNode = k;
        int32_t           aliasNode = -1;
        const HOPAstNode* kn = &c->ast->nodes[k];
        if (kn->kind == HOPAst_CASE_PATTERN) {
            exprNode = HOPFmtFirstChild(c->ast, k);
            aliasNode = exprNode >= 0 ? HOPFmtNextSibling(c->ast, exprNode) : -1;
            if (exprNode < 0) {
                return -1;
            }
        }
        if (!first) {
            len += 2u;
        }
        {
            uint32_t exprLen;
            if (HOPFmtMeasureExprLen(c, exprNode, 0, &exprLen) != 0) {
                return -1;
            }
            len += exprLen;
        }
        if (aliasNode >= 0) {
            const HOPAstNode* alias = &c->ast->nodes[aliasNode];
            len += 4u + (alias->dataEnd - alias->dataStart); /* " as " + alias */
        }
        first = 0;
        k = next;
    }
    if (bodyNodeId < 0 || c->ast->nodes[bodyNodeId].kind != HOPAst_BLOCK) {
        return -1;
    }
    *outLen = len;
    *outBodyNodeId = bodyNodeId;
    return 0;
}

static int HOPFmtEmitCaseHead(HOPFmtCtx* c, int32_t caseNodeId) {
    int32_t k = HOPFmtFirstChild(c->ast, caseNodeId);
    int     first = 1;
    if (HOPFmtWriteCStr(c, "case ") != 0) {
        return -1;
    }
    while (k >= 0) {
        int32_t next = HOPFmtNextSibling(c->ast, k);
        int32_t exprNode = k;
        int32_t aliasNode = -1;
        if (next < 0) {
            break;
        }
        if (c->ast->nodes[k].kind == HOPAst_CASE_PATTERN) {
            exprNode = HOPFmtFirstChild(c->ast, k);
            aliasNode = exprNode >= 0 ? HOPFmtNextSibling(c->ast, exprNode) : -1;
            if (exprNode < 0) {
                return -1;
            }
        }
        if (!first && HOPFmtWriteCStr(c, ", ") != 0) {
            return -1;
        }
        if (HOPFmtEmitExpr(c, exprNode, 0) != 0) {
            return -1;
        }
        if (aliasNode >= 0) {
            const HOPAstNode* alias = &c->ast->nodes[aliasNode];
            if (HOPFmtWriteCStr(c, " as ") != 0
                || HOPFmtWriteSlice(c, alias->dataStart, alias->dataEnd) != 0)
            {
                return -1;
            }
        }
        first = 0;
        k = next;
    }
    return 0;
}

static int HOPFmtEmitSwitchClauseGroup(
    HOPFmtCtx* c,
    int32_t    firstClauseNodeId,
    int32_t*   outLastClauseNodeId,
    int32_t*   outNextClauseNodeId) {
    int32_t                cur = firstClauseNodeId;
    int32_t                prev = -1;
    uint32_t               count = 0;
    uint32_t               i;
    HOPFmtSwitchClauseRow* rows;
    uint32_t*              commentRunMaxLens;
    uint32_t*              inlineRunMaxHeadLens;

    while (cur >= 0) {
        int32_t next = HOPFmtNextSibling(c->ast, cur);
        if (c->ast->nodes[cur].kind != HOPAst_CASE && c->ast->nodes[cur].kind != HOPAst_DEFAULT) {
            break;
        }
        if (prev >= 0 && !HOPFmtCanContinueAlignedGroup(c, prev, cur)) {
            break;
        }
        count++;
        prev = cur;
        cur = next;
    }

    rows = (HOPFmtSwitchClauseRow*)HOPArenaAlloc(
        c->out.arena,
        count * (uint32_t)sizeof(HOPFmtSwitchClauseRow),
        (uint32_t)_Alignof(HOPFmtSwitchClauseRow));
    if (rows == NULL) {
        return -1;
    }
    memset(rows, 0, count * (uint32_t)sizeof(HOPFmtSwitchClauseRow));

    inlineRunMaxHeadLens = (uint32_t*)HOPArenaAlloc(
        c->out.arena, count * (uint32_t)sizeof(uint32_t), (uint32_t)_Alignof(uint32_t));
    if (inlineRunMaxHeadLens == NULL) {
        return -1;
    }
    memset(inlineRunMaxHeadLens, 0, count * (uint32_t)sizeof(uint32_t));

    commentRunMaxLens = (uint32_t*)HOPArenaAlloc(
        c->out.arena, count * (uint32_t)sizeof(uint32_t), (uint32_t)_Alignof(uint32_t));
    if (commentRunMaxLens == NULL) {
        return -1;
    }
    memset(commentRunMaxLens, 0, count * (uint32_t)sizeof(uint32_t));

    cur = firstClauseNodeId;
    for (i = 0; i < count; i++) {
        HOPFmtSwitchClauseRow* r = &rows[i];
        int32_t                next = HOPFmtNextSibling(c->ast, cur);
        r->nodeId = cur;
        r->isDefault = (uint8_t)(c->ast->nodes[cur].kind == HOPAst_DEFAULT);
        if (r->isDefault) {
            r->headLen = 7u;
            r->bodyNodeId = HOPFmtFirstChild(c->ast, cur);
            if (r->bodyNodeId < 0 || c->ast->nodes[r->bodyNodeId].kind != HOPAst_BLOCK) {
                return -1;
            }
        } else if (HOPFmtMeasureCaseHeadLen(c, cur, &r->headLen, &r->bodyNodeId) != 0) {
            return -1;
        }
        {
            int32_t stmtNodeId = -1;
            if (HOPFmtCanInlineSingleStmtBlock(c, r->bodyNodeId, &stmtNodeId)) {
                r->inlineBody = 1;
                if (HOPFmtMeasureInlineSingleStmtBlockLen(c, r->bodyNodeId, &r->inlineBodyLen) != 0)
                {
                    return -1;
                }
            }
        }
        r->hasTrailingComment = (uint8_t)HOPFmtHasUnusedTrailingCommentsForNodes(c, &cur, 1u);
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
        HOPFmtSwitchClauseRow* r = &rows[i];
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
        HOPFmtSwitchClauseRow* r = &rows[i];
        uint32_t               lineLen = 0;
        uint32_t               padBeforeBody = 1u;
        if (r->inlineBody) {
            padBeforeBody = (inlineRunMaxHeadLens[i] - r->headLen) + 1u;
        } else if (i > 0u && rows[i - 1u].inlineBody) {
            uint32_t prevRunMaxHeadLen = inlineRunMaxHeadLens[i - 1u];
            if (prevRunMaxHeadLen > r->headLen) {
                padBeforeBody = (prevRunMaxHeadLen - r->headLen) + 1u;
            }
        }
        if (HOPFmtEmitLeadingCommentsForNode(c, r->nodeId) != 0) {
            return -1;
        }
        if (r->isDefault) {
            if (HOPFmtWriteCStr(c, "default") != 0) {
                return -1;
            }
        } else if (HOPFmtEmitCaseHead(c, r->nodeId) != 0) {
            return -1;
        }
        lineLen = r->headLen;
        if (HOPFmtWriteSpaces(c, padBeforeBody) != 0) {
            return -1;
        }
        lineLen += padBeforeBody;
        if (r->inlineBody) {
            if (HOPFmtEmitInlineSingleStmtBlock(c, r->bodyNodeId) != 0) {
                return -1;
            }
            lineLen += r->inlineBodyLen;
        } else if (HOPFmtEmitBlock(c, r->bodyNodeId) != 0) {
            return -1;
        }
        if (r->hasTrailingComment) {
            int32_t  nodeId = r->nodeId;
            uint32_t padComment = 1u;
            if (r->inlineBody && commentRunMaxLens[i] > 0u) {
                padComment = (commentRunMaxLens[i] - lineLen) + 1u;
            }
            if (HOPFmtEmitTrailingCommentsForNodes(c, &nodeId, 1u, padComment) != 0) {
                return -1;
            }
        }
        if (i + 1u < count && HOPFmtNewline(c) != 0) {
            return -1;
        }
    }

    *outLastClauseNodeId = rows[count - 1u].nodeId;
    *outNextClauseNodeId = cur;
    return 0;
}

static int HOPFmtEmitBlock(HOPFmtCtx* c, int32_t nodeId) {
    int32_t               stmt;
    int32_t               prevEmitted = -1;
    HOPFmtAssignCarryHint carryHint;
    memset(&carryHint, 0, sizeof(carryHint));
    if (HOPFmtWriteChar(c, '{') != 0) {
        return -1;
    }
    stmt = HOPFmtFirstChild(c->ast, nodeId);
    if (stmt >= 0) {
        if (HOPFmtNewline(c) != 0) {
            return -1;
        }
        c->indent++;
        while (stmt >= 0) {
            int32_t  last = stmt;
            int32_t  next = HOPFmtNextSibling(c->ast, stmt);
            int32_t  lhsNode = -1;
            int32_t  rhsNode = -1;
            uint16_t op = 0;
            if (prevEmitted >= 0) {
                if (!HOPFmtCanContinueAlignedGroup(c, prevEmitted, stmt)) {
                    carryHint.valid = 0;
                }
                if (HOPFmtNewline(c) != 0) {
                    return -1;
                }
                if (HOPFmtNeedsBlankLineBeforeNode(c, prevEmitted, stmt) && HOPFmtNewline(c) != 0) {
                    return -1;
                }
            }
            if (c->ast->nodes[stmt].kind == HOPAst_VAR && !HOPFmtIsGroupedVarLike(c, stmt)) {
                if (HOPFmtEmitAlignedVarOrConstGroup(c, stmt, "var", &last, &next, &carryHint) != 0)
                {
                    return -1;
                }
            } else if (c->ast->nodes[stmt].kind == HOPAst_CONST && !HOPFmtIsGroupedVarLike(c, stmt))
            {
                if (HOPFmtEmitAlignedVarOrConstGroup(c, stmt, "const", &last, &next, &carryHint)
                    != 0)
                {
                    return -1;
                }
            } else if (HOPFmtGetAssignStmtParts(c, stmt, &lhsNode, &rhsNode, &op)) {
                uint32_t minLhsLen = 0;
                if (carryHint.valid
                    && HOPFmtIsIndexedAssignOnNamedBase(
                        c, lhsNode, carryHint.baseNameStart, carryHint.baseNameEnd))
                {
                    minLhsLen = carryHint.minLhsLen;
                }
                if (HOPFmtEmitAlignedAssignGroup(c, stmt, &last, &next, minLhsLen) != 0) {
                    return -1;
                }
                carryHint.valid = 0;
            } else {
                if (HOPFmtEmitStmt(c, stmt) != 0) {
                    return -1;
                }
                carryHint.valid = 0;
                last = stmt;
            }
            prevEmitted = last;
            stmt = next;
        }
        c->indent--;
        if (HOPFmtNewline(c) != 0) {
            return -1;
        }
    }
    return HOPFmtWriteChar(c, '}');
}

static int HOPFmtEmitVarLike(HOPFmtCtx* c, int32_t nodeId, const char* kw) {
    int32_t firstChild = HOPFmtFirstChild(c->ast, nodeId);
    int32_t type = -1;
    int32_t init = -1;
    if (firstChild >= 0 && c->ast->nodes[firstChild].kind == HOPAst_NAME_LIST) {
        uint32_t i;
        uint32_t nameCount = HOPFmtListCount(c->ast, firstChild);
        int32_t  afterNames = HOPFmtNextSibling(c->ast, firstChild);
        if (afterNames >= 0 && HOPFmtIsTypeNodeKind(c->ast->nodes[afterNames].kind)) {
            type = afterNames;
            init = HOPFmtNextSibling(c->ast, afterNames);
        } else {
            init = afterNames;
        }
        HOPFmtRewriteVarTypeFromLiteralCast(c->ast, c->src, kw, nameCount, &type, &init);
        HOPFmtRewriteRedundantVarType(
            c->ast, c->src, kw, nameCount, c->ast->nodes[nodeId].start, &type, &init);
        if (HOPFmtWriteCStr(c, kw) != 0 || HOPFmtWriteChar(c, ' ') != 0) {
            return -1;
        }
        for (i = 0; i < nameCount; i++) {
            int32_t nameNode = HOPFmtListItemAt(c->ast, firstChild, i);
            if (nameNode < 0) {
                return -1;
            }
            if (i > 0 && HOPFmtWriteCStr(c, ", ") != 0) {
                return -1;
            }
            if (HOPFmtWriteSlice(
                    c, c->ast->nodes[nameNode].dataStart, c->ast->nodes[nameNode].dataEnd)
                != 0)
            {
                return -1;
            }
        }
        if (type >= 0) {
            if (HOPFmtWriteChar(c, ' ') != 0 || HOPFmtEmitType(c, type) != 0) {
                return -1;
            }
        }
        if (init >= 0) {
            if (HOPFmtWriteCStr(c, " = ") != 0) {
                return -1;
            }
            if (c->ast->nodes[init].kind == HOPAst_EXPR_LIST) {
                if (HOPFmtEmitExprList(c, init) != 0) {
                    return -1;
                }
            } else if (HOPFmtEmitExpr(c, init, 0) != 0) {
                return -1;
            }
        }
        return 0;
    }

    {
        const HOPAstNode* n = &c->ast->nodes[nodeId];
        type = firstChild;
        if (type >= 0 && !HOPFmtIsTypeNodeKind(c->ast->nodes[type].kind)) {
            init = type;
            type = -1;
        } else if (type >= 0) {
            init = HOPFmtNextSibling(c->ast, type);
        }
        HOPFmtRewriteVarTypeFromLiteralCast(c->ast, c->src, kw, 1u, &type, &init);
        HOPFmtRewriteRedundantVarType(c->ast, c->src, kw, 1u, n->start, &type, &init);
        if (HOPFmtWriteCStr(c, kw) != 0 || HOPFmtWriteChar(c, ' ') != 0
            || HOPFmtWriteSlice(c, n->dataStart, n->dataEnd) != 0)
        {
            return -1;
        }
        if (type >= 0) {
            if (HOPFmtWriteChar(c, ' ') != 0 || HOPFmtEmitType(c, type) != 0) {
                return -1;
            }
        }
        if (init >= 0) {
            if (HOPFmtWriteCStr(c, " = ") != 0 || HOPFmtEmitExpr(c, init, 0) != 0) {
                return -1;
            }
        }
        return 0;
    }
}

static int HOPFmtEmitMultiAssign(HOPFmtCtx* c, int32_t nodeId) {
    int32_t  lhsList = HOPFmtFirstChild(c->ast, nodeId);
    int32_t  rhsList = lhsList >= 0 ? HOPFmtNextSibling(c->ast, lhsList) : -1;
    uint32_t i;
    uint32_t lhsCount;
    if (lhsList < 0 || rhsList < 0 || c->ast->nodes[lhsList].kind != HOPAst_EXPR_LIST
        || c->ast->nodes[rhsList].kind != HOPAst_EXPR_LIST)
    {
        return -1;
    }
    lhsCount = HOPFmtListCount(c->ast, lhsList);
    for (i = 0; i < lhsCount; i++) {
        int32_t lhsNode = HOPFmtListItemAt(c->ast, lhsList, i);
        if (lhsNode < 0) {
            return -1;
        }
        if (i > 0 && HOPFmtWriteCStr(c, ", ") != 0) {
            return -1;
        }
        if (HOPFmtEmitExpr(c, lhsNode, 0) != 0) {
            return -1;
        }
    }
    if (HOPFmtWriteCStr(c, " = ") != 0) {
        return -1;
    }
    if (HOPFmtEmitExprList(c, rhsList) != 0) {
        return -1;
    }
    return 0;
}

static int HOPFmtEmitShortAssign(HOPFmtCtx* c, int32_t nodeId) {
    int32_t  nameList = HOPFmtFirstChild(c->ast, nodeId);
    int32_t  rhsList = nameList >= 0 ? HOPFmtNextSibling(c->ast, nameList) : -1;
    uint32_t i;
    uint32_t nameCount;
    if (nameList < 0 || rhsList < 0 || c->ast->nodes[nameList].kind != HOPAst_NAME_LIST
        || c->ast->nodes[rhsList].kind != HOPAst_EXPR_LIST)
    {
        return -1;
    }
    nameCount = HOPFmtListCount(c->ast, nameList);
    for (i = 0; i < nameCount; i++) {
        int32_t nameNode = HOPFmtListItemAt(c->ast, nameList, i);
        if (nameNode < 0) {
            return -1;
        }
        if (i > 0 && HOPFmtWriteCStr(c, ", ") != 0) {
            return -1;
        }
        if (HOPFmtEmitExpr(c, nameNode, 0) != 0) {
            return -1;
        }
    }
    if (HOPFmtWriteCStr(c, " := ") != 0) {
        return -1;
    }
    return HOPFmtEmitExprList(c, rhsList);
}

static int HOPFmtEmitForHeaderFromSource(
    HOPFmtCtx* c, int32_t nodeId, int32_t bodyNode, int32_t* parts, uint32_t partLen) {
    const HOPAstNode* n = &c->ast->nodes[nodeId];
    uint32_t          hdrStart = n->start;
    uint32_t          hdrEnd = c->ast->nodes[bodyNode].start;
    uint32_t          i;
    uint32_t          s1 = UINT32_MAX;
    uint32_t          s2 = UINT32_MAX;
    uint32_t          idx = 0;
    uint32_t          aStart;
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
            if (i > 0 && HOPFmtWriteCStr(c, "; ") != 0) {
                return -1;
            }
            if (c->ast->nodes[parts[i]].kind == HOPAst_VAR) {
                if (HOPFmtEmitVarLike(c, parts[i], "var") != 0) {
                    return -1;
                }
            } else if (c->ast->nodes[parts[i]].kind == HOPAst_SHORT_ASSIGN) {
                if (HOPFmtEmitShortAssign(c, parts[i]) != 0) {
                    return -1;
                }
            } else if (HOPFmtEmitExpr(c, parts[i], 0) != 0) {
                return -1;
            }
        }
        while (i < 3u) {
            if (HOPFmtWriteCStr(c, ";") != 0) {
                return -1;
            }
            if (i < 2u && HOPFmtWriteChar(c, ' ') != 0) {
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
            if (c->ast->nodes[parts[idx]].kind == HOPAst_VAR) {
                if (HOPFmtEmitVarLike(c, parts[idx], "var") != 0) {
                    return -1;
                }
            } else if (c->ast->nodes[parts[idx]].kind == HOPAst_SHORT_ASSIGN) {
                if (HOPFmtEmitShortAssign(c, parts[idx]) != 0) {
                    return -1;
                }
            } else if (HOPFmtEmitExpr(c, parts[idx], 0) != 0) {
                return -1;
            }
            idx++;
        }
        if (HOPFmtWriteCStr(c, "; ") != 0) {
            return -1;
        }
        if (seg1Has && idx < partLen) {
            if (HOPFmtEmitExpr(c, parts[idx], 0) != 0) {
                return -1;
            }
            idx++;
        }
        if (HOPFmtWriteCStr(c, "; ") != 0) {
            return -1;
        }
        if (seg2Has && idx < partLen) {
            if (HOPFmtEmitExpr(c, parts[idx], 0) != 0) {
                return -1;
            }
            idx++;
        }
    }
    return 0;
}

static int HOPFmtEmitStmtInline(HOPFmtCtx* c, int32_t nodeId) {
    const HOPAstNode* n;
    int32_t           ch;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return -1;
    }
    n = &c->ast->nodes[nodeId];
    switch (n->kind) {
        case HOPAst_BLOCK: return HOPFmtEmitBlock(c, nodeId);
        case HOPAst_VAR:   return HOPFmtEmitVarLike(c, nodeId, "var");
        case HOPAst_CONST: return HOPFmtEmitVarLike(c, nodeId, "const");
        case HOPAst_CONST_BLOCK:
            ch = HOPFmtFirstChild(c->ast, nodeId);
            if (HOPFmtWriteCStr(c, "const ") != 0) {
                return -1;
            }
            return ch >= 0 ? HOPFmtEmitBlock(c, ch) : 0;
        case HOPAst_MULTI_ASSIGN: return HOPFmtEmitMultiAssign(c, nodeId);
        case HOPAst_SHORT_ASSIGN: return HOPFmtEmitShortAssign(c, nodeId);
        case HOPAst_IF:           {
            int32_t cond = HOPFmtFirstChild(c->ast, nodeId);
            int32_t thenNode = cond >= 0 ? HOPFmtNextSibling(c->ast, cond) : -1;
            int32_t elseNode = thenNode >= 0 ? HOPFmtNextSibling(c->ast, thenNode) : -1;
            if (HOPFmtWriteCStr(c, "if ") != 0 || (cond >= 0 && HOPFmtEmitExpr(c, cond, 0) != 0)
                || HOPFmtWriteChar(c, ' ') != 0
                || (thenNode >= 0 && HOPFmtEmitBlock(c, thenNode) != 0))
            {
                return -1;
            }
            if (elseNode >= 0) {
                if (HOPFmtWriteCStr(c, " else ") != 0) {
                    return -1;
                }
                if (c->ast->nodes[elseNode].kind == HOPAst_IF) {
                    if (HOPFmtEmitStmtInline(c, elseNode) != 0) {
                        return -1;
                    }
                } else if (HOPFmtEmitBlock(c, elseNode) != 0) {
                    return -1;
                }
            }
            return 0;
        }
        case HOPAst_FOR: {
            int32_t  parts[4];
            uint32_t partLen = 0;
            int32_t  cur = HOPFmtFirstChild(c->ast, nodeId);
            int32_t  bodyNode;
            while (cur >= 0 && partLen < 4u) {
                parts[partLen++] = cur;
                cur = HOPFmtNextSibling(c->ast, cur);
            }
            if (partLen == 0) {
                return HOPFmtWriteCStr(c, "for {}");
            }
            bodyNode = parts[partLen - 1u];
            if (HOPFmtWriteCStr(c, "for") != 0) {
                return -1;
            }
            if ((n->flags & HOPAstFlag_FOR_IN) != 0) {
                int     hasKey = (n->flags & HOPAstFlag_FOR_IN_HAS_KEY) != 0;
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
                    if (HOPFmtWriteChar(c, ' ') != 0) {
                        return -1;
                    }
                    if ((n->flags & HOPAstFlag_FOR_IN_KEY_REF) != 0 && HOPFmtWriteChar(c, '&') != 0)
                    {
                        return -1;
                    }
                    if (HOPFmtEmitExpr(c, keyNode, 0) != 0 || HOPFmtWriteCStr(c, ", ") != 0) {
                        return -1;
                    }
                } else {
                    valueNode = parts[0];
                    sourceNode = parts[1];
                    if (HOPFmtWriteChar(c, ' ') != 0) {
                        return -1;
                    }
                }
                if ((n->flags & HOPAstFlag_FOR_IN_VALUE_DISCARD) == 0) {
                    if ((n->flags & HOPAstFlag_FOR_IN_VALUE_REF) != 0
                        && HOPFmtWriteChar(c, '&') != 0)
                    {
                        return -1;
                    }
                }
                if (HOPFmtEmitExpr(c, valueNode, 0) != 0 || HOPFmtWriteCStr(c, " in ") != 0
                    || HOPFmtEmitExpr(c, sourceNode, 0) != 0 || HOPFmtWriteChar(c, ' ') != 0
                    || HOPFmtEmitBlock(c, bodyNode) != 0)
                {
                    return -1;
                }
                return 0;
            }
            if (partLen == 1u && c->ast->nodes[bodyNode].kind == HOPAst_BLOCK) {
                if (HOPFmtWriteChar(c, ' ') != 0 || HOPFmtEmitBlock(c, bodyNode) != 0) {
                    return -1;
                }
                return 0;
            }
            if (HOPFmtContainsSemicolonInRange(c->src, n->start, c->ast->nodes[bodyNode].start)) {
                if (HOPFmtWriteChar(c, ' ') != 0
                    || HOPFmtEmitForHeaderFromSource(c, nodeId, bodyNode, parts, partLen - 1u) != 0
                    || HOPFmtWriteChar(c, ' ') != 0 || HOPFmtEmitBlock(c, bodyNode) != 0)
                {
                    return -1;
                }
                return 0;
            }
            if (HOPFmtWriteChar(c, ' ') != 0 || HOPFmtEmitExpr(c, parts[0], 0) != 0
                || HOPFmtWriteChar(c, ' ') != 0 || HOPFmtEmitBlock(c, bodyNode) != 0)
            {
                return -1;
            }
            return 0;
        }
        case HOPAst_SWITCH: {
            int32_t cur = HOPFmtFirstChild(c->ast, nodeId);
            int32_t prevClause = -1;
            if (HOPFmtWriteCStr(c, "switch") != 0) {
                return -1;
            }
            if (n->flags == 1 && cur >= 0) {
                if (HOPFmtWriteChar(c, ' ') != 0 || HOPFmtEmitExpr(c, cur, 0) != 0) {
                    return -1;
                }
                cur = HOPFmtNextSibling(c->ast, cur);
            }
            if (HOPFmtWriteCStr(c, " {") != 0) {
                return -1;
            }
            if (cur >= 0) {
                if (HOPFmtNewline(c) != 0) {
                    return -1;
                }
                c->indent++;
                while (cur >= 0) {
                    int32_t lastClause = cur;
                    int32_t nextClause = HOPFmtNextSibling(c->ast, cur);
                    if (prevClause >= 0) {
                        if (HOPFmtNewline(c) != 0) {
                            return -1;
                        }
                        if (HOPFmtNeedsBlankLineBeforeNode(c, prevClause, cur)
                            && HOPFmtNewline(c) != 0)
                        {
                            return -1;
                        }
                    }
                    if (HOPFmtEmitSwitchClauseGroup(c, cur, &lastClause, &nextClause) != 0) {
                        return -1;
                    }
                    prevClause = lastClause;
                    cur = nextClause;
                }
                c->indent--;
                if (HOPFmtNewline(c) != 0) {
                    return -1;
                }
            }
            return HOPFmtWriteChar(c, '}');
        }
        case HOPAst_RETURN:
            ch = HOPFmtFirstChild(c->ast, nodeId);
            if (HOPFmtWriteCStr(c, "return") != 0) {
                return -1;
            }
            if (ch >= 0) {
                if (HOPFmtWriteChar(c, ' ') != 0) {
                    return -1;
                }
                if (c->ast->nodes[ch].kind == HOPAst_EXPR_LIST) {
                    if (HOPFmtEmitExprList(c, ch) != 0) {
                        return -1;
                    }
                } else if (HOPFmtEmitExpr(c, ch, 0) != 0) {
                    return -1;
                }
            }
            return 0;
        case HOPAst_BREAK:    return HOPFmtWriteCStr(c, "break");
        case HOPAst_CONTINUE: return HOPFmtWriteCStr(c, "continue");
        case HOPAst_DEFER:
            ch = HOPFmtFirstChild(c->ast, nodeId);
            if (HOPFmtWriteCStr(c, "defer ") != 0) {
                return -1;
            }
            if (ch >= 0 && c->ast->nodes[ch].kind == HOPAst_BLOCK) {
                int32_t stmtNodeId;
                if (HOPFmtCanInlineSingleStmtBlock(c, ch, &stmtNodeId)) {
                    return HOPFmtEmitStmtInline(c, stmtNodeId);
                }
            }
            return ch >= 0 ? HOPFmtEmitStmtInline(c, ch) : 0;
        case HOPAst_ASSERT:
            ch = HOPFmtFirstChild(c->ast, nodeId);
            if (HOPFmtWriteCStr(c, "assert ") != 0) {
                return -1;
            }
            while (ch >= 0) {
                int32_t next = HOPFmtNextSibling(c->ast, ch);
                if (HOPFmtEmitExpr(c, ch, 0) != 0) {
                    return -1;
                }
                if (next >= 0 && HOPFmtWriteCStr(c, ", ") != 0) {
                    return -1;
                }
                ch = next;
            }
            return 0;
        case HOPAst_DEL:
            ch = HOPFmtFirstChild(c->ast, nodeId);
            if (HOPFmtWriteCStr(c, "del ") != 0) {
                return -1;
            }
            if ((n->flags & HOPAstFlag_DEL_HAS_ALLOC) != 0) {
                int32_t scan = ch;
                while (scan >= 0) {
                    int32_t next = HOPFmtNextSibling(c->ast, scan);
                    if (next < 0) {
                        break;
                    }
                    scan = next;
                }
                while (ch >= 0 && ch != scan) {
                    int32_t next = HOPFmtNextSibling(c->ast, ch);
                    if (HOPFmtEmitExpr(c, ch, 0) != 0) {
                        return -1;
                    }
                    if (next >= 0 && next != scan && HOPFmtWriteCStr(c, ", ") != 0) {
                        return -1;
                    }
                    ch = next;
                }
                if (scan >= 0
                    && (HOPFmtWriteCStr(c, " in ") != 0 || HOPFmtEmitExpr(c, scan, 0) != 0))
                {
                    return -1;
                }
                return 0;
            }
            while (ch >= 0) {
                int32_t next = HOPFmtNextSibling(c->ast, ch);
                if (HOPFmtEmitExpr(c, ch, 0) != 0) {
                    return -1;
                }
                if (next >= 0 && HOPFmtWriteCStr(c, ", ") != 0) {
                    return -1;
                }
                ch = next;
            }
            return 0;
        case HOPAst_EXPR_STMT:
            ch = HOPFmtFirstChild(c->ast, nodeId);
            return ch >= 0 ? HOPFmtEmitExpr(c, ch, 0) : 0;
        default: return HOPFmtWriteSlice(c, n->start, n->end);
    }
}

static int HOPFmtEmitStmt(HOPFmtCtx* c, int32_t nodeId) {
    if (HOPFmtEmitLeadingCommentsForNode(c, nodeId) != 0) {
        return -1;
    }
    if (HOPFmtEmitStmtInline(c, nodeId) != 0) {
        return -1;
    }
    return HOPFmtEmitTrailingCommentsForNode(c, nodeId);
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
} HOPFmtImportRow;

static int HOPFmtImportParseRow(HOPFmtCtx* c, int32_t nodeId, HOPFmtImportRow* outRow) {
    const HOPAstNode* n;
    int32_t           child;
    int32_t           aliasNodeId = -1;
    int32_t           symStartNodeId = -1;
    uint32_t          symbolsLen = 0;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len
        || c->ast->nodes[nodeId].kind != HOPAst_IMPORT)
    {
        return -1;
    }
    n = &c->ast->nodes[nodeId];
    child = HOPFmtFirstChild(c->ast, nodeId);
    if (child >= 0 && c->ast->nodes[child].kind == HOPAst_IDENT) {
        aliasNodeId = child;
        child = HOPFmtNextSibling(c->ast, child);
    }
    symStartNodeId = child;

    if (symStartNodeId >= 0) {
        int32_t sym = symStartNodeId;
        symbolsLen = 4u;
        while (sym >= 0) {
            const HOPAstNode* sn = &c->ast->nodes[sym];
            int32_t           salias = HOPFmtFirstChild(c->ast, sym);
            int32_t           next = HOPFmtNextSibling(c->ast, sym);
            symbolsLen += sn->dataEnd - sn->dataStart;
            if (salias >= 0) {
                const HOPAstNode* an = &c->ast->nodes[salias];
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
        const HOPAstNode* an = &c->ast->nodes[aliasNodeId];
        outRow->aliasLen = 4u + (an->dataEnd - an->dataStart);
    }
    outRow->symbolsLen = symbolsLen;
    outRow->hasSymbols = (uint8_t)(symStartNodeId >= 0);
    outRow->headLen = 7u + outRow->pathLen + outRow->aliasLen;
    outRow->hasTrailingComment = (uint8_t)HOPFmtHasUnusedTrailingCommentsInNodeRange(c, nodeId);
    return 0;
}

static int HOPFmtCompareNodePathText(HOPFmtCtx* c, int32_t aNodeId, int32_t bNodeId) {
    const HOPAstNode* a = &c->ast->nodes[aNodeId];
    const HOPAstNode* b = &c->ast->nodes[bNodeId];
    uint32_t          ai = a->dataStart;
    uint32_t          bi = b->dataStart;
    uint32_t          aEnd = a->dataEnd;
    uint32_t          bEnd = b->dataEnd;
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

static int HOPFmtCompareImportRows(
    HOPFmtCtx* c, const HOPFmtImportRow* a, const HOPFmtImportRow* b) {
    int cmp = HOPFmtCompareNodePathText(c, a->nodeId, b->nodeId);
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
        cmp = HOPFmtCompareNodePathText(c, a->aliasNodeId, b->aliasNodeId);
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

static void HOPFmtSortImportRows(HOPFmtCtx* c, HOPFmtImportRow* rows, uint32_t len) {
    uint32_t i;
    for (i = 1; i < len; i++) {
        HOPFmtImportRow key = rows[i];
        uint32_t        j = i;
        while (j > 0 && HOPFmtCompareImportRows(c, &key, &rows[j - 1u]) < 0) {
            rows[j] = rows[j - 1u];
            j--;
        }
        rows[j] = key;
    }
}

static int HOPFmtEmitImportSymbolsInline(HOPFmtCtx* c, int32_t symStartNodeId) {
    int32_t sym = symStartNodeId;
    if (HOPFmtWriteCStr(c, "{ ") != 0) {
        return -1;
    }
    while (sym >= 0) {
        const HOPAstNode* sn = &c->ast->nodes[sym];
        int32_t           salias = HOPFmtFirstChild(c->ast, sym);
        int32_t           next = HOPFmtNextSibling(c->ast, sym);
        if (HOPFmtWriteSlice(c, sn->dataStart, sn->dataEnd) != 0) {
            return -1;
        }
        if (salias >= 0) {
            const HOPAstNode* an = &c->ast->nodes[salias];
            if (HOPFmtWriteCStr(c, " as ") != 0
                || HOPFmtWriteSlice(c, an->dataStart, an->dataEnd) != 0)
            {
                return -1;
            }
        }
        if (next >= 0 && HOPFmtWriteCStr(c, ", ") != 0) {
            return -1;
        }
        sym = next;
    }
    return HOPFmtWriteCStr(c, " }");
}

static int HOPFmtCanContinueImportGroup(HOPFmtCtx* c, int32_t prevNodeId, int32_t nextNodeId) {
    uint32_t gapNl;
    if (prevNodeId < 0 || nextNodeId < 0 || c->ast->nodes[nextNodeId].kind != HOPAst_IMPORT) {
        return 0;
    }
    gapNl = HOPFmtCountNewlinesInRange(
        c->src, c->ast->nodes[prevNodeId].end, c->ast->nodes[nextNodeId].start);
    if (gapNl > 1u) {
        return 0;
    }
    if (HOPFmtHasUnusedLeadingCommentsForNode(c, nextNodeId)) {
        return 0;
    }
    return 1;
}

static int HOPFmtEmitImportGroup(
    HOPFmtCtx* c, int32_t firstNodeId, int32_t* outLastSourceNodeId, int32_t* outNextNodeId) {
    int32_t          cur = firstNodeId;
    int32_t          prev = -1;
    uint32_t         count = 0;
    uint32_t         i;
    HOPFmtImportRow* rows;
    uint32_t*        commentRunMaxLens;
    uint32_t*        braceRunMaxHeadLens;

    while (cur >= 0 && c->ast->nodes[cur].kind == HOPAst_IMPORT) {
        int32_t next = HOPFmtNextSibling(c->ast, cur);
        if (prev >= 0 && !HOPFmtCanContinueImportGroup(c, prev, cur)) {
            break;
        }
        count++;
        prev = cur;
        cur = next;
    }

    rows = (HOPFmtImportRow*)HOPArenaAlloc(
        c->out.arena,
        count * (uint32_t)sizeof(HOPFmtImportRow),
        (uint32_t)_Alignof(HOPFmtImportRow));
    if (rows == NULL) {
        return -1;
    }
    cur = firstNodeId;
    for (i = 0; i < count; i++) {
        int32_t next = HOPFmtNextSibling(c->ast, cur);
        if (HOPFmtImportParseRow(c, cur, &rows[i]) != 0) {
            return -1;
        }
        cur = next;
    }

    HOPFmtSortImportRows(c, rows, count);

    braceRunMaxHeadLens = (uint32_t*)HOPArenaAlloc(
        c->out.arena, count * (uint32_t)sizeof(uint32_t), (uint32_t)_Alignof(uint32_t));
    if (braceRunMaxHeadLens == NULL) {
        return -1;
    }
    memset(braceRunMaxHeadLens, 0, count * (uint32_t)sizeof(uint32_t));

    commentRunMaxLens = (uint32_t*)HOPArenaAlloc(
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
        const HOPAstNode* n = &c->ast->nodes[rows[i].nodeId];
        uint32_t          lineLen = rows[i].headLen;
        int32_t           nodeId = rows[i].nodeId;
        if (HOPFmtEmitLeadingCommentsForNode(c, rows[i].nodeId) != 0) {
            return -1;
        }
        if (HOPFmtWriteCStr(c, "import ") != 0
            || HOPFmtWriteSlice(c, n->dataStart, n->dataEnd) != 0)
        {
            return -1;
        }
        if (rows[i].aliasNodeId >= 0) {
            const HOPAstNode* an = &c->ast->nodes[rows[i].aliasNodeId];
            if (HOPFmtWriteCStr(c, " as ") != 0
                || HOPFmtWriteSlice(c, an->dataStart, an->dataEnd) != 0)
            {
                return -1;
            }
        }
        if (rows[i].hasSymbols) {
            uint32_t padBeforeBrace = (braceRunMaxHeadLens[i] - rows[i].headLen) + 1u;
            if (HOPFmtWriteSpaces(c, padBeforeBrace) != 0
                || HOPFmtEmitImportSymbolsInline(c, rows[i].symStartNodeId) != 0)
            {
                return -1;
            }
            lineLen += padBeforeBrace + rows[i].symbolsLen;
        }
        if (rows[i].hasTrailingComment) {
            uint32_t padComment = (commentRunMaxLens[i] - lineLen) + 1u;
            if (HOPFmtEmitTrailingCommentsInNodeRange(c, nodeId, padComment) != 0) {
                return -1;
            }
        }
        if (i + 1u < count && HOPFmtNewline(c) != 0) {
            return -1;
        }
    }

    *outLastSourceNodeId = prev;
    *outNextNodeId = cur;
    return 0;
}

static int HOPFmtEmitImport(HOPFmtCtx* c, int32_t nodeId) {
    HOPFmtImportRow   row;
    const HOPAstNode* n;
    uint32_t          lineLen;
    int32_t           id = nodeId;
    if (HOPFmtImportParseRow(c, nodeId, &row) != 0) {
        return -1;
    }
    n = &c->ast->nodes[nodeId];
    lineLen = row.headLen;
    if (HOPFmtWriteCStr(c, "import ") != 0 || HOPFmtWriteSlice(c, n->dataStart, n->dataEnd) != 0) {
        return -1;
    }
    if (row.aliasNodeId >= 0) {
        const HOPAstNode* an = &c->ast->nodes[row.aliasNodeId];
        if (HOPFmtWriteCStr(c, " as ") != 0 || HOPFmtWriteSlice(c, an->dataStart, an->dataEnd) != 0)
        {
            return -1;
        }
    }
    if (row.hasSymbols) {
        if (HOPFmtWriteChar(c, ' ') != 0
            || HOPFmtEmitImportSymbolsInline(c, row.symStartNodeId) != 0)
        {
            return -1;
        }
        lineLen += 1u + row.symbolsLen;
    }
    if (row.hasTrailingComment) {
        if (HOPFmtEmitTrailingCommentsInNodeRange(c, id, 1u) != 0) {
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
} HOPFmtAlignedFieldRow;

typedef struct {
    int32_t  nodeId;
    int32_t  valueNodeId;
    uint32_t nameLen;
    uint32_t valueLen;
    uint32_t codeLen;
    uint8_t  hasValue;
    uint8_t  hasTrailingComment;
    uint8_t  _pad[2];
} HOPFmtAlignedEnumRow;

static int HOPFmtFieldTypesMatch(HOPFmtCtx* c, int32_t aTypeNodeId, int32_t bTypeNodeId) {
    const HOPAstNode* a;
    const HOPAstNode* b;
    uint32_t          aLen;
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

static int HOPFmtIsAnonAggregateTypeNode(HOPFmtCtx* c, int32_t typeNodeId) {
    HOPAstKind kind;
    if (typeNodeId < 0 || (uint32_t)typeNodeId >= c->ast->len) {
        return 0;
    }
    kind = c->ast->nodes[typeNodeId].kind;
    return kind == HOPAst_TYPE_ANON_STRUCT || kind == HOPAst_TYPE_ANON_UNION;
}

static int HOPFmtCanMergeFieldNames(
    HOPFmtCtx* c, int32_t leftFieldNodeId, int32_t rightFieldNodeId) {
    const HOPAstNode* left = &c->ast->nodes[leftFieldNodeId];
    const HOPAstNode* right = &c->ast->nodes[rightFieldNodeId];
    uint32_t          i;
    int               sawComma = 0;
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

static uint32_t HOPFmtMergedFieldNameLen(
    HOPFmtCtx* c, int32_t firstFieldNodeId, int32_t lastFieldNodeId) {
    uint32_t len = 0;
    int32_t  cur = firstFieldNodeId;
    while (cur >= 0) {
        const HOPAstNode* n = &c->ast->nodes[cur];
        int32_t           next = HOPFmtNextSibling(c->ast, cur);
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

static int HOPFmtEmitMergedFieldNames(
    HOPFmtCtx* c, int32_t firstFieldNodeId, int32_t lastFieldNodeId) {
    int32_t cur = firstFieldNodeId;
    while (cur >= 0) {
        const HOPAstNode* n = &c->ast->nodes[cur];
        int32_t           next = HOPFmtNextSibling(c->ast, cur);
        if (HOPFmtWriteSlice(c, n->dataStart, n->dataEnd) != 0) {
            return -1;
        }
        if (cur == lastFieldNodeId) {
            break;
        }
        if (HOPFmtWriteCStr(c, ", ") != 0) {
            return -1;
        }
        cur = next;
    }
    return 0;
}

static int HOPFmtBuildFieldRow(
    HOPFmtCtx* c, int32_t startFieldNodeId, HOPFmtAlignedFieldRow* outRow, int32_t* outNextNodeId) {
    int32_t           typeNodeId;
    int32_t           defaultNodeId;
    const HOPAstNode* startNode;
    int32_t           lastNodeId;

    if (startFieldNodeId < 0 || (uint32_t)startFieldNodeId >= c->ast->len) {
        return -1;
    }
    startNode = &c->ast->nodes[startFieldNodeId];
    if (startNode->kind != HOPAst_FIELD) {
        return -1;
    }

    typeNodeId = HOPFmtFirstChild(c->ast, startFieldNodeId);
    defaultNodeId = typeNodeId >= 0 ? HOPFmtNextSibling(c->ast, typeNodeId) : -1;
    lastNodeId = startFieldNodeId;

    if ((startNode->flags & HOPAstFlag_FIELD_EMBEDDED) == 0 && defaultNodeId < 0) {
        int32_t next = HOPFmtNextSibling(c->ast, lastNodeId);
        while (next >= 0 && c->ast->nodes[next].kind == HOPAst_FIELD) {
            const HOPAstNode* nn = &c->ast->nodes[next];
            int32_t           nextType = HOPFmtFirstChild(c->ast, next);
            int32_t nextDefault = nextType >= 0 ? HOPFmtNextSibling(c->ast, nextType) : -1;
            if ((nn->flags & HOPAstFlag_FIELD_EMBEDDED) != 0 || nextDefault >= 0) {
                break;
            }
            if (!HOPFmtFieldTypesMatch(c, typeNodeId, nextType)) {
                break;
            }
            if (!HOPFmtCanMergeFieldNames(c, lastNodeId, next)) {
                break;
            }
            lastNodeId = next;
            next = HOPFmtNextSibling(c->ast, next);
        }
    }

    memset(outRow, 0, sizeof(*outRow));
    outRow->firstNodeId = startFieldNodeId;
    outRow->lastNodeId = lastNodeId;
    outRow->typeNodeId = typeNodeId;
    outRow->defaultNodeId = defaultNodeId;
    outRow->hasDefault = (uint8_t)(defaultNodeId >= 0);
    outRow->nameLen = HOPFmtMergedFieldNameLen(c, startFieldNodeId, lastNodeId);
    if (typeNodeId >= 0 && HOPFmtMeasureTypeLen(c, typeNodeId, &outRow->typeLen) != 0) {
        return -1;
    }
    if (defaultNodeId >= 0 && HOPFmtMeasureExprLen(c, defaultNodeId, 0, &outRow->defaultLen) != 0) {
        return -1;
    }
    outRow->hasTrailingComment = (uint8_t)HOPFmtHasUnusedTrailingCommentsForNodes(
        c, &startFieldNodeId, 1u);
    *outNextNodeId = HOPFmtNextSibling(c->ast, lastNodeId);
    return 0;
}

static int HOPFmtEmitSimpleFieldDecl(HOPFmtCtx* c, int32_t fieldNodeId) {
    const HOPAstNode* fn = &c->ast->nodes[fieldNodeId];
    int32_t           typeNode = HOPFmtFirstChild(c->ast, fieldNodeId);
    int32_t           defaultNode = typeNode >= 0 ? HOPFmtNextSibling(c->ast, typeNode) : -1;
    int32_t           nodeIds[1];
    int               hasTrailing;
    if ((fn->flags & HOPAstFlag_FIELD_EMBEDDED) != 0) {
        if (typeNode >= 0 && HOPFmtEmitType(c, typeNode) != 0) {
            return -1;
        }
    } else if (
        HOPFmtWriteSlice(c, fn->dataStart, fn->dataEnd) != 0 || HOPFmtWriteChar(c, ' ') != 0
        || (typeNode >= 0 && HOPFmtEmitType(c, typeNode) != 0))
    {
        return -1;
    }
    if (defaultNode >= 0) {
        if (HOPFmtWriteCStr(c, " = ") != 0 || HOPFmtEmitExpr(c, defaultNode, 0) != 0) {
            return -1;
        }
    }
    nodeIds[0] = fieldNodeId;
    hasTrailing = HOPFmtHasUnusedTrailingCommentsForNodes(c, nodeIds, 1u);
    if (hasTrailing) {
        return HOPFmtEmitTrailingCommentsForNodes(c, nodeIds, 1u, 1u);
    }
    {
        uint32_t cmStart;
        uint32_t cmEnd;
        if (HOPFmtFindSourceTrailingLineComment(c, fieldNodeId, &cmStart, &cmEnd)) {
            if (HOPFmtWriteChar(c, ' ') != 0 || HOPFmtWriteSlice(c, cmStart, cmEnd) != 0) {
                return -1;
            }
            HOPFmtMarkCommentUsedAtStart(c, cmStart);
        }
    }
    return 0;
}

static int HOPFmtEmitAlignedFieldGroup(
    HOPFmtCtx* c, int32_t firstFieldNodeId, int32_t* outLastNodeId, int32_t* outNextNodeId) {
    int32_t                cur = firstFieldNodeId;
    int32_t                prev = -1;
    int                    prevInlineAnon = 0;
    uint32_t               count = 0;
    uint32_t               i;
    uint32_t               maxNameLen = 0;
    uint32_t               maxBeforeOpLen = 0;
    HOPFmtAlignedFieldRow* rows;
    uint32_t*              commentRunMaxLens;

    while (cur >= 0) {
        HOPFmtAlignedFieldRow row;
        int32_t               next;
        int                   curInlineAnon;
        if (c->ast->nodes[cur].kind != HOPAst_FIELD) {
            break;
        }
        if ((c->ast->nodes[cur].flags & HOPAstFlag_FIELD_EMBEDDED) != 0) {
            break;
        }
        if (HOPFmtBuildFieldRow(c, cur, &row, &next) != 0) {
            return -1;
        }
        curInlineAnon = HOPFmtIsInlineAnonAggregateType(c, row.typeNodeId);
        if (prev >= 0) {
            if (!HOPFmtCanContinueAlignedGroup(c, prev, row.firstNodeId)) {
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
        *outNextNodeId = HOPFmtNextSibling(c->ast, firstFieldNodeId);
        return 0;
    }

    rows = (HOPFmtAlignedFieldRow*)HOPArenaAlloc(
        c->out.arena,
        count * (uint32_t)sizeof(HOPFmtAlignedFieldRow),
        (uint32_t)_Alignof(HOPFmtAlignedFieldRow));
    if (rows == NULL) {
        return -1;
    }

    cur = firstFieldNodeId;
    for (i = 0; i < count; i++) {
        if (HOPFmtBuildFieldRow(c, cur, &rows[i], &cur) != 0) {
            return -1;
        }
        if (rows[i].nameLen > maxNameLen) {
            maxNameLen = rows[i].nameLen;
        }
    }

    for (i = 0; i < count; i++) {
        int isAnon = HOPFmtIsAnonAggregateTypeNode(c, rows[i].typeNodeId);
        int prevAnon = i > 0u && HOPFmtIsAnonAggregateTypeNode(c, rows[i - 1u].typeNodeId);
        int nextAnon = i + 1u < count && HOPFmtIsAnonAggregateTypeNode(c, rows[i + 1u].typeNodeId);
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

    commentRunMaxLens = (uint32_t*)HOPArenaAlloc(
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
        if (HOPFmtEmitLeadingCommentsForNode(c, rows[i].firstNodeId) != 0) {
            return -1;
        }
        if (HOPFmtEmitMergedFieldNames(c, rows[i].firstNodeId, rows[i].lastNodeId) != 0) {
            return -1;
        }
        lineLen = rows[i].nameLen;
        if (rows[i].noTypeAlign) {
            if (HOPFmtWriteChar(c, ' ') != 0) {
                return -1;
            }
        } else {
            if (HOPFmtWriteSpaces(c, maxNameLen - rows[i].nameLen + 1u) != 0) {
                return -1;
            }
            lineLen = maxNameLen + 1u;
        }
        if (rows[i].typeNodeId >= 0 && HOPFmtEmitType(c, rows[i].typeNodeId) != 0) {
            return -1;
        }
        lineLen += rows[i].typeLen;
        if (rows[i].hasDefault) {
            if (rows[i].noTypeAlign) {
                if (HOPFmtWriteCStr(c, " = ") != 0
                    || HOPFmtEmitExpr(c, rows[i].defaultNodeId, 0) != 0)
                {
                    return -1;
                }
                lineLen += 3u + rows[i].defaultLen;
            } else {
                uint32_t padBeforeOp = (maxBeforeOpLen + 1u) - lineLen;
                if (HOPFmtWriteSpaces(c, padBeforeOp) != 0 || HOPFmtWriteChar(c, '=') != 0
                    || HOPFmtWriteChar(c, ' ') != 0
                    || HOPFmtEmitExpr(c, rows[i].defaultNodeId, 0) != 0)
                {
                    return -1;
                }
                lineLen += padBeforeOp + 1u + 1u + rows[i].defaultLen;
            }
        }
        if (rows[i].hasTrailingComment) {
            uint32_t padComment = (commentRunMaxLens[i] - lineLen) + 1u;
            nodeIds[0] = rows[i].firstNodeId;
            if (HOPFmtEmitTrailingCommentsForNodes(c, nodeIds, 1u, padComment) != 0) {
                return -1;
            }
        } else {
            uint32_t cmStart;
            uint32_t cmEnd;
            if (HOPFmtFindSourceTrailingLineComment(c, rows[i].lastNodeId, &cmStart, &cmEnd)) {
                if (HOPFmtWriteChar(c, ' ') != 0 || HOPFmtWriteSlice(c, cmStart, cmEnd) != 0) {
                    return -1;
                }
                HOPFmtMarkCommentUsedAtStart(c, cmStart);
            }
        }
        if (i + 1u < count && HOPFmtNewline(c) != 0) {
            return -1;
        }
    }

    *outLastNodeId = rows[count - 1u].lastNodeId;
    *outNextNodeId = cur;
    return 0;
}

static int HOPFmtEmitAlignedEnumGroup(
    HOPFmtCtx* c, int32_t firstFieldNodeId, int32_t* outLastNodeId, int32_t* outNextNodeId) {
    int32_t               cur = firstFieldNodeId;
    int32_t               prev = -1;
    uint32_t              count = 0;
    uint32_t              i;
    uint32_t              maxNameLenForValues = 0;
    HOPFmtAlignedEnumRow* rows;
    uint32_t*             commentRunMaxLens;

    while (cur >= 0) {
        int32_t next = HOPFmtNextSibling(c->ast, cur);
        if (c->ast->nodes[cur].kind != HOPAst_FIELD) {
            break;
        }
        if (prev >= 0 && !HOPFmtCanContinueAlignedGroup(c, prev, cur)) {
            break;
        }
        count++;
        prev = cur;
        cur = next;
    }

    rows = (HOPFmtAlignedEnumRow*)HOPArenaAlloc(
        c->out.arena,
        count * (uint32_t)sizeof(HOPFmtAlignedEnumRow),
        (uint32_t)_Alignof(HOPFmtAlignedEnumRow));
    if (rows == NULL) {
        return -1;
    }
    memset(rows, 0, count * (uint32_t)sizeof(HOPFmtAlignedEnumRow));

    cur = firstFieldNodeId;
    for (i = 0; i < count; i++) {
        const HOPAstNode* n = &c->ast->nodes[cur];
        int32_t           valueNode = HOPFmtFirstChild(c->ast, cur);
        rows[i].nodeId = cur;
        rows[i].nameLen = n->dataEnd - n->dataStart;
        while (valueNode >= 0 && c->ast->nodes[valueNode].kind == HOPAst_FIELD) {
            valueNode = HOPFmtNextSibling(c->ast, valueNode);
        }
        rows[i].valueNodeId = valueNode;
        rows[i].hasValue = (uint8_t)(rows[i].valueNodeId >= 0);
        if (rows[i].hasValue
            && HOPFmtMeasureExprLen(c, rows[i].valueNodeId, 0, &rows[i].valueLen) != 0)
        {
            return -1;
        }
        rows[i].hasTrailingComment = (uint8_t)HOPFmtHasUnusedTrailingCommentsForNodes(c, &cur, 1u);
        if (rows[i].hasValue && rows[i].nameLen > maxNameLenForValues) {
            maxNameLenForValues = rows[i].nameLen;
        }
        cur = HOPFmtNextSibling(c->ast, cur);
    }

    commentRunMaxLens = (uint32_t*)HOPArenaAlloc(
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
        uint32_t          lineLen = rows[i].nameLen;
        int32_t           nodeIds[1];
        const HOPAstNode* n = &c->ast->nodes[rows[i].nodeId];
        if (HOPFmtEmitLeadingCommentsForNode(c, rows[i].nodeId) != 0) {
            return -1;
        }
        if (HOPFmtWriteSlice(c, n->dataStart, n->dataEnd) != 0) {
            return -1;
        }
        if (rows[i].hasValue) {
            uint32_t padBeforeOp = (maxNameLenForValues + 1u) - rows[i].nameLen;
            if (HOPFmtWriteSpaces(c, padBeforeOp) != 0 || HOPFmtWriteChar(c, '=') != 0
                || HOPFmtWriteChar(c, ' ') != 0 || HOPFmtEmitExpr(c, rows[i].valueNodeId, 0) != 0)
            {
                return -1;
            }
            lineLen += padBeforeOp + 1u + 1u + rows[i].valueLen;
        }
        if (rows[i].hasTrailingComment) {
            uint32_t padComment = (commentRunMaxLens[i] - lineLen) + 1u;
            nodeIds[0] = rows[i].nodeId;
            if (HOPFmtEmitTrailingCommentsForNodes(c, nodeIds, 1u, padComment) != 0) {
                return -1;
            }
        }
        if (i + 1u < count && HOPFmtNewline(c) != 0) {
            return -1;
        }
    }

    *outLastNodeId = rows[count - 1u].nodeId;
    *outNextNodeId = cur;
    return 0;
}

static int HOPFmtIsNestedTypeDeclNodeKind(HOPAstKind kind) {
    return kind == HOPAst_STRUCT || kind == HOPAst_UNION || kind == HOPAst_ENUM
        || kind == HOPAst_TYPE_ALIAS;
}

static int HOPFmtEmitAggregateFieldBody(HOPFmtCtx* c, int32_t firstFieldNodeId) {
    int32_t child = firstFieldNodeId;
    int32_t prevEmitted = -1;

    if (child < 0) {
        return 0;
    }
    if (HOPFmtNewline(c) != 0) {
        return -1;
    }
    c->indent++;
    while (child >= 0) {
        HOPAstKind kind = c->ast->nodes[child].kind;
        if (kind != HOPAst_FIELD && !HOPFmtIsNestedTypeDeclNodeKind(kind)) {
            break;
        }
        if (prevEmitted >= 0) {
            if (HOPFmtNewline(c) != 0) {
                return -1;
            }
            if (HOPFmtNeedsBlankLineBeforeNode(c, prevEmitted, child) && HOPFmtNewline(c) != 0) {
                return -1;
            }
        }
        if (kind == HOPAst_FIELD) {
            int32_t next = HOPFmtNextSibling(c->ast, child);
            int32_t last = child;
            if ((c->ast->nodes[child].flags & HOPAstFlag_FIELD_EMBEDDED) != 0) {
                if (HOPFmtEmitLeadingCommentsForNode(c, child) != 0
                    || HOPFmtEmitSimpleFieldDecl(c, child) != 0)
                {
                    return -1;
                }
            } else if (HOPFmtEmitAlignedFieldGroup(c, child, &last, &next) != 0) {
                return -1;
            }
            prevEmitted = last;
            child = next;
            continue;
        }
        if (HOPFmtEmitDecl(c, child) != 0) {
            return -1;
        }
        prevEmitted = child;
        child = HOPFmtNextSibling(c->ast, child);
    }
    c->indent--;
    if (HOPFmtNewline(c) != 0) {
        return -1;
    }
    return 0;
}

static int HOPFmtEmitAggregateDecl(HOPFmtCtx* c, int32_t nodeId, const char* kw) {
    const HOPAstNode* n = &c->ast->nodes[nodeId];
    int32_t           child = HOPFmtFirstChild(c->ast, nodeId);
    int32_t           underType = -1;
    int32_t           prevEmitted = -1;
    while (child >= 0 && c->ast->nodes[child].kind == HOPAst_TYPE_PARAM) {
        child = HOPFmtNextSibling(c->ast, child);
    }
    if (n->kind == HOPAst_ENUM && child >= 0 && HOPFmtIsTypeNodeKind(c->ast->nodes[child].kind)) {
        underType = child;
        child = HOPFmtNextSibling(c->ast, child);
    }

    if ((n->flags & HOPAstFlag_PUB) != 0 && HOPFmtWriteCStr(c, "pub ") != 0) {
        return -1;
    }
    if (HOPFmtWriteCStr(c, kw) != 0 || HOPFmtWriteChar(c, ' ') != 0
        || HOPFmtWriteSlice(c, n->dataStart, n->dataEnd) != 0)
    {
        return -1;
    }
    child = HOPFmtFirstChild(c->ast, nodeId);
    if (HOPFmtEmitTypeParamList(c, &child) != 0) {
        return -1;
    }
    if (underType >= 0) {
        if (HOPFmtWriteChar(c, ' ') != 0 || HOPFmtEmitType(c, underType) != 0) {
            return -1;
        }
        child = HOPFmtNextSibling(c->ast, underType);
    }
    if (HOPFmtWriteChar(c, ' ') != 0 || HOPFmtWriteChar(c, '{') != 0) {
        return -1;
    }

    if (child >= 0) {
        if (n->kind == HOPAst_ENUM) {
            int     enumHasPayload = 0;
            int32_t scanItem = child;
            while (scanItem >= 0) {
                int32_t vch = HOPFmtFirstChild(c->ast, scanItem);
                while (vch >= 0) {
                    if (c->ast->nodes[vch].kind == HOPAst_FIELD) {
                        enumHasPayload = 1;
                        break;
                    }
                    vch = HOPFmtNextSibling(c->ast, vch);
                }
                if (enumHasPayload) {
                    break;
                }
                scanItem = HOPFmtNextSibling(c->ast, scanItem);
            }
            if (HOPFmtNewline(c) != 0) {
                return -1;
            }
            c->indent++;
            if (!enumHasPayload) {
                while (child >= 0) {
                    int32_t next = HOPFmtNextSibling(c->ast, child);
                    int32_t last = child;
                    if (prevEmitted >= 0) {
                        if (HOPFmtNewline(c) != 0) {
                            return -1;
                        }
                        if (HOPFmtNeedsBlankLineBeforeNode(c, prevEmitted, child)
                            && HOPFmtNewline(c) != 0)
                        {
                            return -1;
                        }
                    }
                    if (HOPFmtEmitAlignedEnumGroup(c, child, &last, &next) != 0) {
                        return -1;
                    }
                    prevEmitted = last;
                    child = next;
                }
            } else {
                while (child >= 0) {
                    int32_t           next = HOPFmtNextSibling(c->ast, child);
                    const HOPAstNode* item = &c->ast->nodes[child];
                    int32_t           payloadFirst = -1;
                    int32_t           tagExpr = -1;
                    int32_t           ch = HOPFmtFirstChild(c->ast, child);
                    if (prevEmitted >= 0) {
                        if (HOPFmtNewline(c) != 0) {
                            return -1;
                        }
                        if (HOPFmtNeedsBlankLineBeforeNode(c, prevEmitted, child)
                            && HOPFmtNewline(c) != 0)
                        {
                            return -1;
                        }
                    }
                    if (HOPFmtEmitLeadingCommentsForNode(c, child) != 0
                        || HOPFmtWriteSlice(c, item->dataStart, item->dataEnd) != 0)
                    {
                        return -1;
                    }
                    while (ch >= 0) {
                        if (c->ast->nodes[ch].kind == HOPAst_FIELD) {
                            if (payloadFirst < 0) {
                                payloadFirst = ch;
                            }
                        } else if (tagExpr < 0) {
                            tagExpr = ch;
                        }
                        ch = HOPFmtNextSibling(c->ast, ch);
                    }
                    if (payloadFirst >= 0) {
                        if (HOPFmtWriteChar(c, '{') != 0
                            || HOPFmtEmitAggregateFieldBody(c, payloadFirst) != 0
                            || HOPFmtWriteChar(c, '}') != 0)
                        {
                            return -1;
                        }
                    }
                    if (tagExpr >= 0) {
                        if (HOPFmtWriteCStr(c, " = ") != 0 || HOPFmtEmitExpr(c, tagExpr, 0) != 0) {
                            return -1;
                        }
                    }
                    if (HOPFmtEmitTrailingCommentsForNode(c, child) != 0) {
                        return -1;
                    }
                    prevEmitted = child;
                    child = next;
                }
            }
            c->indent--;
            if (HOPFmtNewline(c) != 0) {
                return -1;
            }
        } else if (HOPFmtEmitAggregateFieldBody(c, child) != 0) {
            return -1;
        }
    }

    if (HOPFmtWriteChar(c, '}') != 0) {
        return -1;
    }
    return HOPFmtEmitTrailingCommentsForNode(c, nodeId);
}

static int HOPFmtEmitFnDecl(HOPFmtCtx* c, int32_t nodeId) {
    const HOPAstNode* n = &c->ast->nodes[nodeId];
    int32_t           child;
    int               firstParam = 1;
    int32_t           retType = -1;
    int32_t           ctxClause = -1;
    int32_t           body = -1;

    if ((n->flags & HOPAstFlag_PUB) != 0 && HOPFmtWriteCStr(c, "pub ") != 0) {
        return -1;
    }
    if (HOPFmtWriteCStr(c, "fn ") != 0 || HOPFmtWriteSlice(c, n->dataStart, n->dataEnd) != 0) {
        return -1;
    }

    child = HOPFmtFirstChild(c->ast, nodeId);
    if (HOPFmtEmitTypeParamList(c, &child) != 0) {
        return -1;
    }
    if (HOPFmtWriteChar(c, '(') != 0) {
        return -1;
    }
    while (child >= 0) {
        const HOPAstNode* ch = &c->ast->nodes[child];
        int32_t           runFirst = child;
        int32_t           runLast = child;
        int32_t           runType = -1;
        uint16_t          runFlags;
        int32_t           nextAfterRun;
        int               firstNameInRun = 1;
        if (ch->kind != HOPAst_PARAM) {
            break;
        }
        runType = HOPFmtFirstChild(c->ast, child);
        runFlags = (uint16_t)(ch->flags & (HOPAstFlag_PARAM_CONST | HOPAstFlag_PARAM_VARIADIC));
        nextAfterRun = HOPFmtNextSibling(c->ast, child);
        while (nextAfterRun >= 0) {
            const HOPAstNode* nextParam = &c->ast->nodes[nextAfterRun];
            int32_t           nextType;
            uint16_t          nextFlags;
            if (nextParam->kind != HOPAst_PARAM) {
                break;
            }
            nextType = HOPFmtFirstChild(c->ast, nextAfterRun);
            nextFlags =
                (uint16_t)(nextParam->flags & (HOPAstFlag_PARAM_CONST | HOPAstFlag_PARAM_VARIADIC));
            if (nextFlags != runFlags
                || !HOPFmtTypeNodesEqualBySource(c->ast, c->src, runType, nextType))
            {
                break;
            }
            runLast = nextAfterRun;
            nextAfterRun = HOPFmtNextSibling(c->ast, nextAfterRun);
        }
        if (!firstParam && HOPFmtWriteCStr(c, ", ") != 0) {
            return -1;
        }
        if ((runFlags & HOPAstFlag_PARAM_CONST) != 0 && HOPFmtWriteCStr(c, "const ") != 0) {
            return -1;
        }
        while (runFirst >= 0) {
            const HOPAstNode* p = &c->ast->nodes[runFirst];
            if (!firstNameInRun && HOPFmtWriteCStr(c, ", ") != 0) {
                return -1;
            }
            if (HOPFmtWriteSlice(c, p->dataStart, p->dataEnd) != 0) {
                return -1;
            }
            firstNameInRun = 0;
            if (runFirst == runLast) {
                break;
            }
            runFirst = HOPFmtNextSibling(c->ast, runFirst);
        }
        if (HOPFmtWriteChar(c, ' ') != 0) {
            return -1;
        }
        if ((runFlags & HOPAstFlag_PARAM_VARIADIC) != 0 && HOPFmtWriteCStr(c, "...") != 0) {
            return -1;
        }
        if (runType >= 0 && HOPFmtEmitType(c, runType) != 0) {
            return -1;
        }
        firstParam = 0;
        child = nextAfterRun;
    }

    if (HOPFmtWriteChar(c, ')') != 0) {
        return -1;
    }

    while (child >= 0) {
        const HOPAstNode* ch = &c->ast->nodes[child];
        if (ch->kind == HOPAst_CONTEXT_CLAUSE) {
            ctxClause = child;
        } else if (ch->kind == HOPAst_BLOCK) {
            body = child;
        } else if (HOPFmtIsTypeNodeKind(ch->kind) && ch->flags == 1) {
            retType = child;
        }
        child = HOPFmtNextSibling(c->ast, child);
    }

    if (retType >= 0) {
        if (HOPFmtWriteChar(c, ' ') != 0 || HOPFmtEmitType(c, retType) != 0) {
            return -1;
        }
    }
    if (ctxClause >= 0) {
        int32_t ctype = HOPFmtFirstChild(c->ast, ctxClause);
        if (HOPFmtWriteCStr(c, " context ") != 0) {
            return -1;
        }
        if (ctype >= 0 && HOPFmtEmitType(c, ctype) != 0) {
            return -1;
        }
    }
    if (body >= 0) {
        if (HOPFmtWriteChar(c, ' ') != 0 || HOPFmtEmitBlock(c, body) != 0) {
            return -1;
        }
    }
    return HOPFmtEmitTrailingCommentsForNode(c, nodeId);
}

static int HOPFmtEmitDirective(HOPFmtCtx* c, int32_t nodeId) {
    const HOPAstNode* n = &c->ast->nodes[nodeId];
    int32_t           child = HOPFmtFirstChild(c->ast, nodeId);
    int               first = 1;
    if (HOPFmtWriteChar(c, '@') != 0 || HOPFmtWriteSlice(c, n->dataStart, n->dataEnd) != 0) {
        return -1;
    }
    if (child >= 0 || HOPFmtSliceHasChar(c->src, n->dataEnd, n->end, '(')) {
        if (HOPFmtWriteChar(c, '(') != 0) {
            return -1;
        }
        while (child >= 0) {
            int32_t next = HOPFmtNextSibling(c->ast, child);
            if (!first && HOPFmtWriteCStr(c, ", ") != 0) {
                return -1;
            }
            if (HOPFmtEmitExpr(c, child, 0) != 0) {
                return -1;
            }
            first = 0;
            child = next;
        }
        if (HOPFmtWriteChar(c, ')') != 0) {
            return -1;
        }
    }
    return HOPFmtEmitTrailingCommentsForNode(c, nodeId);
}

static int HOPFmtEmitDecl(HOPFmtCtx* c, int32_t nodeId) {
    const HOPAstNode* n = &c->ast->nodes[nodeId];
    if (HOPFmtEmitLeadingCommentsForNode(c, nodeId) != 0) {
        return -1;
    }
    switch (n->kind) {
        case HOPAst_IMPORT:     return HOPFmtEmitImport(c, nodeId);
        case HOPAst_DIRECTIVE:  return HOPFmtEmitDirective(c, nodeId);
        case HOPAst_STRUCT:     return HOPFmtEmitAggregateDecl(c, nodeId, "struct");
        case HOPAst_UNION:      return HOPFmtEmitAggregateDecl(c, nodeId, "union");
        case HOPAst_ENUM:       return HOPFmtEmitAggregateDecl(c, nodeId, "enum");
        case HOPAst_TYPE_ALIAS: {
            int32_t type = HOPFmtFirstChild(c->ast, nodeId);
            if ((n->flags & HOPAstFlag_PUB) != 0 && HOPFmtWriteCStr(c, "pub ") != 0) {
                return -1;
            }
            if (HOPFmtWriteCStr(c, "type ") != 0
                || HOPFmtWriteSlice(c, n->dataStart, n->dataEnd) != 0)
            {
                return -1;
            }
            if (HOPFmtEmitTypeParamList(c, &type) != 0) {
                return -1;
            }
            if (HOPFmtWriteChar(c, ' ') != 0 || (type >= 0 && HOPFmtEmitType(c, type) != 0)) {
                return -1;
            }
            return HOPFmtEmitTrailingCommentsForNode(c, nodeId);
        }
        case HOPAst_FN: return HOPFmtEmitFnDecl(c, nodeId);
        case HOPAst_VAR:
            if ((n->flags & HOPAstFlag_PUB) != 0 && HOPFmtWriteCStr(c, "pub ") != 0) {
                return -1;
            }
            if (HOPFmtEmitVarLike(c, nodeId, "var") != 0) {
                return -1;
            }
            return HOPFmtEmitTrailingCommentsForNode(c, nodeId);
        case HOPAst_CONST:
            if ((n->flags & HOPAstFlag_PUB) != 0 && HOPFmtWriteCStr(c, "pub ") != 0) {
                return -1;
            }
            if (HOPFmtEmitVarLike(c, nodeId, "const") != 0) {
                return -1;
            }
            return HOPFmtEmitTrailingCommentsForNode(c, nodeId);
        default:
            if (HOPFmtWriteSlice(c, n->start, n->end) != 0) {
                return -1;
            }
            return HOPFmtEmitTrailingCommentsForNode(c, nodeId);
    }
}

static int HOPFmtEmitDirectiveGroup(HOPFmtCtx* c, int32_t firstDirective, int32_t* outNext) {
    int32_t child = firstDirective;
    int32_t next = -1;
    int     first = 1;
    while (child >= 0 && c->ast->nodes[child].kind == HOPAst_DIRECTIVE) {
        next = HOPFmtNextSibling(c->ast, child);
        if (!first && HOPFmtNewline(c) != 0) {
            return -1;
        }
        if (HOPFmtEmitDecl(c, child) != 0) {
            return -1;
        }
        first = 0;
        child = next;
    }
    if (child >= 0) {
        if (!first && HOPFmtNewline(c) != 0) {
            return -1;
        }
        if (HOPFmtEmitDecl(c, child) != 0) {
            return -1;
        }
        next = HOPFmtNextSibling(c->ast, child);
    }
    *outNext = next;
    return 0;
}

static int HOPFmtEmitFile(HOPFmtCtx* c) {
    int32_t child = HOPFmtFirstChild(c->ast, c->ast->root);
    int     first = 1;
    while (child >= 0) {
        int32_t next;
        if (!first) {
            if (HOPFmtNewline(c) != 0 || HOPFmtNewline(c) != 0) {
                return -1;
            }
        }
        next = HOPFmtNextSibling(c->ast, child);
        if (c->ast->nodes[child].kind == HOPAst_IMPORT) {
            int32_t lastSourceNode = child;
            if (HOPFmtEmitImportGroup(c, child, &lastSourceNode, &next) != 0) {
                return -1;
            }
        } else if (c->ast->nodes[child].kind == HOPAst_DIRECTIVE) {
            if (HOPFmtEmitDirectiveGroup(c, child, &next) != 0) {
                return -1;
            }
        } else if (HOPFmtEmitDecl(c, child) != 0) {
            return -1;
        }
        first = 0;
        child = next;
    }
    if (!first && HOPFmtNewline(c) != 0) {
        return -1;
    }
    if (HOPFmtEmitRemainingComments(c) != 0) {
        return -1;
    }
    if (c->out.len == 0 || c->out.v[c->out.len - 1u] != '\n') {
        if (HOPFmtNewline(c) != 0) {
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

int HOPFormat(
    HOPArena*  arena,
    HOPStrView src,
    const HOPFormatOptions* _Nullable options,
    HOPStrView* out,
    HOPDiag* _Nullable diag) {
    HOPAst          ast;
    HOPParseExtras  extras;
    HOPParseOptions parseOptions;
    HOPFmtCtx       c;
    uint32_t        indentWidth = 4u;

    if (diag != NULL) {
        *diag = (HOPDiag){ 0 };
    }
    if (out == NULL) {
        return -1;
    }
    out->ptr = NULL;
    out->len = 0;

    if (options != NULL && options->indentWidth != 0) {
        indentWidth = options->indentWidth;
    }

    parseOptions.flags = HOPParseFlag_COLLECT_FORMATTING;
    if (HOPParse(arena, src, &parseOptions, &ast, &extras, diag) != 0) {
        return -1;
    }
    if (HOPFmtRewriteAst(&ast, src, options) != 0) {
        if (diag != NULL) {
            diag->code = HOPDiag_ARENA_OOM;
            diag->type = HOPDiagTypeOfCode(HOPDiag_ARENA_OOM);
            diag->start = 0;
            diag->end = 0;
            diag->argStart = 0;
            diag->argEnd = 0;
            diag->relatedStart = 0;
            diag->relatedEnd = 0;
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
        c.commentUsed = (uint8_t*)HOPArenaAlloc(
            arena, c.commentLen * (uint32_t)sizeof(uint8_t), (uint32_t)_Alignof(uint8_t));
        if (c.commentUsed == NULL) {
            if (diag != NULL) {
                diag->code = HOPDiag_ARENA_OOM;
                diag->type = HOPDiagTypeOfCode(HOPDiag_ARENA_OOM);
                diag->start = 0;
                diag->end = 0;
                diag->argStart = 0;
                diag->argEnd = 0;
                diag->relatedStart = 0;
                diag->relatedEnd = 0;
                diag->detail = NULL;
                diag->hintOverride = NULL;
            }
            return -1;
        }
        memset(c.commentUsed, 0, c.commentLen);
    }

    if (HOPFmtEmitFile(&c) != 0) {
        if (diag != NULL && diag->code == HOPDiag_NONE) {
            diag->code = HOPDiag_ARENA_OOM;
            diag->type = HOPDiagTypeOfCode(HOPDiag_ARENA_OOM);
            diag->start = 0;
            diag->end = 0;
            diag->argStart = 0;
            diag->argEnd = 0;
            diag->relatedStart = 0;
            diag->relatedEnd = 0;
            diag->detail = NULL;
            diag->hintOverride = NULL;
        }
        return -1;
    }

    if (HOPFmtBufAppendChar(&c.out, '\0') != 0) {
        if (diag != NULL) {
            diag->code = HOPDiag_ARENA_OOM;
            diag->type = HOPDiagTypeOfCode(HOPDiag_ARENA_OOM);
            diag->start = 0;
            diag->end = 0;
            diag->argStart = 0;
            diag->argEnd = 0;
            diag->relatedStart = 0;
            diag->relatedEnd = 0;
            diag->detail = NULL;
            diag->hintOverride = NULL;
        }
        return -1;
    }

    out->ptr = c.out.v;
    out->len = c.out.len - 1u;
    return 0;
}

HOP_API_END
