#pragma once
#include "libhop.h"

HOP_API_BEGIN

int RunProgramEval(
    const char* entryPath,
    const char* _Nullable platformTarget,
    const char* _Nullable archTarget,
    int testingBuild);

HOP_API_END
