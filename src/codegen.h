#pragma once
#include "libsl.h"
#include "mir.h"

SL_API_BEGIN

typedef enum {
    SLForeignLinkage_NONE = 0,
    SLForeignLinkage_WASM_IMPORT_FN,
    SLForeignLinkage_WASM_IMPORT_CONST,
    SLForeignLinkage_WASM_IMPORT_VAR,
    SLForeignLinkage_EXPORT_FN,
} SLForeignLinkageKind;

enum {
    SLForeignLinkageFlag_NONE = 0,
    SLForeignLinkageFlag_PLATFORM_PANIC = 1u << 0,
};

typedef struct {
    uint8_t* _Nullable bytes;
    uint32_t len;
} SLForeignLinkageBytes;

typedef struct {
    uint32_t              functionIndex;
    SLForeignLinkageKind  kind;
    uint8_t               flags;
    uint8_t               _reserved[2];
    uint32_t              start;
    uint32_t              end;
    SLForeignLinkageBytes arg0;
    SLForeignLinkageBytes arg1;
} SLForeignLinkageEntry;

typedef struct {
    const SLForeignLinkageEntry* _Nullable entries;
    uint32_t len;
} SLForeignLinkageInfo;

typedef struct {
    const char* packageName;
    const char* source;
    uint32_t    sourceLen;
    const char* _Nullable platformTarget;
    const SLMirProgram* _Nullable mirProgram;
    const SLForeignLinkageInfo* _Nullable foreignLinkage;
    uint8_t usesPlatform;
    uint8_t _reserved[3];
} SLCodegenUnit;

typedef struct {
    uint8_t* _Nullable data;
    uint32_t len;
    uint8_t  isBinary;
    uint8_t  _reserved[3];
} SLCodegenArtifact;

typedef struct {
    const char* _Nullable headerGuard; /* optional */
    const char* _Nullable implMacro;   /* optional */
    uint32_t emitNodeStartOffset;      /* optional, only used when emitNodeStartOffsetEnabled */
    uint8_t  emitNodeStartOffsetEnabled;
    void* _Nullable allocatorCtx;      /* optional */
    SLArenaGrowFn _Nullable arenaGrow; /* optional; required for emit output allocation */
    SLArenaFreeFn _Nullable arenaFree; /* optional */
} SLCodegenOptions;

struct SLCodegenBackend;

typedef int (*SLCodegenEmitFn)(
    const struct SLCodegenBackend* backend,
    const SLCodegenUnit*           unit,
    const SLCodegenOptions* _Nullable options,
    SLCodegenArtifact* _Nonnull outArtifact,
    SLDiag* _Nullable diag);

typedef struct SLCodegenBackend {
    const char*     name;
    SLCodegenEmitFn emit;
} SLCodegenBackend;

const SLCodegenBackend* _Nullable SLCodegenFindBackend(const char* _Nullable name);

SL_API_END
