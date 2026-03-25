#include "libsl-impl.h"
#include "ctfe_exec.h"

SL_API_BEGIN

static int SLCTFEExecNameEqSlice(
    SLStrView src, uint32_t aStart, uint32_t aEnd, uint32_t bStart, uint32_t bEnd) {
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

void SLCTFEExecResetReason(SLCTFEExecCtx* c) {
    if (c == NULL) {
        return;
    }
    c->nonConstReason = NULL;
    c->nonConstStart = 0;
    c->nonConstEnd = 0;
}

void SLCTFEExecSetReason(SLCTFEExecCtx* c, uint32_t start, uint32_t end, const char* reason) {
    if (c == NULL || reason == NULL || reason[0] == '\0' || c->nonConstReason != NULL) {
        return;
    }
    c->nonConstReason = reason;
    c->nonConstStart = start;
    c->nonConstEnd = end;
}

void SLCTFEExecSetReasonNode(SLCTFEExecCtx* c, int32_t nodeId, const char* reason) {
    if (c == NULL || c->ast == NULL || nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        SLCTFEExecSetReason(c, 0, 0, reason);
        return;
    }
    SLCTFEExecSetReason(c, c->ast->nodes[nodeId].start, c->ast->nodes[nodeId].end, reason);
}

int SLCTFEExecEnvLookup(
    const SLCTFEExecCtx* c, uint32_t nameStart, uint32_t nameEnd, SLCTFEValue* outValue) {
    const SLCTFEExecEnv* frame;
    if (c == NULL || outValue == NULL) {
        return 0;
    }
    frame = c->env;
    while (frame != NULL) {
        uint32_t i = frame->bindingLen;
        while (i > 0) {
            const SLCTFEExecBinding* b;
            i--;
            b = &frame->bindings[i];
            if (SLCTFEExecNameEqSlice(c->src, b->nameStart, b->nameEnd, nameStart, nameEnd)) {
                *outValue = b->value;
                return 1;
            }
        }
        frame = frame->parent;
    }
    return 0;
}

SL_API_END
