#pragma once
#include "codegen.h"

HOP_API_BEGIN

struct stat;

#ifndef HOP_WITH_C_BACKEND
    #define HOP_WITH_C_BACKEND 1
#endif

typedef struct {
    char* _Nullable path;
    char* _Nullable source;
    uint32_t sourceLen;
    void* _Nullable arenaMem;
    HOPAst ast;
    void* _Nullable typecheckArenaMem;
    HOPArena typecheckArena;
    void* _Nullable typecheckCtx;
    int hasTypecheckCtx;
} HOPParsedFile;

struct HOPPackage;
struct HOPPackageLoader;

typedef struct {
    char* alias; /* internal mangle prefix */
    char* _Nullable bindName;
    char* path;
    struct HOPPackage* _Nullable target;
    uint32_t fileIndex;
    uint32_t start;
    uint32_t end;
} HOPImportRef;

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
} HOPImportSymbolRef;

typedef struct {
    HOPAstKind kind;
    char*      name;
    char*      declText;
    int        hasBody;
    uint32_t   fileIndex;
    int32_t    nodeId;
} HOPSymbolDecl;

typedef struct {
    char*    text;
    uint32_t fileIndex;
    int32_t  nodeId;
    uint32_t sourceStart;
    uint32_t sourceEnd;
} HOPDeclText;

typedef struct HOPPackage {
    char*       dirPath;
    char*       name;
    int         loadState; /* 0=new, 1=loading, 2=loaded */
    int         checked;
    HOPFeatures features; /* accumulated from all parsed files */

    HOPParsedFile* files;
    uint32_t       fileLen;
    uint32_t       fileCap;

    HOPImportRef* imports;
    uint32_t      importLen;
    uint32_t      importCap;

    HOPImportSymbolRef* importSymbols;
    uint32_t            importSymbolLen;
    uint32_t            importSymbolCap;

    HOPSymbolDecl* decls;
    uint32_t       declLen;
    uint32_t       declCap;

    HOPSymbolDecl* pubDecls;
    uint32_t       pubDeclLen;
    uint32_t       pubDeclCap;

    HOPDeclText* declTexts;
    uint32_t     declTextLen;
    uint32_t     declTextCap;
} HOPPackage;

typedef struct HOPPackageLoader {
    char* _Nullable rootDir;
    char* _Nullable platformTarget;
    char* _Nullable archTarget;
    int testingBuild;
    struct HOPPackage* _Nullable selectedPlatformPkg;
    HOPPackage* _Nullable packages;
    uint32_t packageLen;
    uint32_t packageCap;
} HOPPackageLoader;

typedef struct {
    const char* name;
    const char* _Nullable replacement;
} HOPIdentMap;

typedef struct {
    char* _Nullable v;
    uint32_t len;
    uint32_t cap;
} HOPStringBuilder;

typedef struct {
    uint32_t combinedStart;
    uint32_t combinedEnd;
    uint32_t sourceStart;
    uint32_t sourceEnd;
    uint32_t fileIndex;
    int32_t  nodeId;
} HOPCombinedSourceSpan;

typedef struct {
    HOPCombinedSourceSpan* _Nullable spans;
    uint32_t len;
    uint32_t cap;
} HOPCombinedSourceMap;

typedef struct {
    HOPForeignLinkageEntry* _Nullable entries;
    uint32_t len;
    uint32_t cap;
} HOPForeignLinkageBuilder;

typedef struct {
    const HOPPackage* pkg;
    uint32_t          pkgIndex;
    char*             key;
    char*             linkPrefix;
    char*             cacheDir;
    char*             cPath;
    char*             oPath;
    char*             sigPath;
    uint64_t          objMtimeNs;
} HOPPackageArtifact;

typedef struct {
    HOPImportRef* imp;
    char*         oldAlias;
    char*         newAlias;
} HOPAliasOverride;

typedef struct {
    HOPImportSymbolRef* sym;
    char*               oldQualifiedName;
    char*               newQualifiedName;
} HOPImportSymbolOverride;

typedef struct {
    uint8_t startMapped;
    uint8_t endMapped;
    uint8_t argStartMapped;
    uint8_t argEndMapped;
} HOPRemapDiagStatus;

#define HOP_DEFAULT_PLATFORM_TARGET  "cli-libc"
#define HOP_EVAL_PLATFORM_TARGET     "cli-eval"
#define HOP_WASM_MIN_PLATFORM_TARGET "wasm-min"
#define HOP_PLAYBIT_PLATFORM_TARGET  "playbit"
#define HOP_PLAYBIT_ENTRY_HOOK_NAME  "hop_entry_main"
#define HOP_EVAL_CALL_MAX_DEPTH      128u

#if defined(__wasm32__)
    #define HOP_DEFAULT_ARCH_TARGET "wasm32"
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define HOP_DEFAULT_ARCH_TARGET "aarch64"
#elif defined(__x86_64__) || defined(_M_X64)
    #define HOP_DEFAULT_ARCH_TARGET "x86_64"
#else
    #define HOP_DEFAULT_ARCH_TARGET "unknown"
#endif

const char* DisplayPath(const char* path);
int         ASTFirstChild(const HOPAst* ast, int32_t nodeId);
int         ASTNextSibling(const HOPAst* ast, int32_t nodeId);
uint32_t    AstListCount(const HOPAst* ast, int32_t listNode);
int32_t     AstListItemAt(const HOPAst* ast, int32_t listNode, uint32_t index);
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
    uint32_t    start,
    uint32_t    end,
    HOPDiagCode code,
    ...);
int ErrorSimple(const char* fmt, ...);
int PrintHOPDiag(
    const char* filename, const char* _Nullable source, const HOPDiag* diag, int includeHint);
int PrintHOPDiagLineCol(
    const char* filename, const char* _Nullable source, const HOPDiag* diag, int includeHint);
void* _Nullable CodegenArenaGrow(void* _Nullable ctx, uint32_t minSize, uint32_t* _Nonnull outSize);
void     CodegenArenaFree(void* _Nullable ctx, void* _Nullable block, uint32_t blockSize);
int      CompactAstInArena(HOPArena* arena, HOPAst* ast);
int      EnsureCap(void** ptr, uint32_t* cap, uint32_t need, size_t elemSize);
uint32_t ArenaBytesUsed(const HOPArena* arena);
uint32_t ArenaBytesCapacity(const HOPArena* arena);
int      ArenaDebugEnabled(void);
void     CombinedSourceMapFree(HOPCombinedSourceMap* map);
int      CombinedSourceMapAdd(
    HOPCombinedSourceMap* map,
    uint32_t              combinedStart,
    uint32_t              combinedEnd,
    uint32_t              sourceStart,
    uint32_t              sourceEnd,
    uint32_t              fileIndex,
    int32_t               nodeId);
int RemapCombinedOffset(
    const HOPCombinedSourceMap* map, uint32_t offset, uint32_t* outOffset, uint32_t* outFileIndex);
void RemapCombinedDiag(
    const HOPCombinedSourceMap* map,
    const HOPDiag*              diagIn,
    HOPDiag*                    diagOut,
    uint32_t*                   outFileIndex,
    const char* _Nullable source,
    HOPRemapDiagStatus* _Nullable outStatus);
int SBReserve(HOPStringBuilder* b, uint32_t extra);
int SBAppend(HOPStringBuilder* b, const char* s, uint32_t len);
int SBAppendCStr(HOPStringBuilder* b, const char* _Nullable s);
int SBAppendSlice(HOPStringBuilder* b, const char* s, uint32_t start, uint32_t end);
char* _Nullable SBFinish(HOPStringBuilder* b, uint32_t* _Nullable outLen);
int StrEq(const char* a, const char* b);
int SliceEqCStr(const char* s, uint32_t start, uint32_t end, const char* cstr);
int SliceEqSlice(
    const char* a, uint32_t aStart, uint32_t aEnd, const char* b, uint32_t bStart, uint32_t bEnd);
char* _Nullable HOPCDupCStr(const char* s);
char* _Nullable HOPCDupSlice(const char* s, uint32_t start, uint32_t end);
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
int IsReservedHOPPrefixName(const char* s);
char* _Nullable BaseNameDup(const char* path);
char* _Nullable LastPathComponentDup(const char* path);
char* _Nullable StripHOPExtensionDup(const char* filename);
int  CompareStringPtrs(const void* a, const void* b);
int  ListHOPFiles(const char* dirPath, char*** outFiles, uint32_t* outLen);
int  ReadFile(const char* filename, char** outData, uint32_t* outLen);
int  ListTopLevelHOPFilesForFmt(const char* dirPath, char*** outFiles, uint32_t* outLen);
int  WriteFileAtomic(const char* filename, const char* data, uint32_t len);
int  RunFmtCommand(int argc, const char* const* argv);
int  DumpTokens(const char* filename, const char* source, uint32_t sourceLen);
int  DumpAST(const char* filename, const char* source, uint32_t sourceLen);
void StdoutWrite(void* ctx, const char* data, uint32_t len);
int  IsFnReturnTypeNodeKind(HOPAstKind kind);

const HOPImportRef* _Nullable FindImportByAliasSlice(
    const HOPPackage* pkg, const char* src, uint32_t aliasStart, uint32_t aliasEnd);
void FreeLoader(HOPPackageLoader* loader);
int  CheckSource(const char* filename, const char* source, uint32_t sourceLen);
int  LoadPackageForFmt(
    const char* entryPath,
    const char* _Nullable platformTarget,
    HOPPackageLoader* outLoader,
    HOPPackage**      outEntryPkg);
int LoadAndCheckPackage(
    const char* entryPath,
    const char* _Nullable platformTarget,
    const char* _Nullable archTarget,
    int               testingBuild,
    HOPPackageLoader* outLoader,
    HOPPackage**      outEntryPkg);
int FindPackageIndex(const HOPPackageLoader* loader, const HOPPackage* pkg);
int ValidateEntryMainSignature(const HOPPackage* _Nullable entryPkg);
int CheckPackageDir(
    const char* entryPath,
    const char* _Nullable platformTarget,
    const char* _Nullable archTarget,
    int testingBuild);
int     IsAsciiSpaceChar(unsigned char c);
int     IsTypeDeclKind(HOPAstKind kind);
int     DirectiveNameEq(const HOPParsedFile* file, int32_t nodeId, const char* name);
int32_t DirectiveArgAt(const HOPAst* ast, int32_t nodeId, uint32_t index);
int     FindAttachedDirectiveRun(
    const HOPAst* ast, int32_t nodeId, int32_t* outFirstDirective, int32_t* outLastDirective);
int LoaderUsesWasmImportDirective(const HOPPackageLoader* loader);
int BuildPrefixedName(const char* alias, const char* name, char** outName);
int RewriteAliasedPubDeclText(
    const HOPPackage* sourcePkg, const HOPSymbolDecl* pubDecl, const char* alias, char** outText);
int BuildFnImportShapeAndWrapper(
    const char* _Nullable aliasedDeclText,
    const char* _Nullable localName,
    const char* _Nullable qualifiedName,
    char** _Nullable outShapeKey,
    char** _Nullable outWrapperDeclText);
int BuildCombinedPackageSource(
    HOPPackageLoader* loader,
    const HOPPackage* pkg,
    int               includePrivateImportDecls,
    char**            outSource,
    uint32_t*         outLen,
    HOPCombinedSourceMap* _Nullable sourceMap,
    uint32_t* _Nullable outOwnDeclStartOffset);

void FreeForeignLinkageInfo(HOPForeignLinkageInfo* info);
int  PackageHasPlatformImport(const HOPPackage* _Nullable pkg);
int  PackageUsesPlatformImport(const HOPPackageLoader* loader);
int  BuildPackageMirProgram(
    const HOPPackageLoader* loader,
    const HOPPackage*       entryPkg,
    int                     includeSelectedPlatform,
    HOPArena*               arena,
    HOPMirProgram*          outProgram,
    HOPForeignLinkageInfo* _Nullable outForeignLinkage,
    HOPDiag* _Nullable diag);
int DumpMIR(
    const char* entryPath,
    const char* _Nullable platformTarget,
    const char* _Nullable archTarget,
    int testingBuild);

int GeneratePackage(
    const char* entryPath,
    const char* backendName,
    const char* _Nullable outFilename,
    const char* _Nullable platformTarget,
    const char* _Nullable archTarget,
    int testingBuild,
    const char* _Nullable cacheDirArg);
int CompileProgram(
    const char* entryPath,
    const char* outExe,
    const char* _Nullable platformTarget,
    const char* _Nullable archTarget,
    int testingBuild,
    const char* _Nullable cacheDirArg);
int RunProgram(
    const char* entryPath,
    const char* _Nullable platformTarget,
    const char* _Nullable archTarget,
    int testingBuild,
    const char* _Nullable cacheDirArg);

HOP_API_END
