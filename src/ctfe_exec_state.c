#include "libhop-impl.h"
#include "ctfe_exec.h"

HOP_API_BEGIN

static int HOPCTFEExecNameEqSlice(
    HOPStrView src, uint32_t aStart, uint32_t aEnd, uint32_t bStart, uint32_t bEnd) {
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

void HOPCTFEExecResetReason(HOPCTFEExecCtx* c) {
    if (c == NULL) {
        return;
    }
    c->nonConstReason = NULL;
    c->nonConstStart = 0;
    c->nonConstEnd = 0;
}

void HOPCTFEExecSetReason(HOPCTFEExecCtx* c, uint32_t start, uint32_t end, const char* reason) {
    if (c == NULL || reason == NULL || reason[0] == '\0' || c->nonConstReason != NULL) {
        return;
    }
    c->nonConstReason = reason;
    c->nonConstStart = start;
    c->nonConstEnd = end;
}

void HOPCTFEExecSetReasonNode(HOPCTFEExecCtx* c, int32_t nodeId, const char* reason) {
    if (c == NULL || c->ast == NULL || nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        HOPCTFEExecSetReason(c, 0, 0, reason);
        return;
    }
    HOPCTFEExecSetReason(c, c->ast->nodes[nodeId].start, c->ast->nodes[nodeId].end, reason);
}

int HOPCTFEExecEnvLookup(
    const HOPCTFEExecCtx* c, uint32_t nameStart, uint32_t nameEnd, HOPCTFEValue* outValue) {
    const HOPCTFEExecEnv* frame;
    if (c == NULL || outValue == NULL) {
        return 0;
    }
    frame = c->env;
    while (frame != NULL) {
        uint32_t i = frame->bindingLen;
        while (i > 0) {
            const HOPCTFEExecBinding* b;
            i--;
            b = &frame->bindings[i];
            if (HOPCTFEExecNameEqSlice(c->src, b->nameStart, b->nameEnd, nameStart, nameEnd)) {
                *outValue = b->value;
                return 1;
            }
        }
        frame = frame->parent;
    }
    return 0;
}

HOP_API_END
