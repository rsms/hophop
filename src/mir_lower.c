#include "libsl-impl.h"
#include "mir.h"
#include "mir_lower.h"

SL_API_BEGIN

int SLMirLowerExprAsFunction(
    SLArena* _Nonnull arena,
    const SLAst* _Nonnull ast,
    SLStrView src,
    int32_t   nodeId,
    SLMirProgram* _Nonnull outProgram,
    int* _Nonnull outSupported,
    SLDiag* _Nullable diag) {
    SLMirChunk          chunk;
    SLMirFunction       function = { 0 };
    SLMirProgramBuilder builder;
    uint32_t            functionIndex = 0;
    uint32_t            i;

    if (outProgram != NULL) {
        *outProgram = (SLMirProgram){ 0 };
    }
    if (arena == NULL || ast == NULL || outProgram == NULL || outSupported == NULL) {
        if (diag != NULL) {
            diag->code = SLDiag_UNEXPECTED_TOKEN;
            diag->type = SLDiagTypeOfCode(diag->code);
            diag->start = 0;
            diag->end = 0;
            diag->argStart = 0;
            diag->argEnd = 0;
        }
        return -1;
    }

    *outSupported = 0;
    if (SLMirBuildExpr(arena, ast, src, nodeId, &chunk, outSupported, diag) != 0) {
        return -1;
    }
    if (!*outSupported) {
        return 0;
    }

    SLMirProgramBuilderInit(&builder, arena);
    function.instStart = 0;
    function.instLen = 0;
    function.paramCount = 0;
    function.localCount = 0;
    function.tempCount = 0;
    function.typeRef = 0;
    function.nameStart = 0;
    function.nameEnd = 0;
    if (SLMirProgramBuilderBeginFunction(&builder, &function, &functionIndex) != 0) {
        if (diag != NULL) {
            diag->code = SLDiag_ARENA_OOM;
            diag->type = SLDiagTypeOfCode(diag->code);
            diag->start = 0;
            diag->end = 0;
            diag->argStart = 0;
            diag->argEnd = 0;
        }
        return -1;
    }
    for (i = 0; i < chunk.len; i++) {
        if (SLMirProgramBuilderAppendInst(&builder, &chunk.v[i]) != 0) {
            if (diag != NULL) {
                diag->code = SLDiag_ARENA_OOM;
                diag->type = SLDiagTypeOfCode(diag->code);
                diag->start = chunk.v[i].start;
                diag->end = chunk.v[i].end;
                diag->argStart = 0;
                diag->argEnd = 0;
            }
            return -1;
        }
    }
    if (SLMirProgramBuilderEndFunction(&builder) != 0 || functionIndex != 0u) {
        if (diag != NULL) {
            diag->code = SLDiag_UNEXPECTED_TOKEN;
            diag->type = SLDiagTypeOfCode(diag->code);
            diag->start = 0;
            diag->end = 0;
            diag->argStart = 0;
            diag->argEnd = 0;
        }
        return -1;
    }
    SLMirProgramBuilderFinish(&builder, outProgram);
    return 0;
}

SL_API_END
