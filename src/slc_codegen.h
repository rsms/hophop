#ifndef SL_SLC_CODEGEN_H
#define SL_SLC_CODEGEN_H

#include <stdint.h>

#include "libsl.h"

typedef struct {
    const char* packageName;
    const char* source;
    uint32_t    sourceLen;
} SLCodegenUnit;

typedef struct {
    const char* headerGuard; /* optional */
    const char* implMacro;   /* optional */
} SLCodegenOptions;

struct SLCodegenBackend;

typedef int (*SLCodegenEmitFn)(
    const struct SLCodegenBackend* backend,
    const SLCodegenUnit*           unit,
    const SLCodegenOptions*        options,
    char**                         outHeader,
    SLDiag*                        diag);

typedef struct SLCodegenBackend {
    const char*     name;
    SLCodegenEmitFn emit;
} SLCodegenBackend;

const SLCodegenBackend* SLCodegenFindBackend(const char* name);

#endif
