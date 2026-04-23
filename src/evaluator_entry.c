#include "libhop-impl.h"
#include "evaluator_internal.inc.h"

H2_API_BEGIN

int RunProgramEval(
    const char* entryPath,
    const char* _Nullable platformTarget,
    const char* _Nullable archTarget,
    int testingBuild) {
    return HOPEvalRunProgramInternal(entryPath, platformTarget, archTarget, testingBuild);
}

H2_API_END
