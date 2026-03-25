#include "libsl-impl.h"
#include "codegen.h"

SL_API_BEGIN

#ifndef SL_WITH_C_BACKEND
    #define SL_WITH_C_BACKEND 1
#endif

#if SL_WITH_C_BACKEND
extern const SLCodegenBackend gSLCodegenBackendC;

static const SLCodegenBackend* const gSLCodegenBackends[] = {
    &gSLCodegenBackendC,
};
#else
static const SLCodegenBackend* const gSLCodegenBackends[] = {};
#endif

static int BackendNameEq(const char* a, const char* b) {
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

const SLCodegenBackend* _Nullable SLCodegenFindBackend(const char* _Nullable name) {
    uint32_t i;
#if SL_WITH_C_BACKEND
    if (name == NULL || name[0] == '\0') {
        return &gSLCodegenBackendC;
    }
#else
    if (name == NULL || name[0] == '\0') {
        return NULL;
    }
#endif
    for (i = 0; i < (uint32_t)(sizeof(gSLCodegenBackends) / sizeof(gSLCodegenBackends[0])); i++) {
        if (BackendNameEq(gSLCodegenBackends[i]->name, name)) {
            return gSLCodegenBackends[i];
        }
    }
    return NULL;
}

SL_API_END
