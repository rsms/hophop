#include "libhop-impl.h"
#include "ctfe_exec.h"

H2_API_BEGIN

static int H2CTFEExecNameEqSlice(
    H2StrView src, uint32_t aStart, uint32_t aEnd, uint32_t bStart, uint32_t bEnd) {
    uint32_t len;
    if (aEnd < aStart || bEnd < bStart || aEnd > src.len || bEnd > src.len) {
        return 0;
    }
    len = aEnd - aStart;
    if (len != bEnd - bStart) {
        return 0;
    }
    return len == 0 || memcmp(src.ptr + aStart, src.ptr + bStart, len) == 0;
}

void H2CTFEExecResetReason(H2CTFEExecCtx* c) {
    if (c == NULL) {
        return;
    }
    c->nonConstReason = NULL;
    c->nonConstStart = 0;
    c->nonConstEnd = 0;
}

void H2CTFEExecSetReason(H2CTFEExecCtx* c, uint32_t start, uint32_t end, const char* reason) {
    if (c == NULL || reason == NULL || reason[0] == '\0' || c->nonConstReason != NULL) {
        return;
    }
    c->nonConstReason = reason;
    c->nonConstStart = start;
    c->nonConstEnd = end;
}

void H2CTFEExecSetReasonNode(H2CTFEExecCtx* c, int32_t nodeId, const char* reason) {
    if (c == NULL || c->ast == NULL || nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        H2CTFEExecSetReason(c, 0, 0, reason);
        return;
    }
    H2CTFEExecSetReason(c, c->ast->nodes[nodeId].start, c->ast->nodes[nodeId].end, reason);
}

int H2CTFEExecEnvLookup(
    const H2CTFEExecCtx* c, uint32_t nameStart, uint32_t nameEnd, H2CTFEValue* outValue) {
    const H2CTFEExecEnv* frame;
    if (c == NULL || outValue == NULL) {
        return 0;
    }
    frame = c->env;
    while (frame != NULL) {
        uint32_t i = frame->bindingLen;
        while (i > 0) {
            const H2CTFEExecBinding* b;
            i--;
            b = &frame->bindings[i];
            if (H2CTFEExecNameEqSlice(c->src, b->nameStart, b->nameEnd, nameStart, nameEnd)) {
                *outValue = b->value;
                return 1;
            }
        }
        frame = frame->parent;
    }
    return 0;
}

H2_API_END
