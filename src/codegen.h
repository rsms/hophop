#pragma once
#include "libhop.h"
#include "mir.h"

H2_API_BEGIN

typedef enum {
    H2ForeignLinkage_NONE = 0,
    H2ForeignLinkage_WASM_IMPORT_FN,
    H2ForeignLinkage_WASM_IMPORT_CONST,
    H2ForeignLinkage_WASM_IMPORT_VAR,
    H2ForeignLinkage_EXPORT_FN,
} H2ForeignLinkageKind;

enum {
    H2ForeignLinkageFlag_NONE = 0,
    H2ForeignLinkageFlag_PLATFORM_PANIC = 1u << 0,
};

typedef struct {
    uint8_t* _Nullable bytes;
    uint32_t len;
} H2ForeignLinkageBytes;

typedef struct {
    uint32_t              functionIndex;
    H2ForeignLinkageKind  kind;
    uint8_t               flags;
    uint8_t               _reserved[2];
    uint32_t              start;
    uint32_t              end;
    H2ForeignLinkageBytes arg0;
    H2ForeignLinkageBytes arg1;
} H2ForeignLinkageEntry;

typedef struct {
    const H2ForeignLinkageEntry* _Nullable entries;
    uint32_t len;
} H2ForeignLinkageInfo;

typedef struct {
    const char* packageName;
    const char* source;
    uint32_t    sourceLen;
    const char* _Nullable platformTarget;
    const H2MirProgram* _Nullable mirProgram;
    const H2ForeignLinkageInfo* _Nullable foreignLinkage;
    uint8_t usesPlatform;
    uint8_t _reserved[3];
} H2CodegenUnit;

typedef struct {
    uint8_t* _Nullable data;
    uint32_t len;
    uint8_t  isBinary;
    uint8_t  _reserved[3];
} H2CodegenArtifact;

typedef struct {
    const char* _Nullable headerGuard; /* optional */
    const char* _Nullable implMacro;   /* optional */
    uint32_t emitNodeStartOffset;      /* optional, only used when emitNodeStartOffsetEnabled */
    uint8_t  emitNodeStartOffsetEnabled;
    void* _Nullable allocatorCtx;      /* optional */
    H2ArenaGrowFn _Nullable arenaGrow; /* optional; required for emit output allocation */
    H2ArenaFreeFn _Nullable arenaFree; /* optional */
} H2CodegenOptions;

struct H2CodegenBackend;

typedef int (*H2CodegenEmitFn)(
    const struct H2CodegenBackend* backend,
    const H2CodegenUnit*           unit,
    const H2CodegenOptions* _Nullable options,
    H2CodegenArtifact* _Nonnull outArtifact,
    H2Diag* _Nullable diag);

typedef struct H2CodegenBackend {
    const char*     name;
    H2CodegenEmitFn emit;
} H2CodegenBackend;

const H2CodegenBackend* _Nullable H2CodegenFindBackend(const char* _Nullable name);

H2_API_END
