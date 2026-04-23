#include "libhop-impl.h"
#include "codegen.h"

H2_API_BEGIN

#ifndef H2_WITH_C_BACKEND
    #define H2_WITH_C_BACKEND 1
#endif

#ifndef H2_WITH_WASM_BACKEND
    #define H2_WITH_WASM_BACKEND 1
#endif

#if H2_WITH_C_BACKEND
extern const H2CodegenBackend gHOPCodegenBackendC;
#endif
#if H2_WITH_WASM_BACKEND
extern const H2CodegenBackend gHOPCodegenBackendWasm;
#endif

static const H2CodegenBackend* const gHOPCodegenBackends[] = {
#if H2_WITH_WASM_BACKEND
    &gHOPCodegenBackendWasm,
#endif
#if H2_WITH_C_BACKEND
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

const H2CodegenBackend* _Nullable H2CodegenFindBackend(const char* _Nullable name) {
    uint32_t i;
#if H2_WITH_C_BACKEND
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

H2_API_END
