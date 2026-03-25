#include "internal.h"

SL_API_BEGIN

int SLTypeCheckEx(
    SLArena*     arena,
    const SLAst* ast,
    SLStrView    src,
    const SLTypeCheckOptions* _Nullable options,
    SLDiag* _Nullable diag) {
    return SLTCBuildCheckedContext(arena, ast, src, options, diag, NULL);
}

int SLTypeCheck(SLArena* arena, const SLAst* ast, SLStrView src, SLDiag* _Nullable diag) {
    return SLTypeCheckEx(arena, ast, src, NULL, diag);
}

int SLConstEvalSessionInit(
    SLArena*             arena,
    const SLAst*         ast,
    SLStrView            src,
    SLConstEvalSession** outSession,
    SLDiag* _Nullable diag) {
    SLConstEvalSession* session;
    uint32_t*           savedFlags = NULL;
    uint32_t            i;
    if (outSession == NULL) {
        return -1;
    }
    *outSession = NULL;
    if (arena == NULL || ast == NULL || ast->nodes == NULL) {
        SLTCSetDiag(diag, SLDiag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }
    session = (SLConstEvalSession*)SLArenaAlloc(
        arena, (uint32_t)sizeof(*session), (uint32_t)_Alignof(SLConstEvalSession));
    if (session == NULL) {
        SLTCSetDiag(diag, SLDiag_ARENA_OOM, 0, 0);
        return -1;
    }
    if (ast->len > 0) {
        savedFlags = (uint32_t*)SLArenaAlloc(
            arena, (uint32_t)sizeof(uint32_t) * ast->len, (uint32_t)_Alignof(uint32_t));
        if (savedFlags == NULL) {
            SLTCSetDiag(diag, SLDiag_ARENA_OOM, 0, 0);
            return -1;
        }
        for (i = 0; i < ast->len; i++) {
            savedFlags[i] = ast->nodes[i].flags;
        }
    }
    if (SLTCBuildCheckedContext(arena, ast, src, NULL, diag, &session->tc) != 0) {
        if (savedFlags != NULL) {
            for (i = 0; i < ast->len; i++) {
                ((SLAstNode*)&ast->nodes[i])->flags = savedFlags[i];
            }
        }
        return -1;
    }
    if (savedFlags != NULL) {
        for (i = 0; i < ast->len; i++) {
            ((SLAstNode*)&ast->nodes[i])->flags = savedFlags[i];
        }
    }
    *outSession = session;
    return 0;
}

int SLConstEvalSessionEvalExpr(
    SLConstEvalSession* session, int32_t exprNode, SLCTFEValue* outValue, int* outIsConst) {
    SLTypeCheckCtx*  c;
    SLTCConstEvalCtx evalCtx;
    if (session == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = &session->tc;
    c->lastConstEvalReason = NULL;
    c->lastConstEvalReasonStart = 0;
    c->lastConstEvalReasonEnd = 0;
    memset(&evalCtx, 0, sizeof(evalCtx));
    evalCtx.tc = c;
    if (SLTCEvalConstExprNode(&evalCtx, exprNode, outValue, outIsConst) != 0) {
        return -1;
    }
    c->lastConstEvalReason = evalCtx.nonConstReason;
    c->lastConstEvalReasonStart = evalCtx.nonConstStart;
    c->lastConstEvalReasonEnd = evalCtx.nonConstEnd;
    return 0;
}

int SLConstEvalSessionEvalIntExpr(
    SLConstEvalSession* session, int32_t exprNode, int64_t* outValue, int* outIsConst) {
    if (session == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    return SLTCConstIntExpr(&session->tc, exprNode, outValue, outIsConst);
}

int SLConstEvalSessionEvalTopLevelConst(
    SLConstEvalSession* session, int32_t constNode, SLCTFEValue* outValue, int* outIsConst) {
    SLTypeCheckCtx*  c;
    SLTCConstEvalCtx evalCtx;
    if (session == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = &session->tc;
    if (constNode < 0 || (uint32_t)constNode >= c->ast->len
        || c->ast->nodes[constNode].kind != SLAst_CONST)
    {
        *outIsConst = 0;
        return 0;
    }
    c->lastConstEvalReason = NULL;
    c->lastConstEvalReasonStart = 0;
    c->lastConstEvalReasonEnd = 0;
    memset(&evalCtx, 0, sizeof(evalCtx));
    evalCtx.tc = c;
    if (SLTCEvalTopLevelConstNode(c, &evalCtx, constNode, outValue, outIsConst) != 0) {
        return -1;
    }
    c->lastConstEvalReason = evalCtx.nonConstReason;
    c->lastConstEvalReasonStart = evalCtx.nonConstStart;
    c->lastConstEvalReasonEnd = evalCtx.nonConstEnd;
    return 0;
}

int SLConstEvalSessionDecodeTypeTag(
    SLConstEvalSession* session, uint64_t typeTag, int32_t* outTypeId) {
    if (session == NULL || outTypeId == NULL) {
        return -1;
    }
    return SLTCDecodeTypeTag(&session->tc, typeTag, outTypeId);
}

int SLConstEvalSessionGetTypeInfo(
    SLConstEvalSession* session, int32_t typeId, SLConstEvalTypeInfo* outTypeInfo) {
    const SLTCType* t;
    if (session == NULL || outTypeInfo == NULL) {
        return -1;
    }
    if (typeId < 0 || (uint32_t)typeId >= session->tc.typeLen) {
        return -1;
    }
    t = &session->tc.types[typeId];
    memset(outTypeInfo, 0, sizeof(*outTypeInfo));
    outTypeInfo->kind = (SLConstEvalTypeKind)t->kind;
    outTypeInfo->builtin = (SLConstEvalBuiltinKind)t->builtin;
    outTypeInfo->baseTypeId = t->baseType;
    outTypeInfo->declNode = t->declNode;
    outTypeInfo->arrayLen = t->arrayLen;
    outTypeInfo->nameStart = t->nameStart;
    outTypeInfo->nameEnd = t->nameEnd;
    outTypeInfo->flags = t->flags;
    return 0;
}

SL_API_END
