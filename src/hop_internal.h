#pragma once
#include "codegen.h"

H2_API_BEGIN

struct stat;

#ifndef H2_WITH_C_BACKEND
    #define H2_WITH_C_BACKEND 1
#endif

typedef struct {
    char* _Nullable path;
    char* _Nullable source;
    uint32_t sourceLen;
    void* _Nullable arenaMem;
    H2Ast ast;
    void* _Nullable typecheckArenaMem;
    H2Arena typecheckArena;
    void* _Nullable typecheckCtx;
    int hasTypecheckCtx;
} H2ParsedFile;

struct H2Package;
struct H2PackageLoader;

typedef struct {
    char* alias; /* internal mangle prefix */
    char* _Nullable bindName;
    char* path;
    struct H2Package* _Nullable target;
    uint32_t fileIndex;
    uint32_t start;
    uint32_t end;
} H2ImportRef;

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
} H2ImportSymbolRef;

typedef struct {
    H2AstKind kind;
    char*     name;
    char*     declText;
    int       hasBody;
    uint32_t  fileIndex;
    int32_t   nodeId;
} H2SymbolDecl;

typedef struct {
    char*    text;
    uint32_t fileIndex;
    int32_t  nodeId;
    uint32_t sourceStart;
    uint32_t sourceEnd;
} H2DeclText;

typedef struct H2Package {
    char*      dirPath;
    char*      name;
    int        loadState; /* 0=new, 1=loading, 2=loaded */
    int        checked;
    H2Features features; /* accumulated from all parsed files */

    H2ParsedFile* files;
    uint32_t      fileLen;
    uint32_t      fileCap;

    H2ImportRef* imports;
    uint32_t     importLen;
    uint32_t     importCap;

    H2ImportSymbolRef* importSymbols;
    uint32_t           importSymbolLen;
    uint32_t           importSymbolCap;

    H2SymbolDecl* decls;
    uint32_t      declLen;
    uint32_t      declCap;

    H2SymbolDecl* pubDecls;
    uint32_t      pubDeclLen;
    uint32_t      pubDeclCap;

    H2DeclText* declTexts;
    uint32_t    declTextLen;
    uint32_t    declTextCap;
} H2Package;

typedef struct H2PackageLoader {
    char* _Nullable rootDir;
    char* _Nullable platformTarget;
    char* _Nullable archTarget;
    int testingBuild;
    struct H2Package* _Nullable selectedPlatformPkg;
    H2Package* _Nullable packages;
    uint32_t packageLen;
    uint32_t packageCap;
} H2PackageLoader;

typedef struct {
    const char* name;
    const char* _Nullable replacement;
} H2IdentMap;

typedef struct {
    char* _Nullable v;
    uint32_t len;
    uint32_t cap;
} H2StringBuilder;

typedef struct {
    uint32_t combinedStart;
    uint32_t combinedEnd;
    uint32_t sourceStart;
    uint32_t sourceEnd;
    uint32_t fileIndex;
    int32_t  nodeId;
    const char* _Nullable path;
    const char* _Nullable source;
} H2CombinedSourceSpan;

typedef struct {
    H2CombinedSourceSpan* _Nullable spans;
    uint32_t len;
    uint32_t cap;
} H2CombinedSourceMap;

typedef struct {
    H2ForeignLinkageEntry* _Nullable entries;
    uint32_t len;
    uint32_t cap;
} H2ForeignLinkageBuilder;

typedef struct {
    const H2Package* pkg;
    uint32_t         pkgIndex;
    char*            key;
    char*            linkPrefix;
    char*            cacheDir;
    char*            cPath;
    char*            oPath;
    char*            sigPath;
    uint64_t         objMtimeNs;
} H2PackageArtifact;

typedef struct {
    H2ImportRef* imp;
    char*        oldAlias;
    char*        newAlias;
} H2AliasOverride;

typedef struct {
    H2ImportSymbolRef* sym;
    char*              oldQualifiedName;
    char*              newQualifiedName;
} H2ImportSymbolOverride;

typedef struct {
    uint8_t startMapped;
    uint8_t endMapped;
    uint8_t argStartMapped;
    uint8_t argEndMapped;
} H2RemapDiagStatus;

typedef enum {
    H2DiagOutputFormat_TEXT = 0,
    H2DiagOutputFormat_JSONL,
} H2DiagOutputFormat;

#define H2_DEFAULT_PLATFORM_TARGET  "cli-libc"
#define H2_EVAL_PLATFORM_TARGET     "cli-eval"
#define H2_WASM_MIN_PLATFORM_TARGET "wasm-min"
#define H2_PLAYBIT_PLATFORM_TARGET  "playbit"
#define H2_PLAYBIT_ENTRY_HOOK_NAME  "hop_entry_main"
#define H2_EVAL_CALL_MAX_DEPTH      128u

#if defined(__wasm32__)
    #define H2_DEFAULT_ARCH_TARGET "wasm32"
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define H2_DEFAULT_ARCH_TARGET "aarch64"
#elif defined(__x86_64__) || defined(_M_X64)
    #define H2_DEFAULT_ARCH_TARGET "x86_64"
#else
    #define H2_DEFAULT_ARCH_TARGET "unknown"
#endif

const char* DisplayPath(const char* path);
int         ASTFirstChild(const H2Ast* ast, int32_t nodeId);
int         ASTNextSibling(const H2Ast* ast, int32_t nodeId);
uint32_t    AstListCount(const H2Ast* ast, int32_t listNode);
int32_t     AstListItemAt(const H2Ast* ast, int32_t listNode, uint32_t index);
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
    H2DiagCode code,
    ...);
int ErrorSimple(const char* fmt, ...);
int PrintHOPDiag(
    const char* filename, const char* _Nullable source, const H2Diag* diag, int includeHint);
int PrintHOPDiagLineCol(
    const char* filename, const char* _Nullable source, const H2Diag* diag, int includeHint);
void               SetDiagOutputFormat(H2DiagOutputFormat format);
H2DiagOutputFormat GetDiagOutputFormat(void);
void* _Nullable CodegenArenaGrow(void* _Nullable ctx, uint32_t minSize, uint32_t* _Nonnull outSize);
void     CodegenArenaFree(void* _Nullable ctx, void* _Nullable block, uint32_t blockSize);
int      CompactAstInArena(H2Arena* arena, H2Ast* ast);
int      EnsureCap(void** ptr, uint32_t* cap, uint32_t need, size_t elemSize);
uint32_t ArenaBytesUsed(const H2Arena* arena);
uint32_t ArenaBytesCapacity(const H2Arena* arena);
int      ArenaDebugEnabled(void);
void     CombinedSourceMapFree(H2CombinedSourceMap* map);
int      CombinedSourceMapAdd(
    H2CombinedSourceMap* map,
    uint32_t             combinedStart,
    uint32_t             combinedEnd,
    uint32_t             sourceStart,
    uint32_t             sourceEnd,
    uint32_t             fileIndex,
    int32_t              nodeId,
    const char* _Nullable path,
    const char* _Nullable source);
int RemapCombinedOffset(
    const H2CombinedSourceMap* map,
    uint32_t                   offset,
    uint32_t*                  outOffset,
    uint32_t*                  outFileIndex,
    const char* _Nullable* _Nullable outPath,
    const char* _Nullable* _Nullable outSource);
void RemapCombinedDiag(
    const H2CombinedSourceMap* map,
    const H2Diag*              diagIn,
    H2Diag*                    diagOut,
    uint32_t*                  outFileIndex,
    const char* _Nullable source,
    H2RemapDiagStatus* _Nullable outStatus);
int SBReserve(H2StringBuilder* b, uint32_t extra);
int SBAppend(H2StringBuilder* b, const char* s, uint32_t len);
int SBAppendCStr(H2StringBuilder* b, const char* _Nullable s);
int SBAppendSlice(H2StringBuilder* b, const char* s, uint32_t start, uint32_t end);
char* _Nullable SBFinish(H2StringBuilder* b, uint32_t* _Nullable outLen);
int StrEq(const char* a, const char* b);
int SliceEqCStr(const char* s, uint32_t start, uint32_t end, const char* cstr);
int SliceEqSlice(
    const char* a, uint32_t aStart, uint32_t aEnd, const char* b, uint32_t bStart, uint32_t bEnd);
char* _Nullable H2CDupCStr(const char* s);
char* _Nullable H2CDupSlice(const char* s, uint32_t start, uint32_t end);
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
int  IsFnReturnTypeNodeKind(H2AstKind kind);

const H2ImportRef* _Nullable FindImportByAliasSlice(
    const H2Package* pkg, const char* src, uint32_t aliasStart, uint32_t aliasEnd);
void FreeLoader(H2PackageLoader* loader);
int  CheckSource(const char* filename, const char* source, uint32_t sourceLen);
int  LoadPackageForFmt(
    const char* entryPath,
    const char* _Nullable platformTarget,
    H2PackageLoader* outLoader,
    H2Package**      outEntryPkg);
int LoadAndCheckPackage(
    const char* entryPath,
    const char* _Nullable platformTarget,
    const char* _Nullable archTarget,
    int              testingBuild,
    H2PackageLoader* outLoader,
    H2Package**      outEntryPkg);
int FindPackageIndex(const H2PackageLoader* loader, const H2Package* pkg);
int ValidateEntryMainSignature(const H2Package* _Nullable entryPkg);
int CheckPackageDir(
    const char* entryPath,
    const char* _Nullable platformTarget,
    const char* _Nullable archTarget,
    int testingBuild);
int     IsAsciiSpaceChar(unsigned char c);
int     IsTypeDeclKind(H2AstKind kind);
int     DirectiveNameEq(const H2ParsedFile* file, int32_t nodeId, const char* name);
int32_t DirectiveArgAt(const H2Ast* ast, int32_t nodeId, uint32_t index);
int     FindAttachedDirectiveRun(
    const H2Ast* ast, int32_t nodeId, int32_t* outFirstDirective, int32_t* outLastDirective);
int LoaderUsesWasmImportDirective(const H2PackageLoader* loader);
int BuildPrefixedName(const char* alias, const char* name, char** outName);
int RewriteAliasedPubDeclText(
    const H2Package* sourcePkg, const H2SymbolDecl* pubDecl, const char* alias, char** outText);
int BuildFnImportShapeAndWrapper(
    const char* _Nullable aliasedDeclText,
    const char* _Nullable localName,
    const char* _Nullable qualifiedName,
    char** _Nullable outShapeKey,
    char** _Nullable outWrapperDeclText);
int BuildCombinedPackageSource(
    H2PackageLoader* loader,
    const H2Package* pkg,
    int              includePrivateImportDecls,
    char**           outSource,
    uint32_t*        outLen,
    H2CombinedSourceMap* _Nullable sourceMap,
    uint32_t* _Nullable outOwnDeclStartOffset);

void FreeForeignLinkageInfo(H2ForeignLinkageInfo* info);
int  PackageHasPlatformImport(const H2Package* _Nullable pkg);
int  PackageUsesPlatformImport(const H2PackageLoader* loader);
int  BuildPackageMirProgram(
    const H2PackageLoader* loader,
    const H2Package*       entryPkg,
    int                    includeSelectedPlatform,
    H2Arena*               arena,
    H2MirProgram*          outProgram,
    H2ForeignLinkageInfo* _Nullable outForeignLinkage,
    H2Diag* _Nullable diag);
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

H2_API_END
