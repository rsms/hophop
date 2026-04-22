#include "internal.h"

HOP_API_BEGIN

int HOPTypeCheckEx(
    HOPArena*     arena,
    const HOPAst* ast,
    HOPStrView    src,
    const HOPTypeCheckOptions* _Nullable options,
    HOPDiag* _Nullable diag) {
    return HOPTCBuildCheckedContext(arena, ast, src, options, diag, NULL);
}

int HOPTypeCheck(HOPArena* arena, const HOPAst* ast, HOPStrView src, HOPDiag* _Nullable diag) {
    return HOPTypeCheckEx(arena, ast, src, NULL, diag);
}

int HOPConstEvalSessionInit(
    HOPArena*             arena,
    const HOPAst*         ast,
    HOPStrView            src,
    HOPConstEvalSession** outSession,
    HOPDiag* _Nullable diag) {
    HOPConstEvalSession* session;
    uint32_t*            savedFlags = NULL;
    uint32_t             i;
    if (outSession == NULL) {
        return -1;
    }
    *outSession = NULL;
    if (arena == NULL || ast == NULL || ast->nodes == NULL) {
        HOPTCSetDiag(diag, HOPDiag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }
    session = (HOPConstEvalSession*)HOPArenaAlloc(
        arena, (uint32_t)sizeof(*session), (uint32_t)_Alignof(HOPConstEvalSession));
    if (session == NULL) {
        HOPTCSetDiag(diag, HOPDiag_ARENA_OOM, 0, 0);
        return -1;
    }
    if (ast->len > 0) {
        savedFlags = (uint32_t*)HOPArenaAlloc(
            arena, (uint32_t)sizeof(uint32_t) * ast->len, (uint32_t)_Alignof(uint32_t));
        if (savedFlags == NULL) {
            HOPTCSetDiag(diag, HOPDiag_ARENA_OOM, 0, 0);
            return -1;
        }
        for (i = 0; i < ast->len; i++) {
            savedFlags[i] = ast->nodes[i].flags;
        }
    }
    if (HOPTCBuildCheckedContext(arena, ast, src, NULL, diag, &session->tc) != 0) {
        if (savedFlags != NULL) {
            for (i = 0; i < ast->len; i++) {
                ((HOPAstNode*)&ast->nodes[i])->flags = savedFlags[i];
            }
        }
        return -1;
    }
    if (savedFlags != NULL) {
        for (i = 0; i < ast->len; i++) {
            ((HOPAstNode*)&ast->nodes[i])->flags = savedFlags[i];
        }
    }
    *outSession = session;
    return 0;
}

int HOPConstEvalSessionEvalExpr(
    HOPConstEvalSession* session, int32_t exprNode, HOPCTFEValue* outValue, int* outIsConst) {
    HOPTypeCheckCtx*  c;
    HOPTCConstEvalCtx evalCtx;
    if (session == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = &session->tc;
    c->lastConstEvalReason = NULL;
    c->lastConstEvalReasonStart = 0;
    c->lastConstEvalReasonEnd = 0;
    memset(&evalCtx, 0, sizeof(evalCtx));
    evalCtx.tc = c;
    if (HOPTCEvalConstExprNode(&evalCtx, exprNode, outValue, outIsConst) != 0) {
        return -1;
    }
    c->lastConstEvalReason = evalCtx.nonConstReason;
    c->lastConstEvalReasonStart = evalCtx.nonConstStart;
    c->lastConstEvalReasonEnd = evalCtx.nonConstEnd;
    return 0;
}

int HOPConstEvalSessionEvalIntExpr(
    HOPConstEvalSession* session, int32_t exprNode, int64_t* outValue, int* outIsConst) {
    if (session == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    return HOPTCConstIntExpr(&session->tc, exprNode, outValue, outIsConst);
}

int HOPConstEvalSessionEvalTopLevelConst(
    HOPConstEvalSession* session, int32_t constNode, HOPCTFEValue* outValue, int* outIsConst) {
    HOPTypeCheckCtx*  c;
    HOPTCConstEvalCtx evalCtx;
    if (session == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = &session->tc;
    if (constNode < 0 || (uint32_t)constNode >= c->ast->len
        || c->ast->nodes[constNode].kind != HOPAst_CONST)
    {
        *outIsConst = 0;
        return 0;
    }
    c->lastConstEvalReason = NULL;
    c->lastConstEvalReasonStart = 0;
    c->lastConstEvalReasonEnd = 0;
    memset(&evalCtx, 0, sizeof(evalCtx));
    evalCtx.tc = c;
    if (HOPTCEvalTopLevelConstNode(c, &evalCtx, constNode, outValue, outIsConst) != 0) {
        return -1;
    }
    c->lastConstEvalReason = evalCtx.nonConstReason;
    c->lastConstEvalReasonStart = evalCtx.nonConstStart;
    c->lastConstEvalReasonEnd = evalCtx.nonConstEnd;
    return 0;
}

int HOPConstEvalSessionDecodeTypeTag(
    HOPConstEvalSession* session, uint64_t typeTag, int32_t* outTypeId) {
    if (session == NULL || outTypeId == NULL) {
        return -1;
    }
    return HOPTCDecodeTypeTag(&session->tc, typeTag, outTypeId);
}

int HOPConstEvalSessionGetTypeInfo(
    HOPConstEvalSession* session, int32_t typeId, HOPConstEvalTypeInfo* outTypeInfo) {
    const HOPTCType* t;
    if (session == NULL || outTypeInfo == NULL) {
        return -1;
    }
    if (typeId < 0 || (uint32_t)typeId >= session->tc.typeLen) {
        return -1;
    }
    t = &session->tc.types[typeId];
    memset(outTypeInfo, 0, sizeof(*outTypeInfo));
    outTypeInfo->kind = (HOPConstEvalTypeKind)t->kind;
    outTypeInfo->builtin = (HOPConstEvalBuiltinKind)t->builtin;
    outTypeInfo->baseTypeId = t->baseType;
    outTypeInfo->declNode = t->declNode;
    outTypeInfo->arrayLen = t->arrayLen;
    outTypeInfo->nameStart = t->nameStart;
    outTypeInfo->nameEnd = t->nameEnd;
    outTypeInfo->flags = t->flags;
    return 0;
}

HOP_API_END
