#pragma once
#include "libhop.h"
#include "mir.h"

HOP_API_BEGIN

typedef enum {
    HOPForeignLinkage_NONE = 0,
    HOPForeignLinkage_WASM_IMPORT_FN,
    HOPForeignLinkage_WASM_IMPORT_CONST,
    HOPForeignLinkage_WASM_IMPORT_VAR,
    HOPForeignLinkage_EXPORT_FN,
} HOPForeignLinkageKind;

enum {
    HOPForeignLinkageFlag_NONE = 0,
    HOPForeignLinkageFlag_PLATFORM_PANIC = 1u << 0,
};

typedef struct {
    uint8_t* _Nullable bytes;
    uint32_t len;
} HOPForeignLinkageBytes;

typedef struct {
    uint32_t               functionIndex;
    HOPForeignLinkageKind  kind;
    uint8_t                flags;
    uint8_t                _reserved[2];
    uint32_t               start;
    uint32_t               end;
    HOPForeignLinkageBytes arg0;
    HOPForeignLinkageBytes arg1;
} HOPForeignLinkageEntry;

typedef struct {
    const HOPForeignLinkageEntry* _Nullable entries;
    uint32_t len;
} HOPForeignLinkageInfo;

typedef struct {
    const char* packageName;
    const char* source;
    uint32_t    sourceLen;
    const char* _Nullable platformTarget;
    const HOPMirProgram* _Nullable mirProgram;
    const HOPForeignLinkageInfo* _Nullable foreignLinkage;
    uint8_t usesPlatform;
    uint8_t _reserved[3];
} HOPCodegenUnit;

typedef struct {
    uint8_t* _Nullable data;
    uint32_t len;
    uint8_t  isBinary;
    uint8_t  _reserved[3];
} HOPCodegenArtifact;

typedef struct {
    const char* _Nullable headerGuard; /* optional */
    const char* _Nullable implMacro;   /* optional */
    uint32_t emitNodeStartOffset;      /* optional, only used when emitNodeStartOffsetEnabled */
    uint8_t  emitNodeStartOffsetEnabled;
    void* _Nullable allocatorCtx;       /* optional */
    HOPArenaGrowFn _Nullable arenaGrow; /* optional; required for emit output allocation */
    HOPArenaFreeFn _Nullable arenaFree; /* optional */
} HOPCodegenOptions;

struct HOPCodegenBackend;

typedef int (*HOPCodegenEmitFn)(
    const struct HOPCodegenBackend* backend,
    const HOPCodegenUnit*           unit,
    const HOPCodegenOptions* _Nullable options,
    HOPCodegenArtifact* _Nonnull outArtifact,
    HOPDiag* _Nullable diag);

typedef struct HOPCodegenBackend {
    const char*      name;
    HOPCodegenEmitFn emit;
} HOPCodegenBackend;

const HOPCodegenBackend* _Nullable HOPCodegenFindBackend(const char* _Nullable name);

HOP_API_END
