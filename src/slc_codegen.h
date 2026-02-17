#pragma once
#include "libsl.h"

SL_API_BEGIN

typedef struct {
    const char* packageName;
    const char* source;
    uint32_t    sourceLen;
} SLCodegenUnit;

typedef struct {
    const char* _Nullable headerGuard; /* optional */
    const char* _Nullable implMacro;   /* optional */
    void* _Nullable allocatorCtx;      /* optional */
    SLArenaGrowFn _Nullable arenaGrow; /* optional; required for emit output allocation */
    SLArenaFreeFn _Nullable arenaFree; /* optional */
} SLCodegenOptions;

struct SLCodegenBackend;

typedef int (*SLCodegenEmitFn)(
    const struct SLCodegenBackend* backend,
    const SLCodegenUnit*           unit,
    const SLCodegenOptions* _Nullable options,
    char** outHeader,
    SLDiag* _Nullable diag);

typedef struct SLCodegenBackend {
    const char*     name;
    SLCodegenEmitFn emit;
} SLCodegenBackend;

const SLCodegenBackend* _Nullable SLCodegenFindBackend(const char* _Nullable name);

SL_API_END
