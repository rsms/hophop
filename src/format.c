#include "libhop-impl.h"
#include "hop_internal.h"

H2_API_BEGIN

typedef struct {
    H2Arena* arena;
    char* _Nullable v;
    uint32_t len;
    uint32_t cap;
} H2FmtBuf;

typedef struct {
    const H2Ast* ast;
    H2StrView    src;
    const H2Comment* _Nullable comments;
    uint32_t commentLen;
    uint8_t* _Nullable commentUsed;
    uint32_t indent;
    uint32_t indentWidth;
    int      lineStart;
    H2FmtBuf out;
} H2FmtCtx;

enum {
    H2FmtFlag_DROP_REDUNDANT_LITERAL_CAST = 0x4000u,
};

typedef enum {
    H2FmtNumericType_INVALID = 0,
    H2FmtNumericType_I8,
    H2FmtNumericType_I16,
    H2FmtNumericType_I32,
    H2FmtNumericType_I64,
    H2FmtNumericType_INT,
    H2FmtNumericType_U8,
    H2FmtNumericType_U16,
    H2FmtNumericType_U32,
    H2FmtNumericType_U64,
    H2FmtNumericType_UINT,
    H2FmtNumericType_F32,
    H2FmtNumericType_F64,
} H2FmtNumericType;

static int H2FmtBufReserve(H2FmtBuf* b, uint32_t extra) {
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
    p = (char*)H2ArenaAlloc(b->arena, cap, (uint32_t)_Alignof(char));
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

static int H2FmtBufAppendChar(H2FmtBuf* b, char c) {
    if (H2FmtBufReserve(b, 1u) != 0) {
        return -1;
    }
    b->v[b->len++] = c;
    return 0;
}

static int     H2FmtWriteChar(H2FmtCtx* c, char ch);
static int32_t H2FmtFindEnclosingFnNode(const H2Ast* ast, int32_t nodeId);
static int32_t H2FmtFindParentNode(const H2Ast* ast, int32_t childNodeId);
static int     H2FmtEmitType(H2FmtCtx* c, int32_t nodeId);

static uint32_t H2FmtCStrLen(const char* s) {
    const char* p = s;
    while (*p != '\0') {
        p++;
    }
    return (uint32_t)(p - s);
}

static int H2FmtWriteIndent(H2FmtCtx* c) {
    uint32_t i;
    if (!c->lineStart) {
        return 0;
    }
    for (i = 0; i < c->indent; i++) {
        if (H2FmtBufAppendChar(&c->out, '\t') != 0) {
            return -1;
        }
    }
    c->lineStart = 0;
    return 0;
}

static int H2FmtWriteSpaces(H2FmtCtx* c, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (H2FmtWriteChar(c, ' ') != 0) {
            return -1;
        }
    }
    return 0;
}

static int H2FmtWriteChar(H2FmtCtx* c, char ch) {
    if (ch != '\n' && c->lineStart) {
        if (H2FmtWriteIndent(c) != 0) {
            return -1;
        }
    }
    if (H2FmtBufAppendChar(&c->out, ch) != 0) {
        return -1;
    }
    c->lineStart = (ch == '\n');
    return 0;
}

static int H2FmtWrite(H2FmtCtx* c, const char* s, uint32_t len) {
    uint32_t i;
    for (i = 0; i < len; i++) {
        if (H2FmtWriteChar(c, s[i]) != 0) {
            return -1;
        }
    }
    return 0;
}

static int H2FmtWriteCStr(H2FmtCtx* c, const char* s) {
    while (*s != '\0') {
        if (H2FmtWriteChar(c, *s++) != 0) {
            return -1;
        }
    }
    return 0;
}

static int H2FmtWriteSlice(H2FmtCtx* c, uint32_t start, uint32_t end) {
    if (end < start || end > c->src.len) {
        return -1;
    }
    return H2FmtWrite(c, c->src.ptr + start, end - start);
}

static int H2FmtWriteSliceLiteral(H2FmtCtx* c, uint32_t start, uint32_t end) {
    uint32_t i;
    if (end < start || end > c->src.len) {
        return -1;
    }
    if (c->lineStart && H2FmtWriteIndent(c) != 0) {
        return -1;
    }
    for (i = start; i < end; i++) {
        char ch = c->src.ptr[i];
        if (H2FmtBufAppendChar(&c->out, ch) != 0) {
            return -1;
        }
        c->lineStart = (ch == '\n');
    }
    return 0;
}

static int H2FmtNewline(H2FmtCtx* c) {
    return H2FmtWriteChar(c, '\n');
}

static int32_t H2FmtFirstChild(const H2Ast* ast, int32_t nodeId) {
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    return ast->nodes[nodeId].firstChild;
}

static int32_t H2FmtNextSibling(const H2Ast* ast, int32_t nodeId) {
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    return ast->nodes[nodeId].nextSibling;
}

static uint32_t H2FmtListCount(const H2Ast* ast, int32_t listNodeId) {
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

static int32_t H2FmtListItemAt(const H2Ast* ast, int32_t listNodeId, uint32_t index) {
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

static int H2FmtSlicesEqual(
    H2StrView src, uint32_t aStart, uint32_t aEnd, uint32_t bStart, uint32_t bEnd) {
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

static int H2FmtSliceEqLiteral(H2StrView src, uint32_t start, uint32_t end, const char* lit) {
    uint32_t len = H2FmtCStrLen(lit);
    if (end < start || end > src.len || (end - start) != len) {
        return 0;
    }
    return len == 0 || memcmp(src.ptr + start, lit, len) == 0;
}

static int H2FmtSliceHasChar(H2StrView src, uint32_t start, uint32_t end, char ch) {
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

static int H2FmtExprIsPlainIdent(
    const H2Ast* ast, int32_t exprNodeId, uint32_t* outStart, uint32_t* outEnd) {
    const H2AstNode* n;
    if (exprNodeId < 0 || (uint32_t)exprNodeId >= ast->len || outStart == NULL || outEnd == NULL) {
        return 0;
    }
    n = &ast->nodes[exprNodeId];
    if (n->kind != H2Ast_IDENT || (n->flags & H2AstFlag_PAREN) != 0 || n->dataEnd <= n->dataStart) {
        return 0;
    }
    *outStart = n->dataStart;
    *outEnd = n->dataEnd;
    return 1;
}

static int H2FmtIsTypeNodeKindRaw(H2AstKind kind) {
    return kind == H2Ast_TYPE_NAME || kind == H2Ast_TYPE_PTR || kind == H2Ast_TYPE_REF
        || kind == H2Ast_TYPE_MUTREF || kind == H2Ast_TYPE_ARRAY || kind == H2Ast_TYPE_VARRAY
        || kind == H2Ast_TYPE_SLICE || kind == H2Ast_TYPE_MUTSLICE || kind == H2Ast_TYPE_OPTIONAL
        || kind == H2Ast_TYPE_FN || kind == H2Ast_TYPE_ANON_STRUCT || kind == H2Ast_TYPE_ANON_UNION
        || kind == H2Ast_TYPE_TUPLE || kind == H2Ast_TYPE_PARAM;
}

static H2FmtNumericType H2FmtNumericTypeFromName(H2StrView src, uint32_t start, uint32_t end) {
    if (H2FmtSliceEqLiteral(src, start, end, "i8")) {
        return H2FmtNumericType_I8;
    }
    if (H2FmtSliceEqLiteral(src, start, end, "i16")) {
        return H2FmtNumericType_I16;
    }
    if (H2FmtSliceEqLiteral(src, start, end, "i32")) {
        return H2FmtNumericType_I32;
    }
    if (H2FmtSliceEqLiteral(src, start, end, "i64")) {
        return H2FmtNumericType_I64;
    }
    if (H2FmtSliceEqLiteral(src, start, end, "int")) {
        return H2FmtNumericType_INT;
    }
    if (H2FmtSliceEqLiteral(src, start, end, "u8")) {
        return H2FmtNumericType_U8;
    }
    if (H2FmtSliceEqLiteral(src, start, end, "u16")) {
        return H2FmtNumericType_U16;
    }
    if (H2FmtSliceEqLiteral(src, start, end, "u32")) {
        return H2FmtNumericType_U32;
    }
    if (H2FmtSliceEqLiteral(src, start, end, "u64")) {
        return H2FmtNumericType_U64;
    }
    if (H2FmtSliceEqLiteral(src, start, end, "uint")) {
        return H2FmtNumericType_UINT;
    }
    if (H2FmtSliceEqLiteral(src, start, end, "f32")) {
        return H2FmtNumericType_F32;
    }
    if (H2FmtSliceEqLiteral(src, start, end, "f64")) {
        return H2FmtNumericType_F64;
    }
    return H2FmtNumericType_INVALID;
}

static H2FmtNumericType H2FmtNumericTypeFromTypeNode(
    const H2Ast* ast, H2StrView src, int32_t nodeId) {
    const H2AstNode* n;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return H2FmtNumericType_INVALID;
    }
    n = &ast->nodes[nodeId];
    if (n->kind != H2Ast_TYPE_NAME) {
        return H2FmtNumericType_INVALID;
    }
    return H2FmtNumericTypeFromName(src, n->dataStart, n->dataEnd);
}

static H2FmtNumericType H2FmtCastLiteralNumericType(
    const H2Ast* ast, H2StrView src, int32_t exprNodeId, int32_t typeNodeId) {
    if (exprNodeId < 0 || typeNodeId < 0 || (uint32_t)exprNodeId >= ast->len
        || (uint32_t)typeNodeId >= ast->len)
    {
        return H2FmtNumericType_INVALID;
    }
    if (ast->nodes[exprNodeId].kind == H2Ast_INT) {
        H2FmtNumericType t = H2FmtNumericTypeFromTypeNode(ast, src, typeNodeId);
        switch (t) {
            case H2FmtNumericType_I8:
            case H2FmtNumericType_I16:
            case H2FmtNumericType_I32:
            case H2FmtNumericType_I64:
            case H2FmtNumericType_INT:
            case H2FmtNumericType_U8:
            case H2FmtNumericType_U16:
            case H2FmtNumericType_U32:
            case H2FmtNumericType_U64:
            case H2FmtNumericType_UINT: return t;
            default:                    return H2FmtNumericType_INVALID;
        }
    }
    if (ast->nodes[exprNodeId].kind == H2Ast_FLOAT) {
        H2FmtNumericType t = H2FmtNumericTypeFromTypeNode(ast, src, typeNodeId);
        return (t == H2FmtNumericType_F32 || t == H2FmtNumericType_F64)
                 ? t
                 : H2FmtNumericType_INVALID;
    }
    return H2FmtNumericType_INVALID;
}

static int H2FmtBinaryOpSharesOperandType(uint16_t op) {
    switch ((H2TokenKind)op) {
        case H2Tok_ADD:
        case H2Tok_SUB:
        case H2Tok_MUL:
        case H2Tok_DIV:
        case H2Tok_MOD:
        case H2Tok_AND:
        case H2Tok_OR:
        case H2Tok_XOR:
        case H2Tok_EQ:
        case H2Tok_NEQ:
        case H2Tok_LT:
        case H2Tok_GT:
        case H2Tok_LTE:
        case H2Tok_GTE: return 1;
        default:        return 0;
    }
}

static int H2FmtIsAssignmentOp(H2TokenKind kind);

static int H2FmtTypeNodesEqualBySource(
    const H2Ast* ast, H2StrView src, int32_t aTypeNodeId, int32_t bTypeNodeId) {
    const H2AstNode* a;
    const H2AstNode* b;
    if (aTypeNodeId < 0 || bTypeNodeId < 0 || (uint32_t)aTypeNodeId >= ast->len
        || (uint32_t)bTypeNodeId >= ast->len)
    {
        return 0;
    }
    a = &ast->nodes[aTypeNodeId];
    b = &ast->nodes[bTypeNodeId];
    return H2FmtSlicesEqual(src, a->start, a->end, b->start, b->end);
}

typedef struct {
    uint32_t nameStart;
    uint32_t nameEnd;
    int32_t  concreteTypeNodeId;
} H2FmtTypeBinding;

typedef struct {
    H2FmtTypeBinding items[16];
    uint32_t         len;
} H2FmtTypeEnv;

typedef struct {
    int32_t  paramNodeId;
    int32_t  typeNodeId;
    uint16_t flags;
} H2FmtCallParam;

typedef struct {
    int32_t  argNodeId;
    int32_t  exprNodeId;
    uint32_t labelStart;
    uint32_t labelEnd;
    uint8_t  hasLabel;
    uint8_t  isSynthetic;
} H2FmtCallActual;

typedef struct {
    int32_t      typeNodeId;
    H2FmtTypeEnv env;
} H2FmtInferredType;

static void H2FmtTypeEnvInit(H2FmtTypeEnv* env) {
    if (env != NULL) {
        env->len = 0;
    }
}

static void H2FmtInferredTypeInit(H2FmtInferredType* inferred) {
    if (inferred != NULL) {
        inferred->typeNodeId = -1;
        H2FmtTypeEnvInit(&inferred->env);
    }
}

static int H2FmtInferredTypeSet(
    H2FmtInferredType* inferred, int32_t typeNodeId, const H2FmtTypeEnv* _Nullable env);
static int H2FmtInferredTypeMatchesNode(
    const H2Ast* ast, H2StrView src, const H2FmtInferredType* inferred, int32_t typeNodeId);

static int32_t H2FmtTypeEnvFind(
    const H2FmtTypeEnv* env, H2StrView src, uint32_t nameStart, uint32_t nameEnd) {
    uint32_t i;
    if (env == NULL || nameEnd <= nameStart) {
        return -1;
    }
    for (i = 0; i < env->len; i++) {
        if (H2FmtSlicesEqual(
                src, env->items[i].nameStart, env->items[i].nameEnd, nameStart, nameEnd))
        {
            return (int32_t)i;
        }
    }
    return -1;
}

static int H2FmtTypeEnvAdd(
    H2FmtTypeEnv* env, uint32_t nameStart, uint32_t nameEnd, int32_t concreteTypeNodeId) {
    if (env == NULL || nameEnd <= nameStart || env->len >= 16u) {
        return 0;
    }
    env->items[env->len].nameStart = nameStart;
    env->items[env->len].nameEnd = nameEnd;
    env->items[env->len].concreteTypeNodeId = concreteTypeNodeId;
    env->len++;
    return 1;
}

static int H2FmtTypeEnvInitFromDeclTypeParams(
    const H2Ast* ast, int32_t declNodeId, H2FmtTypeEnv* env) {
    int32_t child;
    H2FmtTypeEnvInit(env);
    if (declNodeId < 0 || (uint32_t)declNodeId >= ast->len || env == NULL) {
        return 0;
    }
    child = H2FmtFirstChild(ast, declNodeId);
    while (child >= 0 && ast->nodes[child].kind == H2Ast_TYPE_PARAM) {
        if (!H2FmtTypeEnvAdd(env, ast->nodes[child].dataStart, ast->nodes[child].dataEnd, -1)) {
            return 0;
        }
        child = H2FmtNextSibling(ast, child);
    }
    return 1;
}

static int H2FmtTypeNameIsBoundParam(
    const H2Ast* ast,
    H2StrView    src,
    int32_t      typeNodeId,
    const H2FmtTypeEnv* _Nullable env,
    int32_t* outBindingIndex) {
    const H2AstNode* n;
    int32_t          bindingIndex;
    if (typeNodeId < 0 || (uint32_t)typeNodeId >= ast->len) {
        return 0;
    }
    n = &ast->nodes[typeNodeId];
    if (n->kind != H2Ast_TYPE_NAME || H2FmtFirstChild(ast, typeNodeId) >= 0) {
        return 0;
    }
    bindingIndex = H2FmtTypeEnvFind(env, src, n->dataStart, n->dataEnd);
    if (bindingIndex < 0) {
        return 0;
    }
    if (outBindingIndex != NULL) {
        *outBindingIndex = bindingIndex;
    }
    return 1;
}

static int H2FmtTypeCompatibleWithEnvs(
    const H2Ast* ast,
    H2StrView    src,
    int32_t      wantNodeId,
    H2FmtTypeEnv* _Nullable wantEnv,
    int32_t gotNodeId,
    const H2FmtTypeEnv* _Nullable gotEnv);

static int H2FmtTypeCompatibleChildren(
    const H2Ast* ast,
    H2StrView    src,
    int32_t      wantNodeId,
    H2FmtTypeEnv* _Nullable wantEnv,
    int32_t gotNodeId,
    const H2FmtTypeEnv* _Nullable gotEnv) {
    int32_t wantChild = H2FmtFirstChild(ast, wantNodeId);
    int32_t gotChild = H2FmtFirstChild(ast, gotNodeId);
    while (wantChild >= 0 && gotChild >= 0) {
        if (!H2FmtTypeCompatibleWithEnvs(ast, src, wantChild, wantEnv, gotChild, gotEnv)) {
            return 0;
        }
        wantChild = H2FmtNextSibling(ast, wantChild);
        gotChild = H2FmtNextSibling(ast, gotChild);
    }
    return wantChild < 0 && gotChild < 0;
}

static int H2FmtTypeCompatibleWithEnvs(
    const H2Ast* ast,
    H2StrView    src,
    int32_t      wantNodeId,
    H2FmtTypeEnv* _Nullable wantEnv,
    int32_t gotNodeId,
    const H2FmtTypeEnv* _Nullable gotEnv) {
    const H2AstNode* want;
    const H2AstNode* got;
    int32_t          bindingIndex;
    if (wantNodeId < 0 || gotNodeId < 0 || (uint32_t)wantNodeId >= ast->len
        || (uint32_t)gotNodeId >= ast->len)
    {
        return 0;
    }
    if (H2FmtTypeNameIsBoundParam(ast, src, wantNodeId, wantEnv, &bindingIndex)) {
        int32_t boundNodeId = wantEnv->items[bindingIndex].concreteTypeNodeId;
        if (boundNodeId >= 0) {
            return H2FmtTypeCompatibleWithEnvs(ast, src, boundNodeId, NULL, gotNodeId, gotEnv);
        }
        if (gotEnv == NULL || !H2FmtTypeNameIsBoundParam(ast, src, gotNodeId, gotEnv, &bindingIndex)
            || gotEnv->items[bindingIndex].concreteTypeNodeId < 0)
        {
            wantEnv
                ->items[H2FmtTypeEnvFind(
                    wantEnv, src, ast->nodes[wantNodeId].dataStart, ast->nodes[wantNodeId].dataEnd)]
                .concreteTypeNodeId = gotNodeId;
            return 1;
        }
    }
    if (gotEnv != NULL && H2FmtTypeNameIsBoundParam(ast, src, gotNodeId, gotEnv, &bindingIndex)) {
        int32_t boundNodeId = gotEnv->items[bindingIndex].concreteTypeNodeId;
        if (boundNodeId < 0) {
            return 0;
        }
        return H2FmtTypeCompatibleWithEnvs(ast, src, wantNodeId, wantEnv, boundNodeId, NULL);
    }
    want = &ast->nodes[wantNodeId];
    got = &ast->nodes[gotNodeId];
    if (want->kind != got->kind) {
        return 0;
    }
    switch (want->kind) {
        case H2Ast_TYPE_NAME:
            if (!H2FmtSlicesEqual(
                    src, want->dataStart, want->dataEnd, got->dataStart, got->dataEnd))
            {
                return 0;
            }
            return H2FmtTypeCompatibleChildren(ast, src, wantNodeId, wantEnv, gotNodeId, gotEnv);
        case H2Ast_TYPE_PTR:
        case H2Ast_TYPE_REF:
        case H2Ast_TYPE_MUTREF:
        case H2Ast_TYPE_SLICE:
        case H2Ast_TYPE_MUTSLICE:
        case H2Ast_TYPE_OPTIONAL:
        case H2Ast_TYPE_ARRAY:
        case H2Ast_TYPE_VARRAY:
        case H2Ast_TYPE_TUPLE:
        case H2Ast_TYPE_FN:
            return H2FmtTypeCompatibleChildren(ast, src, wantNodeId, wantEnv, gotNodeId, gotEnv);
        default: return H2FmtSlicesEqual(src, want->start, want->end, got->start, got->end);
    }
}

static int H2FmtInferLocalCallAgainstFn(
    const H2Ast* ast,
    H2StrView    src,
    int32_t      callNodeId,
    int32_t      fnNodeId,
    uint32_t     beforePos,
    int32_t      targetArgNodeId,
    H2FmtInferredType* _Nullable outReturnType,
    H2FmtInferredType* _Nullable outTargetParamType);

static int H2FmtCanDropLiteralCastFromLocalCall(
    const H2Ast* ast, H2StrView src, int32_t castNodeId, int32_t castTypeNodeId) {
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
    parentNodeId = H2FmtFindParentNode(ast, castNodeId);
    if (parentNodeId < 0) {
        return 0;
    }
    if (ast->nodes[parentNodeId].kind == H2Ast_CALL_ARG) {
        argNodeId = parentNodeId;
        callNodeId = H2FmtFindParentNode(ast, parentNodeId);
    } else if (ast->nodes[parentNodeId].kind == H2Ast_CALL) {
        argNodeId = castNodeId;
        callNodeId = parentNodeId;
    } else {
        return 0;
    }
    if (callNodeId < 0 || ast->nodes[callNodeId].kind != H2Ast_CALL) {
        return 0;
    }
    calleeNodeId = H2FmtFirstChild(ast, callNodeId);
    if (calleeNodeId < 0 || (uint32_t)calleeNodeId >= ast->len) {
        return 0;
    }
    if (ast->nodes[calleeNodeId].kind == H2Ast_IDENT) {
        calleeNameStart = ast->nodes[calleeNodeId].dataStart;
        calleeNameEnd = ast->nodes[calleeNodeId].dataEnd;
    } else if (ast->nodes[calleeNodeId].kind == H2Ast_FIELD_EXPR) {
        calleeNameStart = ast->nodes[calleeNodeId].dataStart;
        calleeNameEnd = ast->nodes[calleeNodeId].dataEnd;
    } else {
        return 0;
    }
    cur = H2FmtFirstChild(ast, ast->root);
    while (cur >= 0) {
        if (ast->nodes[cur].kind == H2Ast_FN
            && H2FmtSlicesEqual(
                src,
                ast->nodes[cur].dataStart,
                ast->nodes[cur].dataEnd,
                calleeNameStart,
                calleeNameEnd))
        {
            H2FmtInferredType targetType;
            H2FmtInferredTypeInit(&targetType);
            if (!H2FmtInferLocalCallAgainstFn(
                    ast,
                    src,
                    callNodeId,
                    cur,
                    ast->nodes[castNodeId].start,
                    argNodeId,
                    NULL,
                    &targetType))
            {
                cur = H2FmtNextSibling(ast, cur);
                continue;
            }
            sawMappedCandidate = 1;
            if (!H2FmtInferredTypeMatchesNode(ast, src, &targetType, castTypeNodeId)) {
                return 0;
            }
        }
        cur = H2FmtNextSibling(ast, cur);
    }
    return sawMappedCandidate;
}

static void H2FmtGetCastParts(
    const H2Ast* ast, int32_t castNodeId, int32_t* outExprNodeId, int32_t* outTypeNodeId) {
    int32_t exprNodeId = -1;
    int32_t typeNodeId = -1;
    if (castNodeId >= 0 && (uint32_t)castNodeId < ast->len
        && ast->nodes[castNodeId].kind == H2Ast_CAST)
    {
        exprNodeId = H2FmtFirstChild(ast, castNodeId);
        typeNodeId = exprNodeId >= 0 ? H2FmtNextSibling(ast, exprNodeId) : -1;
    }
    if (outExprNodeId != NULL) {
        *outExprNodeId = exprNodeId;
    }
    if (outTypeNodeId != NULL) {
        *outTypeNodeId = typeNodeId;
    }
}

static void H2FmtGetVarLikeTypeAndInit(
    const H2Ast* ast, int32_t nodeId, int32_t* outTypeNodeId, int32_t* outInitNodeId) {
    int32_t firstChild = H2FmtFirstChild(ast, nodeId);
    int32_t typeNodeId = -1;
    int32_t initNodeId = -1;
    if (firstChild >= 0 && ast->nodes[firstChild].kind == H2Ast_NAME_LIST) {
        int32_t afterNames = H2FmtNextSibling(ast, firstChild);
        if (afterNames >= 0 && H2FmtIsTypeNodeKindRaw(ast->nodes[afterNames].kind)) {
            typeNodeId = afterNames;
            initNodeId = H2FmtNextSibling(ast, afterNames);
        } else {
            initNodeId = afterNames;
        }
    } else if (firstChild >= 0 && H2FmtIsTypeNodeKindRaw(ast->nodes[firstChild].kind)) {
        typeNodeId = firstChild;
        initNodeId = H2FmtNextSibling(ast, firstChild);
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

static int32_t H2FmtFindFnReturnTypeNode(const H2Ast* ast, int32_t fnNodeId) {
    int32_t child;
    if (fnNodeId < 0 || (uint32_t)fnNodeId >= ast->len || ast->nodes[fnNodeId].kind != H2Ast_FN) {
        return -1;
    }
    child = H2FmtFirstChild(ast, fnNodeId);
    while (child >= 0) {
        const H2AstNode* n = &ast->nodes[child];
        if (H2FmtIsTypeNodeKindRaw(n->kind) && n->flags == 1) {
            return child;
        }
        child = H2FmtNextSibling(ast, child);
    }
    return -1;
}

static int H2FmtFnReturnTypeIsGenericParam(const H2Ast* ast, H2StrView src, int32_t fnNodeId) {
    H2FmtTypeEnv env;
    int32_t      retTypeNodeId = H2FmtFindFnReturnTypeNode(ast, fnNodeId);
    int32_t      bindingIndex = -1;
    if (retTypeNodeId < 0 || !H2FmtTypeEnvInitFromDeclTypeParams(ast, fnNodeId, &env)) {
        return 0;
    }
    return H2FmtTypeNameIsBoundParam(ast, src, retTypeNodeId, &env, &bindingIndex);
}

static int32_t H2FmtFindParentNode(const H2Ast* ast, int32_t childNodeId) {
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

static int H2FmtNodeDeclaresNameRange(
    const H2Ast* ast, H2StrView src, int32_t nodeId, uint32_t nameStart, uint32_t nameEnd) {
    const H2AstNode* n;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len || nameEnd <= nameStart) {
        return 0;
    }
    n = &ast->nodes[nodeId];
    if ((n->kind == H2Ast_PARAM || n->kind == H2Ast_VAR || n->kind == H2Ast_CONST)
        && H2FmtSlicesEqual(src, n->dataStart, n->dataEnd, nameStart, nameEnd))
    {
        return 1;
    }
    if (n->kind == H2Ast_VAR || n->kind == H2Ast_CONST) {
        int32_t child = H2FmtFirstChild(ast, nodeId);
        if (child >= 0 && (uint32_t)child < ast->len && ast->nodes[child].kind == H2Ast_NAME_LIST) {
            int32_t nameNode = H2FmtFirstChild(ast, child);
            while (nameNode >= 0) {
                const H2AstNode* nn = &ast->nodes[nameNode];
                if (nn->kind == H2Ast_IDENT
                    && H2FmtSlicesEqual(src, nn->dataStart, nn->dataEnd, nameStart, nameEnd))
                {
                    return 1;
                }
                nameNode = H2FmtNextSibling(ast, nameNode);
            }
        }
    }
    return 0;
}

static int H2FmtNodeDeclaresNameLiteral(
    const H2Ast* ast, H2StrView src, int32_t nodeId, const char* nameLit) {
    const H2AstNode* n;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return 0;
    }
    n = &ast->nodes[nodeId];
    if ((n->kind == H2Ast_PARAM || n->kind == H2Ast_VAR || n->kind == H2Ast_CONST)
        && H2FmtSliceEqLiteral(src, n->dataStart, n->dataEnd, nameLit))
    {
        return 1;
    }
    if (n->kind == H2Ast_VAR || n->kind == H2Ast_CONST) {
        int32_t child = H2FmtFirstChild(ast, nodeId);
        if (child >= 0 && (uint32_t)child < ast->len && ast->nodes[child].kind == H2Ast_NAME_LIST) {
            int32_t nameNode = H2FmtFirstChild(ast, child);
            while (nameNode >= 0) {
                const H2AstNode* nn = &ast->nodes[nameNode];
                if (nn->kind == H2Ast_IDENT
                    && H2FmtSliceEqLiteral(src, nn->dataStart, nn->dataEnd, nameLit))
                {
                    return 1;
                }
                nameNode = H2FmtNextSibling(ast, nameNode);
            }
        }
    }
    return 0;
}

static int H2FmtFindLocalBindingBefore(
    const H2Ast* ast,
    H2StrView    src,
    int32_t      identNodeId,
    uint32_t     beforePos,
    int32_t* _Nullable outDeclNodeId,
    int32_t* _Nullable outTypeNodeId,
    int32_t* _Nullable outInitNodeId);

static int H2FmtFindFieldTypeOnLocalNamedType(
    const H2Ast*             ast,
    H2StrView                src,
    const H2FmtInferredType* baseType,
    uint32_t                 fieldStart,
    uint32_t                 fieldEnd,
    H2FmtInferredType*       outType);

static int H2FmtInferLocalCallAgainstFn(
    const H2Ast* ast,
    H2StrView    src,
    int32_t      callNodeId,
    int32_t      fnNodeId,
    uint32_t     beforePos,
    int32_t      targetArgNodeId,
    H2FmtInferredType* _Nullable outReturnType,
    H2FmtInferredType* _Nullable outTargetParamType);

static int H2FmtInferExprTypeEx(
    const H2Ast*       ast,
    H2StrView          src,
    int32_t            exprNodeId,
    uint32_t           beforePos,
    uint32_t           depth,
    H2FmtInferredType* outType) {
    const H2AstNode* n;
    if (outType != NULL) {
        H2FmtInferredTypeInit(outType);
    }
    if (exprNodeId < 0 || (uint32_t)exprNodeId >= ast->len) {
        return 0;
    }
    if (depth >= 32u || outType == NULL) {
        return 0;
    }
    n = &ast->nodes[exprNodeId];
    if (n->kind == H2Ast_CAST) {
        int32_t castExprNodeId = -1;
        int32_t castTypeNodeId = -1;
        H2FmtGetCastParts(ast, exprNodeId, &castExprNodeId, &castTypeNodeId);
        (void)castExprNodeId;
        return H2FmtInferredTypeSet(outType, castTypeNodeId, NULL);
    }
    if (n->kind == H2Ast_IDENT) {
        int32_t declNodeId = -1;
        int32_t typeNodeId = -1;
        int32_t initNodeId = -1;
        if (!H2FmtFindLocalBindingBefore(
                ast, src, exprNodeId, beforePos, &declNodeId, &typeNodeId, &initNodeId))
        {
            return 0;
        }
        if (typeNodeId >= 0) {
            return H2FmtInferredTypeSet(outType, typeNodeId, NULL);
        }
        if (initNodeId >= 0) {
            return H2FmtInferExprTypeEx(
                ast, src, initNodeId, ast->nodes[declNodeId].start, depth + 1u, outType);
        }
        return 0;
    }
    if (n->kind == H2Ast_FIELD_EXPR) {
        H2FmtInferredType baseType;
        int32_t           baseNodeId = H2FmtFirstChild(ast, exprNodeId);
        H2FmtInferredTypeInit(&baseType);
        return baseNodeId >= 0
            && H2FmtInferExprTypeEx(ast, src, baseNodeId, beforePos, depth + 1u, &baseType)
            && H2FmtFindFieldTypeOnLocalNamedType(
                   ast, src, &baseType, n->dataStart, n->dataEnd, outType);
    }
    if (n->kind == H2Ast_UNARY && n->op == H2Tok_MUL) {
        H2FmtInferredType targetType;
        int32_t           targetNodeId = H2FmtFirstChild(ast, exprNodeId);
        int32_t           targetTypeNodeId;
        const H2AstNode*  targetTypeNode;
        H2FmtInferredTypeInit(&targetType);
        if (targetNodeId < 0
            || !H2FmtInferExprTypeEx(ast, src, targetNodeId, beforePos, depth + 1u, &targetType))
        {
            return 0;
        }
        targetTypeNodeId = targetType.typeNodeId;
        if (targetTypeNodeId < 0 || (uint32_t)targetTypeNodeId >= ast->len) {
            return 0;
        }
        targetTypeNode = &ast->nodes[targetTypeNodeId];
        if (targetTypeNode->kind != H2Ast_TYPE_PTR && targetTypeNode->kind != H2Ast_TYPE_REF
            && targetTypeNode->kind != H2Ast_TYPE_MUTREF)
        {
            return 0;
        }
        return H2FmtInferredTypeSet(
            outType, H2FmtFirstChild(ast, targetTypeNodeId), &targetType.env);
    }
    if (n->kind == H2Ast_CALL) {
        int32_t  calleeNodeId = H2FmtFirstChild(ast, exprNodeId);
        uint32_t calleeNameStart;
        uint32_t calleeNameEnd;
        int32_t  cur;
        if (calleeNodeId < 0 || (uint32_t)calleeNodeId >= ast->len) {
            return 0;
        }
        if (ast->nodes[calleeNodeId].kind == H2Ast_IDENT) {
            calleeNameStart = ast->nodes[calleeNodeId].dataStart;
            calleeNameEnd = ast->nodes[calleeNodeId].dataEnd;
        } else if (ast->nodes[calleeNodeId].kind == H2Ast_FIELD_EXPR) {
            calleeNameStart = ast->nodes[calleeNodeId].dataStart;
            calleeNameEnd = ast->nodes[calleeNodeId].dataEnd;
        } else {
            return 0;
        }
        cur = H2FmtFirstChild(ast, ast->root);
        while (cur >= 0) {
            if (ast->nodes[cur].kind == H2Ast_FN
                && H2FmtSlicesEqual(
                    src,
                    ast->nodes[cur].dataStart,
                    ast->nodes[cur].dataEnd,
                    calleeNameStart,
                    calleeNameEnd)
                && H2FmtInferLocalCallAgainstFn(
                    ast, src, exprNodeId, cur, beforePos, -1, outType, NULL))
            {
                return 1;
            }
            cur = H2FmtNextSibling(ast, cur);
        }
        return 0;
    }
    if (n->kind == H2Ast_COMPOUND_LIT) {
        int32_t typeNodeId = H2FmtFirstChild(ast, exprNodeId);
        if (typeNodeId >= 0 && H2FmtIsTypeNodeKindRaw(ast->nodes[typeNodeId].kind)) {
            return H2FmtInferredTypeSet(outType, typeNodeId, NULL);
        }
    }
    return 0;
}

static int H2FmtCanDropRedundantLiteralCast(
    const H2Ast* ast, H2StrView src, const H2FormatOptions* _Nullable options, int32_t castNodeId) {
    const H2AstNode* castNode;
    int32_t          castExprNodeId = -1;
    int32_t          castTypeNodeId = -1;
    int32_t          parentNodeId;
    if (castNodeId < 0 || (uint32_t)castNodeId >= ast->len) {
        return 0;
    }
    castNode = &ast->nodes[castNodeId];
    if (castNode->kind != H2Ast_CAST) {
        return 0;
    }
    H2FmtGetCastParts(ast, castNodeId, &castExprNodeId, &castTypeNodeId);
    if (H2FmtCastLiteralNumericType(ast, src, castExprNodeId, castTypeNodeId)
        == H2FmtNumericType_INVALID)
    {
        return 0;
    }
    parentNodeId = H2FmtFindParentNode(ast, castNodeId);
    if (parentNodeId < 0) {
        return 0;
    }
    if (ast->nodes[parentNodeId].kind == H2Ast_UNARY && ast->nodes[parentNodeId].op == H2Tok_SUB) {
        int32_t           binaryNodeId = H2FmtFindParentNode(ast, parentNodeId);
        int32_t           lhsNodeId;
        int32_t           rhsNodeId;
        int32_t           otherNodeId = -1;
        H2FmtInferredType otherType;
        if (H2FmtFirstChild(ast, parentNodeId) != castNodeId || binaryNodeId < 0
            || ast->nodes[binaryNodeId].kind != H2Ast_BINARY
            || !H2FmtBinaryOpSharesOperandType(ast->nodes[binaryNodeId].op))
        {
            return 0;
        }
        lhsNodeId = H2FmtFirstChild(ast, binaryNodeId);
        rhsNodeId = lhsNodeId >= 0 ? H2FmtNextSibling(ast, lhsNodeId) : -1;
        if (lhsNodeId == parentNodeId) {
            otherNodeId = rhsNodeId;
        } else if (rhsNodeId == parentNodeId) {
            otherNodeId = lhsNodeId;
        }
        if (otherNodeId < 0) {
            return 0;
        }
        H2FmtInferredTypeInit(&otherType);
        return H2FmtInferExprTypeEx(ast, src, otherNodeId, castNode->start, 0u, &otherType)
            && H2FmtInferredTypeMatchesNode(ast, src, &otherType, castTypeNodeId);
    }
    switch (ast->nodes[parentNodeId].kind) {
        case H2Ast_VAR:
        case H2Ast_CONST: {
            int32_t declTypeNodeId = -1;
            int32_t initNodeId = -1;
            H2FmtGetVarLikeTypeAndInit(ast, parentNodeId, &declTypeNodeId, &initNodeId);
            if (initNodeId == castNodeId
                && H2FmtTypeNodesEqualBySource(ast, src, castTypeNodeId, declTypeNodeId))
            {
                return 1;
            }
            return 0;
        }
        case H2Ast_RETURN: {
            int32_t fnNodeId;
            int32_t retTypeNodeId;
            int32_t retExprNodeId = H2FmtFirstChild(ast, parentNodeId);
            if (retExprNodeId != castNodeId) {
                return 0;
            }
            fnNodeId = H2FmtFindEnclosingFnNode(ast, parentNodeId);
            retTypeNodeId = H2FmtFindFnReturnTypeNode(ast, fnNodeId);
            return H2FmtTypeNodesEqualBySource(ast, src, castTypeNodeId, retTypeNodeId)
                || H2FmtFnReturnTypeIsGenericParam(ast, src, fnNodeId);
        }
        case H2Ast_BINARY: {
            int32_t           lhsNodeId = H2FmtFirstChild(ast, parentNodeId);
            int32_t           rhsNodeId = lhsNodeId >= 0 ? H2FmtNextSibling(ast, lhsNodeId) : -1;
            int32_t           otherNodeId = -1;
            H2FmtInferredType otherType;
            if (rhsNodeId == castNodeId
                && H2FmtIsAssignmentOp((H2TokenKind)ast->nodes[parentNodeId].op))
            {
                H2FmtInferredTypeInit(&otherType);
                return H2FmtInferExprTypeEx(ast, src, lhsNodeId, castNode->start, 0u, &otherType)
                    && H2FmtInferredTypeMatchesNode(ast, src, &otherType, castTypeNodeId);
            }
            if (!H2FmtBinaryOpSharesOperandType(ast->nodes[parentNodeId].op)) {
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
            H2FmtInferredTypeInit(&otherType);
            return H2FmtInferExprTypeEx(ast, src, otherNodeId, castNode->start, 0u, &otherType)
                && H2FmtInferredTypeMatchesNode(ast, src, &otherType, castTypeNodeId);
        }
        default:
            if (H2FmtCanDropLiteralCastFromLocalCall(ast, src, castNodeId, castTypeNodeId)) {
                return 1;
            }
            if (options != NULL && options->canDropLiteralCast != NULL) {
                return options->canDropLiteralCast(options->ctx, ast, src, castNodeId);
            }
            return 0;
    }
}

static int H2FmtKeywordIsVar(const char* kw) {
    return StrEq(kw, "var");
}

static void H2FmtRewriteVarTypeFromLiteralCast(
    const H2Ast* ast,
    H2StrView    src,
    const char*  kw,
    uint32_t     nameCount,
    int32_t*     ioTypeNodeId,
    int32_t*     ioInitNodeId) {
    int32_t initNodeId;
    int32_t castExprNodeId = -1;
    int32_t castTypeNodeId = -1;
    if (ioTypeNodeId == NULL || ioInitNodeId == NULL || !H2FmtKeywordIsVar(kw) || nameCount != 1u) {
        return;
    }
    if (*ioTypeNodeId >= 0 || *ioInitNodeId < 0 || (uint32_t)*ioInitNodeId >= ast->len) {
        return;
    }
    initNodeId = *ioInitNodeId;
    if (ast->nodes[initNodeId].kind != H2Ast_CAST) {
        return;
    }
    H2FmtGetCastParts(ast, initNodeId, &castExprNodeId, &castTypeNodeId);
    if (H2FmtCastLiteralNumericType(ast, src, castExprNodeId, castTypeNodeId)
        == H2FmtNumericType_INVALID)
    {
        return;
    }
    *ioTypeNodeId = castTypeNodeId;
    *ioInitNodeId = castExprNodeId;
}

static void H2FmtRewriteRedundantVarType(
    const H2Ast* ast,
    H2StrView    src,
    const char*  kw,
    uint32_t     nameCount,
    uint32_t     declStart,
    int32_t*     ioTypeNodeId,
    int32_t*     ioInitNodeId) {
    H2FmtInferredType initType;
    int32_t           typeNodeId;
    int32_t           initNodeId;
    if (ioTypeNodeId == NULL || ioInitNodeId == NULL || !H2FmtKeywordIsVar(kw) || nameCount != 1u) {
        return;
    }
    typeNodeId = *ioTypeNodeId;
    initNodeId = *ioInitNodeId;
    if (typeNodeId < 0 || initNodeId < 0 || (uint32_t)typeNodeId >= ast->len
        || (uint32_t)initNodeId >= ast->len)
    {
        return;
    }
    if (ast->nodes[initNodeId].kind == H2Ast_CAST || ast->nodes[initNodeId].kind == H2Ast_CALL) {
        return;
    }
    H2FmtInferredTypeInit(&initType);
    if (!H2FmtInferExprTypeEx(ast, src, initNodeId, declStart, 0u, &initType)) {
        return;
    }
    if (H2FmtInferredTypeMatchesNode(ast, src, &initType, typeNodeId)) {
        *ioTypeNodeId = -1;
    }
}

static int H2FmtHasEarlierLocalBindingNamed(
    const H2Ast* ast, H2StrView src, int32_t nodeId, uint32_t nameStart, uint32_t nameEnd) {
    int32_t fnNodeId;
    int32_t child;
    if (nameEnd <= nameStart) {
        return 0;
    }
    fnNodeId = H2FmtFindEnclosingFnNode(ast, nodeId);
    if (fnNodeId < 0) {
        return 0;
    }
    child = H2FmtFirstChild(ast, fnNodeId);
    while (child >= 0 && ast->nodes[child].kind == H2Ast_TYPE_PARAM) {
        child = H2FmtNextSibling(ast, child);
    }
    while (child >= 0 && ast->nodes[child].kind == H2Ast_PARAM) {
        if (ast->nodes[child].start < ast->nodes[nodeId].start
            && H2FmtNodeDeclaresNameRange(ast, src, child, nameStart, nameEnd))
        {
            return 1;
        }
        child = H2FmtNextSibling(ast, child);
    }
    for (uint32_t i = 0; i < ast->len; i++) {
        const H2AstNode* n = &ast->nodes[i];
        if (n->kind == H2Ast_SHORT_ASSIGN && n->start >= ast->nodes[fnNodeId].start
            && n->end <= ast->nodes[fnNodeId].end && n->end <= ast->nodes[nodeId].start)
        {
            int32_t nameList = H2FmtFirstChild(ast, (int32_t)i);
            int32_t nameNode;
            if (nameList < 0 || ast->nodes[nameList].kind != H2Ast_NAME_LIST) {
                continue;
            }
            nameNode = H2FmtFirstChild(ast, nameList);
            while (nameNode >= 0) {
                const H2AstNode* name = &ast->nodes[nameNode];
                if (name->kind == H2Ast_IDENT
                    && H2FmtSlicesEqual(src, name->dataStart, name->dataEnd, nameStart, nameEnd))
                {
                    return 1;
                }
                nameNode = H2FmtNextSibling(ast, nameNode);
            }
            continue;
        }
        if ((n->kind == H2Ast_VAR || n->kind == H2Ast_CONST)
            && n->start >= ast->nodes[fnNodeId].start && n->end <= ast->nodes[fnNodeId].end
            && n->end <= ast->nodes[nodeId].start
            && H2FmtNodeDeclaresNameRange(ast, src, (int32_t)i, nameStart, nameEnd))
        {
            return 1;
        }
    }
    return 0;
}

static int H2FmtCanEmitShortVarDecl(
    const H2Ast* ast,
    H2StrView    src,
    const char*  kw,
    uint32_t     nameCount,
    int32_t      nodeId,
    int32_t      typeNodeId,
    int32_t      initNodeId) {
    int32_t          parentNodeId;
    const H2AstNode* n;
    const H2AstNode* init;
    if (!H2FmtKeywordIsVar(kw) || nameCount != 1u || typeNodeId >= 0 || initNodeId < 0) {
        return 0;
    }
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len || (uint32_t)initNodeId >= ast->len) {
        return 0;
    }
    n = &ast->nodes[nodeId];
    init = &ast->nodes[initNodeId];
    switch (init->kind) {
        case H2Ast_INT:
        case H2Ast_FLOAT:
        case H2Ast_STRING:
        case H2Ast_RUNE:
        case H2Ast_BOOL:
        case H2Ast_IDENT:
        case H2Ast_COMPOUND_LIT: break;
        default:                 return 0;
    }
    parentNodeId = H2FmtFindParentNode(ast, nodeId);
    return parentNodeId >= 0 && ast->nodes[parentNodeId].kind == H2Ast_BLOCK
        && !H2FmtHasEarlierLocalBindingNamed(ast, src, nodeId, n->dataStart, n->dataEnd);
}

static int32_t H2FmtFindEnclosingFnNode(const H2Ast* ast, int32_t nodeId) {
    const H2AstNode* target;
    int32_t          best = -1;
    uint32_t         bestSpan = UINT32_MAX;
    uint32_t         i;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    target = &ast->nodes[nodeId];
    for (i = 0; i < ast->len; i++) {
        const H2AstNode* n = &ast->nodes[i];
        if (n->kind != H2Ast_FN) {
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

static int H2FmtFnHasImplicitContextLocal(const H2Ast* ast, H2StrView src, int32_t fnNodeId) {
    const H2AstNode* fn;
    int32_t          child;
    if (fnNodeId < 0 || (uint32_t)fnNodeId >= ast->len) {
        return 0;
    }
    fn = &ast->nodes[fnNodeId];
    if (fn->kind != H2Ast_FN) {
        return 0;
    }
    if (H2FmtSliceEqLiteral(src, fn->dataStart, fn->dataEnd, "main")) {
        return 1;
    }
    child = H2FmtFirstChild(ast, fnNodeId);
    while (child >= 0) {
        if (ast->nodes[child].kind == H2Ast_CONTEXT_CLAUSE) {
            return 1;
        }
        child = H2FmtNextSibling(ast, child);
    }
    return 0;
}

static int H2FmtFnHasShadowingContextLocalBefore(
    const H2Ast* ast, H2StrView src, int32_t fnNodeId, uint32_t beforePos) {
    const H2AstNode* fn;
    uint32_t         i;
    if (fnNodeId < 0 || (uint32_t)fnNodeId >= ast->len) {
        return 0;
    }
    fn = &ast->nodes[fnNodeId];
    if (fn->kind != H2Ast_FN) {
        return 0;
    }
    for (i = 0; i < ast->len; i++) {
        const H2AstNode* n = &ast->nodes[i];
        if (n->start < fn->start || n->end > fn->end || n->start >= beforePos) {
            continue;
        }
        if (H2FmtNodeDeclaresNameLiteral(ast, src, (int32_t)i, "context")) {
            return 1;
        }
    }
    return 0;
}

static int H2FmtExprIsImplicitContextFieldForBind(
    const H2Ast* ast, H2StrView src, const H2AstNode* bindNode, int32_t exprNodeId) {
    const H2AstNode* exprNode;
    int32_t          baseNodeId;
    uint32_t         baseStart;
    uint32_t         baseEnd;
    if (exprNodeId < 0 || (uint32_t)exprNodeId >= ast->len) {
        return 0;
    }
    exprNode = &ast->nodes[exprNodeId];
    if (exprNode->kind != H2Ast_FIELD_EXPR) {
        return 0;
    }
    if (!H2FmtSlicesEqual(
            src, bindNode->dataStart, bindNode->dataEnd, exprNode->dataStart, exprNode->dataEnd))
    {
        return 0;
    }
    baseNodeId = H2FmtFirstChild(ast, exprNodeId);
    if (!H2FmtExprIsPlainIdent(ast, baseNodeId, &baseStart, &baseEnd)) {
        return 0;
    }
    return H2FmtSliceEqLiteral(src, baseStart, baseEnd, "context");
}

typedef int (*H2FmtExprRewriteRule)(const H2Ast* ast, H2StrView src, int32_t* exprNodeId);

static int H2FmtRewriteExprIdentity(const H2Ast* ast, H2StrView src, int32_t* exprNodeId) {
    (void)ast;
    (void)src;
    (void)exprNodeId;
    return 0;
}

static int H2FmtRewriteExpr(const H2Ast* ast, H2StrView src, int32_t* exprNodeId) {
    static const H2FmtExprRewriteRule rules[] = {
        H2FmtRewriteExprIdentity,
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

static int H2FmtRewriteCallArgShorthand(const H2Ast* ast, H2StrView src, int32_t nodeId) {
    const H2AstNode* node;
    H2AstNode*       mutNode;
    int32_t          exprNode;
    uint32_t         identStart;
    uint32_t         identEnd;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    node = &ast->nodes[nodeId];
    if (node->kind != H2Ast_CALL_ARG || node->dataEnd <= node->dataStart) {
        return 0;
    }
    exprNode = H2FmtFirstChild(ast, nodeId);
    if (exprNode < 0) {
        return 0;
    }
    if (H2FmtRewriteExpr(ast, src, &exprNode) != 0) {
        return -1;
    }
    if (!H2FmtExprIsPlainIdent(ast, exprNode, &identStart, &identEnd)) {
        return 0;
    }
    if (!H2FmtSlicesEqual(src, node->dataStart, node->dataEnd, identStart, identEnd)) {
        return 0;
    }
    mutNode = (H2AstNode*)&ast->nodes[nodeId];
    mutNode->dataStart = 0;
    mutNode->dataEnd = 0;
    return 0;
}

static int H2FmtRewriteCompoundFieldShorthand(const H2Ast* ast, H2StrView src, int32_t nodeId) {
    const H2AstNode* node;
    H2AstNode*       mutNode;
    int32_t          exprNode;
    uint32_t         identStart;
    uint32_t         identEnd;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    node = &ast->nodes[nodeId];
    if (node->kind != H2Ast_COMPOUND_FIELD
        || (node->flags & H2AstFlag_COMPOUND_FIELD_SHORTHAND) != 0
        || node->dataEnd <= node->dataStart || node->dataEnd > src.len)
    {
        return 0;
    }
    if (H2FmtSliceHasChar(src, node->dataStart, node->dataEnd, '.')) {
        return 0;
    }
    exprNode = H2FmtFirstChild(ast, nodeId);
    if (exprNode < 0) {
        return 0;
    }
    if (H2FmtRewriteExpr(ast, src, &exprNode) != 0) {
        return -1;
    }
    if (!H2FmtExprIsPlainIdent(ast, exprNode, &identStart, &identEnd)) {
        return 0;
    }
    if (!H2FmtSlicesEqual(src, node->dataStart, node->dataEnd, identStart, identEnd)) {
        return 0;
    }
    mutNode = (H2AstNode*)&ast->nodes[nodeId];
    mutNode->flags |= H2AstFlag_COMPOUND_FIELD_SHORTHAND;
    return 0;
}

static int H2FmtRewriteContextBindShorthand(const H2Ast* ast, H2StrView src, int32_t nodeId) {
    const H2AstNode* node;
    H2AstNode*       mutNode;
    int32_t          exprNode;
    int32_t          enclosingFn;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    node = &ast->nodes[nodeId];
    if (node->kind != H2Ast_CONTEXT_BIND || node->dataEnd <= node->dataStart
        || (node->flags & H2AstFlag_CONTEXT_BIND_SHORTHAND) != 0)
    {
        return 0;
    }
    exprNode = H2FmtFirstChild(ast, nodeId);
    if (exprNode < 0) {
        return 0;
    }
    if (H2FmtRewriteExpr(ast, src, &exprNode) != 0) {
        return -1;
    }
    if (!H2FmtExprIsImplicitContextFieldForBind(ast, src, node, exprNode)) {
        return 0;
    }
    enclosingFn = H2FmtFindEnclosingFnNode(ast, nodeId);
    if (enclosingFn < 0 || !H2FmtFnHasImplicitContextLocal(ast, src, enclosingFn)
        || H2FmtFnHasShadowingContextLocalBefore(ast, src, enclosingFn, node->start))
    {
        return 0;
    }
    mutNode = (H2AstNode*)&ast->nodes[nodeId];
    mutNode->flags |= H2AstFlag_CONTEXT_BIND_SHORTHAND;
    return 0;
}

static int H2FmtRewriteDropRedundantCastParenFlag(const H2Ast* ast, int32_t nodeId) {
    const H2AstNode* n;
    H2AstNode*       mutNode;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    n = &ast->nodes[nodeId];
    if (n->kind != H2Ast_CAST || (n->flags & H2AstFlag_PAREN) == 0) {
        return 0;
    }
    mutNode = (H2AstNode*)&ast->nodes[nodeId];
    mutNode->flags &= ~H2AstFlag_PAREN;
    return 0;
}

static int H2FmtRewriteBinaryCastParens(const H2Ast* ast, int32_t nodeId) {
    int32_t lhs;
    int32_t rhs;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    if (ast->nodes[nodeId].kind != H2Ast_BINARY) {
        return 0;
    }
    lhs = H2FmtFirstChild(ast, nodeId);
    rhs = lhs >= 0 ? H2FmtNextSibling(ast, lhs) : -1;
    /* Cast is postfix and binds tighter than all infix binary operators. */
    if (H2FmtRewriteDropRedundantCastParenFlag(ast, lhs) != 0
        || H2FmtRewriteDropRedundantCastParenFlag(ast, rhs) != 0)
    {
        return -1;
    }
    return 0;
}

static int H2FmtRewriteRedundantLiteralCast(
    const H2Ast* ast, H2StrView src, const H2FormatOptions* _Nullable options, int32_t nodeId) {
    const H2AstNode* node;
    H2AstNode*       mutNode;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    node = &ast->nodes[nodeId];
    if (node->kind != H2Ast_CAST) {
        return 0;
    }
    if (!H2FmtCanDropRedundantLiteralCast(ast, src, options, nodeId)) {
        return 0;
    }
    mutNode = (H2AstNode*)&ast->nodes[nodeId];
    mutNode->flags |= H2FmtFlag_DROP_REDUNDANT_LITERAL_CAST;
    return 0;
}

static int H2FmtRewriteReturnParens(const H2Ast* ast, int32_t nodeId) {
    int32_t          exprNodeId;
    const H2AstNode* exprNode;
    H2AstNode*       mutExprNode;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    if (ast->nodes[nodeId].kind != H2Ast_RETURN) {
        return 0;
    }
    exprNodeId = H2FmtFirstChild(ast, nodeId);
    if (exprNodeId < 0 || (uint32_t)exprNodeId >= ast->len) {
        return 0;
    }
    exprNode = &ast->nodes[exprNodeId];
    mutExprNode = (H2AstNode*)exprNode;
    if ((exprNode->flags & H2AstFlag_PAREN) != 0) {
        mutExprNode->flags &= ~H2AstFlag_PAREN;
        exprNode = mutExprNode;
    }
    if (exprNode->kind == H2Ast_TUPLE_EXPR && (exprNode->flags & H2AstFlag_PAREN) == 0) {
        mutExprNode->kind = H2Ast_EXPR_LIST;
    }
    return 0;
}

static int H2FmtRewriteAst(
    const H2Ast* ast, H2StrView src, const H2FormatOptions* _Nullable options) {
    uint32_t i;
    if (ast == NULL || ast->nodes == NULL) {
        return -1;
    }
    for (i = 0; i < ast->len; i++) {
        int32_t nodeId = (int32_t)i;
        if (H2FmtRewriteCallArgShorthand(ast, src, nodeId) != 0
            || H2FmtRewriteCompoundFieldShorthand(ast, src, nodeId) != 0
            || H2FmtRewriteContextBindShorthand(ast, src, nodeId) != 0
            || H2FmtRewriteRedundantLiteralCast(ast, src, options, nodeId) != 0
            || H2FmtRewriteBinaryCastParens(ast, nodeId) != 0
            || H2FmtRewriteReturnParens(ast, nodeId) != 0)
        {
            return -1;
        }
    }
    return 0;
}

static int H2FmtIsTypeNodeKind(H2AstKind kind) {
    return H2FmtIsTypeNodeKindRaw(kind);
}

static int H2FmtIsStmtNodeKind(H2AstKind kind) {
    switch (kind) {
        case H2Ast_BLOCK:
        case H2Ast_VAR:
        case H2Ast_CONST:
        case H2Ast_CONST_BLOCK:
        case H2Ast_IF:
        case H2Ast_FOR:
        case H2Ast_SWITCH:
        case H2Ast_RETURN:
        case H2Ast_BREAK:
        case H2Ast_CONTINUE:
        case H2Ast_DEFER:
        case H2Ast_ASSERT:
        case H2Ast_DEL:
        case H2Ast_MULTI_ASSIGN:
        case H2Ast_SHORT_ASSIGN:
        case H2Ast_EXPR_STMT:    return 1;
        default:                 return 0;
    }
}

static int H2FmtIsGroupedVarLike(const H2FmtCtx* c, int32_t nodeId) {
    int32_t firstChild;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return 0;
    }
    firstChild = H2FmtFirstChild(c->ast, nodeId);
    return firstChild >= 0 && c->ast->nodes[firstChild].kind == H2Ast_NAME_LIST;
}

static int H2FmtIsAssignmentOp(H2TokenKind kind) {
    switch (kind) {
        case H2Tok_ASSIGN:
        case H2Tok_ADD_ASSIGN:
        case H2Tok_SUB_ASSIGN:
        case H2Tok_MUL_ASSIGN:
        case H2Tok_DIV_ASSIGN:
        case H2Tok_MOD_ASSIGN:
        case H2Tok_AND_ASSIGN:
        case H2Tok_OR_ASSIGN:
        case H2Tok_XOR_ASSIGN:
        case H2Tok_LSHIFT_ASSIGN:
        case H2Tok_RSHIFT_ASSIGN: return 1;
        default:                  return 0;
    }
}

static int H2FmtBinPrec(H2TokenKind kind) {
    if (H2FmtIsAssignmentOp(kind)) {
        return 1;
    }
    switch (kind) {
        case H2Tok_LOGICAL_OR:  return 2;
        case H2Tok_LOGICAL_AND: return 3;
        case H2Tok_EQ:
        case H2Tok_NEQ:
        case H2Tok_LT:
        case H2Tok_GT:
        case H2Tok_LTE:
        case H2Tok_GTE:         return 4;
        case H2Tok_OR:
        case H2Tok_XOR:
        case H2Tok_ADD:
        case H2Tok_SUB:         return 5;
        case H2Tok_AND:
        case H2Tok_LSHIFT:
        case H2Tok_RSHIFT:
        case H2Tok_MUL:
        case H2Tok_DIV:
        case H2Tok_MOD:         return 6;
        default:                return 0;
    }
}

static const char* H2FmtTokenOpText(H2TokenKind kind) {
    switch (kind) {
        case H2Tok_ASSIGN:        return "=";
        case H2Tok_ADD:           return "+";
        case H2Tok_SUB:           return "-";
        case H2Tok_MUL:           return "*";
        case H2Tok_DIV:           return "/";
        case H2Tok_MOD:           return "%";
        case H2Tok_AND:           return "&";
        case H2Tok_OR:            return "|";
        case H2Tok_XOR:           return "^";
        case H2Tok_NOT:           return "!";
        case H2Tok_LSHIFT:        return "<<";
        case H2Tok_RSHIFT:        return ">>";
        case H2Tok_EQ:            return "==";
        case H2Tok_NEQ:           return "!=";
        case H2Tok_LT:            return "<";
        case H2Tok_GT:            return ">";
        case H2Tok_LTE:           return "<=";
        case H2Tok_GTE:           return ">=";
        case H2Tok_LOGICAL_AND:   return "&&";
        case H2Tok_LOGICAL_OR:    return "||";
        case H2Tok_SHORT_ASSIGN:  return ":=";
        case H2Tok_ADD_ASSIGN:    return "+=";
        case H2Tok_SUB_ASSIGN:    return "-=";
        case H2Tok_MUL_ASSIGN:    return "*=";
        case H2Tok_DIV_ASSIGN:    return "/=";
        case H2Tok_MOD_ASSIGN:    return "%=";
        case H2Tok_AND_ASSIGN:    return "&=";
        case H2Tok_OR_ASSIGN:     return "|=";
        case H2Tok_XOR_ASSIGN:    return "^=";
        case H2Tok_LSHIFT_ASSIGN: return "<<=";
        case H2Tok_RSHIFT_ASSIGN: return ">>=";
        case H2Tok_AS:            return "as";
        default:                  return "?";
    }
}

static int H2FmtContainsSemicolonInRange(H2StrView src, uint32_t start, uint32_t end) {
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

static int H2FmtRangeHasChar(H2StrView src, uint32_t start, uint32_t end, char ch) {
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

static int H2FmtFindCharForwardInRange(
    H2StrView src, uint32_t start, uint32_t end, char ch, uint32_t* outPos) {
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

static int H2FmtFindCharBackwardInRange(
    H2StrView src, uint32_t start, uint32_t end, char ch, uint32_t* outPos) {
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

static uint32_t H2FmtTrimSliceEnd(const char* s, uint32_t start, uint32_t end) {
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

static int H2FmtEmitCommentText(H2FmtCtx* c, const H2Comment* cm) {
    uint32_t end = H2FmtTrimSliceEnd(c->src.ptr, cm->start, cm->end);
    if (end < cm->start) {
        end = cm->start;
    }
    return H2FmtWriteSliceLiteral(c, cm->start, end);
}

static int H2FmtIsLeadingCommentForNode(const H2FmtCtx* c, const H2Comment* cm, int32_t nodeId) {
    const H2AstNode* node;
    const H2AstNode* anchor;
    if (c == NULL || cm == NULL || nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return 0;
    }
    if (cm->attachment != H2CommentAttachment_LEADING
        && cm->attachment != H2CommentAttachment_FLOATING)
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

static int H2FmtEmitLeadingCommentsForNode(H2FmtCtx* c, int32_t nodeId) {
    uint32_t i;
    for (i = 0; i < c->commentLen; i++) {
        const H2Comment* cm = &c->comments[i];
        if (c->commentUsed[i]) {
            continue;
        }
        if (!H2FmtIsLeadingCommentForNode(c, cm, nodeId)) {
            continue;
        }
        if (!c->lineStart) {
            if (H2FmtNewline(c) != 0) {
                return -1;
            }
        }
        if (H2FmtEmitCommentText(c, cm) != 0 || H2FmtNewline(c) != 0) {
            return -1;
        }
        c->commentUsed[i] = 1;
    }
    return 0;
}

static int H2FmtEmitTrailingCommentsForNode(H2FmtCtx* c, int32_t nodeId) {
    uint32_t i;
    int      first = 1;
    for (i = 0; i < c->commentLen; i++) {
        const H2Comment* cm = &c->comments[i];
        if (c->commentUsed[i]) {
            continue;
        }
        if (cm->anchorNode != nodeId || cm->attachment != H2CommentAttachment_TRAILING) {
            continue;
        }
        if (first) {
            if (H2FmtWriteChar(c, ' ') != 0 || H2FmtEmitCommentText(c, cm) != 0) {
                return -1;
            }
            first = 0;
        } else {
            if (H2FmtNewline(c) != 0 || H2FmtEmitCommentText(c, cm) != 0) {
                return -1;
            }
        }
        c->commentUsed[i] = 1;
    }
    return 0;
}

static int H2FmtEmitRemainingComments(H2FmtCtx* c) {
    uint32_t i;
    for (i = 0; i < c->commentLen; i++) {
        if (c->commentUsed[i]) {
            continue;
        }
        if (!c->lineStart && H2FmtNewline(c) != 0) {
            return -1;
        }
        if (H2FmtEmitCommentText(c, &c->comments[i]) != 0 || H2FmtNewline(c) != 0) {
            return -1;
        }
        c->commentUsed[i] = 1;
    }
    return 0;
}

static uint32_t H2FmtCountNewlinesInRange(H2StrView src, uint32_t start, uint32_t end) {
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

static int H2FmtGapHasIntentionalBlankLine(H2StrView src, uint32_t start, uint32_t end) {
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

static int H2FmtNodeContainsAnchor(const H2Ast* ast, int32_t nodeId, int32_t anchorNodeId) {
    const H2AstNode* n;
    const H2AstNode* a;
    if (nodeId < 0 || anchorNodeId < 0 || (uint32_t)nodeId >= ast->len
        || (uint32_t)anchorNodeId >= ast->len)
    {
        return 0;
    }
    n = &ast->nodes[nodeId];
    a = &ast->nodes[anchorNodeId];
    return a->start >= n->start && a->end <= n->end;
}

static int H2FmtCommentAnchoredToAnyNode(
    const H2Ast* ast, const H2Comment* cm, const int32_t* nodeIds, uint32_t nodeLen) {
    uint32_t i;
    for (i = 0; i < nodeLen; i++) {
        if (H2FmtNodeContainsAnchor(ast, nodeIds[i], cm->anchorNode)) {
            return 1;
        }
    }
    return 0;
}

static int H2FmtHasUnusedLeadingCommentsForNode(const H2FmtCtx* c, int32_t nodeId) {
    uint32_t i;
    for (i = 0; i < c->commentLen; i++) {
        const H2Comment* cm = &c->comments[i];
        if (c->commentUsed[i]) {
            continue;
        }
        if (H2FmtIsLeadingCommentForNode(c, cm, nodeId)) {
            return 1;
        }
    }
    return 0;
}

static int H2FmtHasUnusedTrailingCommentsForNodes(
    const H2FmtCtx* c, const int32_t* nodeIds, uint32_t nodeLen) {
    uint32_t i;
    for (i = 0; i < c->commentLen; i++) {
        const H2Comment* cm = &c->comments[i];
        if (c->commentUsed[i] || cm->attachment != H2CommentAttachment_TRAILING) {
            continue;
        }
        if (H2FmtCommentAnchoredToAnyNode(c->ast, cm, nodeIds, nodeLen)) {
            return 1;
        }
    }
    return 0;
}

static int H2FmtCommentWithinNodeRange(const H2Ast* ast, const H2Comment* cm, int32_t nodeId) {
    const H2AstNode* n;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return 0;
    }
    n = &ast->nodes[nodeId];
    return cm->start >= n->start && cm->start < n->end;
}

static int H2FmtHasUnusedTrailingCommentsInNodeRange(const H2FmtCtx* c, int32_t nodeId) {
    uint32_t i;
    for (i = 0; i < c->commentLen; i++) {
        const H2Comment* cm = &c->comments[i];
        if (c->commentUsed[i] || cm->attachment != H2CommentAttachment_TRAILING) {
            continue;
        }
        if (H2FmtCommentWithinNodeRange(c->ast, cm, nodeId)) {
            return 1;
        }
    }
    return 0;
}

static int H2FmtEmitTrailingCommentsInNodeRange(H2FmtCtx* c, int32_t nodeId, uint32_t padBefore) {
    uint32_t i;
    int      first = 1;
    for (i = 0; i < c->commentLen; i++) {
        const H2Comment* cm = &c->comments[i];
        if (c->commentUsed[i] || cm->attachment != H2CommentAttachment_TRAILING) {
            continue;
        }
        if (!H2FmtCommentWithinNodeRange(c->ast, cm, nodeId)) {
            continue;
        }
        if (first) {
            uint32_t pad = padBefore > 0 ? padBefore : 1u;
            if (H2FmtWriteSpaces(c, pad) != 0 || H2FmtEmitCommentText(c, cm) != 0) {
                return -1;
            }
            first = 0;
        } else {
            if (H2FmtNewline(c) != 0 || H2FmtEmitCommentText(c, cm) != 0) {
                return -1;
            }
        }
        c->commentUsed[i] = 1;
    }
    return 0;
}

static int H2FmtEmitTrailingCommentsForNodes(
    H2FmtCtx* c, const int32_t* nodeIds, uint32_t nodeLen, uint32_t padBefore) {
    uint32_t i;
    int      first = 1;
    for (i = 0; i < c->commentLen; i++) {
        const H2Comment* cm = &c->comments[i];
        if (c->commentUsed[i] || cm->attachment != H2CommentAttachment_TRAILING) {
            continue;
        }
        if (!H2FmtCommentAnchoredToAnyNode(c->ast, cm, nodeIds, nodeLen)) {
            continue;
        }
        if (first) {
            uint32_t pad = padBefore > 0 ? padBefore : 1u;
            if (H2FmtWriteSpaces(c, pad) != 0 || H2FmtEmitCommentText(c, cm) != 0) {
                return -1;
            }
            first = 0;
        } else {
            if (H2FmtNewline(c) != 0 || H2FmtEmitCommentText(c, cm) != 0) {
                return -1;
            }
        }
        c->commentUsed[i] = 1;
    }
    return 0;
}

static int H2FmtFindSourceTrailingLineComment(
    const H2FmtCtx* c, int32_t nodeId, uint32_t* outStart, uint32_t* outEnd) {
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
            *outEnd = H2FmtTrimSliceEnd(c->src.ptr, i, lineEnd);
            return 1;
        }
        if (c->src.ptr[i] != ' ' && c->src.ptr[i] != '\t' && c->src.ptr[i] != '\r') {
            return 0;
        }
        i++;
    }
    return 0;
}

static void H2FmtMarkCommentUsedAtStart(H2FmtCtx* c, uint32_t start) {
    uint32_t i;
    for (i = 0; i < c->commentLen; i++) {
        if (c->comments[i].start == start) {
            c->commentUsed[i] = 1;
            return;
        }
    }
}

static int H2FmtEmitType(H2FmtCtx* c, int32_t nodeId);
static int H2FmtEmitExpr(H2FmtCtx* c, int32_t nodeId, int forceParen);
static int H2FmtEmitBlock(H2FmtCtx* c, int32_t nodeId);
static int H2FmtEmitStmtInline(H2FmtCtx* c, int32_t nodeId);
static int H2FmtEmitDecl(H2FmtCtx* c, int32_t nodeId);
static int H2FmtEmitDirectiveGroup(
    H2FmtCtx* c, int32_t firstDirective, int32_t* outLast, int32_t* outNext);
static int H2FmtEmitDirective(H2FmtCtx* c, int32_t nodeId);
static int H2FmtEmitAggregateFieldBody(H2FmtCtx* c, int32_t firstFieldNodeId);
static int H2FmtEmitExprList(H2FmtCtx* c, int32_t listNodeId);

static int H2FmtEmitCompoundFieldWithAlign(H2FmtCtx* c, int32_t nodeId, uint32_t maxKeyLen) {
    const H2AstNode* n;
    int32_t          exprNode;
    uint32_t         keyLen;
    uint32_t         pad;

    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return -1;
    }
    n = &c->ast->nodes[nodeId];
    if (n->kind != H2Ast_COMPOUND_FIELD) {
        return -1;
    }
    exprNode = H2FmtFirstChild(c->ast, nodeId);
    if ((n->flags & H2AstFlag_COMPOUND_FIELD_SHORTHAND) != 0) {
        return H2FmtWriteSlice(c, n->dataStart, n->dataEnd);
    }
    keyLen = n->dataEnd - n->dataStart;
    if (H2FmtWriteSlice(c, n->dataStart, n->dataEnd) != 0 || H2FmtWriteChar(c, ':') != 0) {
        return -1;
    }
    if (maxKeyLen > keyLen) {
        pad = (maxKeyLen - keyLen) + 1u;
        if (H2FmtWriteSpaces(c, pad) != 0) {
            return -1;
        }
    } else if (H2FmtWriteChar(c, ' ') != 0) {
        return -1;
    }
    return exprNode >= 0 ? H2FmtEmitExpr(c, exprNode, 0) : 0;
}

static int H2FmtEmitTypeParamList(H2FmtCtx* c, int32_t* ioChild) {
    int32_t cur;
    int     first = 1;
    if (ioChild == NULL) {
        return -1;
    }
    cur = *ioChild;
    if (cur < 0 || c->ast->nodes[cur].kind != H2Ast_TYPE_PARAM) {
        return 0;
    }
    if (H2FmtWriteChar(c, '[') != 0) {
        return -1;
    }
    while (cur >= 0 && c->ast->nodes[cur].kind == H2Ast_TYPE_PARAM) {
        if (!first && H2FmtWriteCStr(c, ", ") != 0) {
            return -1;
        }
        if (H2FmtWriteSlice(c, c->ast->nodes[cur].dataStart, c->ast->nodes[cur].dataEnd) != 0) {
            return -1;
        }
        first = 0;
        cur = H2FmtNextSibling(c->ast, cur);
    }
    if (H2FmtWriteChar(c, ']') != 0) {
        return -1;
    }
    *ioChild = cur;
    return 0;
}

static int H2FmtEmitType(H2FmtCtx* c, int32_t nodeId) {
    const H2AstNode* n;
    int32_t          ch;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return -1;
    }
    n = &c->ast->nodes[nodeId];
    switch (n->kind) {
        case H2Ast_TYPE_NAME:
            if (H2FmtWriteSlice(c, n->dataStart, n->dataEnd) != 0) {
                return -1;
            }
            ch = H2FmtFirstChild(c->ast, nodeId);
            if (ch >= 0) {
                int first = 1;
                if (H2FmtWriteChar(c, '[') != 0) {
                    return -1;
                }
                while (ch >= 0) {
                    if (!first && H2FmtWriteCStr(c, ", ") != 0) {
                        return -1;
                    }
                    if (H2FmtEmitType(c, ch) != 0) {
                        return -1;
                    }
                    first = 0;
                    ch = H2FmtNextSibling(c->ast, ch);
                }
                if (H2FmtWriteChar(c, ']') != 0) {
                    return -1;
                }
            }
            return 0;
        case H2Ast_TYPE_PARAM: return H2FmtWriteSlice(c, n->dataStart, n->dataEnd);
        case H2Ast_TYPE_OPTIONAL:
            ch = H2FmtFirstChild(c->ast, nodeId);
            if (H2FmtWriteChar(c, '?') != 0) {
                return -1;
            }
            return ch >= 0 ? H2FmtEmitType(c, ch) : 0;
        case H2Ast_TYPE_PTR:
            ch = H2FmtFirstChild(c->ast, nodeId);
            if (H2FmtWriteChar(c, '*') != 0) {
                return -1;
            }
            return ch >= 0 ? H2FmtEmitType(c, ch) : 0;
        case H2Ast_TYPE_REF:
            ch = H2FmtFirstChild(c->ast, nodeId);
            if (H2FmtWriteChar(c, '&') != 0) {
                return -1;
            }
            return ch >= 0 ? H2FmtEmitType(c, ch) : 0;
        case H2Ast_TYPE_MUTREF:
            ch = H2FmtFirstChild(c->ast, nodeId);
            if (H2FmtWriteCStr(c, "&mut ") != 0) {
                return -1;
            }
            return ch >= 0 ? H2FmtEmitType(c, ch) : 0;
        case H2Ast_TYPE_SLICE:
            ch = H2FmtFirstChild(c->ast, nodeId);
            if (H2FmtWriteChar(c, '[') != 0 || (ch >= 0 && H2FmtEmitType(c, ch) != 0)
                || H2FmtWriteChar(c, ']') != 0)
            {
                return -1;
            }
            return 0;
        case H2Ast_TYPE_MUTSLICE:
            ch = H2FmtFirstChild(c->ast, nodeId);
            if (H2FmtWriteCStr(c, "[mut ") != 0 || (ch >= 0 && H2FmtEmitType(c, ch) != 0)
                || H2FmtWriteChar(c, ']') != 0)
            {
                return -1;
            }
            return 0;
        case H2Ast_TYPE_ARRAY:
        case H2Ast_TYPE_VARRAY:
            ch = H2FmtFirstChild(c->ast, nodeId);
            if (H2FmtWriteChar(c, '[') != 0 || (ch >= 0 && H2FmtEmitType(c, ch) != 0)
                || H2FmtWriteChar(c, ' ') != 0)
            {
                return -1;
            }
            if (n->kind == H2Ast_TYPE_VARRAY && H2FmtWriteChar(c, '.') != 0) {
                return -1;
            }
            if (H2FmtWriteSlice(c, n->dataStart, n->dataEnd) != 0 || H2FmtWriteChar(c, ']') != 0) {
                return -1;
            }
            return 0;
        case H2Ast_TYPE_FN: {
            int32_t retType = -1;
            int32_t cur = H2FmtFirstChild(c->ast, nodeId);
            int     first = 1;
            if (H2FmtWriteCStr(c, "fn(") != 0) {
                return -1;
            }
            while (cur >= 0) {
                const H2AstNode* chn = &c->ast->nodes[cur];
                if (chn->flags == 1 && H2FmtIsTypeNodeKind(chn->kind)) {
                    retType = cur;
                    break;
                }
                if (!first && H2FmtWriteCStr(c, ", ") != 0) {
                    return -1;
                }
                if ((chn->flags & H2AstFlag_PARAM_CONST) != 0 && H2FmtWriteCStr(c, "const ") != 0) {
                    return -1;
                }
                if ((chn->flags & H2AstFlag_PARAM_VARIADIC) != 0 && H2FmtWriteCStr(c, "...") != 0) {
                    return -1;
                }
                if (H2FmtEmitType(c, cur) != 0) {
                    return -1;
                }
                first = 0;
                cur = H2FmtNextSibling(c->ast, cur);
            }
            if (H2FmtWriteChar(c, ')') != 0) {
                return -1;
            }
            if (retType >= 0) {
                if (H2FmtWriteChar(c, ' ') != 0 || H2FmtEmitType(c, retType) != 0) {
                    return -1;
                }
            }
            return 0;
        }
        case H2Ast_TYPE_ANON_STRUCT:
        case H2Ast_TYPE_ANON_UNION:  {
            int32_t field = H2FmtFirstChild(c->ast, nodeId);
            if (H2FmtCountNewlinesInRange(c->src, n->start, n->end) == 0u) {
                return H2FmtWriteSlice(c, n->start, n->end);
            }
            if (n->kind == H2Ast_TYPE_ANON_UNION) {
                if (H2FmtWriteCStr(c, "union ") != 0) {
                    return -1;
                }
            } else if (H2FmtWriteCStr(c, "struct ") != 0) {
                return -1;
            }
            if (H2FmtWriteChar(c, '{') != 0) {
                return -1;
            }
            if (field >= 0 && H2FmtEmitAggregateFieldBody(c, field) != 0) {
                return -1;
            }
            return H2FmtWriteChar(c, '}');
        }
        case H2Ast_TYPE_TUPLE: {
            int32_t cur = H2FmtFirstChild(c->ast, nodeId);
            int     first = 1;
            if (H2FmtWriteChar(c, '(') != 0) {
                return -1;
            }
            while (cur >= 0) {
                if (!first && H2FmtWriteCStr(c, ", ") != 0) {
                    return -1;
                }
                if (H2FmtEmitType(c, cur) != 0) {
                    return -1;
                }
                first = 0;
                cur = H2FmtNextSibling(c->ast, cur);
            }
            return H2FmtWriteChar(c, ')');
        }
        default: return H2FmtWriteSlice(c, n->start, n->end);
    }
}

static int H2FmtEmitExprList(H2FmtCtx* c, int32_t listNodeId) {
    uint32_t i;
    uint32_t exprCount;
    if (listNodeId < 0 || (uint32_t)listNodeId >= c->ast->len
        || c->ast->nodes[listNodeId].kind != H2Ast_EXPR_LIST)
    {
        return -1;
    }
    exprCount = H2FmtListCount(c->ast, listNodeId);
    for (i = 0; i < exprCount; i++) {
        int32_t exprNode = H2FmtListItemAt(c->ast, listNodeId, i);
        if (exprNode < 0) {
            return -1;
        }
        if (i > 0 && H2FmtWriteCStr(c, ", ") != 0) {
            return -1;
        }
        if (H2FmtEmitExpr(c, exprNode, 0) != 0) {
            return -1;
        }
    }
    return 0;
}

static int H2FmtExprNeedsParensForBinaryChild(
    const H2Ast* ast, int32_t parentId, int32_t childId, int rightChild) {
    const H2AstNode* p;
    const H2AstNode* ch;
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
    if (ch->kind != H2Ast_BINARY) {
        return 0;
    }
    pp = H2FmtBinPrec((H2TokenKind)p->op);
    cp = H2FmtBinPrec((H2TokenKind)ch->op);
    rightAssoc = H2FmtIsAssignmentOp((H2TokenKind)p->op);
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

static int H2FmtUseTightMulDivModSpacing(
    H2FmtCtx* c, int32_t nodeId, int32_t lhsNodeId, int32_t rhsNodeId) {
    const H2AstNode* n;
    int32_t          curNodeId;
    int              curPrec;
    if (nodeId < 0 || lhsNodeId < 0 || rhsNodeId < 0 || (uint32_t)nodeId >= c->ast->len
        || (uint32_t)lhsNodeId >= c->ast->len || (uint32_t)rhsNodeId >= c->ast->len)
    {
        return 0;
    }
    n = &c->ast->nodes[nodeId];
    if (n->op != H2Tok_MUL && n->op != H2Tok_DIV && n->op != H2Tok_MOD) {
        return 0;
    }
    if ((n->flags & H2AstFlag_PAREN) != 0) {
        return 0;
    }

    curNodeId = nodeId;
    curPrec = H2FmtBinPrec((H2TokenKind)n->op);
    while (curNodeId >= 0) {
        int32_t          parentNodeId = H2FmtFindParentNode(c->ast, curNodeId);
        const H2AstNode* parentNode;
        int              parentPrec;
        if (parentNodeId < 0) {
            break;
        }
        parentNode = &c->ast->nodes[parentNodeId];
        if (parentNode->kind != H2Ast_BINARY) {
            break;
        }
        if (H2FmtIsAssignmentOp((H2TokenKind)parentNode->op)) {
            break;
        }
        switch ((H2TokenKind)parentNode->op) {
            case H2Tok_LOGICAL_OR:
            case H2Tok_LOGICAL_AND:
            case H2Tok_EQ:
            case H2Tok_NEQ:
            case H2Tok_LT:
            case H2Tok_GT:
            case H2Tok_LTE:
            case H2Tok_GTE:         return 0;
            default:                break;
        }
        parentPrec = H2FmtBinPrec((H2TokenKind)parentNode->op);
        if (parentPrec > 0 && parentPrec < curPrec) {
            return 1;
        }
        if (parentPrec == 0 || parentPrec > curPrec) {
            break;
        }
        if ((parentNode->flags & H2AstFlag_PAREN) != 0) {
            break;
        }
        curNodeId = parentNodeId;
    }
    return 0;
}

static int H2FmtEmitExprCore(H2FmtCtx* c, int32_t nodeId) {
    const H2AstNode* n;
    int32_t          ch;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return -1;
    }
    n = &c->ast->nodes[nodeId];
    switch (n->kind) {
        case H2Ast_IDENT:
        case H2Ast_INT:
        case H2Ast_FLOAT:
        case H2Ast_BOOL:   return H2FmtWriteSlice(c, n->dataStart, n->dataEnd);
        case H2Ast_STRING: return H2FmtWriteSliceLiteral(c, n->dataStart, n->dataEnd);
        case H2Ast_RUNE:   return H2FmtWriteSliceLiteral(c, n->dataStart, n->dataEnd);
        case H2Ast_NULL:   return H2FmtWriteCStr(c, "null");
        case H2Ast_UNARY:  {
            const char* op = H2FmtTokenOpText((H2TokenKind)n->op);
            ch = H2FmtFirstChild(c->ast, nodeId);
            if (H2FmtWriteCStr(c, op) != 0) {
                return -1;
            }
            if (ch >= 0) {
                const H2AstNode* cn = &c->ast->nodes[ch];
                int              need = cn->kind == H2Ast_BINARY;
                if (H2FmtEmitExpr(c, ch, need) != 0) {
                    return -1;
                }
            }
            return 0;
        }
        case H2Ast_BINARY: {
            const char* op = H2FmtTokenOpText((H2TokenKind)n->op);
            int32_t     lhs = H2FmtFirstChild(c->ast, nodeId);
            int32_t     rhs = lhs >= 0 ? H2FmtNextSibling(c->ast, lhs) : -1;
            int         tightOp = H2FmtUseTightMulDivModSpacing(c, nodeId, lhs, rhs);
            if (lhs >= 0
                && H2FmtEmitExpr(c, lhs, H2FmtExprNeedsParensForBinaryChild(c->ast, nodeId, lhs, 0))
                       != 0)
            {
                return -1;
            }
            if (tightOp) {
                if (H2FmtWriteCStr(c, op) != 0) {
                    return -1;
                }
            } else if (
                H2FmtWriteChar(c, ' ') != 0 || H2FmtWriteCStr(c, op) != 0
                || H2FmtWriteChar(c, ' ') != 0)
            {
                return -1;
            }
            if (rhs >= 0
                && H2FmtEmitExpr(c, rhs, H2FmtExprNeedsParensForBinaryChild(c->ast, nodeId, rhs, 1))
                       != 0)
            {
                return -1;
            }
            return 0;
        }
        case H2Ast_CALL: {
            int32_t arg;
            ch = H2FmtFirstChild(c->ast, nodeId);
            if (ch >= 0 && H2FmtEmitExpr(c, ch, 0) != 0) {
                return -1;
            }
            if (H2FmtWriteChar(c, '(') != 0) {
                return -1;
            }
            arg = ch >= 0 ? H2FmtNextSibling(c->ast, ch) : -1;
            while (arg >= 0) {
                int32_t next = H2FmtNextSibling(c->ast, arg);
                if (H2FmtEmitExpr(c, arg, 0) != 0) {
                    return -1;
                }
                if (next >= 0 && H2FmtWriteCStr(c, ", ") != 0) {
                    return -1;
                }
                arg = next;
            }
            return H2FmtWriteChar(c, ')');
        }
        case H2Ast_CALL_ARG: {
            int32_t exprNode = H2FmtFirstChild(c->ast, nodeId);
            if (n->dataEnd > n->dataStart) {
                if (H2FmtWriteSlice(c, n->dataStart, n->dataEnd) != 0
                    || H2FmtWriteCStr(c, ": ") != 0)
                {
                    return -1;
                }
            }
            if (exprNode >= 0 && H2FmtEmitExpr(c, exprNode, 0) != 0) {
                return -1;
            }
            if ((n->flags & H2AstFlag_CALL_ARG_SPREAD) != 0) {
                return H2FmtWriteCStr(c, "...");
            }
            return 0;
        }
        case H2Ast_TUPLE_EXPR: {
            int32_t cur = H2FmtFirstChild(c->ast, nodeId);
            int     first = 1;
            if (H2FmtWriteChar(c, '(') != 0) {
                return -1;
            }
            while (cur >= 0) {
                if (!first && H2FmtWriteCStr(c, ", ") != 0) {
                    return -1;
                }
                if (H2FmtEmitExpr(c, cur, 0) != 0) {
                    return -1;
                }
                first = 0;
                cur = H2FmtNextSibling(c->ast, cur);
            }
            return H2FmtWriteChar(c, ')');
        }
        case H2Ast_ARRAY_LIT: {
            int32_t  cur = H2FmtFirstChild(c->ast, nodeId);
            uint32_t rbPos;
            if (!H2FmtFindCharBackwardInRange(c->src, n->start, n->end, ']', &rbPos)) {
                return -1;
            }
            if (H2FmtWriteChar(c, '[') != 0) {
                return -1;
            }
            if (cur < 0) {
                return H2FmtWriteChar(c, ']');
            }
            if (!H2FmtRangeHasChar(c->src, n->start, n->end, '\n')) {
                int first = 1;
                while (cur >= 0) {
                    if (!first && H2FmtWriteCStr(c, ", ") != 0) {
                        return -1;
                    }
                    if (H2FmtEmitExpr(c, cur, 0) != 0) {
                        return -1;
                    }
                    first = 0;
                    cur = H2FmtNextSibling(c->ast, cur);
                }
                return H2FmtWriteChar(c, ']');
            }
            c->indent++;
            while (cur >= 0) {
                int32_t  next = H2FmtNextSibling(c->ast, cur);
                uint32_t gapStart = c->ast->nodes[cur].end;
                uint32_t gapEnd = next >= 0 ? c->ast->nodes[next].start : rbPos;
                int      hasNewline = H2FmtRangeHasChar(c->src, gapStart, gapEnd, '\n');
                int      hasComma = H2FmtRangeHasChar(c->src, gapStart, gapEnd, ',');
                if (cur == H2FmtFirstChild(c->ast, nodeId) && H2FmtNewline(c) != 0) {
                    return -1;
                }
                if (H2FmtEmitExpr(c, cur, 0) != 0) {
                    return -1;
                }
                if (next >= 0) {
                    if (!hasNewline && hasComma) {
                        if (H2FmtWriteCStr(c, ", ") != 0) {
                            return -1;
                        }
                    } else if (H2FmtNewline(c) != 0) {
                        return -1;
                    }
                }
                cur = next;
            }
            c->indent--;
            if (H2FmtNewline(c) != 0) {
                return -1;
            }
            return H2FmtWriteChar(c, ']');
        }
        case H2Ast_CALL_WITH_CONTEXT: {
            int32_t callNode = H2FmtFirstChild(c->ast, nodeId);
            int32_t ctxNode = callNode >= 0 ? H2FmtNextSibling(c->ast, callNode) : -1;
            if (callNode >= 0 && H2FmtEmitExpr(c, callNode, 0) != 0) {
                return -1;
            }
            if (H2FmtWriteCStr(c, " context ") != 0) {
                return -1;
            }
            if ((n->flags & H2AstFlag_CALL_WITH_CONTEXT_PASSTHROUGH) != 0) {
                return H2FmtWriteCStr(c, "context");
            }
            return ctxNode >= 0 ? H2FmtEmitExpr(c, ctxNode, 0) : 0;
        }
        case H2Ast_CONTEXT_OVERLAY: {
            int32_t bind = H2FmtFirstChild(c->ast, nodeId);
            if (H2FmtWriteChar(c, '{') != 0) {
                return -1;
            }
            if (bind < 0) {
                return H2FmtWriteChar(c, '}');
            }
            if (H2FmtWriteChar(c, ' ') != 0) {
                return -1;
            }
            while (bind >= 0) {
                int32_t next = H2FmtNextSibling(c->ast, bind);
                if (H2FmtEmitExpr(c, bind, 0) != 0) {
                    return -1;
                }
                if (next >= 0 && H2FmtWriteCStr(c, ", ") != 0) {
                    return -1;
                }
                bind = next;
            }
            return H2FmtWriteCStr(c, " }");
        }
        case H2Ast_CONTEXT_BIND: {
            ch = H2FmtFirstChild(c->ast, nodeId);
            if (H2FmtWriteSlice(c, n->dataStart, n->dataEnd) != 0) {
                return -1;
            }
            if ((n->flags & H2AstFlag_CONTEXT_BIND_SHORTHAND) == 0 && ch >= 0) {
                if (H2FmtWriteCStr(c, ": ") != 0 || H2FmtEmitExpr(c, ch, 0) != 0) {
                    return -1;
                }
            }
            return 0;
        }
        case H2Ast_INDEX: {
            int32_t base = H2FmtFirstChild(c->ast, nodeId);
            int32_t a = base >= 0 ? H2FmtNextSibling(c->ast, base) : -1;
            int32_t b = a >= 0 ? H2FmtNextSibling(c->ast, a) : -1;
            if (base >= 0 && H2FmtEmitExpr(c, base, 0) != 0) {
                return -1;
            }
            if (H2FmtWriteChar(c, '[') != 0) {
                return -1;
            }
            if ((n->flags & H2AstFlag_INDEX_SLICE) != 0) {
                if ((n->flags & H2AstFlag_INDEX_HAS_START) != 0 && a >= 0
                    && H2FmtEmitExpr(c, a, 0) != 0)
                {
                    return -1;
                }
                if (H2FmtWriteChar(c, ':') != 0) {
                    return -1;
                }
                if ((n->flags & H2AstFlag_INDEX_HAS_END) != 0) {
                    int32_t endNode = (n->flags & H2AstFlag_INDEX_HAS_START) != 0 ? b : a;
                    if (endNode >= 0 && H2FmtEmitExpr(c, endNode, 0) != 0) {
                        return -1;
                    }
                }
            } else if (a >= 0 && H2FmtEmitExpr(c, a, 0) != 0) {
                return -1;
            }
            return H2FmtWriteChar(c, ']');
        }
        case H2Ast_FIELD_EXPR:
            ch = H2FmtFirstChild(c->ast, nodeId);
            if (ch >= 0 && H2FmtEmitExpr(c, ch, 0) != 0) {
                return -1;
            }
            if (H2FmtWriteChar(c, '.') != 0) {
                return -1;
            }
            return H2FmtWriteSlice(c, n->dataStart, n->dataEnd);
        case H2Ast_CAST: {
            int32_t expr = H2FmtFirstChild(c->ast, nodeId);
            int32_t type = expr >= 0 ? H2FmtNextSibling(c->ast, expr) : -1;
            if ((n->flags & H2FmtFlag_DROP_REDUNDANT_LITERAL_CAST) != 0) {
                return expr >= 0 ? H2FmtEmitExpr(c, expr, 0) : 0;
            }
            if (expr >= 0 && H2FmtEmitExpr(c, expr, 0) != 0) {
                return -1;
            }
            if (H2FmtWriteCStr(c, " as ") != 0) {
                return -1;
            }
            return type >= 0 ? H2FmtEmitType(c, type) : 0;
        }
        case H2Ast_SIZEOF:
            ch = H2FmtFirstChild(c->ast, nodeId);
            if (H2FmtWriteCStr(c, "sizeof(") != 0) {
                return -1;
            }
            if (ch >= 0) {
                if (n->flags == 1) {
                    if (H2FmtEmitType(c, ch) != 0) {
                        return -1;
                    }
                } else if (H2FmtEmitExpr(c, ch, 0) != 0) {
                    return -1;
                }
            }
            return H2FmtWriteChar(c, ')');
        case H2Ast_NEW: {
            int32_t type = H2FmtFirstChild(c->ast, nodeId);
            int32_t next = type >= 0 ? H2FmtNextSibling(c->ast, type) : -1;
            int32_t count = -1;
            int32_t init = -1;
            int32_t alloc = -1;
            if ((n->flags & H2AstFlag_NEW_HAS_COUNT) != 0) {
                count = next;
                next = count >= 0 ? H2FmtNextSibling(c->ast, count) : -1;
            }
            if ((n->flags & H2AstFlag_NEW_HAS_INIT) != 0) {
                init = next;
                next = init >= 0 ? H2FmtNextSibling(c->ast, init) : -1;
            }
            if ((n->flags & H2AstFlag_NEW_HAS_ALLOC) != 0) {
                alloc = next;
            }
            if (H2FmtWriteCStr(c, "new ") != 0) {
                return -1;
            }
            if ((n->flags & H2AstFlag_NEW_HAS_ARRAY_LIT) != 0) {
                if (type >= 0 && H2FmtEmitExpr(c, type, 0) != 0) {
                    return -1;
                }
            } else if (count >= 0) {
                if (H2FmtWriteChar(c, '[') != 0 || (type >= 0 && H2FmtEmitType(c, type) != 0)
                    || H2FmtWriteChar(c, ' ') != 0 || H2FmtEmitExpr(c, count, 0) != 0
                    || H2FmtWriteChar(c, ']') != 0)
                {
                    return -1;
                }
            } else {
                if (type >= 0 && H2FmtEmitType(c, type) != 0) {
                    return -1;
                }
                if (init >= 0) {
                    int32_t initFirst = H2FmtFirstChild(c->ast, init);
                    int     initTight =
                        c->ast->nodes[init].kind == H2Ast_COMPOUND_LIT
                        && (initFirst < 0 || !H2FmtIsTypeNodeKind(c->ast->nodes[initFirst].kind));
                    if (!initTight && H2FmtWriteChar(c, ' ') != 0) {
                        return -1;
                    }
                    if (H2FmtEmitExpr(c, init, 0) != 0) {
                        return -1;
                    }
                }
            }
            if (alloc >= 0) {
                if (H2FmtWriteCStr(c, " in ") != 0 || H2FmtEmitExpr(c, alloc, 0) != 0) {
                    return -1;
                }
            }
            return 0;
        }
        case H2Ast_COMPOUND_LIT: {
            int32_t  cur = H2FmtFirstChild(c->ast, nodeId);
            int32_t  type = -1;
            int32_t  field;
            uint32_t maxKeyLen = 0;
            uint32_t lbPos;
            uint32_t rbPos;
            if (cur >= 0 && H2FmtIsTypeNodeKind(c->ast->nodes[cur].kind)) {
                type = cur;
                cur = H2FmtNextSibling(c->ast, cur);
            }
            if (type >= 0 && H2FmtEmitType(c, type) != 0) {
                return -1;
            }
            if (!H2FmtFindCharForwardInRange(c->src, n->start, n->end, '{', &lbPos)
                || !H2FmtFindCharBackwardInRange(c->src, n->start, n->end, '}', &rbPos))
            {
                return -1;
            }
            if (H2FmtWriteChar(c, '{') != 0) {
                return -1;
            }
            if (cur < 0) {
                return H2FmtWriteChar(c, '}');
            }
            if (!H2FmtRangeHasChar(c->src, lbPos + 1u, rbPos, '\n')) {
                if (H2FmtWriteChar(c, ' ') != 0) {
                    return -1;
                }
                field = cur;
                while (field >= 0) {
                    int32_t next = H2FmtNextSibling(c->ast, field);
                    if (H2FmtEmitExpr(c, field, 0) != 0) {
                        return -1;
                    }
                    if (next >= 0 && H2FmtWriteCStr(c, ", ") != 0) {
                        return -1;
                    }
                    field = next;
                }
                return H2FmtWriteCStr(c, " }");
            }
            c->indent++;
            for (field = cur; field >= 0; field = H2FmtNextSibling(c->ast, field)) {
                const H2AstNode* fn = &c->ast->nodes[field];
                uint32_t         keyLen = fn->dataEnd - fn->dataStart;
                if (fn->kind == H2Ast_COMPOUND_FIELD && keyLen > maxKeyLen) {
                    maxKeyLen = keyLen;
                }
            }
            field = cur;
            while (field >= 0) {
                int32_t  next = H2FmtNextSibling(c->ast, field);
                uint32_t gapStart = c->ast->nodes[field].end;
                uint32_t gapEnd = next >= 0 ? c->ast->nodes[next].start : rbPos;
                int      hasComma = H2FmtRangeHasChar(c->src, gapStart, gapEnd, ',');
                int      hasNewline = H2FmtRangeHasChar(c->src, gapStart, gapEnd, '\n');
                if (field == cur) {
                    int firstHasNewline = H2FmtRangeHasChar(
                        c->src, lbPos + 1u, c->ast->nodes[field].start, '\n');
                    if (firstHasNewline) {
                        if (H2FmtNewline(c) != 0) {
                            return -1;
                        }
                    } else if (H2FmtWriteChar(c, ' ') != 0) {
                        return -1;
                    }
                }
                if (H2FmtEmitCompoundFieldWithAlign(c, field, maxKeyLen) != 0) {
                    return -1;
                }
                if (hasComma && H2FmtWriteChar(c, ',') != 0) {
                    return -1;
                }
                if (next >= 0) {
                    if (hasNewline) {
                        if (H2FmtNewline(c) != 0) {
                            return -1;
                        }
                    } else if (H2FmtWriteChar(c, ' ') != 0) {
                        return -1;
                    }
                } else {
                    c->indent--;
                    if (hasNewline) {
                        if (H2FmtNewline(c) != 0) {
                            return -1;
                        }
                    } else if (H2FmtWriteChar(c, ' ') != 0) {
                        return -1;
                    }
                    return H2FmtWriteChar(c, '}');
                }
                field = next;
            }
            c->indent--;
            return H2FmtWriteChar(c, '}');
        }
        case H2Ast_COMPOUND_FIELD: return H2FmtEmitCompoundFieldWithAlign(c, nodeId, 0u);
        case H2Ast_TYPE_VALUE:
            ch = H2FmtFirstChild(c->ast, nodeId);
            if (H2FmtWriteCStr(c, "type ") != 0) {
                return -1;
            }
            return ch >= 0 ? H2FmtEmitType(c, ch) : 0;
        case H2Ast_UNWRAP:
            ch = H2FmtFirstChild(c->ast, nodeId);
            if (ch >= 0 && H2FmtEmitExpr(c, ch, 0) != 0) {
                return -1;
            }
            return H2FmtWriteChar(c, '!');
        default: return H2FmtWriteSlice(c, n->start, n->end);
    }
}

static int H2FmtEmitExpr(H2FmtCtx* c, int32_t nodeId, int forceParen) {
    const H2AstNode* n;
    int              needParen;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return -1;
    }
    n = &c->ast->nodes[nodeId];
    needParen = forceParen || ((n->flags & H2AstFlag_PAREN) != 0);
    if (needParen && H2FmtWriteChar(c, '(') != 0) {
        return -1;
    }
    if (H2FmtEmitExprCore(c, nodeId) != 0) {
        return -1;
    }
    if (needParen && H2FmtWriteChar(c, ')') != 0) {
        return -1;
    }
    return 0;
}

static int H2FmtMeasureTypeLen(H2FmtCtx* c, int32_t nodeId, uint32_t* outLen) {
    H2FmtCtx m = *c;
    m.lineStart = 0;
    m.indent = 0;
    m.out.v = NULL;
    m.out.len = 0;
    m.out.cap = 0;
    if (H2FmtEmitType(&m, nodeId) != 0) {
        return -1;
    }
    *outLen = m.out.len;
    return 0;
}

static int H2FmtMeasureExprLen(H2FmtCtx* c, int32_t nodeId, int forceParen, uint32_t* outLen) {
    H2FmtCtx m = *c;
    m.lineStart = 0;
    m.indent = 0;
    m.out.v = NULL;
    m.out.len = 0;
    m.out.cap = 0;
    if (H2FmtEmitExpr(&m, nodeId, forceParen) != 0) {
        return -1;
    }
    *outLen = m.out.len;
    return 0;
}

static int H2FmtNeedsBlankLineBeforeNode(H2FmtCtx* c, int32_t prevNodeId, int32_t nextNodeId) {
    H2AstKind nextKind;
    uint32_t  gapNl;
    if (prevNodeId < 0 || nextNodeId < 0) {
        return 0;
    }
    nextKind = c->ast->nodes[nextNodeId].kind;
    gapNl = H2FmtCountNewlinesInRange(
        c->src, c->ast->nodes[prevNodeId].end, c->ast->nodes[nextNodeId].start);
    if (gapNl <= 1u) {
        return 0;
    }
    if (H2FmtHasUnusedLeadingCommentsForNode(c, nextNodeId)) {
        if (!H2FmtIsStmtNodeKind(nextKind)) {
            return 0;
        }
        if (!H2FmtGapHasIntentionalBlankLine(
                c->src, c->ast->nodes[prevNodeId].end, c->ast->nodes[nextNodeId].start))
        {
            return 0;
        }
    }
    return 1;
}

static int H2FmtTypeEnvCopy(H2FmtTypeEnv* dst, const H2FmtTypeEnv* src) {
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

static int32_t H2FmtResolveTypeNodeThroughEnv(
    const H2Ast* ast, H2StrView src, int32_t typeNodeId, const H2FmtTypeEnv* _Nullable env) {
    int32_t bindingIndex;
    if (env == NULL || typeNodeId < 0 || (uint32_t)typeNodeId >= ast->len) {
        return typeNodeId;
    }
    if (!H2FmtTypeNameIsBoundParam(ast, src, typeNodeId, env, &bindingIndex)) {
        return typeNodeId;
    }
    if (env->items[bindingIndex].concreteTypeNodeId < 0) {
        return typeNodeId;
    }
    return env->items[bindingIndex].concreteTypeNodeId;
}

static int H2FmtInferredTypeSet(
    H2FmtInferredType* inferred, int32_t typeNodeId, const H2FmtTypeEnv* _Nullable env) {
    if (inferred == NULL || typeNodeId < 0) {
        return 0;
    }
    inferred->typeNodeId = typeNodeId;
    return H2FmtTypeEnvCopy(&inferred->env, env);
}

static int H2FmtInferredTypeMatchesNode(
    const H2Ast* ast, H2StrView src, const H2FmtInferredType* inferred, int32_t typeNodeId) {
    H2FmtTypeEnv        envCopy;
    const H2FmtTypeEnv* env;
    if (inferred == NULL || inferred->typeNodeId < 0 || typeNodeId < 0) {
        return 0;
    }
    env = inferred->env.len > 0 ? &inferred->env : NULL;
    if (env != NULL && !H2FmtTypeEnvCopy(&envCopy, env)) {
        return 0;
    }
    return H2FmtTypeCompatibleWithEnvs(
               ast, src, inferred->typeNodeId, env != NULL ? &envCopy : NULL, typeNodeId, NULL)
        && H2FmtTypeCompatibleWithEnvs(ast, src, typeNodeId, NULL, inferred->typeNodeId, env);
}

static int32_t H2FmtFindLocalNamedTypeDeclByRange(
    const H2Ast* ast, H2StrView src, uint32_t nameStart, uint32_t nameEnd) {
    int32_t cur;
    if (nameEnd <= nameStart) {
        return -1;
    }
    cur = H2FmtFirstChild(ast, ast->root);
    while (cur >= 0) {
        const H2AstNode* n = &ast->nodes[cur];
        if ((n->kind == H2Ast_STRUCT || n->kind == H2Ast_UNION || n->kind == H2Ast_ENUM)
            && H2FmtSlicesEqual(src, n->dataStart, n->dataEnd, nameStart, nameEnd))
        {
            return cur;
        }
        cur = H2FmtNextSibling(ast, cur);
    }
    return -1;
}

static int32_t H2FmtFindLocalNamedTypeDeclForTypeNode(
    const H2Ast* ast, H2StrView src, int32_t typeNodeId, const H2FmtTypeEnv* _Nullable env) {
    int32_t resolvedTypeNodeId = H2FmtResolveTypeNodeThroughEnv(ast, src, typeNodeId, env);
    if (resolvedTypeNodeId < 0 || (uint32_t)resolvedTypeNodeId >= ast->len) {
        return -1;
    }
    if (ast->nodes[resolvedTypeNodeId].kind != H2Ast_TYPE_NAME) {
        return -1;
    }
    return H2FmtFindLocalNamedTypeDeclByRange(
        ast, src, ast->nodes[resolvedTypeNodeId].dataStart, ast->nodes[resolvedTypeNodeId].dataEnd);
}

static int H2FmtBindStructTypeArgsFromInstance(
    const H2Ast*        ast,
    H2StrView           src,
    int32_t             declNodeId,
    int32_t             instTypeNodeId,
    const H2FmtTypeEnv* instEnv,
    H2FmtTypeEnv*       outEnv) {
    int32_t  declParamNodeId;
    int32_t  argNodeId;
    uint32_t i;
    if (!H2FmtTypeEnvInitFromDeclTypeParams(ast, declNodeId, outEnv)) {
        return 0;
    }
    declParamNodeId = H2FmtFirstChild(ast, declNodeId);
    argNodeId = H2FmtFirstChild(ast, instTypeNodeId);
    for (i = 0; i < outEnv->len; i++) {
        int32_t resolvedArgNodeId;
        if (declParamNodeId < 0 || ast->nodes[declParamNodeId].kind != H2Ast_TYPE_PARAM
            || argNodeId < 0)
        {
            return 0;
        }
        resolvedArgNodeId = H2FmtResolveTypeNodeThroughEnv(ast, src, argNodeId, instEnv);
        outEnv->items[i].concreteTypeNodeId = resolvedArgNodeId;
        declParamNodeId = H2FmtNextSibling(ast, declParamNodeId);
        argNodeId = H2FmtNextSibling(ast, argNodeId);
    }
    return argNodeId < 0;
}

static int H2FmtFindFieldTypeOnLocalNamedType(
    const H2Ast*             ast,
    H2StrView                src,
    const H2FmtInferredType* baseType,
    uint32_t                 fieldStart,
    uint32_t                 fieldEnd,
    H2FmtInferredType*       outType) {
    int32_t      declNodeId;
    int32_t      instTypeNodeId;
    int32_t      child;
    H2FmtTypeEnv declEnv;
    if (outType != NULL) {
        H2FmtInferredTypeInit(outType);
    }
    if (baseType == NULL || outType == NULL || baseType->typeNodeId < 0 || fieldEnd <= fieldStart) {
        return 0;
    }
    instTypeNodeId = H2FmtResolveTypeNodeThroughEnv(ast, src, baseType->typeNodeId, &baseType->env);
    if (instTypeNodeId < 0 || (uint32_t)instTypeNodeId >= ast->len
        || ast->nodes[instTypeNodeId].kind != H2Ast_TYPE_NAME)
    {
        return 0;
    }
    declNodeId = H2FmtFindLocalNamedTypeDeclForTypeNode(ast, src, instTypeNodeId, NULL);
    if (declNodeId < 0 || ast->nodes[declNodeId].kind != H2Ast_STRUCT) {
        return 0;
    }
    if (!H2FmtBindStructTypeArgsFromInstance(
            ast, src, declNodeId, instTypeNodeId, &baseType->env, &declEnv))
    {
        return 0;
    }
    child = H2FmtFirstChild(ast, declNodeId);
    while (child >= 0 && ast->nodes[child].kind == H2Ast_TYPE_PARAM) {
        child = H2FmtNextSibling(ast, child);
    }
    while (child >= 0) {
        if (ast->nodes[child].kind == H2Ast_FIELD
            && H2FmtSlicesEqual(
                src, ast->nodes[child].dataStart, ast->nodes[child].dataEnd, fieldStart, fieldEnd))
        {
            int32_t fieldTypeNodeId = H2FmtFirstChild(ast, child);
            return fieldTypeNodeId >= 0 && H2FmtInferredTypeSet(outType, fieldTypeNodeId, &declEnv);
        }
        child = H2FmtNextSibling(ast, child);
    }
    return 0;
}

static int H2FmtFindLocalBindingBefore(
    const H2Ast* ast,
    H2StrView    src,
    int32_t      identNodeId,
    uint32_t     beforePos,
    int32_t* _Nullable outDeclNodeId,
    int32_t* _Nullable outTypeNodeId,
    int32_t* _Nullable outInitNodeId) {
    const H2AstNode* identNode;
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
    if (identNode->kind != H2Ast_IDENT || identNode->dataEnd <= identNode->dataStart) {
        return 0;
    }
    fnNodeId = H2FmtFindEnclosingFnNode(ast, identNodeId);
    if (fnNodeId < 0) {
        return 0;
    }

    {
        int32_t child = H2FmtFirstChild(ast, fnNodeId);
        while (child >= 0 && ast->nodes[child].kind == H2Ast_TYPE_PARAM) {
            child = H2FmtNextSibling(ast, child);
        }
        while (child >= 0 && ast->nodes[child].kind == H2Ast_PARAM) {
            int32_t paramTypeNodeId = H2FmtFirstChild(ast, child);
            if (paramTypeNodeId >= 0 && H2FmtIsTypeNodeKindRaw(ast->nodes[paramTypeNodeId].kind)
                && H2FmtNodeDeclaresNameRange(
                    ast, src, child, identNode->dataStart, identNode->dataEnd)
                && ast->nodes[child].start < beforePos)
            {
                bestStart = ast->nodes[child].start;
                bestDeclNodeId = child;
                bestTypeNodeId = paramTypeNodeId;
                bestInitNodeId = -1;
            }
            child = H2FmtNextSibling(ast, child);
        }
    }

    {
        const H2AstNode* fn = &ast->nodes[fnNodeId];
        uint32_t         i;
        for (i = 0; i < ast->len; i++) {
            const H2AstNode* n = &ast->nodes[i];
            int32_t          typeNodeId = -1;
            int32_t          initNodeId = -1;
            if (n->kind != H2Ast_VAR && n->kind != H2Ast_CONST) {
                continue;
            }
            if (n->start < fn->start || n->end > fn->end || n->end > beforePos) {
                continue;
            }
            H2FmtGetVarLikeTypeAndInit(ast, (int32_t)i, &typeNodeId, &initNodeId);
            if (!H2FmtNodeDeclaresNameRange(
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

static int H2FmtInferExprTypeEx(
    const H2Ast*       ast,
    H2StrView          src,
    int32_t            exprNodeId,
    uint32_t           beforePos,
    uint32_t           depth,
    H2FmtInferredType* outType);

static int H2FmtLiteralExprMatchesConcreteType(
    const H2Ast* ast, H2StrView src, int32_t exprNodeId, int32_t typeNodeId) {
    int32_t innerExprNodeId = exprNodeId;
    if (exprNodeId < 0 || typeNodeId < 0 || (uint32_t)exprNodeId >= ast->len) {
        return 0;
    }
    if (ast->nodes[exprNodeId].kind == H2Ast_UNARY && ast->nodes[exprNodeId].op == H2Tok_SUB) {
        innerExprNodeId = H2FmtFirstChild(ast, exprNodeId);
    }
    return innerExprNodeId >= 0
        && H2FmtCastLiteralNumericType(ast, src, innerExprNodeId, typeNodeId)
               != H2FmtNumericType_INVALID;
}

static int H2FmtMapCallActuals(
    const H2Ast*     ast,
    int32_t          callNodeId,
    H2FmtCallActual* actuals,
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
    calleeNodeId = H2FmtFirstChild(ast, callNodeId);
    if (calleeNodeId < 0) {
        return 0;
    }
    if (ast->nodes[calleeNodeId].kind == H2Ast_FIELD_EXPR) {
        int32_t recvNodeId = H2FmtFirstChild(ast, calleeNodeId);
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
    cur = H2FmtNextSibling(ast, calleeNodeId);
    while (cur >= 0) {
        const H2AstNode* argNode = &ast->nodes[cur];
        int32_t          exprNodeId;
        if (actualCount >= actualCap || (argNode->flags & H2AstFlag_CALL_ARG_SPREAD) != 0) {
            return 0;
        }
        exprNodeId = argNode->kind == H2Ast_CALL_ARG ? H2FmtFirstChild(ast, cur) : cur;
        if (exprNodeId < 0) {
            return 0;
        }
        actuals[actualCount].argNodeId = cur;
        actuals[actualCount].exprNodeId = exprNodeId;
        actuals[actualCount].labelStart = argNode->dataStart;
        actuals[actualCount].labelEnd = argNode->dataEnd;
        actuals[actualCount].hasLabel =
            (uint8_t)(argNode->kind == H2Ast_CALL_ARG && argNode->dataEnd > argNode->dataStart);
        actuals[actualCount].isSynthetic = 0;
        actualCount++;
        cur = H2FmtNextSibling(ast, cur);
    }
    if (outActualCount != NULL) {
        *outActualCount = actualCount;
    }
    return 1;
}

static int H2FmtGetFnParamForActual(
    const H2Ast*           ast,
    H2StrView              src,
    int32_t                fnNodeId,
    const H2FmtCallActual* actuals,
    uint32_t               actualCount,
    uint32_t               actualIndex,
    int32_t* _Nullable outParamNodeId,
    int32_t* _Nullable outParamTypeNodeId) {
    H2FmtCallParam params[128];
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
    cur = H2FmtFirstChild(ast, fnNodeId);
    while (cur >= 0 && ast->nodes[cur].kind == H2Ast_TYPE_PARAM) {
        cur = H2FmtNextSibling(ast, cur);
    }
    while (cur >= 0 && ast->nodes[cur].kind == H2Ast_PARAM) {
        int32_t typeNodeId = H2FmtFirstChild(ast, cur);
        if (paramCount >= 128u || typeNodeId < 0) {
            return 0;
        }
        params[paramCount].paramNodeId = cur;
        params[paramCount].typeNodeId = typeNodeId;
        params[paramCount].flags = ast->nodes[cur].flags;
        if ((ast->nodes[cur].flags & H2AstFlag_PARAM_VARIADIC) != 0) {
            hasVariadic = 1;
        }
        paramCount++;
        cur = H2FmtNextSibling(ast, cur);
    }
    fixedCount = hasVariadic ? (paramCount > 0 ? paramCount - 1u : 0u) : paramCount;
    if ((!hasVariadic && actualCount != paramCount) || (hasVariadic && actualCount < fixedCount)) {
        return 0;
    }

    for (i = 0; i < actualCount; i++) {
        const H2FmtCallActual* actual = &actuals[i];
        uint32_t               paramIndex = UINT32_MAX;
        uint32_t               p;
        if (actual->hasLabel) {
            for (p = 0; p < fixedCount; p++) {
                if (!assigned[p]
                    && H2FmtSlicesEqual(
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

static int H2FmtInferLocalCallAgainstFn(
    const H2Ast* ast,
    H2StrView    src,
    int32_t      callNodeId,
    int32_t      fnNodeId,
    uint32_t     beforePos,
    int32_t      targetArgNodeId,
    H2FmtInferredType* _Nullable outReturnType,
    H2FmtInferredType* _Nullable outTargetParamType) {
    H2FmtCallActual actuals[128];
    uint32_t        actualCount = 0;
    uint32_t        i;
    H2FmtTypeEnv    fnEnv;
    int32_t         retTypeNodeId;
    int32_t         targetActualIndex = -1;
    if (outReturnType != NULL) {
        H2FmtInferredTypeInit(outReturnType);
    }
    if (outTargetParamType != NULL) {
        H2FmtInferredTypeInit(outTargetParamType);
    }
    if (!H2FmtMapCallActuals(ast, callNodeId, actuals, &actualCount, 128u)
        || !H2FmtTypeEnvInitFromDeclTypeParams(ast, fnNodeId, &fnEnv))
    {
        return 0;
    }
    for (i = 0; i < actualCount; i++) {
        H2FmtInferredType actualType;
        int32_t           paramTypeNodeId = -1;
        int32_t           concreteParamTypeNodeId;
        int               isTargetActual = 0;
        if (!H2FmtGetFnParamForActual(
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
        H2FmtInferredTypeInit(&actualType);
        if (!H2FmtInferExprTypeEx(ast, src, actuals[i].exprNodeId, beforePos, 0u, &actualType)) {
            concreteParamTypeNodeId = H2FmtResolveTypeNodeThroughEnv(
                ast, src, paramTypeNodeId, &fnEnv);
            if (!H2FmtLiteralExprMatchesConcreteType(
                    ast, src, actuals[i].exprNodeId, concreteParamTypeNodeId))
            {
                return 0;
            }
        } else if (!H2FmtTypeCompatibleWithEnvs(
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
        if (!H2FmtGetFnParamForActual(
                ast,
                src,
                fnNodeId,
                actuals,
                actualCount,
                (uint32_t)targetActualIndex,
                NULL,
                &targetParamTypeNodeId)
            || !H2FmtInferredTypeSet(outTargetParamType, targetParamTypeNodeId, &fnEnv))
        {
            return 0;
        }
    }
    retTypeNodeId = H2FmtFindFnReturnTypeNode(ast, fnNodeId);
    if (outReturnType == NULL) {
        return 1;
    }
    return retTypeNodeId >= 0 && H2FmtInferredTypeSet(outReturnType, retTypeNodeId, &fnEnv);
}

static int H2FmtCanContinueAlignedGroup(H2FmtCtx* c, int32_t prevNodeId, int32_t nextNodeId) {
    uint32_t gapNl;
    if (prevNodeId < 0 || nextNodeId < 0) {
        return 0;
    }
    gapNl = H2FmtCountNewlinesInRange(
        c->src, c->ast->nodes[prevNodeId].end, c->ast->nodes[nextNodeId].start);
    if (gapNl != 1u) {
        return 0;
    }
    if (H2FmtHasUnusedLeadingCommentsForNode(c, nextNodeId)) {
        return 0;
    }
    return 1;
}

static int H2FmtEmitStmt(H2FmtCtx* c, int32_t nodeId);

typedef struct {
    int32_t     nodeId;
    int32_t     typeNode;
    int32_t     initNode;
    const char* kw;
    uint32_t    nameLen;
    uint32_t    typeLen;
    uint32_t    initLen;
    uint32_t    codeLen;
    uint32_t    kwLen;
    uint8_t     hasType;
    uint8_t     hasInit;
    uint8_t     shortVar;
    uint8_t     hasTrailingComment;
} H2FmtAlignedVarRow;

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
} H2FmtAlignedAssignRow;

typedef struct {
    uint32_t minLhsLen;
    uint32_t baseNameStart;
    uint32_t baseNameEnd;
    uint8_t  valid;
    uint8_t  _pad[3];
} H2FmtAssignCarryHint;

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
} H2FmtSwitchClauseRow;

static int H2FmtGetAssignStmtParts(
    H2FmtCtx* c, int32_t stmtNodeId, int32_t* outLhs, int32_t* outRhs, uint16_t* outOp) {
    int32_t          exprNodeId;
    const H2AstNode* exprNode;
    int32_t          lhsNodeId;
    int32_t          rhsNodeId;
    if (stmtNodeId < 0 || (uint32_t)stmtNodeId >= c->ast->len) {
        return 0;
    }
    if (c->ast->nodes[stmtNodeId].kind != H2Ast_EXPR_STMT) {
        return 0;
    }
    exprNodeId = H2FmtFirstChild(c->ast, stmtNodeId);
    if (exprNodeId < 0 || (uint32_t)exprNodeId >= c->ast->len) {
        return 0;
    }
    exprNode = &c->ast->nodes[exprNodeId];
    if (exprNode->kind != H2Ast_BINARY || !H2FmtIsAssignmentOp((H2TokenKind)exprNode->op)) {
        return 0;
    }
    lhsNodeId = H2FmtFirstChild(c->ast, exprNodeId);
    rhsNodeId = lhsNodeId >= 0 ? H2FmtNextSibling(c->ast, lhsNodeId) : -1;
    if (lhsNodeId < 0 || rhsNodeId < 0) {
        return 0;
    }
    *outLhs = lhsNodeId;
    *outRhs = rhsNodeId;
    *outOp = exprNode->op;
    return 1;
}

static int H2FmtHasUnusedCommentsInNodeRange(const H2FmtCtx* c, int32_t nodeId) {
    uint32_t i;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return 0;
    }
    for (i = 0; i < c->commentLen; i++) {
        const H2Comment* cm = &c->comments[i];
        if (c->commentUsed[i]) {
            continue;
        }
        if (H2FmtCommentWithinNodeRange(c->ast, cm, nodeId)) {
            return 1;
        }
    }
    return 0;
}

static int H2FmtCanInlineSingleStmtBlock(H2FmtCtx* c, int32_t blockNodeId, int32_t* outStmtNodeId) {
    int32_t stmtNodeId;
    if (blockNodeId < 0 || (uint32_t)blockNodeId >= c->ast->len
        || c->ast->nodes[blockNodeId].kind != H2Ast_BLOCK)
    {
        return 0;
    }
    if (H2FmtHasUnusedCommentsInNodeRange(c, blockNodeId)) {
        return 0;
    }
    stmtNodeId = H2FmtFirstChild(c->ast, blockNodeId);
    if (stmtNodeId < 0 || H2FmtNextSibling(c->ast, stmtNodeId) >= 0) {
        return 0;
    }
    *outStmtNodeId = stmtNodeId;
    return 1;
}

static int H2FmtEmitInlineSingleStmtBlock(H2FmtCtx* c, int32_t blockNodeId) {
    int32_t stmtNodeId;
    if (!H2FmtCanInlineSingleStmtBlock(c, blockNodeId, &stmtNodeId)) {
        return -1;
    }
    if (H2FmtWriteCStr(c, "{ ") != 0 || H2FmtEmitStmtInline(c, stmtNodeId) != 0) {
        return -1;
    }
    return H2FmtWriteCStr(c, " }");
}

static int H2FmtMeasureInlineSingleStmtBlockLen(
    H2FmtCtx* c, int32_t blockNodeId, uint32_t* outLen) {
    H2FmtCtx m = *c;
    m.lineStart = 0;
    m.indent = 0;
    m.out.v = NULL;
    m.out.len = 0;
    m.out.cap = 0;
    if (H2FmtEmitInlineSingleStmtBlock(&m, blockNodeId) != 0) {
        return -1;
    }
    *outLen = m.out.len;
    return 0;
}

static int H2FmtNodeSourceTextEqual(H2FmtCtx* c, int32_t aNodeId, int32_t bNodeId) {
    const H2AstNode* a;
    const H2AstNode* b;
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

static int H2FmtIsInlineAnonAggregateType(H2FmtCtx* c, int32_t typeNodeId) {
    H2AstKind kind;
    if (typeNodeId < 0 || (uint32_t)typeNodeId >= c->ast->len) {
        return 0;
    }
    kind = c->ast->nodes[typeNodeId].kind;
    if (kind != H2Ast_TYPE_ANON_STRUCT && kind != H2Ast_TYPE_ANON_UNION) {
        return 0;
    }
    return H2FmtCountNewlinesInRange(
               c->src, c->ast->nodes[typeNodeId].start, c->ast->nodes[typeNodeId].end)
        == 0u;
}

static int H2FmtEmitAlignedVarOrConstGroup(
    H2FmtCtx*             c,
    int32_t               firstNodeId,
    const char*           kw,
    int                   allowMixedKind,
    int32_t*              outLast,
    int32_t*              outNext,
    H2FmtAssignCarryHint* outCarryHint) {
    const H2AstKind     kind = c->ast->nodes[firstNodeId].kind;
    int32_t             cur = firstNodeId;
    int32_t             prev = -1;
    uint32_t            count = 0;
    uint32_t            i;
    uint32_t            maxKwLen = H2FmtCStrLen(kw);
    uint32_t            maxNameLenWithType = 0;
    uint32_t            maxNameLenNoType = 0;
    uint32_t            maxBeforeOpLen = 0;
    H2FmtAlignedVarRow* rows;
    uint32_t*           commentRunMaxLens;
    int                 firstShortVar = -1;

    while (cur >= 0) {
        H2AstKind        curKind = c->ast->nodes[cur].kind;
        const H2AstNode* curNode = &c->ast->nodes[cur];
        const char*      curKw;
        int32_t          next = H2FmtNextSibling(c->ast, cur);
        int32_t          typeNode = -1;
        int32_t          initNode = -1;
        int              curShortVar;
        if (allowMixedKind) {
            if (curKind != H2Ast_VAR && curKind != H2Ast_CONST) {
                break;
            }
        } else if (curKind != kind) {
            break;
        }
        if (H2FmtIsGroupedVarLike(c, cur)) {
            break;
        }
        if ((c->ast->nodes[cur].flags & H2AstFlag_PUB) != 0) {
            break;
        }
        if (prev >= 0 && !H2FmtCanContinueAlignedGroup(c, prev, cur)) {
            break;
        }
        curKw = allowMixedKind ? (curKind == H2Ast_CONST ? "const" : "var") : kw;
        H2FmtGetVarLikeTypeAndInit(c->ast, cur, &typeNode, &initNode);
        H2FmtRewriteVarTypeFromLiteralCast(c->ast, c->src, curKw, 1u, &typeNode, &initNode);
        H2FmtRewriteRedundantVarType(
            c->ast, c->src, curKw, 1u, curNode->start, &typeNode, &initNode);
        curShortVar = H2FmtCanEmitShortVarDecl(c->ast, c->src, curKw, 1u, cur, typeNode, initNode);
        if (firstShortVar < 0) {
            firstShortVar = curShortVar;
        } else if (curShortVar != firstShortVar) {
            break;
        }
        count++;
        prev = cur;
        cur = next;
    }

    rows = (H2FmtAlignedVarRow*)H2ArenaAlloc(
        c->out.arena,
        count * (uint32_t)sizeof(H2FmtAlignedVarRow),
        (uint32_t)_Alignof(H2FmtAlignedVarRow));
    if (rows == NULL) {
        return -1;
    }
    memset(rows, 0, count * (uint32_t)sizeof(H2FmtAlignedVarRow));

    cur = firstNodeId;
    for (i = 0; i < count; i++) {
        const H2AstNode* n = &c->ast->nodes[cur];
        int32_t          typeNode = H2FmtFirstChild(c->ast, cur);
        int32_t          initNode = -1;
        rows[i].kw = allowMixedKind ? (n->kind == H2Ast_CONST ? "const" : "var") : kw;
        rows[i].kwLen = H2FmtCStrLen(rows[i].kw);
        if (rows[i].kwLen > maxKwLen) {
            maxKwLen = rows[i].kwLen;
        }
        rows[i].nodeId = cur;
        rows[i].nameLen = n->dataEnd - n->dataStart;
        if (typeNode >= 0 && !H2FmtIsTypeNodeKind(c->ast->nodes[typeNode].kind)) {
            initNode = typeNode;
            typeNode = -1;
        } else if (typeNode >= 0) {
            initNode = H2FmtNextSibling(c->ast, typeNode);
        }
        H2FmtRewriteVarTypeFromLiteralCast(c->ast, c->src, rows[i].kw, 1u, &typeNode, &initNode);
        H2FmtRewriteRedundantVarType(
            c->ast, c->src, rows[i].kw, 1u, n->start, &typeNode, &initNode);
        rows[i].typeNode = typeNode;
        rows[i].initNode = initNode;
        rows[i].shortVar = (uint8_t)H2FmtCanEmitShortVarDecl(
            c->ast, c->src, rows[i].kw, 1u, cur, typeNode, initNode);
        rows[i].hasType = (uint8_t)(typeNode >= 0);
        rows[i].hasInit = (uint8_t)(initNode >= 0);
        if (rows[i].shortVar) {
            /* Short declarations are emitted as statement syntax, not var-column rows. */
        } else if (rows[i].hasType) {
            if (rows[i].nameLen > maxNameLenWithType) {
                maxNameLenWithType = rows[i].nameLen;
            }
        } else if (rows[i].nameLen > maxNameLenNoType) {
            maxNameLenNoType = rows[i].nameLen;
        }
        if (typeNode >= 0 && H2FmtMeasureTypeLen(c, typeNode, &rows[i].typeLen) != 0) {
            return -1;
        }
        if (initNode >= 0 && H2FmtMeasureExprLen(c, initNode, 0, &rows[i].initLen) != 0) {
            return -1;
        }
        rows[i].hasTrailingComment = (uint8_t)H2FmtHasUnusedTrailingCommentsForNodes(c, &cur, 1u);
        cur = H2FmtNextSibling(c->ast, cur);
    }

    for (i = 0; i < count; i++) {
        if (!rows[i].shortVar && rows[i].hasInit) {
            uint32_t nameColLen = rows[i].hasType ? maxNameLenWithType : maxNameLenNoType;
            uint32_t beforeOpLen =
                maxKwLen + 1u + nameColLen + (rows[i].hasType ? 1u : 0u)
                + (rows[i].hasType ? rows[i].typeLen : 0u);
            if (beforeOpLen > maxBeforeOpLen) {
                maxBeforeOpLen = beforeOpLen;
            }
        }
    }

    commentRunMaxLens = (uint32_t*)H2ArenaAlloc(
        c->out.arena, count * (uint32_t)sizeof(uint32_t), (uint32_t)_Alignof(uint32_t));
    if (commentRunMaxLens == NULL) {
        return -1;
    }
    memset(commentRunMaxLens, 0, count * (uint32_t)sizeof(uint32_t));

    for (i = 0; i < count; i++) {
        uint32_t nameColLen = rows[i].hasType ? maxNameLenWithType : maxNameLenNoType;
        uint32_t codeLen =
            rows[i].shortVar
                ? rows[i].nameLen + 4u
                : maxKwLen + 1u + nameColLen + (rows[i].hasType ? 1u : 0u)
                      + (rows[i].hasType ? rows[i].typeLen : 0u);
        if (rows[i].shortVar) {
            codeLen += rows[i].initLen;
        } else if (rows[i].hasInit) {
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
        const H2AstNode* n = &c->ast->nodes[rows[i].nodeId];
        uint32_t         nameColLen = rows[i].hasType ? maxNameLenWithType : maxNameLenNoType;
        uint32_t         lineLen;
        if (H2FmtEmitLeadingCommentsForNode(c, rows[i].nodeId) != 0) {
            return -1;
        }
        if (rows[i].shortVar) {
            if (H2FmtWriteSlice(c, n->dataStart, n->dataEnd) != 0 || H2FmtWriteCStr(c, " := ") != 0
                || H2FmtEmitExpr(c, rows[i].initNode, 0) != 0)
            {
                return -1;
            }
            lineLen = rows[i].nameLen + 4u + rows[i].initLen;
            if (rows[i].hasTrailingComment) {
                uint32_t padComment = (commentRunMaxLens[i] - lineLen) + 1u;
                int32_t  nodeId = rows[i].nodeId;
                if (H2FmtEmitTrailingCommentsForNodes(c, &nodeId, 1u, padComment) != 0) {
                    return -1;
                }
            }
            if (i + 1u < count && H2FmtNewline(c) != 0) {
                return -1;
            }
            continue;
        }
        if (H2FmtWriteCStr(c, rows[i].kw) != 0
            || H2FmtWriteSpaces(c, (maxKwLen - rows[i].kwLen) + 1u) != 0
            || H2FmtWriteSlice(c, n->dataStart, n->dataEnd) != 0)
        {
            return -1;
        }
        if (H2FmtWriteSpaces(c, nameColLen - rows[i].nameLen + (rows[i].hasType ? 1u : 0u)) != 0) {
            return -1;
        }
        lineLen = maxKwLen + 1u + nameColLen + (rows[i].hasType ? 1u : 0u);
        if (rows[i].hasType) {
            if (H2FmtEmitType(c, rows[i].typeNode) != 0) {
                return -1;
            }
            lineLen += rows[i].typeLen;
        }
        if (rows[i].hasInit) {
            uint32_t padBeforeOp = (maxBeforeOpLen + 1u) - lineLen;
            if (H2FmtWriteSpaces(c, padBeforeOp) != 0 || H2FmtWriteChar(c, '=') != 0
                || H2FmtWriteChar(c, ' ') != 0 || H2FmtEmitExpr(c, rows[i].initNode, 0) != 0)
            {
                return -1;
            }
            lineLen += padBeforeOp + 1u + 1u + rows[i].initLen;
        }
        if (rows[i].hasTrailingComment) {
            uint32_t padComment = (commentRunMaxLens[i] - lineLen) + 1u;
            int32_t  nodeId = rows[i].nodeId;
            if (H2FmtEmitTrailingCommentsForNodes(c, &nodeId, 1u, padComment) != 0) {
                return -1;
            }
        }
        if (i + 1u < count && H2FmtNewline(c) != 0) {
            return -1;
        }
    }

    outCarryHint->valid = 0;
    if (count == 1u && !rows[0].shortVar && !rows[0].hasType && rows[0].hasInit) {
        const H2AstNode* n = &c->ast->nodes[rows[0].nodeId];
        outCarryHint->minLhsLen = maxBeforeOpLen;
        outCarryHint->baseNameStart = n->dataStart;
        outCarryHint->baseNameEnd = n->dataEnd;
        outCarryHint->valid = 1;
    }

    *outLast = rows[count - 1u].nodeId;
    *outNext = cur;
    return 0;
}

static int H2FmtEmitAlignedAssignGroup(
    H2FmtCtx* c, int32_t firstNodeId, int32_t* outLast, int32_t* outNext, uint32_t minLhsLen) {
    int32_t                cur = firstNodeId;
    int32_t                prev = -1;
    int32_t                prevLhs = -1;
    uint32_t               count = 0;
    uint32_t               i;
    uint32_t               maxLhsLen = 0;
    uint32_t               maxOpLen = 0;
    H2FmtAlignedAssignRow* rows;

    while (cur >= 0) {
        int32_t  lhsNode;
        int32_t  rhsNode;
        uint16_t op;
        int32_t  next = H2FmtNextSibling(c->ast, cur);
        if (!H2FmtGetAssignStmtParts(c, cur, &lhsNode, &rhsNode, &op)) {
            break;
        }
        if (prev >= 0 && !H2FmtCanContinueAlignedGroup(c, prev, cur)) {
            break;
        }
        if (prevLhs >= 0 && !H2FmtNodeSourceTextEqual(c, prevLhs, lhsNode)) {
            break;
        }
        count++;
        prev = cur;
        prevLhs = lhsNode;
        cur = next;
    }

    rows = (H2FmtAlignedAssignRow*)H2ArenaAlloc(
        c->out.arena,
        count * (uint32_t)sizeof(H2FmtAlignedAssignRow),
        (uint32_t)_Alignof(H2FmtAlignedAssignRow));
    if (rows == NULL) {
        return -1;
    }
    memset(rows, 0, count * (uint32_t)sizeof(H2FmtAlignedAssignRow));

    cur = firstNodeId;
    for (i = 0; i < count; i++) {
        int32_t     lhsNode;
        int32_t     rhsNode;
        uint16_t    op;
        const char* opText;
        if (!H2FmtGetAssignStmtParts(c, cur, &lhsNode, &rhsNode, &op)) {
            return -1;
        }
        rows[i].nodeId = cur;
        rows[i].lhsNode = lhsNode;
        rows[i].rhsNode = rhsNode;
        rows[i].op = op;
        if (H2FmtMeasureExprLen(c, lhsNode, 0, &rows[i].lhsLen) != 0
            || H2FmtMeasureExprLen(c, rhsNode, 0, &rows[i].rhsLen) != 0)
        {
            return -1;
        }
        opText = H2FmtTokenOpText((H2TokenKind)op);
        rows[i].opLen = H2FmtCStrLen(opText);
        if (rows[i].lhsLen > maxLhsLen) {
            maxLhsLen = rows[i].lhsLen;
        }
        if (rows[i].opLen > maxOpLen) {
            maxOpLen = rows[i].opLen;
        }
        rows[i].hasTrailingComment = (uint8_t)H2FmtHasUnusedTrailingCommentsForNodes(c, &cur, 1u);
        cur = H2FmtNextSibling(c->ast, cur);
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
        const char* opText = H2FmtTokenOpText((H2TokenKind)rows[i].op);
        if (H2FmtEmitLeadingCommentsForNode(c, rows[i].nodeId) != 0) {
            return -1;
        }
        if (H2FmtEmitExpr(c, rows[i].lhsNode, 0) != 0 || H2FmtWriteSpaces(c, padBeforeOp) != 0
            || H2FmtWriteCStr(c, opText) != 0 || H2FmtWriteSpaces(c, padAfterOp) != 0
            || H2FmtEmitExpr(c, rows[i].rhsNode, 0) != 0)
        {
            return -1;
        }
        if (rows[i].hasTrailingComment) {
            uint32_t padComment = 1u;
            int32_t  nodeId = rows[i].nodeId;
            if (H2FmtEmitTrailingCommentsForNodes(c, &nodeId, 1u, padComment) != 0) {
                return -1;
            }
        }
        if (i + 1u < count && H2FmtNewline(c) != 0) {
            return -1;
        }
    }

    *outLast = rows[count - 1u].nodeId;
    *outNext = cur;
    return 0;
}

static int H2FmtIsIndexedAssignOnNamedBase(
    H2FmtCtx* c, int32_t lhsNodeId, uint32_t nameStart, uint32_t nameEnd) {
    const H2AstNode* lhsNode;
    int32_t          baseNodeId;
    uint32_t         identStart = 0;
    uint32_t         identEnd = 0;
    if (lhsNodeId < 0 || (uint32_t)lhsNodeId >= c->ast->len || nameEnd <= nameStart) {
        return 0;
    }
    lhsNode = &c->ast->nodes[lhsNodeId];
    if (lhsNode->kind != H2Ast_INDEX || (lhsNode->flags & H2AstFlag_INDEX_SLICE) != 0) {
        return 0;
    }
    baseNodeId = H2FmtFirstChild(c->ast, lhsNodeId);
    if (!H2FmtExprIsPlainIdent(c->ast, baseNodeId, &identStart, &identEnd)) {
        return 0;
    }
    return H2FmtSlicesEqual(c->src, identStart, identEnd, nameStart, nameEnd);
}

static int H2FmtMeasureCaseHeadLen(
    H2FmtCtx* c, int32_t caseNodeId, uint32_t* outLen, int32_t* outBodyNodeId) {
    int32_t  k = H2FmtFirstChild(c->ast, caseNodeId);
    uint32_t len = 5u;
    int      first = 1;
    int32_t  bodyNodeId = -1;
    while (k >= 0) {
        int32_t next = H2FmtNextSibling(c->ast, k);
        if (next < 0) {
            bodyNodeId = k;
            break;
        }
        int32_t          exprNode = k;
        int32_t          aliasNode = -1;
        const H2AstNode* kn = &c->ast->nodes[k];
        if (kn->kind == H2Ast_CASE_PATTERN) {
            exprNode = H2FmtFirstChild(c->ast, k);
            aliasNode = exprNode >= 0 ? H2FmtNextSibling(c->ast, exprNode) : -1;
            if (exprNode < 0) {
                return -1;
            }
        }
        if (!first) {
            len += 2u;
        }
        {
            uint32_t exprLen;
            if (H2FmtMeasureExprLen(c, exprNode, 0, &exprLen) != 0) {
                return -1;
            }
            len += exprLen;
        }
        if (aliasNode >= 0) {
            const H2AstNode* alias = &c->ast->nodes[aliasNode];
            len += 4u + (alias->dataEnd - alias->dataStart); /* " as " + alias */
        }
        first = 0;
        k = next;
    }
    if (bodyNodeId < 0 || c->ast->nodes[bodyNodeId].kind != H2Ast_BLOCK) {
        return -1;
    }
    *outLen = len;
    *outBodyNodeId = bodyNodeId;
    return 0;
}

static int H2FmtEmitCaseHead(H2FmtCtx* c, int32_t caseNodeId) {
    int32_t k = H2FmtFirstChild(c->ast, caseNodeId);
    int     first = 1;
    if (H2FmtWriteCStr(c, "case ") != 0) {
        return -1;
    }
    while (k >= 0) {
        int32_t next = H2FmtNextSibling(c->ast, k);
        int32_t exprNode = k;
        int32_t aliasNode = -1;
        if (next < 0) {
            break;
        }
        if (c->ast->nodes[k].kind == H2Ast_CASE_PATTERN) {
            exprNode = H2FmtFirstChild(c->ast, k);
            aliasNode = exprNode >= 0 ? H2FmtNextSibling(c->ast, exprNode) : -1;
            if (exprNode < 0) {
                return -1;
            }
        }
        if (!first && H2FmtWriteCStr(c, ", ") != 0) {
            return -1;
        }
        if (H2FmtEmitExpr(c, exprNode, 0) != 0) {
            return -1;
        }
        if (aliasNode >= 0) {
            const H2AstNode* alias = &c->ast->nodes[aliasNode];
            if (H2FmtWriteCStr(c, " as ") != 0
                || H2FmtWriteSlice(c, alias->dataStart, alias->dataEnd) != 0)
            {
                return -1;
            }
        }
        first = 0;
        k = next;
    }
    return 0;
}

static int H2FmtEmitSwitchClauseGroup(
    H2FmtCtx* c,
    int32_t   firstClauseNodeId,
    int32_t*  outLastClauseNodeId,
    int32_t*  outNextClauseNodeId) {
    int32_t               cur = firstClauseNodeId;
    int32_t               prev = -1;
    uint32_t              count = 0;
    uint32_t              i;
    H2FmtSwitchClauseRow* rows;
    uint32_t*             commentRunMaxLens;
    uint32_t*             inlineRunMaxHeadLens;

    while (cur >= 0) {
        int32_t next = H2FmtNextSibling(c->ast, cur);
        if (c->ast->nodes[cur].kind != H2Ast_CASE && c->ast->nodes[cur].kind != H2Ast_DEFAULT) {
            break;
        }
        if (prev >= 0 && !H2FmtCanContinueAlignedGroup(c, prev, cur)) {
            break;
        }
        count++;
        prev = cur;
        cur = next;
    }

    rows = (H2FmtSwitchClauseRow*)H2ArenaAlloc(
        c->out.arena,
        count * (uint32_t)sizeof(H2FmtSwitchClauseRow),
        (uint32_t)_Alignof(H2FmtSwitchClauseRow));
    if (rows == NULL) {
        return -1;
    }
    memset(rows, 0, count * (uint32_t)sizeof(H2FmtSwitchClauseRow));

    inlineRunMaxHeadLens = (uint32_t*)H2ArenaAlloc(
        c->out.arena, count * (uint32_t)sizeof(uint32_t), (uint32_t)_Alignof(uint32_t));
    if (inlineRunMaxHeadLens == NULL) {
        return -1;
    }
    memset(inlineRunMaxHeadLens, 0, count * (uint32_t)sizeof(uint32_t));

    commentRunMaxLens = (uint32_t*)H2ArenaAlloc(
        c->out.arena, count * (uint32_t)sizeof(uint32_t), (uint32_t)_Alignof(uint32_t));
    if (commentRunMaxLens == NULL) {
        return -1;
    }
    memset(commentRunMaxLens, 0, count * (uint32_t)sizeof(uint32_t));

    cur = firstClauseNodeId;
    for (i = 0; i < count; i++) {
        H2FmtSwitchClauseRow* r = &rows[i];
        int32_t               next = H2FmtNextSibling(c->ast, cur);
        r->nodeId = cur;
        r->isDefault = (uint8_t)(c->ast->nodes[cur].kind == H2Ast_DEFAULT);
        if (r->isDefault) {
            r->headLen = 7u;
            r->bodyNodeId = H2FmtFirstChild(c->ast, cur);
            if (r->bodyNodeId < 0 || c->ast->nodes[r->bodyNodeId].kind != H2Ast_BLOCK) {
                return -1;
            }
        } else if (H2FmtMeasureCaseHeadLen(c, cur, &r->headLen, &r->bodyNodeId) != 0) {
            return -1;
        }
        {
            int32_t stmtNodeId = -1;
            if (H2FmtCanInlineSingleStmtBlock(c, r->bodyNodeId, &stmtNodeId)) {
                r->inlineBody = 1;
                if (H2FmtMeasureInlineSingleStmtBlockLen(c, r->bodyNodeId, &r->inlineBodyLen) != 0)
                {
                    return -1;
                }
            }
        }
        r->hasTrailingComment = (uint8_t)H2FmtHasUnusedTrailingCommentsForNodes(c, &cur, 1u);
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
        H2FmtSwitchClauseRow* r = &rows[i];
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
        H2FmtSwitchClauseRow* r = &rows[i];
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
        if (H2FmtEmitLeadingCommentsForNode(c, r->nodeId) != 0) {
            return -1;
        }
        if (r->isDefault) {
            if (H2FmtWriteCStr(c, "default") != 0) {
                return -1;
            }
        } else if (H2FmtEmitCaseHead(c, r->nodeId) != 0) {
            return -1;
        }
        lineLen = r->headLen;
        if (H2FmtWriteSpaces(c, padBeforeBody) != 0) {
            return -1;
        }
        lineLen += padBeforeBody;
        if (r->inlineBody) {
            if (H2FmtEmitInlineSingleStmtBlock(c, r->bodyNodeId) != 0) {
                return -1;
            }
            lineLen += r->inlineBodyLen;
        } else if (H2FmtEmitBlock(c, r->bodyNodeId) != 0) {
            return -1;
        }
        if (r->hasTrailingComment) {
            int32_t  nodeId = r->nodeId;
            uint32_t padComment = 1u;
            if (r->inlineBody && commentRunMaxLens[i] > 0u) {
                padComment = (commentRunMaxLens[i] - lineLen) + 1u;
            }
            if (H2FmtEmitTrailingCommentsForNodes(c, &nodeId, 1u, padComment) != 0) {
                return -1;
            }
        }
        if (i + 1u < count && H2FmtNewline(c) != 0) {
            return -1;
        }
    }

    *outLastClauseNodeId = rows[count - 1u].nodeId;
    *outNextClauseNodeId = cur;
    return 0;
}

static int H2FmtEmitBlock(H2FmtCtx* c, int32_t nodeId) {
    int32_t              stmt;
    int32_t              prevEmitted = -1;
    H2FmtAssignCarryHint carryHint;
    memset(&carryHint, 0, sizeof(carryHint));
    if (H2FmtWriteChar(c, '{') != 0) {
        return -1;
    }
    stmt = H2FmtFirstChild(c->ast, nodeId);
    if (stmt >= 0) {
        if (H2FmtNewline(c) != 0) {
            return -1;
        }
        c->indent++;
        while (stmt >= 0) {
            int32_t  last = stmt;
            int32_t  next = H2FmtNextSibling(c->ast, stmt);
            int32_t  lhsNode = -1;
            int32_t  rhsNode = -1;
            uint16_t op = 0;
            if (prevEmitted >= 0) {
                if (!H2FmtCanContinueAlignedGroup(c, prevEmitted, stmt)) {
                    carryHint.valid = 0;
                }
                if (H2FmtNewline(c) != 0) {
                    return -1;
                }
                if (H2FmtNeedsBlankLineBeforeNode(c, prevEmitted, stmt) && H2FmtNewline(c) != 0) {
                    return -1;
                }
            }
            if (c->ast->nodes[stmt].kind == H2Ast_VAR && !H2FmtIsGroupedVarLike(c, stmt)) {
                if (H2FmtEmitAlignedVarOrConstGroup(c, stmt, "var", 0, &last, &next, &carryHint)
                    != 0)
                {
                    return -1;
                }
            } else if (c->ast->nodes[stmt].kind == H2Ast_CONST && !H2FmtIsGroupedVarLike(c, stmt)) {
                if (H2FmtEmitAlignedVarOrConstGroup(c, stmt, "const", 0, &last, &next, &carryHint)
                    != 0)
                {
                    return -1;
                }
            } else if (H2FmtGetAssignStmtParts(c, stmt, &lhsNode, &rhsNode, &op)) {
                uint32_t minLhsLen = 0;
                if (carryHint.valid
                    && H2FmtIsIndexedAssignOnNamedBase(
                        c, lhsNode, carryHint.baseNameStart, carryHint.baseNameEnd))
                {
                    minLhsLen = carryHint.minLhsLen;
                }
                if (H2FmtEmitAlignedAssignGroup(c, stmt, &last, &next, minLhsLen) != 0) {
                    return -1;
                }
                carryHint.valid = 0;
            } else {
                if (H2FmtEmitStmt(c, stmt) != 0) {
                    return -1;
                }
                carryHint.valid = 0;
                last = stmt;
            }
            prevEmitted = last;
            stmt = next;
        }
        c->indent--;
        if (H2FmtNewline(c) != 0) {
            return -1;
        }
    }
    return H2FmtWriteChar(c, '}');
}

static int H2FmtEmitVarLike(H2FmtCtx* c, int32_t nodeId, const char* kw) {
    int32_t firstChild = H2FmtFirstChild(c->ast, nodeId);
    int32_t type = -1;
    int32_t init = -1;
    if (firstChild >= 0 && c->ast->nodes[firstChild].kind == H2Ast_NAME_LIST) {
        uint32_t i;
        uint32_t nameCount = H2FmtListCount(c->ast, firstChild);
        int32_t  afterNames = H2FmtNextSibling(c->ast, firstChild);
        if (afterNames >= 0 && H2FmtIsTypeNodeKind(c->ast->nodes[afterNames].kind)) {
            type = afterNames;
            init = H2FmtNextSibling(c->ast, afterNames);
        } else {
            init = afterNames;
        }
        H2FmtRewriteVarTypeFromLiteralCast(c->ast, c->src, kw, nameCount, &type, &init);
        H2FmtRewriteRedundantVarType(
            c->ast, c->src, kw, nameCount, c->ast->nodes[nodeId].start, &type, &init);
        if (H2FmtWriteCStr(c, kw) != 0 || H2FmtWriteChar(c, ' ') != 0) {
            return -1;
        }
        for (i = 0; i < nameCount; i++) {
            int32_t nameNode = H2FmtListItemAt(c->ast, firstChild, i);
            if (nameNode < 0) {
                return -1;
            }
            if (i > 0 && H2FmtWriteCStr(c, ", ") != 0) {
                return -1;
            }
            if (H2FmtWriteSlice(
                    c, c->ast->nodes[nameNode].dataStart, c->ast->nodes[nameNode].dataEnd)
                != 0)
            {
                return -1;
            }
        }
        if (type >= 0) {
            if (H2FmtWriteChar(c, ' ') != 0 || H2FmtEmitType(c, type) != 0) {
                return -1;
            }
        }
        if (init >= 0) {
            if (H2FmtWriteCStr(c, " = ") != 0) {
                return -1;
            }
            if (c->ast->nodes[init].kind == H2Ast_EXPR_LIST) {
                if (H2FmtEmitExprList(c, init) != 0) {
                    return -1;
                }
            } else if (H2FmtEmitExpr(c, init, 0) != 0) {
                return -1;
            }
        }
        return 0;
    }

    {
        const H2AstNode* n = &c->ast->nodes[nodeId];
        type = firstChild;
        if (type >= 0 && !H2FmtIsTypeNodeKind(c->ast->nodes[type].kind)) {
            init = type;
            type = -1;
        } else if (type >= 0) {
            init = H2FmtNextSibling(c->ast, type);
        }
        H2FmtRewriteVarTypeFromLiteralCast(c->ast, c->src, kw, 1u, &type, &init);
        H2FmtRewriteRedundantVarType(c->ast, c->src, kw, 1u, n->start, &type, &init);
        if (H2FmtCanEmitShortVarDecl(c->ast, c->src, kw, 1u, nodeId, type, init)) {
            if (H2FmtWriteSlice(c, n->dataStart, n->dataEnd) != 0 || H2FmtWriteCStr(c, " := ") != 0
                || H2FmtEmitExpr(c, init, 0) != 0)
            {
                return -1;
            }
            return 0;
        }
        if (H2FmtWriteCStr(c, kw) != 0 || H2FmtWriteChar(c, ' ') != 0
            || H2FmtWriteSlice(c, n->dataStart, n->dataEnd) != 0)
        {
            return -1;
        }
        if (type >= 0) {
            if (H2FmtWriteChar(c, ' ') != 0 || H2FmtEmitType(c, type) != 0) {
                return -1;
            }
        }
        if (init >= 0) {
            if (H2FmtWriteCStr(c, " = ") != 0 || H2FmtEmitExpr(c, init, 0) != 0) {
                return -1;
            }
        }
        return 0;
    }
}

static int H2FmtEmitMultiAssign(H2FmtCtx* c, int32_t nodeId) {
    int32_t  lhsList = H2FmtFirstChild(c->ast, nodeId);
    int32_t  rhsList = lhsList >= 0 ? H2FmtNextSibling(c->ast, lhsList) : -1;
    uint32_t i;
    uint32_t lhsCount;
    if (lhsList < 0 || rhsList < 0 || c->ast->nodes[lhsList].kind != H2Ast_EXPR_LIST
        || c->ast->nodes[rhsList].kind != H2Ast_EXPR_LIST)
    {
        return -1;
    }
    lhsCount = H2FmtListCount(c->ast, lhsList);
    for (i = 0; i < lhsCount; i++) {
        int32_t lhsNode = H2FmtListItemAt(c->ast, lhsList, i);
        if (lhsNode < 0) {
            return -1;
        }
        if (i > 0 && H2FmtWriteCStr(c, ", ") != 0) {
            return -1;
        }
        if (H2FmtEmitExpr(c, lhsNode, 0) != 0) {
            return -1;
        }
    }
    if (H2FmtWriteCStr(c, " = ") != 0) {
        return -1;
    }
    if (H2FmtEmitExprList(c, rhsList) != 0) {
        return -1;
    }
    return 0;
}

static int H2FmtEmitShortAssign(H2FmtCtx* c, int32_t nodeId) {
    int32_t  nameList = H2FmtFirstChild(c->ast, nodeId);
    int32_t  rhsList = nameList >= 0 ? H2FmtNextSibling(c->ast, nameList) : -1;
    uint32_t i;
    uint32_t nameCount;
    if (nameList < 0 || rhsList < 0 || c->ast->nodes[nameList].kind != H2Ast_NAME_LIST
        || c->ast->nodes[rhsList].kind != H2Ast_EXPR_LIST)
    {
        return -1;
    }
    nameCount = H2FmtListCount(c->ast, nameList);
    for (i = 0; i < nameCount; i++) {
        int32_t nameNode = H2FmtListItemAt(c->ast, nameList, i);
        if (nameNode < 0) {
            return -1;
        }
        if (i > 0 && H2FmtWriteCStr(c, ", ") != 0) {
            return -1;
        }
        if (H2FmtEmitExpr(c, nameNode, 0) != 0) {
            return -1;
        }
    }
    if (H2FmtWriteCStr(c, " := ") != 0) {
        return -1;
    }
    return H2FmtEmitExprList(c, rhsList);
}

static int H2FmtEmitForHeaderFromSource(
    H2FmtCtx* c, int32_t nodeId, int32_t bodyNode, int32_t* parts, uint32_t partLen) {
    const H2AstNode* n = &c->ast->nodes[nodeId];
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
            if (i > 0 && H2FmtWriteCStr(c, "; ") != 0) {
                return -1;
            }
            if (c->ast->nodes[parts[i]].kind == H2Ast_VAR) {
                if (H2FmtEmitVarLike(c, parts[i], "var") != 0) {
                    return -1;
                }
            } else if (c->ast->nodes[parts[i]].kind == H2Ast_SHORT_ASSIGN) {
                if (H2FmtEmitShortAssign(c, parts[i]) != 0) {
                    return -1;
                }
            } else if (H2FmtEmitExpr(c, parts[i], 0) != 0) {
                return -1;
            }
        }
        while (i < 3u) {
            if (H2FmtWriteCStr(c, ";") != 0) {
                return -1;
            }
            if (i < 2u && H2FmtWriteChar(c, ' ') != 0) {
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
            if (c->ast->nodes[parts[idx]].kind == H2Ast_VAR) {
                if (H2FmtEmitVarLike(c, parts[idx], "var") != 0) {
                    return -1;
                }
            } else if (c->ast->nodes[parts[idx]].kind == H2Ast_SHORT_ASSIGN) {
                if (H2FmtEmitShortAssign(c, parts[idx]) != 0) {
                    return -1;
                }
            } else if (H2FmtEmitExpr(c, parts[idx], 0) != 0) {
                return -1;
            }
            idx++;
        }
        if (H2FmtWriteCStr(c, "; ") != 0) {
            return -1;
        }
        if (seg1Has && idx < partLen) {
            if (H2FmtEmitExpr(c, parts[idx], 0) != 0) {
                return -1;
            }
            idx++;
        }
        if (H2FmtWriteCStr(c, "; ") != 0) {
            return -1;
        }
        if (seg2Has && idx < partLen) {
            if (H2FmtEmitExpr(c, parts[idx], 0) != 0) {
                return -1;
            }
            idx++;
        }
    }
    return 0;
}

static int H2FmtEmitStmtInline(H2FmtCtx* c, int32_t nodeId) {
    const H2AstNode* n;
    int32_t          ch;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return -1;
    }
    n = &c->ast->nodes[nodeId];
    switch (n->kind) {
        case H2Ast_BLOCK: return H2FmtEmitBlock(c, nodeId);
        case H2Ast_VAR:   return H2FmtEmitVarLike(c, nodeId, "var");
        case H2Ast_CONST: return H2FmtEmitVarLike(c, nodeId, "const");
        case H2Ast_CONST_BLOCK:
            ch = H2FmtFirstChild(c->ast, nodeId);
            if (H2FmtWriteCStr(c, "const ") != 0) {
                return -1;
            }
            return ch >= 0 ? H2FmtEmitBlock(c, ch) : 0;
        case H2Ast_MULTI_ASSIGN: return H2FmtEmitMultiAssign(c, nodeId);
        case H2Ast_SHORT_ASSIGN: return H2FmtEmitShortAssign(c, nodeId);
        case H2Ast_IF:           {
            int32_t cond = H2FmtFirstChild(c->ast, nodeId);
            int32_t thenNode = cond >= 0 ? H2FmtNextSibling(c->ast, cond) : -1;
            int32_t elseNode = thenNode >= 0 ? H2FmtNextSibling(c->ast, thenNode) : -1;
            if (H2FmtWriteCStr(c, "if ") != 0 || (cond >= 0 && H2FmtEmitExpr(c, cond, 0) != 0)
                || H2FmtWriteChar(c, ' ') != 0
                || (thenNode >= 0 && H2FmtEmitBlock(c, thenNode) != 0))
            {
                return -1;
            }
            if (elseNode >= 0) {
                if (H2FmtWriteCStr(c, " else ") != 0) {
                    return -1;
                }
                if (c->ast->nodes[elseNode].kind == H2Ast_IF) {
                    if (H2FmtEmitStmtInline(c, elseNode) != 0) {
                        return -1;
                    }
                } else if (H2FmtEmitBlock(c, elseNode) != 0) {
                    return -1;
                }
            }
            return 0;
        }
        case H2Ast_FOR: {
            int32_t  parts[4];
            uint32_t partLen = 0;
            int32_t  cur = H2FmtFirstChild(c->ast, nodeId);
            int32_t  bodyNode;
            while (cur >= 0 && partLen < 4u) {
                parts[partLen++] = cur;
                cur = H2FmtNextSibling(c->ast, cur);
            }
            if (partLen == 0) {
                return H2FmtWriteCStr(c, "for {}");
            }
            bodyNode = parts[partLen - 1u];
            if (H2FmtWriteCStr(c, "for") != 0) {
                return -1;
            }
            if ((n->flags & H2AstFlag_FOR_IN) != 0) {
                int     hasKey = (n->flags & H2AstFlag_FOR_IN_HAS_KEY) != 0;
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
                    if (H2FmtWriteChar(c, ' ') != 0) {
                        return -1;
                    }
                    if ((n->flags & H2AstFlag_FOR_IN_KEY_REF) != 0 && H2FmtWriteChar(c, '&') != 0) {
                        return -1;
                    }
                    if (H2FmtEmitExpr(c, keyNode, 0) != 0 || H2FmtWriteCStr(c, ", ") != 0) {
                        return -1;
                    }
                } else {
                    valueNode = parts[0];
                    sourceNode = parts[1];
                    if (H2FmtWriteChar(c, ' ') != 0) {
                        return -1;
                    }
                }
                if ((n->flags & H2AstFlag_FOR_IN_VALUE_DISCARD) == 0) {
                    if ((n->flags & H2AstFlag_FOR_IN_VALUE_REF) != 0 && H2FmtWriteChar(c, '&') != 0)
                    {
                        return -1;
                    }
                }
                if (H2FmtEmitExpr(c, valueNode, 0) != 0 || H2FmtWriteCStr(c, " in ") != 0
                    || H2FmtEmitExpr(c, sourceNode, 0) != 0 || H2FmtWriteChar(c, ' ') != 0
                    || H2FmtEmitBlock(c, bodyNode) != 0)
                {
                    return -1;
                }
                return 0;
            }
            if (partLen == 1u && c->ast->nodes[bodyNode].kind == H2Ast_BLOCK) {
                if (H2FmtWriteChar(c, ' ') != 0 || H2FmtEmitBlock(c, bodyNode) != 0) {
                    return -1;
                }
                return 0;
            }
            if (H2FmtContainsSemicolonInRange(c->src, n->start, c->ast->nodes[bodyNode].start)) {
                if (H2FmtWriteChar(c, ' ') != 0
                    || H2FmtEmitForHeaderFromSource(c, nodeId, bodyNode, parts, partLen - 1u) != 0
                    || H2FmtWriteChar(c, ' ') != 0 || H2FmtEmitBlock(c, bodyNode) != 0)
                {
                    return -1;
                }
                return 0;
            }
            if (H2FmtWriteChar(c, ' ') != 0 || H2FmtEmitExpr(c, parts[0], 0) != 0
                || H2FmtWriteChar(c, ' ') != 0 || H2FmtEmitBlock(c, bodyNode) != 0)
            {
                return -1;
            }
            return 0;
        }
        case H2Ast_SWITCH: {
            int32_t cur = H2FmtFirstChild(c->ast, nodeId);
            int32_t prevClause = -1;
            if (H2FmtWriteCStr(c, "switch") != 0) {
                return -1;
            }
            if (n->flags == 1 && cur >= 0) {
                if (H2FmtWriteChar(c, ' ') != 0 || H2FmtEmitExpr(c, cur, 0) != 0) {
                    return -1;
                }
                cur = H2FmtNextSibling(c->ast, cur);
            }
            if (H2FmtWriteCStr(c, " {") != 0) {
                return -1;
            }
            if (cur >= 0) {
                if (H2FmtNewline(c) != 0) {
                    return -1;
                }
                c->indent++;
                while (cur >= 0) {
                    int32_t lastClause = cur;
                    int32_t nextClause = H2FmtNextSibling(c->ast, cur);
                    if (prevClause >= 0) {
                        if (H2FmtNewline(c) != 0) {
                            return -1;
                        }
                        if (H2FmtNeedsBlankLineBeforeNode(c, prevClause, cur)
                            && H2FmtNewline(c) != 0)
                        {
                            return -1;
                        }
                    }
                    if (H2FmtEmitSwitchClauseGroup(c, cur, &lastClause, &nextClause) != 0) {
                        return -1;
                    }
                    prevClause = lastClause;
                    cur = nextClause;
                }
                c->indent--;
                if (H2FmtNewline(c) != 0) {
                    return -1;
                }
            }
            return H2FmtWriteChar(c, '}');
        }
        case H2Ast_RETURN:
            ch = H2FmtFirstChild(c->ast, nodeId);
            if (H2FmtWriteCStr(c, "return") != 0) {
                return -1;
            }
            if (ch >= 0) {
                if (H2FmtWriteChar(c, ' ') != 0) {
                    return -1;
                }
                if (c->ast->nodes[ch].kind == H2Ast_EXPR_LIST) {
                    if (H2FmtEmitExprList(c, ch) != 0) {
                        return -1;
                    }
                } else if (H2FmtEmitExpr(c, ch, 0) != 0) {
                    return -1;
                }
            }
            return 0;
        case H2Ast_BREAK:    return H2FmtWriteCStr(c, "break");
        case H2Ast_CONTINUE: return H2FmtWriteCStr(c, "continue");
        case H2Ast_DEFER:
            ch = H2FmtFirstChild(c->ast, nodeId);
            if (H2FmtWriteCStr(c, "defer ") != 0) {
                return -1;
            }
            if (ch >= 0 && c->ast->nodes[ch].kind == H2Ast_BLOCK) {
                int32_t stmtNodeId;
                if (H2FmtCanInlineSingleStmtBlock(c, ch, &stmtNodeId)) {
                    return H2FmtEmitStmtInline(c, stmtNodeId);
                }
            }
            return ch >= 0 ? H2FmtEmitStmtInline(c, ch) : 0;
        case H2Ast_ASSERT:
            ch = H2FmtFirstChild(c->ast, nodeId);
            if (H2FmtWriteCStr(c, "assert ") != 0) {
                return -1;
            }
            while (ch >= 0) {
                int32_t next = H2FmtNextSibling(c->ast, ch);
                if (H2FmtEmitExpr(c, ch, 0) != 0) {
                    return -1;
                }
                if (next >= 0 && H2FmtWriteCStr(c, ", ") != 0) {
                    return -1;
                }
                ch = next;
            }
            return 0;
        case H2Ast_DEL:
            ch = H2FmtFirstChild(c->ast, nodeId);
            if (H2FmtWriteCStr(c, "del ") != 0) {
                return -1;
            }
            if ((n->flags & H2AstFlag_DEL_HAS_ALLOC) != 0) {
                int32_t scan = ch;
                while (scan >= 0) {
                    int32_t next = H2FmtNextSibling(c->ast, scan);
                    if (next < 0) {
                        break;
                    }
                    scan = next;
                }
                while (ch >= 0 && ch != scan) {
                    int32_t next = H2FmtNextSibling(c->ast, ch);
                    if (H2FmtEmitExpr(c, ch, 0) != 0) {
                        return -1;
                    }
                    if (next >= 0 && next != scan && H2FmtWriteCStr(c, ", ") != 0) {
                        return -1;
                    }
                    ch = next;
                }
                if (scan >= 0 && (H2FmtWriteCStr(c, " in ") != 0 || H2FmtEmitExpr(c, scan, 0) != 0))
                {
                    return -1;
                }
                return 0;
            }
            while (ch >= 0) {
                int32_t next = H2FmtNextSibling(c->ast, ch);
                if (H2FmtEmitExpr(c, ch, 0) != 0) {
                    return -1;
                }
                if (next >= 0 && H2FmtWriteCStr(c, ", ") != 0) {
                    return -1;
                }
                ch = next;
            }
            return 0;
        case H2Ast_EXPR_STMT:
            ch = H2FmtFirstChild(c->ast, nodeId);
            return ch >= 0 ? H2FmtEmitExpr(c, ch, 0) : 0;
        default: return H2FmtWriteSlice(c, n->start, n->end);
    }
}

static int H2FmtEmitStmt(H2FmtCtx* c, int32_t nodeId) {
    if (H2FmtEmitLeadingCommentsForNode(c, nodeId) != 0) {
        return -1;
    }
    if (H2FmtEmitStmtInline(c, nodeId) != 0) {
        return -1;
    }
    return H2FmtEmitTrailingCommentsForNode(c, nodeId);
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
} H2FmtImportRow;

static int H2FmtImportParseRow(H2FmtCtx* c, int32_t nodeId, H2FmtImportRow* outRow) {
    const H2AstNode* n;
    int32_t          child;
    int32_t          aliasNodeId = -1;
    int32_t          symStartNodeId = -1;
    uint32_t         symbolsLen = 0;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len || c->ast->nodes[nodeId].kind != H2Ast_IMPORT)
    {
        return -1;
    }
    n = &c->ast->nodes[nodeId];
    child = H2FmtFirstChild(c->ast, nodeId);
    if (child >= 0 && c->ast->nodes[child].kind == H2Ast_IDENT) {
        aliasNodeId = child;
        child = H2FmtNextSibling(c->ast, child);
    }
    symStartNodeId = child;

    if (symStartNodeId >= 0) {
        int32_t sym = symStartNodeId;
        symbolsLen = 4u;
        while (sym >= 0) {
            const H2AstNode* sn = &c->ast->nodes[sym];
            int32_t          salias = H2FmtFirstChild(c->ast, sym);
            int32_t          next = H2FmtNextSibling(c->ast, sym);
            symbolsLen += sn->dataEnd - sn->dataStart;
            if (salias >= 0) {
                const H2AstNode* an = &c->ast->nodes[salias];
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
        const H2AstNode* an = &c->ast->nodes[aliasNodeId];
        outRow->aliasLen = 4u + (an->dataEnd - an->dataStart);
    }
    outRow->symbolsLen = symbolsLen;
    outRow->hasSymbols = (uint8_t)(symStartNodeId >= 0);
    outRow->headLen = 7u + outRow->pathLen + outRow->aliasLen;
    outRow->hasTrailingComment = (uint8_t)H2FmtHasUnusedTrailingCommentsInNodeRange(c, nodeId);
    return 0;
}

static int H2FmtCompareNodePathText(H2FmtCtx* c, int32_t aNodeId, int32_t bNodeId) {
    const H2AstNode* a = &c->ast->nodes[aNodeId];
    const H2AstNode* b = &c->ast->nodes[bNodeId];
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

static int H2FmtCompareImportRows(H2FmtCtx* c, const H2FmtImportRow* a, const H2FmtImportRow* b) {
    int cmp = H2FmtCompareNodePathText(c, a->nodeId, b->nodeId);
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
        cmp = H2FmtCompareNodePathText(c, a->aliasNodeId, b->aliasNodeId);
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

static void H2FmtSortImportRows(H2FmtCtx* c, H2FmtImportRow* rows, uint32_t len) {
    uint32_t i;
    for (i = 1; i < len; i++) {
        H2FmtImportRow key = rows[i];
        uint32_t       j = i;
        while (j > 0 && H2FmtCompareImportRows(c, &key, &rows[j - 1u]) < 0) {
            rows[j] = rows[j - 1u];
            j--;
        }
        rows[j] = key;
    }
}

static int H2FmtEmitImportSymbolsInline(H2FmtCtx* c, int32_t symStartNodeId) {
    int32_t sym = symStartNodeId;
    if (H2FmtWriteCStr(c, "{ ") != 0) {
        return -1;
    }
    while (sym >= 0) {
        const H2AstNode* sn = &c->ast->nodes[sym];
        int32_t          salias = H2FmtFirstChild(c->ast, sym);
        int32_t          next = H2FmtNextSibling(c->ast, sym);
        if (H2FmtWriteSlice(c, sn->dataStart, sn->dataEnd) != 0) {
            return -1;
        }
        if (salias >= 0) {
            const H2AstNode* an = &c->ast->nodes[salias];
            if (H2FmtWriteCStr(c, " as ") != 0
                || H2FmtWriteSlice(c, an->dataStart, an->dataEnd) != 0)
            {
                return -1;
            }
        }
        if (next >= 0 && H2FmtWriteCStr(c, ", ") != 0) {
            return -1;
        }
        sym = next;
    }
    return H2FmtWriteCStr(c, " }");
}

static int H2FmtCanContinueImportGroup(H2FmtCtx* c, int32_t prevNodeId, int32_t nextNodeId) {
    uint32_t gapNl;
    if (prevNodeId < 0 || nextNodeId < 0 || c->ast->nodes[nextNodeId].kind != H2Ast_IMPORT) {
        return 0;
    }
    gapNl = H2FmtCountNewlinesInRange(
        c->src, c->ast->nodes[prevNodeId].end, c->ast->nodes[nextNodeId].start);
    if (gapNl > 1u) {
        return 0;
    }
    if (H2FmtHasUnusedLeadingCommentsForNode(c, nextNodeId)) {
        return 0;
    }
    return 1;
}

static int H2FmtEmitImportGroup(
    H2FmtCtx* c, int32_t firstNodeId, int32_t* outLastSourceNodeId, int32_t* outNextNodeId) {
    int32_t         cur = firstNodeId;
    int32_t         prev = -1;
    uint32_t        count = 0;
    uint32_t        i;
    H2FmtImportRow* rows;
    uint32_t*       commentRunMaxLens;
    uint32_t*       braceRunMaxHeadLens;

    while (cur >= 0 && c->ast->nodes[cur].kind == H2Ast_IMPORT) {
        int32_t next = H2FmtNextSibling(c->ast, cur);
        if (prev >= 0 && !H2FmtCanContinueImportGroup(c, prev, cur)) {
            break;
        }
        count++;
        prev = cur;
        cur = next;
    }

    rows = (H2FmtImportRow*)H2ArenaAlloc(
        c->out.arena, count * (uint32_t)sizeof(H2FmtImportRow), (uint32_t)_Alignof(H2FmtImportRow));
    if (rows == NULL) {
        return -1;
    }
    cur = firstNodeId;
    for (i = 0; i < count; i++) {
        int32_t next = H2FmtNextSibling(c->ast, cur);
        if (H2FmtImportParseRow(c, cur, &rows[i]) != 0) {
            return -1;
        }
        cur = next;
    }

    H2FmtSortImportRows(c, rows, count);

    braceRunMaxHeadLens = (uint32_t*)H2ArenaAlloc(
        c->out.arena, count * (uint32_t)sizeof(uint32_t), (uint32_t)_Alignof(uint32_t));
    if (braceRunMaxHeadLens == NULL) {
        return -1;
    }
    memset(braceRunMaxHeadLens, 0, count * (uint32_t)sizeof(uint32_t));

    commentRunMaxLens = (uint32_t*)H2ArenaAlloc(
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
        const H2AstNode* n = &c->ast->nodes[rows[i].nodeId];
        uint32_t         lineLen = rows[i].headLen;
        int32_t          nodeId = rows[i].nodeId;
        if (H2FmtEmitLeadingCommentsForNode(c, rows[i].nodeId) != 0) {
            return -1;
        }
        if (H2FmtWriteCStr(c, "import ") != 0 || H2FmtWriteSlice(c, n->dataStart, n->dataEnd) != 0)
        {
            return -1;
        }
        if (rows[i].aliasNodeId >= 0) {
            const H2AstNode* an = &c->ast->nodes[rows[i].aliasNodeId];
            if (H2FmtWriteCStr(c, " as ") != 0
                || H2FmtWriteSlice(c, an->dataStart, an->dataEnd) != 0)
            {
                return -1;
            }
        }
        if (rows[i].hasSymbols) {
            uint32_t padBeforeBrace = (braceRunMaxHeadLens[i] - rows[i].headLen) + 1u;
            if (H2FmtWriteSpaces(c, padBeforeBrace) != 0
                || H2FmtEmitImportSymbolsInline(c, rows[i].symStartNodeId) != 0)
            {
                return -1;
            }
            lineLen += padBeforeBrace + rows[i].symbolsLen;
        }
        if (rows[i].hasTrailingComment) {
            uint32_t padComment = (commentRunMaxLens[i] - lineLen) + 1u;
            if (H2FmtEmitTrailingCommentsInNodeRange(c, nodeId, padComment) != 0) {
                return -1;
            }
        }
        if (i + 1u < count && H2FmtNewline(c) != 0) {
            return -1;
        }
    }

    *outLastSourceNodeId = prev;
    *outNextNodeId = cur;
    return 0;
}

static int H2FmtEmitImport(H2FmtCtx* c, int32_t nodeId) {
    H2FmtImportRow   row;
    const H2AstNode* n;
    uint32_t         lineLen;
    int32_t          id = nodeId;
    if (H2FmtImportParseRow(c, nodeId, &row) != 0) {
        return -1;
    }
    n = &c->ast->nodes[nodeId];
    lineLen = row.headLen;
    if (H2FmtWriteCStr(c, "import ") != 0 || H2FmtWriteSlice(c, n->dataStart, n->dataEnd) != 0) {
        return -1;
    }
    if (row.aliasNodeId >= 0) {
        const H2AstNode* an = &c->ast->nodes[row.aliasNodeId];
        if (H2FmtWriteCStr(c, " as ") != 0 || H2FmtWriteSlice(c, an->dataStart, an->dataEnd) != 0) {
            return -1;
        }
    }
    if (row.hasSymbols) {
        if (H2FmtWriteChar(c, ' ') != 0 || H2FmtEmitImportSymbolsInline(c, row.symStartNodeId) != 0)
        {
            return -1;
        }
        lineLen += 1u + row.symbolsLen;
    }
    if (row.hasTrailingComment) {
        if (H2FmtEmitTrailingCommentsInNodeRange(c, id, 1u) != 0) {
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
} H2FmtAlignedFieldRow;

typedef struct {
    int32_t  nodeId;
    int32_t  valueNodeId;
    uint32_t nameLen;
    uint32_t valueLen;
    uint32_t codeLen;
    uint8_t  hasValue;
    uint8_t  hasTrailingComment;
    uint8_t  _pad[2];
} H2FmtAlignedEnumRow;

static int H2FmtFieldTypesMatch(H2FmtCtx* c, int32_t aTypeNodeId, int32_t bTypeNodeId) {
    const H2AstNode* a;
    const H2AstNode* b;
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

static int H2FmtIsAnonAggregateTypeNode(H2FmtCtx* c, int32_t typeNodeId) {
    H2AstKind kind;
    if (typeNodeId < 0 || (uint32_t)typeNodeId >= c->ast->len) {
        return 0;
    }
    kind = c->ast->nodes[typeNodeId].kind;
    return kind == H2Ast_TYPE_ANON_STRUCT || kind == H2Ast_TYPE_ANON_UNION;
}

static int H2FmtCanMergeFieldNames(H2FmtCtx* c, int32_t leftFieldNodeId, int32_t rightFieldNodeId) {
    const H2AstNode* left = &c->ast->nodes[leftFieldNodeId];
    const H2AstNode* right = &c->ast->nodes[rightFieldNodeId];
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

static uint32_t H2FmtMergedFieldNameLen(
    H2FmtCtx* c, int32_t firstFieldNodeId, int32_t lastFieldNodeId) {
    uint32_t len = 0;
    int32_t  cur = firstFieldNodeId;
    while (cur >= 0) {
        const H2AstNode* n = &c->ast->nodes[cur];
        int32_t          next = H2FmtNextSibling(c->ast, cur);
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

static int H2FmtEmitMergedFieldNames(
    H2FmtCtx* c, int32_t firstFieldNodeId, int32_t lastFieldNodeId) {
    int32_t cur = firstFieldNodeId;
    while (cur >= 0) {
        const H2AstNode* n = &c->ast->nodes[cur];
        int32_t          next = H2FmtNextSibling(c->ast, cur);
        if (H2FmtWriteSlice(c, n->dataStart, n->dataEnd) != 0) {
            return -1;
        }
        if (cur == lastFieldNodeId) {
            break;
        }
        if (H2FmtWriteCStr(c, ", ") != 0) {
            return -1;
        }
        cur = next;
    }
    return 0;
}

static int H2FmtBuildFieldRow(
    H2FmtCtx* c, int32_t startFieldNodeId, H2FmtAlignedFieldRow* outRow, int32_t* outNextNodeId) {
    int32_t          typeNodeId;
    int32_t          defaultNodeId;
    const H2AstNode* startNode;
    int32_t          lastNodeId;

    if (startFieldNodeId < 0 || (uint32_t)startFieldNodeId >= c->ast->len) {
        return -1;
    }
    startNode = &c->ast->nodes[startFieldNodeId];
    if (startNode->kind != H2Ast_FIELD) {
        return -1;
    }

    typeNodeId = H2FmtFirstChild(c->ast, startFieldNodeId);
    defaultNodeId = typeNodeId >= 0 ? H2FmtNextSibling(c->ast, typeNodeId) : -1;
    lastNodeId = startFieldNodeId;

    if ((startNode->flags & H2AstFlag_FIELD_EMBEDDED) == 0 && defaultNodeId < 0) {
        int32_t next = H2FmtNextSibling(c->ast, lastNodeId);
        while (next >= 0 && c->ast->nodes[next].kind == H2Ast_FIELD) {
            const H2AstNode* nn = &c->ast->nodes[next];
            int32_t          nextType = H2FmtFirstChild(c->ast, next);
            int32_t          nextDefault = nextType >= 0 ? H2FmtNextSibling(c->ast, nextType) : -1;
            if ((nn->flags & H2AstFlag_FIELD_EMBEDDED) != 0 || nextDefault >= 0) {
                break;
            }
            if (!H2FmtFieldTypesMatch(c, typeNodeId, nextType)) {
                break;
            }
            if (!H2FmtCanMergeFieldNames(c, lastNodeId, next)) {
                break;
            }
            lastNodeId = next;
            next = H2FmtNextSibling(c->ast, next);
        }
    }

    memset(outRow, 0, sizeof(*outRow));
    outRow->firstNodeId = startFieldNodeId;
    outRow->lastNodeId = lastNodeId;
    outRow->typeNodeId = typeNodeId;
    outRow->defaultNodeId = defaultNodeId;
    outRow->hasDefault = (uint8_t)(defaultNodeId >= 0);
    outRow->nameLen = H2FmtMergedFieldNameLen(c, startFieldNodeId, lastNodeId);
    if (typeNodeId >= 0 && H2FmtMeasureTypeLen(c, typeNodeId, &outRow->typeLen) != 0) {
        return -1;
    }
    if (defaultNodeId >= 0 && H2FmtMeasureExprLen(c, defaultNodeId, 0, &outRow->defaultLen) != 0) {
        return -1;
    }
    outRow->hasTrailingComment = (uint8_t)H2FmtHasUnusedTrailingCommentsForNodes(
        c, &startFieldNodeId, 1u);
    *outNextNodeId = H2FmtNextSibling(c->ast, lastNodeId);
    return 0;
}

static int H2FmtEmitSimpleFieldDecl(H2FmtCtx* c, int32_t fieldNodeId) {
    const H2AstNode* fn = &c->ast->nodes[fieldNodeId];
    int32_t          typeNode = H2FmtFirstChild(c->ast, fieldNodeId);
    int32_t          defaultNode = typeNode >= 0 ? H2FmtNextSibling(c->ast, typeNode) : -1;
    int32_t          nodeIds[1];
    int              hasTrailing;
    if ((fn->flags & H2AstFlag_FIELD_EMBEDDED) != 0) {
        if (typeNode >= 0 && H2FmtEmitType(c, typeNode) != 0) {
            return -1;
        }
    } else if (
        H2FmtWriteSlice(c, fn->dataStart, fn->dataEnd) != 0 || H2FmtWriteChar(c, ' ') != 0
        || (typeNode >= 0 && H2FmtEmitType(c, typeNode) != 0))
    {
        return -1;
    }
    if (defaultNode >= 0) {
        if (H2FmtWriteCStr(c, " = ") != 0 || H2FmtEmitExpr(c, defaultNode, 0) != 0) {
            return -1;
        }
    }
    nodeIds[0] = fieldNodeId;
    hasTrailing = H2FmtHasUnusedTrailingCommentsForNodes(c, nodeIds, 1u);
    if (hasTrailing) {
        return H2FmtEmitTrailingCommentsForNodes(c, nodeIds, 1u, 1u);
    }
    {
        uint32_t cmStart;
        uint32_t cmEnd;
        if (H2FmtFindSourceTrailingLineComment(c, fieldNodeId, &cmStart, &cmEnd)) {
            if (H2FmtWriteChar(c, ' ') != 0 || H2FmtWriteSlice(c, cmStart, cmEnd) != 0) {
                return -1;
            }
            H2FmtMarkCommentUsedAtStart(c, cmStart);
        }
    }
    return 0;
}

static int H2FmtEmitAlignedFieldGroup(
    H2FmtCtx* c, int32_t firstFieldNodeId, int32_t* outLastNodeId, int32_t* outNextNodeId) {
    int32_t               cur = firstFieldNodeId;
    int32_t               prev = -1;
    int                   prevInlineAnon = 0;
    uint32_t              count = 0;
    uint32_t              i;
    uint32_t              maxNameLen = 0;
    uint32_t              maxBeforeOpLen = 0;
    H2FmtAlignedFieldRow* rows;
    uint32_t*             commentRunMaxLens;

    while (cur >= 0) {
        H2FmtAlignedFieldRow row;
        int32_t              next;
        int                  curInlineAnon;
        if (c->ast->nodes[cur].kind != H2Ast_FIELD) {
            break;
        }
        if ((c->ast->nodes[cur].flags & H2AstFlag_FIELD_EMBEDDED) != 0) {
            break;
        }
        if (H2FmtBuildFieldRow(c, cur, &row, &next) != 0) {
            return -1;
        }
        curInlineAnon = H2FmtIsInlineAnonAggregateType(c, row.typeNodeId);
        if (prev >= 0) {
            if (!H2FmtCanContinueAlignedGroup(c, prev, row.firstNodeId)) {
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
        *outNextNodeId = H2FmtNextSibling(c->ast, firstFieldNodeId);
        return 0;
    }

    rows = (H2FmtAlignedFieldRow*)H2ArenaAlloc(
        c->out.arena,
        count * (uint32_t)sizeof(H2FmtAlignedFieldRow),
        (uint32_t)_Alignof(H2FmtAlignedFieldRow));
    if (rows == NULL) {
        return -1;
    }

    cur = firstFieldNodeId;
    for (i = 0; i < count; i++) {
        if (H2FmtBuildFieldRow(c, cur, &rows[i], &cur) != 0) {
            return -1;
        }
        if (rows[i].nameLen > maxNameLen) {
            maxNameLen = rows[i].nameLen;
        }
    }

    for (i = 0; i < count; i++) {
        int isAnon = H2FmtIsAnonAggregateTypeNode(c, rows[i].typeNodeId);
        int prevAnon = i > 0u && H2FmtIsAnonAggregateTypeNode(c, rows[i - 1u].typeNodeId);
        int nextAnon = i + 1u < count && H2FmtIsAnonAggregateTypeNode(c, rows[i + 1u].typeNodeId);
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

    commentRunMaxLens = (uint32_t*)H2ArenaAlloc(
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
        if (H2FmtEmitLeadingCommentsForNode(c, rows[i].firstNodeId) != 0) {
            return -1;
        }
        if (H2FmtEmitMergedFieldNames(c, rows[i].firstNodeId, rows[i].lastNodeId) != 0) {
            return -1;
        }
        lineLen = rows[i].nameLen;
        if (rows[i].noTypeAlign) {
            if (H2FmtWriteChar(c, ' ') != 0) {
                return -1;
            }
        } else {
            if (H2FmtWriteSpaces(c, maxNameLen - rows[i].nameLen + 1u) != 0) {
                return -1;
            }
            lineLen = maxNameLen + 1u;
        }
        if (rows[i].typeNodeId >= 0 && H2FmtEmitType(c, rows[i].typeNodeId) != 0) {
            return -1;
        }
        lineLen += rows[i].typeLen;
        if (rows[i].hasDefault) {
            if (rows[i].noTypeAlign) {
                if (H2FmtWriteCStr(c, " = ") != 0
                    || H2FmtEmitExpr(c, rows[i].defaultNodeId, 0) != 0)
                {
                    return -1;
                }
                lineLen += 3u + rows[i].defaultLen;
            } else {
                uint32_t padBeforeOp = (maxBeforeOpLen + 1u) - lineLen;
                if (H2FmtWriteSpaces(c, padBeforeOp) != 0 || H2FmtWriteChar(c, '=') != 0
                    || H2FmtWriteChar(c, ' ') != 0
                    || H2FmtEmitExpr(c, rows[i].defaultNodeId, 0) != 0)
                {
                    return -1;
                }
                lineLen += padBeforeOp + 1u + 1u + rows[i].defaultLen;
            }
        }
        if (rows[i].hasTrailingComment) {
            uint32_t padComment = (commentRunMaxLens[i] - lineLen) + 1u;
            nodeIds[0] = rows[i].firstNodeId;
            if (H2FmtEmitTrailingCommentsForNodes(c, nodeIds, 1u, padComment) != 0) {
                return -1;
            }
        } else {
            uint32_t cmStart;
            uint32_t cmEnd;
            if (H2FmtFindSourceTrailingLineComment(c, rows[i].lastNodeId, &cmStart, &cmEnd)) {
                if (H2FmtWriteChar(c, ' ') != 0 || H2FmtWriteSlice(c, cmStart, cmEnd) != 0) {
                    return -1;
                }
                H2FmtMarkCommentUsedAtStart(c, cmStart);
            }
        }
        if (i + 1u < count && H2FmtNewline(c) != 0) {
            return -1;
        }
    }

    *outLastNodeId = rows[count - 1u].lastNodeId;
    *outNextNodeId = cur;
    return 0;
}

static int H2FmtEmitAlignedEnumGroup(
    H2FmtCtx* c, int32_t firstFieldNodeId, int32_t* outLastNodeId, int32_t* outNextNodeId) {
    int32_t              cur = firstFieldNodeId;
    int32_t              prev = -1;
    uint32_t             count = 0;
    uint32_t             i;
    uint32_t             maxNameLenForValues = 0;
    H2FmtAlignedEnumRow* rows;
    uint32_t*            commentRunMaxLens;

    while (cur >= 0) {
        int32_t next = H2FmtNextSibling(c->ast, cur);
        if (c->ast->nodes[cur].kind != H2Ast_FIELD) {
            break;
        }
        if (prev >= 0 && !H2FmtCanContinueAlignedGroup(c, prev, cur)) {
            break;
        }
        count++;
        prev = cur;
        cur = next;
    }

    rows = (H2FmtAlignedEnumRow*)H2ArenaAlloc(
        c->out.arena,
        count * (uint32_t)sizeof(H2FmtAlignedEnumRow),
        (uint32_t)_Alignof(H2FmtAlignedEnumRow));
    if (rows == NULL) {
        return -1;
    }
    memset(rows, 0, count * (uint32_t)sizeof(H2FmtAlignedEnumRow));

    cur = firstFieldNodeId;
    for (i = 0; i < count; i++) {
        const H2AstNode* n = &c->ast->nodes[cur];
        int32_t          valueNode = H2FmtFirstChild(c->ast, cur);
        rows[i].nodeId = cur;
        rows[i].nameLen = n->dataEnd - n->dataStart;
        while (valueNode >= 0 && c->ast->nodes[valueNode].kind == H2Ast_FIELD) {
            valueNode = H2FmtNextSibling(c->ast, valueNode);
        }
        rows[i].valueNodeId = valueNode;
        rows[i].hasValue = (uint8_t)(rows[i].valueNodeId >= 0);
        if (rows[i].hasValue
            && H2FmtMeasureExprLen(c, rows[i].valueNodeId, 0, &rows[i].valueLen) != 0)
        {
            return -1;
        }
        rows[i].hasTrailingComment = (uint8_t)H2FmtHasUnusedTrailingCommentsForNodes(c, &cur, 1u);
        if (rows[i].hasValue && rows[i].nameLen > maxNameLenForValues) {
            maxNameLenForValues = rows[i].nameLen;
        }
        cur = H2FmtNextSibling(c->ast, cur);
    }

    commentRunMaxLens = (uint32_t*)H2ArenaAlloc(
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
        const H2AstNode* n = &c->ast->nodes[rows[i].nodeId];
        if (H2FmtEmitLeadingCommentsForNode(c, rows[i].nodeId) != 0) {
            return -1;
        }
        if (H2FmtWriteSlice(c, n->dataStart, n->dataEnd) != 0) {
            return -1;
        }
        if (rows[i].hasValue) {
            uint32_t padBeforeOp = (maxNameLenForValues + 1u) - rows[i].nameLen;
            if (H2FmtWriteSpaces(c, padBeforeOp) != 0 || H2FmtWriteChar(c, '=') != 0
                || H2FmtWriteChar(c, ' ') != 0 || H2FmtEmitExpr(c, rows[i].valueNodeId, 0) != 0)
            {
                return -1;
            }
            lineLen += padBeforeOp + 1u + 1u + rows[i].valueLen;
        }
        if (rows[i].hasTrailingComment) {
            uint32_t padComment = (commentRunMaxLens[i] - lineLen) + 1u;
            nodeIds[0] = rows[i].nodeId;
            if (H2FmtEmitTrailingCommentsForNodes(c, nodeIds, 1u, padComment) != 0) {
                return -1;
            }
        }
        if (i + 1u < count && H2FmtNewline(c) != 0) {
            return -1;
        }
    }

    *outLastNodeId = rows[count - 1u].nodeId;
    *outNextNodeId = cur;
    return 0;
}

static int H2FmtIsNestedTypeDeclNodeKind(H2AstKind kind) {
    return kind == H2Ast_STRUCT || kind == H2Ast_UNION || kind == H2Ast_ENUM
        || kind == H2Ast_TYPE_ALIAS;
}

static int H2FmtEmitAggregateFieldBody(H2FmtCtx* c, int32_t firstFieldNodeId) {
    int32_t child = firstFieldNodeId;
    int32_t prevEmitted = -1;

    if (child < 0) {
        return 0;
    }
    if (H2FmtNewline(c) != 0) {
        return -1;
    }
    c->indent++;
    while (child >= 0) {
        H2AstKind kind = c->ast->nodes[child].kind;
        if (kind != H2Ast_FIELD && !H2FmtIsNestedTypeDeclNodeKind(kind)) {
            break;
        }
        if (prevEmitted >= 0) {
            if (H2FmtNewline(c) != 0) {
                return -1;
            }
            if (H2FmtNeedsBlankLineBeforeNode(c, prevEmitted, child) && H2FmtNewline(c) != 0) {
                return -1;
            }
        }
        if (kind == H2Ast_FIELD) {
            int32_t next = H2FmtNextSibling(c->ast, child);
            int32_t last = child;
            if ((c->ast->nodes[child].flags & H2AstFlag_FIELD_EMBEDDED) != 0) {
                if (H2FmtEmitLeadingCommentsForNode(c, child) != 0
                    || H2FmtEmitSimpleFieldDecl(c, child) != 0)
                {
                    return -1;
                }
            } else if (H2FmtEmitAlignedFieldGroup(c, child, &last, &next) != 0) {
                return -1;
            }
            prevEmitted = last;
            child = next;
            continue;
        }
        if (H2FmtEmitDecl(c, child) != 0) {
            return -1;
        }
        prevEmitted = child;
        child = H2FmtNextSibling(c->ast, child);
    }
    c->indent--;
    if (H2FmtNewline(c) != 0) {
        return -1;
    }
    return 0;
}

static int H2FmtEmitAggregateDecl(H2FmtCtx* c, int32_t nodeId, const char* kw) {
    const H2AstNode* n = &c->ast->nodes[nodeId];
    int32_t          child = H2FmtFirstChild(c->ast, nodeId);
    int32_t          underType = -1;
    int32_t          prevEmitted = -1;
    while (child >= 0 && c->ast->nodes[child].kind == H2Ast_TYPE_PARAM) {
        child = H2FmtNextSibling(c->ast, child);
    }
    if (n->kind == H2Ast_ENUM && child >= 0 && H2FmtIsTypeNodeKind(c->ast->nodes[child].kind)) {
        underType = child;
        child = H2FmtNextSibling(c->ast, child);
    }

    if ((n->flags & H2AstFlag_PUB) != 0 && H2FmtWriteCStr(c, "pub ") != 0) {
        return -1;
    }
    if (H2FmtWriteCStr(c, kw) != 0 || H2FmtWriteChar(c, ' ') != 0
        || H2FmtWriteSlice(c, n->dataStart, n->dataEnd) != 0)
    {
        return -1;
    }
    child = H2FmtFirstChild(c->ast, nodeId);
    if (H2FmtEmitTypeParamList(c, &child) != 0) {
        return -1;
    }
    if (underType >= 0) {
        if (H2FmtWriteChar(c, ' ') != 0 || H2FmtEmitType(c, underType) != 0) {
            return -1;
        }
        child = H2FmtNextSibling(c->ast, underType);
    }
    if (H2FmtWriteChar(c, ' ') != 0 || H2FmtWriteChar(c, '{') != 0) {
        return -1;
    }

    if (child >= 0) {
        if (n->kind == H2Ast_ENUM) {
            int     enumHasPayload = 0;
            int32_t scanItem = child;
            while (scanItem >= 0) {
                int32_t vch = H2FmtFirstChild(c->ast, scanItem);
                while (vch >= 0) {
                    if (c->ast->nodes[vch].kind == H2Ast_FIELD) {
                        enumHasPayload = 1;
                        break;
                    }
                    vch = H2FmtNextSibling(c->ast, vch);
                }
                if (enumHasPayload) {
                    break;
                }
                scanItem = H2FmtNextSibling(c->ast, scanItem);
            }
            if (H2FmtNewline(c) != 0) {
                return -1;
            }
            c->indent++;
            if (!enumHasPayload) {
                while (child >= 0) {
                    int32_t next = H2FmtNextSibling(c->ast, child);
                    int32_t last = child;
                    if (prevEmitted >= 0) {
                        if (H2FmtNewline(c) != 0) {
                            return -1;
                        }
                        if (H2FmtNeedsBlankLineBeforeNode(c, prevEmitted, child)
                            && H2FmtNewline(c) != 0)
                        {
                            return -1;
                        }
                    }
                    if (H2FmtEmitAlignedEnumGroup(c, child, &last, &next) != 0) {
                        return -1;
                    }
                    prevEmitted = last;
                    child = next;
                }
            } else {
                while (child >= 0) {
                    int32_t          next = H2FmtNextSibling(c->ast, child);
                    const H2AstNode* item = &c->ast->nodes[child];
                    int32_t          payloadFirst = -1;
                    int32_t          tagExpr = -1;
                    int32_t          ch = H2FmtFirstChild(c->ast, child);
                    if (prevEmitted >= 0) {
                        if (H2FmtNewline(c) != 0) {
                            return -1;
                        }
                        if (H2FmtNeedsBlankLineBeforeNode(c, prevEmitted, child)
                            && H2FmtNewline(c) != 0)
                        {
                            return -1;
                        }
                    }
                    if (H2FmtEmitLeadingCommentsForNode(c, child) != 0
                        || H2FmtWriteSlice(c, item->dataStart, item->dataEnd) != 0)
                    {
                        return -1;
                    }
                    while (ch >= 0) {
                        if (c->ast->nodes[ch].kind == H2Ast_FIELD) {
                            if (payloadFirst < 0) {
                                payloadFirst = ch;
                            }
                        } else if (tagExpr < 0) {
                            tagExpr = ch;
                        }
                        ch = H2FmtNextSibling(c->ast, ch);
                    }
                    if (payloadFirst >= 0) {
                        if (H2FmtWriteChar(c, '{') != 0
                            || H2FmtEmitAggregateFieldBody(c, payloadFirst) != 0
                            || H2FmtWriteChar(c, '}') != 0)
                        {
                            return -1;
                        }
                    }
                    if (tagExpr >= 0) {
                        if (H2FmtWriteCStr(c, " = ") != 0 || H2FmtEmitExpr(c, tagExpr, 0) != 0) {
                            return -1;
                        }
                    }
                    if (H2FmtEmitTrailingCommentsForNode(c, child) != 0) {
                        return -1;
                    }
                    prevEmitted = child;
                    child = next;
                }
            }
            c->indent--;
            if (H2FmtNewline(c) != 0) {
                return -1;
            }
        } else if (H2FmtEmitAggregateFieldBody(c, child) != 0) {
            return -1;
        }
    }

    if (H2FmtWriteChar(c, '}') != 0) {
        return -1;
    }
    return H2FmtEmitTrailingCommentsForNode(c, nodeId);
}

static int H2FmtEmitFnDecl(H2FmtCtx* c, int32_t nodeId) {
    const H2AstNode* n = &c->ast->nodes[nodeId];
    int32_t          child;
    int              firstParam = 1;
    int32_t          retType = -1;
    int32_t          ctxClause = -1;
    int32_t          body = -1;

    if ((n->flags & H2AstFlag_PUB) != 0 && H2FmtWriteCStr(c, "pub ") != 0) {
        return -1;
    }
    if (H2FmtWriteCStr(c, "fn ") != 0 || H2FmtWriteSlice(c, n->dataStart, n->dataEnd) != 0) {
        return -1;
    }

    child = H2FmtFirstChild(c->ast, nodeId);
    if (H2FmtEmitTypeParamList(c, &child) != 0) {
        return -1;
    }
    if (H2FmtWriteChar(c, '(') != 0) {
        return -1;
    }
    while (child >= 0) {
        const H2AstNode* ch = &c->ast->nodes[child];
        int32_t          runFirst = child;
        int32_t          runLast = child;
        int32_t          runType = -1;
        uint16_t         runFlags;
        int32_t          nextAfterRun;
        int              firstNameInRun = 1;
        if (ch->kind != H2Ast_PARAM) {
            break;
        }
        runType = H2FmtFirstChild(c->ast, child);
        runFlags = (uint16_t)(ch->flags & (H2AstFlag_PARAM_CONST | H2AstFlag_PARAM_VARIADIC));
        nextAfterRun = H2FmtNextSibling(c->ast, child);
        while (nextAfterRun >= 0) {
            const H2AstNode* nextParam = &c->ast->nodes[nextAfterRun];
            int32_t          nextType;
            uint16_t         nextFlags;
            if (nextParam->kind != H2Ast_PARAM) {
                break;
            }
            nextType = H2FmtFirstChild(c->ast, nextAfterRun);
            nextFlags =
                (uint16_t)(nextParam->flags & (H2AstFlag_PARAM_CONST | H2AstFlag_PARAM_VARIADIC));
            if (nextFlags != runFlags
                || !H2FmtTypeNodesEqualBySource(c->ast, c->src, runType, nextType))
            {
                break;
            }
            runLast = nextAfterRun;
            nextAfterRun = H2FmtNextSibling(c->ast, nextAfterRun);
        }
        if (!firstParam && H2FmtWriteCStr(c, ", ") != 0) {
            return -1;
        }
        if ((runFlags & H2AstFlag_PARAM_CONST) != 0 && H2FmtWriteCStr(c, "const ") != 0) {
            return -1;
        }
        while (runFirst >= 0) {
            const H2AstNode* p = &c->ast->nodes[runFirst];
            if (!firstNameInRun && H2FmtWriteCStr(c, ", ") != 0) {
                return -1;
            }
            if (H2FmtWriteSlice(c, p->dataStart, p->dataEnd) != 0) {
                return -1;
            }
            firstNameInRun = 0;
            if (runFirst == runLast) {
                break;
            }
            runFirst = H2FmtNextSibling(c->ast, runFirst);
        }
        if (H2FmtWriteChar(c, ' ') != 0) {
            return -1;
        }
        if ((runFlags & H2AstFlag_PARAM_VARIADIC) != 0 && H2FmtWriteCStr(c, "...") != 0) {
            return -1;
        }
        if (runType >= 0 && H2FmtEmitType(c, runType) != 0) {
            return -1;
        }
        firstParam = 0;
        child = nextAfterRun;
    }

    if (H2FmtWriteChar(c, ')') != 0) {
        return -1;
    }

    while (child >= 0) {
        const H2AstNode* ch = &c->ast->nodes[child];
        if (ch->kind == H2Ast_CONTEXT_CLAUSE) {
            ctxClause = child;
        } else if (ch->kind == H2Ast_BLOCK) {
            body = child;
        } else if (H2FmtIsTypeNodeKind(ch->kind) && ch->flags == 1) {
            retType = child;
        }
        child = H2FmtNextSibling(c->ast, child);
    }

    if (retType >= 0) {
        if (H2FmtWriteChar(c, ' ') != 0 || H2FmtEmitType(c, retType) != 0) {
            return -1;
        }
    }
    if (ctxClause >= 0) {
        int32_t ctype = H2FmtFirstChild(c->ast, ctxClause);
        if (H2FmtWriteCStr(c, " context ") != 0) {
            return -1;
        }
        if (ctype >= 0 && H2FmtEmitType(c, ctype) != 0) {
            return -1;
        }
    }
    if (body >= 0) {
        if (H2FmtWriteChar(c, ' ') != 0 || H2FmtEmitBlock(c, body) != 0) {
            return -1;
        }
    }
    return H2FmtEmitTrailingCommentsForNode(c, nodeId);
}

static int H2FmtEmitDirective(H2FmtCtx* c, int32_t nodeId) {
    const H2AstNode* n = &c->ast->nodes[nodeId];
    int32_t          child = H2FmtFirstChild(c->ast, nodeId);
    int              first = 1;
    if (H2FmtWriteChar(c, '@') != 0 || H2FmtWriteSlice(c, n->dataStart, n->dataEnd) != 0) {
        return -1;
    }
    if (child >= 0 || H2FmtSliceHasChar(c->src, n->dataEnd, n->end, '(')) {
        if (H2FmtWriteChar(c, '(') != 0) {
            return -1;
        }
        while (child >= 0) {
            int32_t next = H2FmtNextSibling(c->ast, child);
            if (!first && H2FmtWriteCStr(c, ", ") != 0) {
                return -1;
            }
            if (H2FmtEmitExpr(c, child, 0) != 0) {
                return -1;
            }
            first = 0;
            child = next;
        }
        if (H2FmtWriteChar(c, ')') != 0) {
            return -1;
        }
    }
    return H2FmtEmitTrailingCommentsForNode(c, nodeId);
}

static int H2FmtEmitDecl(H2FmtCtx* c, int32_t nodeId) {
    const H2AstNode* n = &c->ast->nodes[nodeId];
    if (H2FmtEmitLeadingCommentsForNode(c, nodeId) != 0) {
        return -1;
    }
    switch (n->kind) {
        case H2Ast_IMPORT:     return H2FmtEmitImport(c, nodeId);
        case H2Ast_DIRECTIVE:  return H2FmtEmitDirective(c, nodeId);
        case H2Ast_STRUCT:     return H2FmtEmitAggregateDecl(c, nodeId, "struct");
        case H2Ast_UNION:      return H2FmtEmitAggregateDecl(c, nodeId, "union");
        case H2Ast_ENUM:       return H2FmtEmitAggregateDecl(c, nodeId, "enum");
        case H2Ast_TYPE_ALIAS: {
            int32_t type = H2FmtFirstChild(c->ast, nodeId);
            if ((n->flags & H2AstFlag_PUB) != 0 && H2FmtWriteCStr(c, "pub ") != 0) {
                return -1;
            }
            if (H2FmtWriteCStr(c, "type ") != 0
                || H2FmtWriteSlice(c, n->dataStart, n->dataEnd) != 0)
            {
                return -1;
            }
            if (H2FmtEmitTypeParamList(c, &type) != 0) {
                return -1;
            }
            if (H2FmtWriteChar(c, ' ') != 0 || (type >= 0 && H2FmtEmitType(c, type) != 0)) {
                return -1;
            }
            return H2FmtEmitTrailingCommentsForNode(c, nodeId);
        }
        case H2Ast_FN: return H2FmtEmitFnDecl(c, nodeId);
        case H2Ast_VAR:
            if ((n->flags & H2AstFlag_PUB) != 0 && H2FmtWriteCStr(c, "pub ") != 0) {
                return -1;
            }
            if (H2FmtEmitVarLike(c, nodeId, "var") != 0) {
                return -1;
            }
            return H2FmtEmitTrailingCommentsForNode(c, nodeId);
        case H2Ast_CONST:
            if ((n->flags & H2AstFlag_PUB) != 0 && H2FmtWriteCStr(c, "pub ") != 0) {
                return -1;
            }
            if (H2FmtEmitVarLike(c, nodeId, "const") != 0) {
                return -1;
            }
            return H2FmtEmitTrailingCommentsForNode(c, nodeId);
        default:
            if (H2FmtWriteSlice(c, n->start, n->end) != 0) {
                return -1;
            }
            return H2FmtEmitTrailingCommentsForNode(c, nodeId);
    }
}

static int H2FmtEmitDirectiveGroup(
    H2FmtCtx* c, int32_t firstDirective, int32_t* outLast, int32_t* outNext) {
    int32_t child = firstDirective;
    int32_t last = -1;
    int32_t next = -1;
    int     first = 1;
    while (child >= 0 && c->ast->nodes[child].kind == H2Ast_DIRECTIVE) {
        next = H2FmtNextSibling(c->ast, child);
        if (!first && H2FmtNewline(c) != 0) {
            return -1;
        }
        if (H2FmtEmitDecl(c, child) != 0) {
            return -1;
        }
        last = child;
        first = 0;
        child = next;
    }
    if (child >= 0) {
        if (!first && H2FmtNewline(c) != 0) {
            return -1;
        }
        if (H2FmtEmitDecl(c, child) != 0) {
            return -1;
        }
        last = child;
        next = H2FmtNextSibling(c->ast, child);
    }
    *outLast = last;
    *outNext = next;
    return 0;
}

static int H2FmtIsTopLevelVarOrConstNode(H2FmtCtx* c, int32_t nodeId) {
    H2AstKind kind;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return 0;
    }
    kind = c->ast->nodes[nodeId].kind;
    return kind == H2Ast_VAR || kind == H2Ast_CONST;
}

static int H2FmtIsSimpleTopLevelAlignedVarOrConstNode(H2FmtCtx* c, int32_t nodeId) {
    if (!H2FmtIsTopLevelVarOrConstNode(c, nodeId)) {
        return 0;
    }
    if ((c->ast->nodes[nodeId].flags & H2AstFlag_PUB) != 0) {
        return 0;
    }
    return !H2FmtIsGroupedVarLike(c, nodeId);
}

static int H2FmtNeedsTopLevelBlankLineBetween(H2FmtCtx* c, int32_t prevNodeId, int32_t nextNodeId) {
    if (H2FmtIsTopLevelVarOrConstNode(c, prevNodeId)
        && H2FmtIsTopLevelVarOrConstNode(c, nextNodeId))
    {
        return H2FmtNeedsBlankLineBeforeNode(c, prevNodeId, nextNodeId);
    }
    return 1;
}

static int H2FmtEmitFile(H2FmtCtx* c) {
    int32_t              child = H2FmtFirstChild(c->ast, c->ast->root);
    int32_t              prevEmitted = -1;
    H2FmtAssignCarryHint carryHint;
    memset(&carryHint, 0, sizeof(carryHint));
    while (child >= 0) {
        int32_t last = child;
        int32_t next = H2FmtNextSibling(c->ast, child);
        if (prevEmitted >= 0) {
            if (H2FmtNewline(c) != 0) {
                return -1;
            }
            if (H2FmtNeedsTopLevelBlankLineBetween(c, prevEmitted, child) && H2FmtNewline(c) != 0) {
                return -1;
            }
        }
        if (c->ast->nodes[child].kind == H2Ast_IMPORT) {
            int32_t lastSourceNode = child;
            if (H2FmtEmitImportGroup(c, child, &lastSourceNode, &next) != 0) {
                return -1;
            }
            last = lastSourceNode;
        } else if (c->ast->nodes[child].kind == H2Ast_DIRECTIVE) {
            if (H2FmtEmitDirectiveGroup(c, child, &last, &next) != 0) {
                return -1;
            }
        } else if (H2FmtIsSimpleTopLevelAlignedVarOrConstNode(c, child)) {
            const char* kw = c->ast->nodes[child].kind == H2Ast_CONST ? "const" : "var";
            if (H2FmtEmitAlignedVarOrConstGroup(c, child, kw, 1, &last, &next, &carryHint) != 0) {
                return -1;
            }
        } else if (H2FmtEmitDecl(c, child) != 0) {
            return -1;
        }
        prevEmitted = last;
        child = next;
    }
    if (prevEmitted >= 0 && H2FmtNewline(c) != 0) {
        return -1;
    }
    if (H2FmtEmitRemainingComments(c) != 0) {
        return -1;
    }
    if (c->out.len == 0 || c->out.v[c->out.len - 1u] != '\n') {
        if (H2FmtNewline(c) != 0) {
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

int H2Format(
    H2Arena*  arena,
    H2StrView src,
    const H2FormatOptions* _Nullable options,
    H2StrView* out,
    H2Diag* _Nullable diag) {
    H2Ast          ast;
    H2ParseExtras  extras;
    H2ParseOptions parseOptions;
    H2FmtCtx       c;
    uint32_t       indentWidth = 4u;

    if (diag != NULL) {
        *diag = (H2Diag){ 0 };
    }
    if (out == NULL) {
        return -1;
    }
    out->ptr = NULL;
    out->len = 0;

    if (options != NULL && options->indentWidth != 0) {
        indentWidth = options->indentWidth;
    }

    parseOptions.flags = H2ParseFlag_COLLECT_FORMATTING;
    if (H2Parse(arena, src, &parseOptions, &ast, &extras, diag) != 0) {
        return -1;
    }
    if (H2FmtRewriteAst(&ast, src, options) != 0) {
        if (diag != NULL) {
            diag->code = H2Diag_ARENA_OOM;
            diag->type = H2DiagTypeOfCode(H2Diag_ARENA_OOM);
            diag->start = 0;
            diag->end = 0;
            diag->argStart = 0;
            diag->argEnd = 0;
            diag->argText = NULL;
            diag->argTextLen = 0;
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
        c.commentUsed = (uint8_t*)H2ArenaAlloc(
            arena, c.commentLen * (uint32_t)sizeof(uint8_t), (uint32_t)_Alignof(uint8_t));
        if (c.commentUsed == NULL) {
            if (diag != NULL) {
                diag->code = H2Diag_ARENA_OOM;
                diag->type = H2DiagTypeOfCode(H2Diag_ARENA_OOM);
                diag->start = 0;
                diag->end = 0;
                diag->argStart = 0;
                diag->argEnd = 0;
                diag->argText = NULL;
                diag->argTextLen = 0;
                diag->relatedStart = 0;
                diag->relatedEnd = 0;
                diag->detail = NULL;
                diag->hintOverride = NULL;
            }
            return -1;
        }
        memset(c.commentUsed, 0, c.commentLen);
    }

    if (H2FmtEmitFile(&c) != 0) {
        if (diag != NULL && diag->code == H2Diag_NONE) {
            diag->code = H2Diag_ARENA_OOM;
            diag->type = H2DiagTypeOfCode(H2Diag_ARENA_OOM);
            diag->start = 0;
            diag->end = 0;
            diag->argStart = 0;
            diag->argEnd = 0;
            diag->argText = NULL;
            diag->argTextLen = 0;
            diag->relatedStart = 0;
            diag->relatedEnd = 0;
            diag->detail = NULL;
            diag->hintOverride = NULL;
        }
        return -1;
    }

    if (H2FmtBufAppendChar(&c.out, '\0') != 0) {
        if (diag != NULL) {
            diag->code = H2Diag_ARENA_OOM;
            diag->type = H2DiagTypeOfCode(H2Diag_ARENA_OOM);
            diag->start = 0;
            diag->end = 0;
            diag->argStart = 0;
            diag->argEnd = 0;
            diag->argText = NULL;
            diag->argTextLen = 0;
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

H2_API_END
