#include <string.h>

#include "slc_codegen.h"

extern const SLCodegenBackend gSLCodegenBackendC;

static const SLCodegenBackend* const gSLCodegenBackends[] = {
    &gSLCodegenBackendC,
};

const SLCodegenBackend* SLCodegenFindBackend(const char* name) {
    uint32_t i;
    if (name == NULL || name[0] == '\0') {
        return &gSLCodegenBackendC;
    }
    for (i = 0; i < (uint32_t)(sizeof(gSLCodegenBackends) / sizeof(gSLCodegenBackends[0])); i++) {
        if (strcmp(gSLCodegenBackends[i]->name, name) == 0) {
            return gSLCodegenBackends[i];
        }
    }
    return NULL;
}
