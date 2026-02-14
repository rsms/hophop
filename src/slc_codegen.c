#include "libsl-impl.h"
#include "slc_codegen.h"

SL_API_BEGIN

extern const SLCodegenBackend gSLCodegenBackendC;

static const SLCodegenBackend* const gSLCodegenBackends[] = {
    &gSLCodegenBackendC,
};

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
    if (name == NULL || name[0] == '\0') {
        return &gSLCodegenBackendC;
    }
    for (i = 0; i < (uint32_t)(sizeof(gSLCodegenBackends) / sizeof(gSLCodegenBackends[0])); i++) {
        if (BackendNameEq(gSLCodegenBackends[i]->name, name)) {
            return gSLCodegenBackends[i];
        }
    }
    return NULL;
}

SL_API_END
