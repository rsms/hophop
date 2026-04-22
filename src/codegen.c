#include "libhop-impl.h"
#include "codegen.h"

HOP_API_BEGIN

#ifndef HOP_WITH_C_BACKEND
    #define HOP_WITH_C_BACKEND 1
#endif

#ifndef HOP_WITH_WASM_BACKEND
    #define HOP_WITH_WASM_BACKEND 1
#endif

#if HOP_WITH_C_BACKEND
extern const HOPCodegenBackend gHOPCodegenBackendC;
#endif
#if HOP_WITH_WASM_BACKEND
extern const HOPCodegenBackend gHOPCodegenBackendWasm;
#endif

static const HOPCodegenBackend* const gHOPCodegenBackends[] = {
#if HOP_WITH_WASM_BACKEND
    &gHOPCodegenBackendWasm,
#endif
#if HOP_WITH_C_BACKEND
    &gHOPCodegenBackendC,
#endif
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

const HOPCodegenBackend* _Nullable HOPCodegenFindBackend(const char* _Nullable name) {
    uint32_t i;
#if HOP_WITH_C_BACKEND
    if (name == NULL || name[0] == '\0') {
        return &gHOPCodegenBackendC;
    }
#endif
    for (i = 0; i < (uint32_t)(sizeof(gHOPCodegenBackends) / sizeof(gHOPCodegenBackends[0])); i++) {
        if (BackendNameEq(gHOPCodegenBackends[i]->name, name)) {
            return gHOPCodegenBackends[i];
        }
    }
    return NULL;
}

HOP_API_END
