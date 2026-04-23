#pragma once
#include "libhop.h"

H2_API_BEGIN

int RunProgramEval(
    const char* entryPath,
    const char* _Nullable platformTarget,
    const char* _Nullable archTarget,
    int testingBuild);

H2_API_END
