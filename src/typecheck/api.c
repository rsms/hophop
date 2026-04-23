#include "internal.h"

H2_API_BEGIN

int H2TypeCheckEx(
    H2Arena*     arena,
    const H2Ast* ast,
    H2StrView    src,
    const H2TypeCheckOptions* _Nullable options,
    H2Diag* _Nullable diag) {
    return H2TCBuildCheckedContext(arena, ast, src, options, diag, NULL);
}

int H2TypeCheck(H2Arena* arena, const H2Ast* ast, H2StrView src, H2Diag* _Nullable diag) {
    return H2TypeCheckEx(arena, ast, src, NULL, diag);
}

int H2ConstEvalSessionInit(
    H2Arena*             arena,
    const H2Ast*         ast,
    H2StrView            src,
    H2ConstEvalSession** outSession,
    H2Diag* _Nullable diag) {
    H2ConstEvalSession* session;
    uint32_t*           savedFlags = NULL;
    uint32_t            i;
    if (outSession == NULL) {
        return -1;
    }
    *outSession = NULL;
    if (arena == NULL || ast == NULL || ast->nodes == NULL) {
        H2TCSetDiag(diag, H2Diag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }
    session = (H2ConstEvalSession*)H2ArenaAlloc(
        arena, (uint32_t)sizeof(*session), (uint32_t)_Alignof(H2ConstEvalSession));
    if (session == NULL) {
        H2TCSetDiag(diag, H2Diag_ARENA_OOM, 0, 0);
        return -1;
    }
    if (ast->len > 0) {
        savedFlags = (uint32_t*)H2ArenaAlloc(
            arena, (uint32_t)sizeof(uint32_t) * ast->len, (uint32_t)_Alignof(uint32_t));
        if (savedFlags == NULL) {
            H2TCSetDiag(diag, H2Diag_ARENA_OOM, 0, 0);
            return -1;
        }
        for (i = 0; i < ast->len; i++) {
            savedFlags[i] = ast->nodes[i].flags;
        }
    }
    if (H2TCBuildCheckedContext(arena, ast, src, NULL, diag, &session->tc) != 0) {
        if (savedFlags != NULL) {
            for (i = 0; i < ast->len; i++) {
                ((H2AstNode*)&ast->nodes[i])->flags = savedFlags[i];
            }
        }
        return -1;
    }
    if (savedFlags != NULL) {
        for (i = 0; i < ast->len; i++) {
            ((H2AstNode*)&ast->nodes[i])->flags = savedFlags[i];
        }
    }
    *outSession = session;
    return 0;
}

int H2ConstEvalSessionEvalExpr(
    H2ConstEvalSession* session, int32_t exprNode, H2CTFEValue* outValue, int* outIsConst) {
    H2TypeCheckCtx*  c;
    H2TCConstEvalCtx evalCtx;
    if (session == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = &session->tc;
    c->lastConstEvalReason = NULL;
    c->lastConstEvalReasonStart = 0;
    c->lastConstEvalReasonEnd = 0;
    memset(&evalCtx, 0, sizeof(evalCtx));
    evalCtx.tc = c;
    if (H2TCEvalConstExprNode(&evalCtx, exprNode, outValue, outIsConst) != 0) {
        return -1;
    }
    c->lastConstEvalReason = evalCtx.nonConstReason;
    c->lastConstEvalReasonStart = evalCtx.nonConstStart;
    c->lastConstEvalReasonEnd = evalCtx.nonConstEnd;
    return 0;
}

int H2ConstEvalSessionEvalIntExpr(
    H2ConstEvalSession* session, int32_t exprNode, int64_t* outValue, int* outIsConst) {
    if (session == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    return H2TCConstIntExpr(&session->tc, exprNode, outValue, outIsConst);
}

int H2ConstEvalSessionEvalTopLevelConst(
    H2ConstEvalSession* session, int32_t constNode, H2CTFEValue* outValue, int* outIsConst) {
    H2TypeCheckCtx*  c;
    H2TCConstEvalCtx evalCtx;
    if (session == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = &session->tc;
    if (constNode < 0 || (uint32_t)constNode >= c->ast->len
        || c->ast->nodes[constNode].kind != H2Ast_CONST)
    {
        *outIsConst = 0;
        return 0;
    }
    c->lastConstEvalReason = NULL;
    c->lastConstEvalReasonStart = 0;
    c->lastConstEvalReasonEnd = 0;
    memset(&evalCtx, 0, sizeof(evalCtx));
    evalCtx.tc = c;
    if (H2TCEvalTopLevelConstNode(c, &evalCtx, constNode, outValue, outIsConst) != 0) {
        return -1;
    }
    c->lastConstEvalReason = evalCtx.nonConstReason;
    c->lastConstEvalReasonStart = evalCtx.nonConstStart;
    c->lastConstEvalReasonEnd = evalCtx.nonConstEnd;
    return 0;
}

int H2ConstEvalSessionDecodeTypeTag(
    H2ConstEvalSession* session, uint64_t typeTag, int32_t* outTypeId) {
    if (session == NULL || outTypeId == NULL) {
        return -1;
    }
    return H2TCDecodeTypeTag(&session->tc, typeTag, outTypeId);
}

int H2ConstEvalSessionGetTypeInfo(
    H2ConstEvalSession* session, int32_t typeId, H2ConstEvalTypeInfo* outTypeInfo) {
    const H2TCType* t;
    if (session == NULL || outTypeInfo == NULL) {
        return -1;
    }
    if (typeId < 0 || (uint32_t)typeId >= session->tc.typeLen) {
        return -1;
    }
    t = &session->tc.types[typeId];
    memset(outTypeInfo, 0, sizeof(*outTypeInfo));
    outTypeInfo->kind = (H2ConstEvalTypeKind)t->kind;
    outTypeInfo->builtin = (H2ConstEvalBuiltinKind)t->builtin;
    outTypeInfo->baseTypeId = t->baseType;
    outTypeInfo->declNode = t->declNode;
    outTypeInfo->arrayLen = t->arrayLen;
    outTypeInfo->nameStart = t->nameStart;
    outTypeInfo->nameEnd = t->nameEnd;
    outTypeInfo->flags = t->flags;
    return 0;
}

H2_API_END
