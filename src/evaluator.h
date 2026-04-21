#pragma once
#include "libsl.h"

SL_API_BEGIN

int RunProgramEval(
    const char* entryPath,
    const char* _Nullable platformTarget,
    const char* _Nullable archTarget,
    int testingBuild);

SL_API_END
