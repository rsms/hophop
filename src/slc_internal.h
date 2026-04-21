#pragma once
#include "codegen.h"

SL_API_BEGIN

struct stat;

#ifndef SL_WITH_C_BACKEND
    #define SL_WITH_C_BACKEND 1
#endif

typedef struct {
    char* _Nullable path;
    char* _Nullable source;
    uint32_t sourceLen;
    void* _Nullable arenaMem;
    SLAst ast;
} SLParsedFile;

struct SLPackage;
struct SLPackageLoader;

typedef struct {
    char* alias; /* internal mangle prefix */
    char* _Nullable bindName;
    char* path;
    struct SLPackage* _Nullable target;
    uint32_t fileIndex;
    uint32_t start;
    uint32_t end;
} SLImportRef;

typedef struct {
    uint32_t importIndex;
    char*    sourceName;
    char*    localName;
    char* _Nullable qualifiedName;
    uint8_t  isType;
    uint8_t  isFunction;
    uint8_t  useWrapper;
    uint32_t exportFileIndex;
    int32_t  exportNodeId;
    char* _Nullable fnShapeKey;
    char* _Nullable wrapperDeclText;
    uint32_t fileIndex;
    uint32_t start;
    uint32_t end;
} SLImportSymbolRef;

typedef struct {
    SLAstKind kind;
    char*     name;
    char*     declText;
    int       hasBody;
    uint32_t  fileIndex;
    int32_t   nodeId;
} SLSymbolDecl;

typedef struct {
    char*    text;
    uint32_t fileIndex;
    int32_t  nodeId;
    uint32_t sourceStart;
    uint32_t sourceEnd;
} SLDeclText;

typedef struct SLPackage {
    char*      dirPath;
    char*      name;
    int        loadState; /* 0=new, 1=loading, 2=loaded */
    int        checked;
    SLFeatures features; /* accumulated from all parsed files */

    SLParsedFile* files;
    uint32_t      fileLen;
    uint32_t      fileCap;

    SLImportRef* imports;
    uint32_t     importLen;
    uint32_t     importCap;

    SLImportSymbolRef* importSymbols;
    uint32_t           importSymbolLen;
    uint32_t           importSymbolCap;

    SLSymbolDecl* decls;
    uint32_t      declLen;
    uint32_t      declCap;

    SLSymbolDecl* pubDecls;
    uint32_t      pubDeclLen;
    uint32_t      pubDeclCap;

    SLDeclText* declTexts;
    uint32_t    declTextLen;
    uint32_t    declTextCap;
} SLPackage;

typedef struct SLPackageLoader {
    char* _Nullable rootDir;
    char* _Nullable platformTarget;
    struct SLPackage* _Nullable selectedPlatformPkg;
    SLPackage* _Nullable packages;
    uint32_t packageLen;
    uint32_t packageCap;
} SLPackageLoader;

typedef struct {
    const char* name;
    const char* _Nullable replacement;
} SLIdentMap;

typedef struct {
    char* _Nullable v;
    uint32_t len;
    uint32_t cap;
} SLStringBuilder;

typedef struct {
    uint32_t combinedStart;
    uint32_t combinedEnd;
    uint32_t sourceStart;
    uint32_t sourceEnd;
    uint32_t fileIndex;
    int32_t  nodeId;
} SLCombinedSourceSpan;

typedef struct {
    SLCombinedSourceSpan* _Nullable spans;
    uint32_t len;
    uint32_t cap;
} SLCombinedSourceMap;

typedef struct {
    SLForeignLinkageEntry* _Nullable entries;
    uint32_t len;
    uint32_t cap;
} SLForeignLinkageBuilder;

typedef struct {
    const SLPackage* pkg;
    uint32_t         pkgIndex;
    char*            key;
    char*            linkPrefix;
    char*            cacheDir;
    char*            cPath;
    char*            oPath;
    char*            sigPath;
    uint64_t         objMtimeNs;
} SLPackageArtifact;

typedef struct {
    SLImportRef* imp;
    char*        oldAlias;
    char*        newAlias;
} SLAliasOverride;

typedef struct {
    SLImportSymbolRef* sym;
    char*              oldQualifiedName;
    char*              newQualifiedName;
} SLImportSymbolOverride;

typedef struct {
    uint8_t startMapped;
    uint8_t endMapped;
    uint8_t argStartMapped;
    uint8_t argEndMapped;
} SLRemapDiagStatus;

#define SL_DEFAULT_PLATFORM_TARGET  "cli-libc"
#define SL_EVAL_PLATFORM_TARGET     "cli-eval"
#define SL_WASM_MIN_PLATFORM_TARGET "wasm-min"
#define SL_PLAYBIT_PLATFORM_TARGET  "playbit"
#define SL_PLAYBIT_ENTRY_HOOK_NAME  "sl_entry_main"
#define SL_EVAL_CALL_MAX_DEPTH      128u

const char* DisplayPath(const char* path);
int         ASTFirstChild(const SLAst* ast, int32_t nodeId);
int         ASTNextSibling(const SLAst* ast, int32_t nodeId);
uint32_t    AstListCount(const SLAst* ast, int32_t listNode);
int32_t     AstListItemAt(const SLAst* ast, int32_t listNode, uint32_t index);
int         Errorf(
    const char* file,
    const char* _Nullable source,
    uint32_t    start,
    uint32_t    end,
    const char* fmt,
    ...);
int ErrorDiagf(
    const char* file,
    const char* _Nullable source,
    uint32_t   start,
    uint32_t   end,
    SLDiagCode code,
    ...);
int ErrorSimple(const char* fmt, ...);
int PrintSLDiag(
    const char* filename, const char* _Nullable source, const SLDiag* diag, int includeHint);
int PrintSLDiagLineCol(
    const char* filename, const char* _Nullable source, const SLDiag* diag, int includeHint);
void* _Nullable CodegenArenaGrow(void* _Nullable ctx, uint32_t minSize, uint32_t* _Nonnull outSize);
void     CodegenArenaFree(void* _Nullable ctx, void* _Nullable block, uint32_t blockSize);
int      CompactAstInArena(SLArena* arena, SLAst* ast);
int      EnsureCap(void** ptr, uint32_t* cap, uint32_t need, size_t elemSize);
uint32_t ArenaBytesUsed(const SLArena* arena);
uint32_t ArenaBytesCapacity(const SLArena* arena);
int      ArenaDebugEnabled(void);
void     CombinedSourceMapFree(SLCombinedSourceMap* map);
int      CombinedSourceMapAdd(
    SLCombinedSourceMap* map,
    uint32_t             combinedStart,
    uint32_t             combinedEnd,
    uint32_t             sourceStart,
    uint32_t             sourceEnd,
    uint32_t             fileIndex,
    int32_t              nodeId);
int RemapCombinedOffset(
    const SLCombinedSourceMap* map, uint32_t offset, uint32_t* outOffset, uint32_t* outFileIndex);
void RemapCombinedDiag(
    const SLCombinedSourceMap* map,
    const SLDiag*              diagIn,
    SLDiag*                    diagOut,
    uint32_t*                  outFileIndex,
    const char* _Nullable source,
    SLRemapDiagStatus* _Nullable outStatus);
int SBReserve(SLStringBuilder* b, uint32_t extra);
int SBAppend(SLStringBuilder* b, const char* s, uint32_t len);
int SBAppendCStr(SLStringBuilder* b, const char* _Nullable s);
int SBAppendSlice(SLStringBuilder* b, const char* s, uint32_t start, uint32_t end);
char* _Nullable SBFinish(SLStringBuilder* b, uint32_t* _Nullable outLen);
int StrEq(const char* a, const char* b);
int SliceEqCStr(const char* s, uint32_t start, uint32_t end, const char* cstr);
int SliceEqSlice(
    const char* a, uint32_t aStart, uint32_t aEnd, const char* b, uint32_t bStart, uint32_t bEnd);
char* _Nullable SLCDupCStr(const char* s);
char* _Nullable SLCDupSlice(const char* s, uint32_t start, uint32_t end);
char* _Nullable JoinPath(const char* _Nullable a, const char* _Nullable b);
char* _Nullable DirNameDup(const char* path);
char* _Nullable GetExeDir(void);
uint64_t StatMtimeNs(const struct stat* st);
int      GetFileMtimeNs(const char* path, uint64_t* outMtimeNs);
int      EnsureDirPath(const char* path);
int      EnsureDirRecursive(const char* path);
char* _Nullable MakeAbsolutePathDup(const char* path);
uint64_t HashFNV1a64(const char* s);
char* _Nullable BuildSanitizedIdent(const char* s, const char* fallback);
int HasSuffix(const char* s, const char* suffix);
int IsIdentStartChar(unsigned char c);
int IsIdentContinueChar(unsigned char c);
int IsValidIdentifier(const char* s);
int IsValidPlatformTargetName(const char* s);
int IsReservedSLPrefixName(const char* s);
char* _Nullable BaseNameDup(const char* path);
char* _Nullable LastPathComponentDup(const char* path);
char* _Nullable StripSLExtensionDup(const char* filename);
int  CompareStringPtrs(const void* a, const void* b);
int  ListSLFiles(const char* dirPath, char*** outFiles, uint32_t* outLen);
int  ReadFile(const char* filename, char** outData, uint32_t* outLen);
int  ListTopLevelSLFilesForFmt(const char* dirPath, char*** outFiles, uint32_t* outLen);
int  WriteFileAtomic(const char* filename, const char* data, uint32_t len);
int  RunFmtCommand(int argc, const char* const* argv);
int  DumpTokens(const char* filename, const char* source, uint32_t sourceLen);
int  DumpAST(const char* filename, const char* source, uint32_t sourceLen);
void StdoutWrite(void* ctx, const char* data, uint32_t len);
int  IsFnReturnTypeNodeKind(SLAstKind kind);

const SLImportRef* _Nullable FindImportByAliasSlice(
    const SLPackage* pkg, const char* src, uint32_t aliasStart, uint32_t aliasEnd);
void FreeLoader(SLPackageLoader* loader);
int  CheckSource(const char* filename, const char* source, uint32_t sourceLen);
int  LoadPackageForFmt(
    const char* entryPath,
    const char* _Nullable platformTarget,
    SLPackageLoader* outLoader,
    SLPackage**      outEntryPkg);
int LoadAndCheckPackage(
    const char* entryPath,
    const char* _Nullable platformTarget,
    SLPackageLoader* outLoader,
    SLPackage**      outEntryPkg);
int     FindPackageIndex(const SLPackageLoader* loader, const SLPackage* pkg);
int     ValidateEntryMainSignature(const SLPackage* _Nullable entryPkg);
int     CheckPackageDir(const char* entryPath, const char* _Nullable platformTarget);
int     IsAsciiSpaceChar(unsigned char c);
int     IsTypeDeclKind(SLAstKind kind);
int     DirectiveNameEq(const SLParsedFile* file, int32_t nodeId, const char* name);
int32_t DirectiveArgAt(const SLAst* ast, int32_t nodeId, uint32_t index);
int     FindAttachedDirectiveRun(
    const SLAst* ast, int32_t nodeId, int32_t* outFirstDirective, int32_t* outLastDirective);
int LoaderUsesWasmImportDirective(const SLPackageLoader* loader);
int BuildPrefixedName(const char* alias, const char* name, char** outName);
int RewriteAliasedPubDeclText(
    const SLPackage* sourcePkg, const SLSymbolDecl* pubDecl, const char* alias, char** outText);
int BuildFnImportShapeAndWrapper(
    const char* _Nullable aliasedDeclText,
    const char* _Nullable localName,
    const char* _Nullable qualifiedName,
    char** _Nullable outShapeKey,
    char** _Nullable outWrapperDeclText);
int BuildCombinedPackageSource(
    SLPackageLoader* loader,
    const SLPackage* pkg,
    int              includePrivateImportDecls,
    char**           outSource,
    uint32_t*        outLen,
    SLCombinedSourceMap* _Nullable sourceMap,
    uint32_t* _Nullable outOwnDeclStartOffset);

void FreeForeignLinkageInfo(SLForeignLinkageInfo* info);
int  PackageHasPlatformImport(const SLPackage* _Nullable pkg);
int  PackageUsesPlatformImport(const SLPackageLoader* loader);
int  BuildPackageMirProgram(
    const SLPackageLoader* loader,
    const SLPackage*       entryPkg,
    int                    includeSelectedPlatform,
    SLArena*               arena,
    SLMirProgram*          outProgram,
    SLForeignLinkageInfo* _Nullable outForeignLinkage,
    SLDiag* _Nullable diag);
int DumpMIR(const char* entryPath, const char* _Nullable platformTarget);

int GeneratePackage(
    const char* entryPath,
    const char* backendName,
    const char* _Nullable outFilename,
    const char* _Nullable platformTarget,
    const char* _Nullable cacheDirArg);
int CompileProgram(
    const char* entryPath,
    const char* outExe,
    const char* _Nullable platformTarget,
    const char* _Nullable cacheDirArg);
int RunProgram(
    const char* entryPath, const char* _Nullable platformTarget, const char* _Nullable cacheDirArg);

SL_API_END
