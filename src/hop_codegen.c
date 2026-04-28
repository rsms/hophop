#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
    #include <mach-o/dyld.h>
#endif

#include "codegen.h"
#include "ctfe.h"
#include "evaluator.h"
#include "libhop-impl.h"
#include "mir.h"
#include "mir_lower_pkg.h"
#include "mir_lower_stmt.h"
#include "hop_internal.h"

H2_API_BEGIN

static int WriteOutput(const char* _Nullable outFilename, const char* data, uint32_t len) {
    FILE*  out;
    size_t nwritten;
    if (outFilename == NULL || StrEq(outFilename, "-")) {
        if (len == 0) {
            return 0;
        }
        nwritten = fwrite(data, 1u, (size_t)len, stdout);
        return nwritten == (size_t)len ? 0 : -1;
    }
    out = fopen(outFilename, "wb");
    if (out == NULL) {
        return -1;
    }
    nwritten = fwrite(data, 1u, (size_t)len, out);
    fclose(out);
    return nwritten == (size_t)len ? 0 : -1;
}

static int HasCBackendBuild(void) {
#if H2_WITH_C_BACKEND
    return 1;
#else
    return 0;
#endif
}

static int HasWasmBackendBuild(void) {
#if H2_WITH_WASM_BACKEND
    return 1;
#else
    return 0;
#endif
}

static int ErrorCBackendDisabled(void) {
    return ErrorSimple("this hop build was compiled without the C backend");
}

int GeneratePackageInput(
    const H2PackageInput* input,
    const char*           backendName,
    const char* _Nullable outFilename,
    const char* _Nullable platformTarget,
    const char* _Nullable archTarget,
    int testingBuild,
    const char* _Nullable cacheDirArg) {
    uint8_t                 mirArenaStorage[4096];
    H2Arena                 mirArena;
    H2PackageLoader         loader;
    H2Package*              entryPkg = NULL;
    char*                   source = NULL;
    uint32_t                sourceLen = 0;
    H2MirProgram            mirProgram = { 0 };
    H2ForeignLinkageInfo    foreignLinkage = { 0 };
    H2CodegenArtifact       artifact = { 0 };
    H2Diag                  diag = { 0 };
    H2CodegenUnit           unit;
    const H2CodegenBackend* backend;
    const char*             effectivePlatformTarget = platformTarget;
    int                     mirIncludeSelectedPlatform = 0;
    int                     retriedWithImplicitPlatformPanic = 0;
    int                     needsMir = 0;
    (void)cacheDirArg;

    memset(&mirArena, 0, sizeof(mirArena));
    if (StrEq(backendName, "wasm")
        && (effectivePlatformTarget == NULL || effectivePlatformTarget[0] == '\0'))
    {
        effectivePlatformTarget = H2_WASM_MIN_PLATFORM_TARGET;
    }

    if (LoadAndCheckPackageInput(
            input, effectivePlatformTarget, archTarget, testingBuild, &loader, &entryPkg)
        != 0)
    {
        return -1;
    }

    if (BuildCombinedPackageSource(&loader, entryPkg, 1, &source, &sourceLen, NULL, NULL) != 0) {
        FreeLoader(&loader);
        return -1;
    }

    if (!IsValidIdentifier(entryPkg->name)) {
        free(source);
        FreeLoader(&loader);
        return ErrorSimple(
            "entry package name \"%s\" is not a valid identifier "
            "(inferred from path)",
            entryPkg->name);
    }

    backend = H2CodegenFindBackend(backendName);
    if (backend == NULL) {
        free(source);
        FreeLoader(&loader);
        if (!HasCBackendBuild()) {
            return ErrorCBackendDisabled();
        }
        return ErrorSimple("unknown backend: %s", backendName);
    }
    if (source == NULL) {
        FreeLoader(&loader);
        return ErrorSimple("out of memory");
    }

    unit.packageName = entryPkg->name;
    unit.source = source;
    unit.sourceLen = sourceLen;
    unit.platformTarget = effectivePlatformTarget;
    unit.mirProgram = NULL;
    unit.foreignLinkage = NULL;
    unit.usesPlatform = PackageHasPlatformImport(entryPkg) ? 1u : 0u;

    needsMir = StrEq(backendName, "wasm");
    mirIncludeSelectedPlatform =
        PackageUsesPlatformImport(&loader)
        || (effectivePlatformTarget != NULL
            && StrEq(effectivePlatformTarget, H2_PLAYBIT_PLATFORM_TARGET));
    if (needsMir) {
    rebuild_mir:
        H2ArenaInit(&mirArena, mirArenaStorage, sizeof(mirArenaStorage));
        H2ArenaSetAllocator(&mirArena, NULL, CodegenArenaGrow, CodegenArenaFree);
        if (BuildPackageMirProgram(
                &loader,
                entryPkg,
                mirIncludeSelectedPlatform,
                &mirArena,
                &mirProgram,
                &foreignLinkage,
                &diag)
            != 0)
        {
            if (diag.code != H2Diag_NONE && entryPkg->fileLen == 1
                && entryPkg->files[0].source != NULL)
            {
                (void)PrintHOPDiagLineCol(
                    entryPkg->files[0].path, entryPkg->files[0].source, &diag, 0);
            } else if (diag.code != H2Diag_NONE) {
                (void)ErrorSimple("invalid MIR program");
            }
            free(source);
            FreeLoader(&loader);
            H2ArenaDispose(&mirArena);
            return -1;
        }
        unit.mirProgram = &mirProgram;
        unit.foreignLinkage = &foreignLinkage;
        unit.usesPlatform = PackageUsesPlatformImport(&loader) ? 1u : 0u;
    }

    H2CodegenOptions codegenOptions = { 0 };
    codegenOptions.arenaGrow = CodegenArenaGrow;
    codegenOptions.arenaFree = CodegenArenaFree;

    if (backend->emit(backend, &unit, &codegenOptions, &artifact, &diag) != 0) {
        if (needsMir && !retriedWithImplicitPlatformPanic && !mirIncludeSelectedPlatform
            && effectivePlatformTarget != NULL
            && StrEq(effectivePlatformTarget, H2_WASM_MIN_PLATFORM_TARGET)
            && diag.code == H2Diag_WASM_BACKEND_UNSUPPORTED_MIR && diag.detail != NULL
            && StrEq(
                diag.detail, "selected platform does not provide imported panic for direct Wasm"))
        {
            if (artifact.data != NULL) {
                free(artifact.data);
                artifact = (H2CodegenArtifact){ 0 };
            }
            FreeForeignLinkageInfo(&foreignLinkage);
            memset(&mirProgram, 0, sizeof(mirProgram));
            H2ArenaDispose(&mirArena);
            diag = (H2Diag){ 0 };
            mirIncludeSelectedPlatform = 1;
            retriedWithImplicitPlatformPanic = 1;
            goto rebuild_mir;
        }
        if (diag.code != H2Diag_NONE) {
            int diagStatus;
            if (entryPkg->fileLen == 1 && entryPkg->importLen == 0) {
                diagStatus = PrintHOPDiagLineCol(
                    entryPkg->files[0].path, entryPkg->files[0].source, &diag, 1);
            } else {
                diagStatus = PrintHOPDiag(entryPkg->dirPath, source, &diag, 1);
            }
            if (!(diagStatus == 0 && artifact.data != NULL)) {
                free(source);
                FreeForeignLinkageInfo(&foreignLinkage);
                FreeLoader(&loader);
                H2ArenaDispose(&mirArena);
                return -1;
            }
        } else {
            fprintf(stderr, "error: codegen failed\n");
            free(source);
            FreeLoader(&loader);
            H2ArenaDispose(&mirArena);
            return -1;
        }
    }

    if (WriteOutput(outFilename, (const char*)artifact.data, artifact.len) != 0) {
        fprintf(stderr, "error: failed to write output\n");
        free(artifact.data);
        free(source);
        FreeLoader(&loader);
        H2ArenaDispose(&mirArena);
        return -1;
    }

    free(artifact.data);
    free(source);
    FreeForeignLinkageInfo(&foreignLinkage);
    FreeLoader(&loader);
    H2ArenaDispose(&mirArena);
    return 0;
}

int GeneratePackage(
    const char* entryPath,
    const char* backendName,
    const char* _Nullable outFilename,
    const char* _Nullable platformTarget,
    const char* _Nullable archTarget,
    int testingBuild,
    const char* _Nullable cacheDirArg) {
    H2PackageInput input = { 0 };
    input.paths = &entryPath;
    input.pathLen = 1;
    return GeneratePackageInput(
        &input, backendName, outFilename, platformTarget, archTarget, testingBuild, cacheDirArg);
}

static int RunCommand(const char* const* argv) {
    pid_t pid = fork();
    int   status;
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        execvp(argv[0], (char* const*)argv);
        perror(argv[0]);
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return 0;
    }
    return -1;
}

static int RunCommandExitCode(const char* const* argv, int* outExitCode) {
    pid_t pid = fork();
    int   status;
    if (outExitCode == NULL) {
        return -1;
    }
    *outExitCode = -1;
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        execvp(argv[0], (char* const*)argv);
        perror(argv[0]);
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    if (WIFEXITED(status)) {
        *outExitCode = WEXITSTATUS(status);
        return 0;
    }
    if (WIFSIGNALED(status)) {
        *outExitCode = 128 + WTERMSIG(status);
        return 0;
    }
    return -1;
}

static void FreePackageArtifacts(H2PackageArtifact* _Nullable artifacts, uint32_t artifactLen) {
    uint32_t i;
    if (artifacts == NULL) {
        return;
    }
    for (i = 0; i < artifactLen; i++) {
        free(artifacts[i].key);
        free(artifacts[i].linkPrefix);
        free(artifacts[i].cacheDir);
        free(artifacts[i].cPath);
        free(artifacts[i].oPath);
        free(artifacts[i].sigPath);
    }
    free(artifacts);
}

static int ResolveLibDir(char** outLibDir) {
    char* exeDir = GetExeDir();
    char* libDir = NULL;
    *outLibDir = NULL;
    if (exeDir != NULL) {
        libDir = JoinPath(exeDir, "lib");
        free(exeDir);
    }
    if (libDir == NULL) {
        return ErrorSimple("cannot locate lib directory (dirname of executable)");
    }
    *outLibDir = libDir;
    return 0;
}

static int ResolveRepoToolPath(const char* relPath, char** outPath) {
    char* exeDir = NULL;
    char* buildDir = NULL;
    char* repoDir = NULL;
    char* toolPath = NULL;
    if (relPath == NULL || outPath == NULL) {
        return -1;
    }
    *outPath = NULL;
    exeDir = GetExeDir();
    if (exeDir == NULL) {
        return ErrorSimple("cannot locate executable directory");
    }
    buildDir = DirNameDup(exeDir);
    free(exeDir);
    if (buildDir == NULL) {
        return ErrorSimple("cannot locate build directory");
    }
    repoDir = DirNameDup(buildDir);
    free(buildDir);
    if (repoDir == NULL) {
        return ErrorSimple("cannot locate repository root");
    }
    toolPath = JoinPath(repoDir, relPath);
    free(repoDir);
    if (toolPath == NULL) {
        return ErrorSimple("out of memory");
    }
    if (access(toolPath, R_OK) != 0) {
        free(toolPath);
        return ErrorSimple("cannot locate %s", relPath);
    }
    *outPath = toolPath;
    return 0;
}

static int ResolvePlaybitPbPath(char** outPath) {
    const char* envPath = NULL;
    const char* home = NULL;
    char*       candidate = NULL;
    if (outPath == NULL) {
        return -1;
    }
    *outPath = NULL;
    envPath = getenv("H2_PLAYBIT_PB");
    if (envPath == NULL || envPath[0] == '\0') {
        envPath = getenv("PLAYBIT_PB");
    }
    if ((envPath == NULL || envPath[0] == '\0')) {
        envPath = getenv("PB");
    }
    if (envPath != NULL && envPath[0] != '\0') {
        *outPath = H2CDupCStr(envPath);
        return *outPath != NULL ? 0 : ErrorSimple("out of memory");
    }
    home = getenv("HOME");
    if (home != NULL && home[0] != '\0') {
        candidate = JoinPath(home, "playbit/engine/_deps/toolchain/bin/pb");
        if (candidate == NULL) {
            return ErrorSimple("out of memory");
        }
        if (access(candidate, X_OK) == 0) {
            *outPath = candidate;
            return 0;
        }
        free(candidate);
        candidate = JoinPath(home, "Library/Application Support/Playbit/toolchain/bin/pb");
        if (candidate == NULL) {
            return ErrorSimple("out of memory");
        }
        if (access(candidate, X_OK) == 0) {
            *outPath = candidate;
            return 0;
        }
        free(candidate);
    }
    *outPath = H2CDupCStr("pb");
    return *outPath != NULL ? 0 : ErrorSimple("out of memory");
}

static int ResolvePlatformPath(
    const char* _Nullable libDir, const char* _Nullable platformTarget, char** outPlatformPath) {
    char* platformDir = NULL;
    char* platformTargetDir = NULL;
    char* platformPath = NULL;
    if (libDir == NULL || platformTarget == NULL || outPlatformPath == NULL) {
        return -1;
    }
    *outPlatformPath = NULL;
    platformDir = JoinPath(libDir, "platform");
    if (platformDir == NULL) {
        return ErrorSimple("out of memory");
    }
    platformTargetDir = JoinPath(platformDir, platformTarget);
    if (platformTargetDir == NULL) {
        free(platformDir);
        return ErrorSimple("out of memory");
    }
    platformPath = JoinPath(platformTargetDir, "platform.c");
    free(platformTargetDir);
    free(platformDir);
    if (platformPath == NULL) {
        return ErrorSimple("out of memory");
    }
    *outPlatformPath = platformPath;
    return 0;
}

static int ResolveBuiltinPath(
    const char* _Nullable libDir, char** outBuiltinPath, char** outBuiltinHeaderPath) {
    char* builtinDir = NULL;
    char* builtinPath = NULL;
    char* builtinHeaderPath = NULL;
    if (libDir == NULL || outBuiltinPath == NULL || outBuiltinHeaderPath == NULL) {
        return -1;
    }
    *outBuiltinPath = NULL;
    *outBuiltinHeaderPath = NULL;
    builtinDir = JoinPath(libDir, "builtin");
    if (builtinDir == NULL) {
        return ErrorSimple("out of memory");
    }
    builtinPath = JoinPath(builtinDir, "builtin.c");
    builtinHeaderPath = JoinPath(builtinDir, "builtin.h");
    free(builtinDir);
    if (builtinPath == NULL || builtinHeaderPath == NULL) {
        free(builtinPath);
        free(builtinHeaderPath);
        return ErrorSimple("out of memory");
    }
    *outBuiltinPath = builtinPath;
    *outBuiltinHeaderPath = builtinHeaderPath;
    return 0;
}

static int ResolveCacheRoot(
    const H2PackageLoader* _Nullable loader,
    const char* _Nullable cacheDirArg,
    char** outCacheRoot) {
    char* cacheRoot;
    if (outCacheRoot == NULL) {
        return -1;
    }
    *outCacheRoot = NULL;
    if (cacheDirArg != NULL) {
        cacheRoot = MakeAbsolutePathDup(cacheDirArg);
    } else {
        if (loader == NULL || loader->rootDir == NULL) {
            return -1;
        }
        cacheRoot = JoinPath(loader->rootDir, ".hop-cache");
    }
    if (cacheRoot == NULL) {
        return ErrorSimple("out of memory");
    }
    *outCacheRoot = cacheRoot;
    return 0;
}

int FindPackageIndex(const H2PackageLoader* loader, const H2Package* pkg) {
    uint32_t i;
    for (i = 0; i < loader->packageLen; i++) {
        if (&loader->packages[i] == pkg) {
            return (int)i;
        }
    }
    return -1;
}

static const char* _Nullable FindPreferredImportPath(
    const H2PackageLoader* loader, const H2Package* pkg) {
    const char* best = NULL;
    uint32_t    i;
    for (i = 0; i < loader->packageLen; i++) {
        const H2Package* src = &loader->packages[i];
        uint32_t         j;
        for (j = 0; j < src->importLen; j++) {
            if (src->imports[j].target != pkg) {
                continue;
            }
            if (best == NULL || strlen(src->imports[j].path) < strlen(best)
                || (strlen(src->imports[j].path) == strlen(best)
                    && strcmp(src->imports[j].path, best) < 0))
            {
                best = src->imports[j].path;
            }
        }
    }
    return best;
}

static char* _Nullable BuildPackageKey(const H2PackageLoader* loader, const H2Package* pkg) {
    const char* hashSource = pkg->dirPath;
    const char* keyHint = FindPreferredImportPath(loader, pkg);
    char*       keyBase = NULL;
    char*       out = NULL;
    uint64_t    h;
    int         n;
    if (pkg->fileLen == 1) {
        hashSource = pkg->files[0].path;
    }
    keyBase = BuildSanitizedIdent(keyHint != NULL ? keyHint : pkg->name, "pkg");
    if (keyBase == NULL) {
        return NULL;
    }
    h = HashFNV1a64(hashSource);
    out = (char*)malloc(strlen(keyBase) + 1u + 16u + 1u);
    if (out == NULL) {
        free(keyBase);
        return NULL;
    }
    n = snprintf(
        out, strlen(keyBase) + 1u + 16u + 1u, "%s-%016llx", keyBase, (unsigned long long)h);
    free(keyBase);
    if (n <= 0) {
        free(out);
        return NULL;
    }
    return out;
}

static char* _Nullable BuildPackageLinkPrefix(const H2Package* pkg) {
    (void)pkg;
    return BuildSanitizedIdent(pkg->name, "pkg");
}

static char* _Nullable BuildPackageMacro(const char* key, const char* suffix) {
    char*  sanitized = BuildSanitizedIdent(key, "pkg");
    size_t keyLen;
    size_t suffixLen;
    char*  out;
    size_t i;
    if (sanitized == NULL) {
        return NULL;
    }
    keyLen = strlen(sanitized);
    suffixLen = strlen(suffix);
    out = (char*)malloc(keyLen + suffixLen + 1u);
    if (out == NULL) {
        free(sanitized);
        return NULL;
    }
    for (i = 0; i < keyLen; i++) {
        char ch = sanitized[i];
        if (ch >= 'a' && ch <= 'z') {
            ch = (char)('A' + (ch - 'a'));
        }
        out[i] = ch;
    }
    memcpy(out + keyLen, suffix, suffixLen + 1u);
    free(sanitized);
    return out;
}

static H2PackageArtifact* _Nullable FindArtifactByPkg(
    H2PackageArtifact* _Nullable artifacts, uint32_t artifactLen, const H2Package* _Nullable pkg) {
    uint32_t i;
    if (artifacts == NULL || pkg == NULL) {
        return NULL;
    }
    for (i = 0; i < artifactLen; i++) {
        if (artifacts[i].pkg == pkg) {
            return &artifacts[i];
        }
    }
    return NULL;
}

static int BuildToolchainSignature(
    const H2PackageLoader* loader, const char* libDir, char** outSignature) {
    H2StringBuilder b = { 0 };
    char*           sig;
    char            tmp[64];
    int             n;
    *outSignature = NULL;
    n = snprintf(tmp, sizeof(tmp), "%d", H2_VERSION);
    if (n <= 0) {
        return -1;
    }
    if (SBAppendCStr(&b, "hop_version=") != 0 || SBAppend(&b, tmp, (uint32_t)n) != 0) {
        free(b.v);
        return -1;
    }
    if (SBAppendCStr(&b, ";source_hash=") != 0 || SBAppendCStr(&b, H2_SOURCE_HASH) != 0
        || SBAppendCStr(&b, ";backend=c;platform=") != 0
        || SBAppendCStr(&b, loader->platformTarget) != 0 || SBAppendCStr(&b, ";arch=") != 0
        || SBAppendCStr(&b, loader->archTarget) != 0 || SBAppendCStr(&b, ";testing=") != 0
        || SBAppendCStr(&b, loader->testingBuild ? "1" : "0") != 0
        || SBAppendCStr(&b, ";cc=cc;-std=c11;-g;-w;lib=") != 0 || SBAppendCStr(&b, libDir) != 0)
    {
        free(b.v);
        return -1;
    }
    sig = SBFinish(&b, NULL);
    if (sig == NULL) {
        return -1;
    }
    *outSignature = sig;
    return 0;
}

static int ToolchainSignatureMatches(const char* sigPath, const char* signature) {
    FILE*    f;
    long     flen;
    char*    actual;
    size_t   nread;
    uint32_t expectedLen = (uint32_t)strlen(signature);
    f = fopen(sigPath, "rb");
    if (f == NULL) {
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    flen = ftell(f);
    if (flen < 0 || (uint64_t)flen > (uint64_t)UINT32_MAX) {
        fclose(f);
        return 0;
    }
    if ((uint32_t)flen != expectedLen) {
        fclose(f);
        return 0;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    actual = (char*)malloc((size_t)expectedLen);
    if (actual == NULL) {
        fclose(f);
        return 0;
    }
    nread = fread(actual, 1u, (size_t)expectedLen, f);
    fclose(f);
    if (nread != (size_t)expectedLen || memcmp(actual, signature, (size_t)expectedLen) != 0) {
        free(actual);
        return 0;
    }
    free(actual);
    return 1;
}

static int PackageNewestSourceMtime(const H2Package* pkg, uint64_t* outMtimeNs) {
    uint64_t maxMtime = 0;
    uint64_t mt;
    uint32_t i;
    if (GetFileMtimeNs(pkg->dirPath, &maxMtime) != 0) {
        return -1;
    }
    for (i = 0; i < pkg->fileLen; i++) {
        if (GetFileMtimeNs(pkg->files[i].path, &mt) != 0) {
            return -1;
        }
        if (mt > maxMtime) {
            maxMtime = mt;
        }
    }
    *outMtimeNs = maxMtime;
    return 0;
}

static int PackageHasUnsupportedImportedPubGlobals(const H2Package* pkg) {
    uint32_t i;
    for (i = 0; i < pkg->importLen; i++) {
        const H2ImportRef* imp = &pkg->imports[i];
        const H2Package*   dep = imp->target;
        uint32_t           j;
        if (dep == NULL) {
            return ErrorSimple("internal error: unresolved import");
        }
        for (j = 0; j < dep->pubDeclLen; j++) {
            if (dep->pubDecls[j].kind == H2Ast_VAR || dep->pubDecls[j].kind == H2Ast_CONST) {
                const H2ParsedFile* file = &pkg->files[imp->fileIndex];
                return Errorf(
                    file->path,
                    file->source,
                    imp->start,
                    imp->end,
                    "imported public globals are not supported in cached multi-object mode");
            }
        }
    }
    return 0;
}

static int IsPackageArtifactUpToDate(
    const H2Package*   pkg,
    H2PackageArtifact* artifact,
    H2PackageArtifact* artifacts,
    uint32_t           artifactLen,
    const char*        toolchainSignature) {
    struct stat st;
    uint64_t    objMtime;
    uint64_t    srcMtime;
    uint32_t    i;
    if (stat(artifact->cPath, &st) != 0 || !S_ISREG(st.st_mode)) {
        return 0;
    }
    if (stat(artifact->oPath, &st) != 0 || !S_ISREG(st.st_mode)) {
        return 0;
    }
    objMtime = StatMtimeNs(&st);
    if (!ToolchainSignatureMatches(artifact->sigPath, toolchainSignature)) {
        return 0;
    }
    if (PackageNewestSourceMtime(pkg, &srcMtime) != 0 || srcMtime > objMtime) {
        return 0;
    }
    for (i = 0; i < pkg->importLen; i++) {
        H2PackageArtifact* depArtifact = FindArtifactByPkg(
            artifacts, artifactLen, pkg->imports[i].target);
        uint64_t depMtime = 0;
        if (depArtifact == NULL) {
            return 0;
        }
        if (depArtifact->objMtimeNs > 0) {
            depMtime = depArtifact->objMtimeNs;
        } else if (GetFileMtimeNs(depArtifact->oPath, &depMtime) != 0) {
            return 0;
        }
        if (depMtime > objMtime) {
            return 0;
        }
    }
    artifact->objMtimeNs = objMtime;
    return 1;
}

static void RestoreImportOverrides(
    H2AliasOverride* _Nullable aliasOverrides,
    uint32_t aliasOverrideLen,
    H2ImportSymbolOverride* _Nullable symbolOverrides,
    uint32_t symbolOverrideLen) {
    uint32_t i;
    for (i = 0; aliasOverrides != NULL && i < aliasOverrideLen; i++) {
        aliasOverrides[i].imp->alias = aliasOverrides[i].oldAlias;
        free(aliasOverrides[i].newAlias);
    }
    for (i = 0; symbolOverrides != NULL && i < symbolOverrideLen; i++) {
        symbolOverrides[i].sym->qualifiedName = symbolOverrides[i].oldQualifiedName;
        free(symbolOverrides[i].newQualifiedName);
    }
    free(aliasOverrides);
    free(symbolOverrides);
}

static int ApplyLinkPrefixImportOverrides(
    H2PackageLoader*         loader,
    H2PackageArtifact*       artifacts,
    uint32_t                 artifactLen,
    H2AliasOverride**        outAliasOverrides,
    uint32_t*                outAliasOverrideLen,
    H2ImportSymbolOverride** outSymbolOverrides,
    uint32_t*                outSymbolOverrideLen) {
    H2AliasOverride*        aliasOverrides = NULL;
    H2ImportSymbolOverride* symbolOverrides = NULL;
    uint32_t                aliasCap = 0;
    uint32_t                symbolCap = 0;
    uint32_t                aliasLen = 0;
    uint32_t                symbolLen = 0;
    uint32_t                i;
    for (i = 0; i < loader->packageLen; i++) {
        aliasCap += loader->packages[i].importLen;
        symbolCap += loader->packages[i].importSymbolLen;
    }
    if (aliasCap > 0) {
        aliasOverrides = (H2AliasOverride*)calloc(aliasCap, sizeof(H2AliasOverride));
        if (aliasOverrides == NULL) {
            return ErrorSimple("out of memory");
        }
    }
    if (symbolCap > 0) {
        symbolOverrides = (H2ImportSymbolOverride*)calloc(
            symbolCap, sizeof(H2ImportSymbolOverride));
        if (symbolOverrides == NULL) {
            free(aliasOverrides);
            return ErrorSimple("out of memory");
        }
    }

    for (i = 0; i < loader->packageLen; i++) {
        H2Package* pkg = &loader->packages[i];
        uint32_t   j;
        for (j = 0; j < pkg->importLen; j++) {
            H2ImportRef*       imp = &pkg->imports[j];
            H2PackageArtifact* depArtifact = FindArtifactByPkg(artifacts, artifactLen, imp->target);
            char*              newAlias;
            if (depArtifact == NULL) {
                RestoreImportOverrides(aliasOverrides, aliasLen, symbolOverrides, symbolLen);
                return ErrorSimple("internal error: unresolved import artifact");
            }
            newAlias = H2CDupCStr(depArtifact->linkPrefix);
            if (newAlias == NULL) {
                RestoreImportOverrides(aliasOverrides, aliasLen, symbolOverrides, symbolLen);
                return ErrorSimple("out of memory");
            }
            if (aliasOverrides == NULL) {
                free(newAlias);
                RestoreImportOverrides(aliasOverrides, aliasLen, symbolOverrides, symbolLen);
                return ErrorSimple("out of memory");
            }
            aliasOverrides[aliasLen].imp = imp;
            aliasOverrides[aliasLen].oldAlias = imp->alias;
            aliasOverrides[aliasLen].newAlias = newAlias;
            imp->alias = newAlias;
            aliasLen++;
        }
    }

    for (i = 0; i < loader->packageLen; i++) {
        H2Package* pkg = &loader->packages[i];
        uint32_t   j;
        for (j = 0; j < pkg->importSymbolLen; j++) {
            H2ImportSymbolRef* sym = &pkg->importSymbols[j];
            H2ImportRef*       imp;
            char*              newQualifiedName = NULL;
            if (sym->importIndex >= pkg->importLen) {
                RestoreImportOverrides(aliasOverrides, aliasLen, symbolOverrides, symbolLen);
                return ErrorSimple("internal error: invalid import symbol mapping");
            }
            imp = &pkg->imports[sym->importIndex];
            if (BuildPrefixedName(imp->alias, sym->sourceName, &newQualifiedName) != 0) {
                RestoreImportOverrides(aliasOverrides, aliasLen, symbolOverrides, symbolLen);
                return ErrorSimple("out of memory");
            }
            if (symbolOverrides == NULL) {
                free(newQualifiedName);
                RestoreImportOverrides(aliasOverrides, aliasLen, symbolOverrides, symbolLen);
                return ErrorSimple("out of memory");
            }
            symbolOverrides[symbolLen].sym = sym;
            symbolOverrides[symbolLen].oldQualifiedName = sym->qualifiedName;
            symbolOverrides[symbolLen].newQualifiedName = newQualifiedName;
            sym->qualifiedName = newQualifiedName;
            symbolLen++;
        }
    }

    *outAliasOverrides = aliasOverrides;
    *outAliasOverrideLen = aliasLen;
    *outSymbolOverrides = symbolOverrides;
    *outSymbolOverrideLen = symbolLen;
    return 0;
}

static int EmitPackageArtifact(
    H2PackageLoader*        loader,
    const H2Package*        pkg,
    H2PackageArtifact*      artifact,
    H2PackageArtifact*      artifacts,
    uint32_t                artifactLen,
    const char*             libDir,
    const char*             cachePkgDir,
    const char*             toolchainSignature,
    const H2CodegenBackend* backend) {
    char*             source = NULL;
    uint32_t          sourceLen = 0;
    uint32_t          ownDeclStartOffset = 0;
    H2CodegenUnit     unit;
    H2CodegenOptions  codegenOptions = { 0 };
    H2Diag            diag = { 0 };
    H2CodegenArtifact outArtifact = { 0 };
    char*             headerGuard = NULL;
    char*             implMacro = NULL;
    H2StringBuilder   cBuilder = { 0 };
    char*             cSource = NULL;
    const char*       ccArgv[16];
    uint32_t          i;
    int               rc = -1;

    if (PackageHasUnsupportedImportedPubGlobals(pkg) != 0) {
        return -1;
    }
    if (BuildCombinedPackageSource(loader, pkg, 1, &source, &sourceLen, NULL, &ownDeclStartOffset)
        != 0)
    {
        return -1;
    }
    if (source == NULL) {
        return ErrorSimple("out of memory");
    }

    unit.packageName = artifact->linkPrefix;
    unit.source = source;
    unit.sourceLen = sourceLen;
    unit.platformTarget = loader->platformTarget;
    unit.mirProgram = NULL;
    unit.foreignLinkage = NULL;
    unit.usesPlatform = PackageHasPlatformImport(pkg) ? 1u : 0u;
    headerGuard = BuildPackageMacro(artifact->key, "_H");
    implMacro = BuildPackageMacro(artifact->key, "_IMPL");
    if (headerGuard == NULL || implMacro == NULL) {
        ErrorSimple("out of memory");
        goto end;
    }
    codegenOptions.headerGuard = headerGuard;
    codegenOptions.implMacro = implMacro;
    codegenOptions.emitNodeStartOffset = ownDeclStartOffset;
    codegenOptions.emitNodeStartOffsetEnabled = 1;
    codegenOptions.arenaGrow = CodegenArenaGrow;
    codegenOptions.arenaFree = CodegenArenaFree;

    if (backend->emit(backend, &unit, &codegenOptions, &outArtifact, &diag) != 0) {
        if (diag.code != H2Diag_NONE) {
            int diagStatus;
            if (pkg->fileLen == 1 && pkg->importLen == 0) {
                diagStatus = PrintHOPDiagLineCol(
                    pkg->files[0].path, pkg->files[0].source, &diag, 1);
            } else {
                diagStatus = PrintHOPDiag(pkg->dirPath, source, &diag, 1);
            }
            if (!(diagStatus == 0 && outArtifact.data != NULL)) {
                goto end;
            }
        } else {
            ErrorSimple("codegen failed");
            goto end;
        }
    }
    if (outArtifact.isBinary) {
        ErrorSimple("internal error: compile pipeline received binary codegen artifact");
        goto end;
    }

    for (i = 0; i < pkg->importLen; i++) {
        const H2Package*   dep = pkg->imports[i].target;
        H2PackageArtifact* depArtifact;
        uint32_t           j;
        int                alreadyIncluded = 0;
        if (dep == NULL || dep->pubDeclLen == 0) {
            continue;
        }
        depArtifact = FindArtifactByPkg(artifacts, artifactLen, dep);
        if (depArtifact == NULL) {
            ErrorSimple("internal error: unresolved import artifact");
            goto end;
        }
        for (j = 0; j < i; j++) {
            const H2Package* prevDep = pkg->imports[j].target;
            if (prevDep != NULL && prevDep == dep) {
                alreadyIncluded = 1;
                break;
            }
        }
        if (alreadyIncluded) {
            continue;
        }
        if (SBAppendCStr(&cBuilder, "#include <") != 0
            || SBAppendCStr(&cBuilder, depArtifact->key) != 0
            || SBAppendCStr(&cBuilder, "/pkg.c>\n") != 0)
        {
            ErrorSimple("out of memory");
            goto end;
        }
    }
    if (cBuilder.len > 0 && SBAppendCStr(&cBuilder, "\n") != 0) {
        ErrorSimple("out of memory");
        goto end;
    }
    if (SBAppend(&cBuilder, (const char*)outArtifact.data, outArtifact.len) != 0) {
        ErrorSimple("out of memory");
        goto end;
    }
    cSource = SBFinish(&cBuilder, NULL);
    if (cSource == NULL) {
        ErrorSimple("out of memory");
        goto end;
    }

    if (EnsureDirRecursive(artifact->cacheDir) != 0) {
        ErrorSimple("failed to create cache directory");
        goto end;
    }
    if (WriteOutput(artifact->cPath, cSource, (uint32_t)strlen(cSource)) != 0) {
        ErrorSimple("failed to write cached C source");
        goto end;
    }

    ccArgv[0] = "cc";
    ccArgv[1] = "-std=c11";
    ccArgv[2] = "-g";
    ccArgv[3] = "-w";
    ccArgv[4] = "-isystem";
    ccArgv[5] = libDir;
    ccArgv[6] = "-I";
    ccArgv[7] = cachePkgDir;
    ccArgv[8] = "-D";
    ccArgv[9] = implMacro;
    ccArgv[10] = "-c";
    ccArgv[11] = artifact->cPath;
    ccArgv[12] = "-o";
    ccArgv[13] = artifact->oPath;
    ccArgv[14] = NULL;
    if (RunCommand(ccArgv) != 0) {
        ErrorSimple("C compilation failed");
        goto end;
    }

    if (WriteOutput(artifact->sigPath, toolchainSignature, (uint32_t)strlen(toolchainSignature))
        != 0)
    {
        ErrorSimple("failed to write cache toolchain signature");
        goto end;
    }
    if (GetFileMtimeNs(artifact->oPath, &artifact->objMtimeNs) != 0) {
        ErrorSimple("failed to stat cached object");
        goto end;
    }
    rc = 0;

end:
    free(headerGuard);
    free(implMacro);
    free(outArtifact.data);
    free(source);
    free(cSource);
    free(cBuilder.v);
    return rc;
}

static int VisitTopoPackage(
    const H2PackageLoader* loader,
    uint32_t               pkgIndex,
    uint8_t*               state,
    uint32_t*              order,
    uint32_t*              orderLen) {
    const H2Package* pkg = &loader->packages[pkgIndex];
    uint32_t         i;
    if (state[pkgIndex] == 2) {
        return 0;
    }
    if (state[pkgIndex] == 1) {
        return ErrorSimple("import cycle detected");
    }
    state[pkgIndex] = 1;
    for (i = 0; i < pkg->importLen; i++) {
        int depIndex = FindPackageIndex(loader, pkg->imports[i].target);
        if (depIndex < 0) {
            return ErrorSimple("internal error: unresolved import");
        }
        if (VisitTopoPackage(loader, (uint32_t)depIndex, state, order, orderLen) != 0) {
            return -1;
        }
    }
    state[pkgIndex] = 2;
    order[(*orderLen)++] = pkgIndex;
    return 0;
}

static int BuildPackageTopologicalOrder(
    const H2PackageLoader* loader,
    uint32_t*              outOrder,
    uint32_t               outOrderCap,
    uint32_t*              outOrderLen) {
    uint8_t* state = NULL;
    uint32_t i;
    *outOrderLen = 0;
    if (outOrderCap < loader->packageLen) {
        return -1;
    }
    state = (uint8_t*)calloc(loader->packageLen, sizeof(uint8_t));
    if (state == NULL) {
        return ErrorSimple("out of memory");
    }
    for (i = 0; i < loader->packageLen; i++) {
        if (VisitTopoPackage(loader, i, state, outOrder, outOrderLen) != 0) {
            free(state);
            return -1;
        }
    }
    free(state);
    return 0;
}

static int BuildCachedPackageArtifacts(
    H2PackageLoader* loader,
    const char* _Nullable cacheDirArg,
    const char*         libDir,
    H2PackageArtifact** outArtifacts,
    uint32_t*           outArtifactLen) {
    H2PackageArtifact*      artifacts = NULL;
    uint32_t                artifactLen = loader->packageLen;
    char*                   cacheRoot = NULL;
    char*                   cacheV1Dir = NULL;
    char*                   cachePkgDir = NULL;
    char*                   toolchainSignature = NULL;
    const H2CodegenBackend* backend;
    uint32_t*               topoOrder = NULL;
    uint32_t                topoOrderLen = 0;
    H2AliasOverride*        aliasOverrides = NULL;
    uint32_t                aliasOverrideLen = 0;
    H2ImportSymbolOverride* symbolOverrides = NULL;
    uint32_t                symbolOverrideLen = 0;
    uint32_t                i;

    *outArtifacts = NULL;
    *outArtifactLen = 0;

    backend = H2CodegenFindBackend("c");
    if (backend == NULL) {
        return ErrorSimple("unknown backend: c");
    }

    if (ResolveCacheRoot(loader, cacheDirArg, &cacheRoot) != 0) {
        return -1;
    }
    cacheV1Dir = JoinPath(cacheRoot, "v1");
    cachePkgDir = cacheV1Dir != NULL ? JoinPath(cacheV1Dir, "pkg") : NULL;
    if (cacheV1Dir == NULL || cachePkgDir == NULL) {
        free(cachePkgDir);
        free(cacheV1Dir);
        free(cacheRoot);
        return ErrorSimple("out of memory");
    }
    if (EnsureDirRecursive(cachePkgDir) != 0) {
        free(cachePkgDir);
        free(cacheV1Dir);
        free(cacheRoot);
        return ErrorSimple("failed to create cache directory");
    }

    artifacts = (H2PackageArtifact*)calloc(artifactLen, sizeof(H2PackageArtifact));
    topoOrder = (uint32_t*)calloc(artifactLen, sizeof(uint32_t));
    if ((artifactLen > 0 && artifacts == NULL) || (artifactLen > 0 && topoOrder == NULL)) {
        free(topoOrder);
        free(artifacts);
        free(cachePkgDir);
        free(cacheV1Dir);
        free(cacheRoot);
        return ErrorSimple("out of memory");
    }

    for (i = 0; i < artifactLen; i++) {
        H2PackageArtifact* a = &artifacts[i];
        a->pkg = &loader->packages[i];
        a->pkgIndex = i;
        a->key = BuildPackageKey(loader, a->pkg);
        a->linkPrefix = BuildPackageLinkPrefix(a->pkg);
        if (a->key == NULL || a->linkPrefix == NULL) {
            goto fail;
        }
        a->cacheDir = JoinPath(cachePkgDir, a->key);
        a->cPath = a->cacheDir != NULL ? JoinPath(a->cacheDir, "pkg.c") : NULL;
        a->oPath = a->cacheDir != NULL ? JoinPath(a->cacheDir, "pkg.o") : NULL;
        a->sigPath = a->cacheDir != NULL ? JoinPath(a->cacheDir, "toolchain.sig") : NULL;
        if (a->cacheDir == NULL || a->cPath == NULL || a->oPath == NULL || a->sigPath == NULL) {
            goto fail;
        }
    }

    if (BuildToolchainSignature(loader, libDir, &toolchainSignature) != 0) {
        goto fail;
    }
    if (BuildPackageTopologicalOrder(loader, topoOrder, artifactLen, &topoOrderLen) != 0) {
        goto fail;
    }
    if (ApplyLinkPrefixImportOverrides(
            loader,
            artifacts,
            artifactLen,
            &aliasOverrides,
            &aliasOverrideLen,
            &symbolOverrides,
            &symbolOverrideLen)
        != 0)
    {
        goto fail;
    }

    for (i = 0; i < topoOrderLen; i++) {
        uint32_t           pkgIndex = topoOrder[i];
        H2PackageArtifact* artifact = &artifacts[pkgIndex];
        if (IsPackageArtifactUpToDate(
                artifact->pkg, artifact, artifacts, artifactLen, toolchainSignature))
        {
            continue;
        }
        if (EmitPackageArtifact(
                loader,
                artifact->pkg,
                artifact,
                artifacts,
                artifactLen,
                libDir,
                cachePkgDir,
                toolchainSignature,
                backend)
            != 0)
        {
            goto fail;
        }
    }

    RestoreImportOverrides(aliasOverrides, aliasOverrideLen, symbolOverrides, symbolOverrideLen);
    free(topoOrder);
    free(toolchainSignature);
    free(cachePkgDir);
    free(cacheV1Dir);
    free(cacheRoot);
    *outArtifacts = artifacts;
    *outArtifactLen = artifactLen;
    return 0;

fail:
    RestoreImportOverrides(aliasOverrides, aliasOverrideLen, symbolOverrides, symbolOverrideLen);
    free(topoOrder);
    free(toolchainSignature);
    free(cachePkgDir);
    free(cacheV1Dir);
    free(cacheRoot);
    FreePackageArtifacts(artifacts, artifactLen);
    return -1;
}

static int BuildCachedPlatformObject(
    const H2PackageLoader* loader,
    const char* _Nullable cacheDirArg,
    const char* libDir,
    const char* _Nullable platformPath,
    const char* _Nullable toolchainSignature,
    char** _Nullable outPlatformObjPath) {
    char*       cacheRoot = NULL;
    char*       cacheV1Dir = NULL;
    char*       cachePlatformDir = NULL;
    char*       cachePlatformTargetDir = NULL;
    char*       platformObjPath = NULL;
    char*       platformSigPath = NULL;
    uint64_t    srcMtimeNs = 0;
    uint64_t    objMtimeNs = 0;
    const char* ccArgv[12];
    int         isUpToDate = 0;

    if (loader == NULL || loader->platformTarget == NULL || libDir == NULL || platformPath == NULL
        || toolchainSignature == NULL || outPlatformObjPath == NULL)
    {
        return -1;
    }
    *outPlatformObjPath = NULL;
    if (ResolveCacheRoot(loader, cacheDirArg, &cacheRoot) != 0) {
        return -1;
    }
    cacheV1Dir = JoinPath(cacheRoot, "v1");
    cachePlatformDir = cacheV1Dir != NULL ? JoinPath(cacheV1Dir, "platform") : NULL;
    cachePlatformTargetDir =
        cachePlatformDir != NULL ? JoinPath(cachePlatformDir, loader->platformTarget) : NULL;
    platformObjPath =
        cachePlatformTargetDir != NULL ? JoinPath(cachePlatformTargetDir, "platform.o") : NULL;
    platformSigPath =
        cachePlatformTargetDir != NULL ? JoinPath(cachePlatformTargetDir, "toolchain.sig") : NULL;
    if (cacheV1Dir == NULL || cachePlatformDir == NULL || cachePlatformTargetDir == NULL
        || platformObjPath == NULL || platformSigPath == NULL)
    {
        ErrorSimple("out of memory");
        goto fail;
    }
    if (EnsureDirRecursive(cachePlatformTargetDir) != 0) {
        ErrorSimple("failed to create cache directory");
        goto fail;
    }

    if (ToolchainSignatureMatches(platformSigPath, toolchainSignature)
        && GetFileMtimeNs(platformPath, &srcMtimeNs) == 0
        && GetFileMtimeNs(platformObjPath, &objMtimeNs) == 0 && srcMtimeNs <= objMtimeNs)
    {
        isUpToDate = 1;
    }

    if (!isUpToDate) {
        ccArgv[0] = "cc";
        ccArgv[1] = "-std=c11";
        ccArgv[2] = "-g";
        ccArgv[3] = "-w";
        ccArgv[4] = "-isystem";
        ccArgv[5] = libDir;
        ccArgv[6] = "-c";
        ccArgv[7] = platformPath;
        ccArgv[8] = "-o";
        ccArgv[9] = platformObjPath;
        ccArgv[10] = NULL;
        if (RunCommand(ccArgv) != 0) {
            ErrorSimple("C compilation failed");
            goto fail;
        }
        if (WriteOutput(platformSigPath, toolchainSignature, (uint32_t)strlen(toolchainSignature))
            != 0)
        {
            ErrorSimple("failed to write cache toolchain signature");
            goto fail;
        }
    }

    free(platformSigPath);
    free(cachePlatformTargetDir);
    free(cachePlatformDir);
    free(cacheV1Dir);
    free(cacheRoot);
    *outPlatformObjPath = platformObjPath;
    return 0;

fail:
    free(platformObjPath);
    free(platformSigPath);
    free(cachePlatformTargetDir);
    free(cachePlatformDir);
    free(cacheV1Dir);
    free(cacheRoot);
    return -1;
}

static int BuildCachedBuiltinObject(
    const H2PackageLoader* loader,
    const char* _Nullable cacheDirArg,
    const char* libDir,
    const char* _Nullable builtinPath,
    const char* _Nullable builtinHeaderPath,
    const char* _Nullable toolchainSignature,
    char** _Nullable outBuiltinObjPath) {
    char*       cacheRoot = NULL;
    char*       cacheV1Dir = NULL;
    char*       cacheBuiltinDir = NULL;
    char*       cacheBuiltinTargetDir = NULL;
    char*       builtinObjPath = NULL;
    char*       builtinSigPath = NULL;
    uint64_t    srcMtimeNs = 0;
    uint64_t    headerMtimeNs = 0;
    uint64_t    objMtimeNs = 0;
    const char* ccArgv[12];
    int         isUpToDate = 0;

    if (loader == NULL || loader->platformTarget == NULL || libDir == NULL || builtinPath == NULL
        || builtinHeaderPath == NULL || toolchainSignature == NULL || outBuiltinObjPath == NULL)
    {
        return -1;
    }
    *outBuiltinObjPath = NULL;
    if (ResolveCacheRoot(loader, cacheDirArg, &cacheRoot) != 0) {
        return -1;
    }
    cacheV1Dir = JoinPath(cacheRoot, "v1");
    cacheBuiltinDir = cacheV1Dir != NULL ? JoinPath(cacheV1Dir, "builtin") : NULL;
    cacheBuiltinTargetDir =
        cacheBuiltinDir != NULL ? JoinPath(cacheBuiltinDir, loader->platformTarget) : NULL;
    builtinObjPath =
        cacheBuiltinTargetDir != NULL ? JoinPath(cacheBuiltinTargetDir, "builtin.o") : NULL;
    builtinSigPath =
        cacheBuiltinTargetDir != NULL ? JoinPath(cacheBuiltinTargetDir, "toolchain.sig") : NULL;
    if (cacheV1Dir == NULL || cacheBuiltinDir == NULL || cacheBuiltinTargetDir == NULL
        || builtinObjPath == NULL || builtinSigPath == NULL)
    {
        ErrorSimple("out of memory");
        goto fail;
    }
    if (EnsureDirRecursive(cacheBuiltinTargetDir) != 0) {
        ErrorSimple("failed to create cache directory");
        goto fail;
    }

    if (ToolchainSignatureMatches(builtinSigPath, toolchainSignature)
        && GetFileMtimeNs(builtinPath, &srcMtimeNs) == 0
        && GetFileMtimeNs(builtinHeaderPath, &headerMtimeNs) == 0
        && GetFileMtimeNs(builtinObjPath, &objMtimeNs) == 0 && srcMtimeNs <= objMtimeNs
        && headerMtimeNs <= objMtimeNs)
    {
        isUpToDate = 1;
    }

    if (!isUpToDate) {
        ccArgv[0] = "cc";
        ccArgv[1] = "-std=c11";
        ccArgv[2] = "-g";
        ccArgv[3] = "-w";
        ccArgv[4] = "-isystem";
        ccArgv[5] = libDir;
        ccArgv[6] = "-c";
        ccArgv[7] = builtinPath;
        ccArgv[8] = "-o";
        ccArgv[9] = builtinObjPath;
        ccArgv[10] = NULL;
        if (RunCommand(ccArgv) != 0) {
            ErrorSimple("C compilation failed");
            goto fail;
        }
        if (WriteOutput(builtinSigPath, toolchainSignature, (uint32_t)strlen(toolchainSignature))
            != 0)
        {
            ErrorSimple("failed to write cache toolchain signature");
            goto fail;
        }
    }

    free(builtinSigPath);
    free(cacheBuiltinTargetDir);
    free(cacheBuiltinDir);
    free(cacheV1Dir);
    free(cacheRoot);
    *outBuiltinObjPath = builtinObjPath;
    return 0;

fail:
    free(builtinObjPath);
    free(builtinSigPath);
    free(cacheBuiltinTargetDir);
    free(cacheBuiltinDir);
    free(cacheV1Dir);
    free(cacheRoot);
    return -1;
}

static int BuildCachedWrapperObject(
    const H2PackageLoader* loader,
    const char* _Nullable cacheDirArg,
    const char* libDir,
    const char* wrapperLinkPrefix,
    const char* toolchainSignature,
    char**      outWrapperObjPath) {
    char*           cacheRoot = NULL;
    char*           cacheV1Dir = NULL;
    char*           cacheWrapperDir = NULL;
    char*           cacheWrapperTargetDir = NULL;
    char*           wrapperKey = NULL;
    char*           cacheWrapperEntryDir = NULL;
    char*           wrapperCPath = NULL;
    char*           wrapperOPath = NULL;
    char*           wrapperSigPath = NULL;
    H2StringBuilder wrapperBuilder = { 0 };
    char*           wrapperSource = NULL;
    uint32_t        wrapperSourceLen = 0;
    char*           existingSource = NULL;
    FILE*           existingSourceFile = NULL;
    long            existingSourceFileLen = 0;
    size_t          nread = 0;
    uint64_t        srcMtimeNs = 0;
    uint64_t        objMtimeNs = 0;
    const char*     ccArgv[12];
    int             sourceMatches = 0;
    int             isUpToDate = 0;

    *outWrapperObjPath = NULL;
    if (ResolveCacheRoot(loader, cacheDirArg, &cacheRoot) != 0) {
        return -1;
    }
    cacheV1Dir = JoinPath(cacheRoot, "v1");
    cacheWrapperDir = cacheV1Dir != NULL ? JoinPath(cacheV1Dir, "wrapper") : NULL;
    cacheWrapperTargetDir =
        cacheWrapperDir != NULL ? JoinPath(cacheWrapperDir, loader->platformTarget) : NULL;
    wrapperKey = BuildSanitizedIdent(wrapperLinkPrefix, "entry");
    cacheWrapperEntryDir =
        (cacheWrapperTargetDir != NULL && wrapperKey != NULL)
            ? JoinPath(cacheWrapperTargetDir, wrapperKey)
            : NULL;
    wrapperCPath =
        cacheWrapperEntryDir != NULL ? JoinPath(cacheWrapperEntryDir, "wrapper.c") : NULL;
    wrapperOPath =
        cacheWrapperEntryDir != NULL ? JoinPath(cacheWrapperEntryDir, "wrapper.o") : NULL;
    wrapperSigPath =
        cacheWrapperEntryDir != NULL ? JoinPath(cacheWrapperEntryDir, "toolchain.sig") : NULL;
    if (cacheV1Dir == NULL || cacheWrapperDir == NULL || cacheWrapperTargetDir == NULL
        || wrapperKey == NULL || cacheWrapperEntryDir == NULL || wrapperCPath == NULL
        || wrapperOPath == NULL || wrapperSigPath == NULL)
    {
        ErrorSimple("out of memory");
        goto fail;
    }
    if (EnsureDirRecursive(cacheWrapperEntryDir) != 0) {
        ErrorSimple("failed to create cache directory");
        goto fail;
    }

    if (SBAppendCStr(&wrapperBuilder, "#include <builtin/builtin.h>\n\nvoid ") != 0
        || SBAppendCStr(&wrapperBuilder, wrapperLinkPrefix) != 0
        || SBAppendCStr(&wrapperBuilder, "__main(__hop_Context *context);\n\n") != 0
        || SBAppendCStr(&wrapperBuilder, "int hop_main(__hop_Context *context) { ") != 0
        || SBAppendCStr(&wrapperBuilder, wrapperLinkPrefix) != 0
        || SBAppendCStr(&wrapperBuilder, "__main(context); return 0; }\n") != 0)
    {
        ErrorSimple("out of memory");
        goto fail;
    }
    wrapperSource = SBFinish(&wrapperBuilder, NULL);
    wrapperBuilder.v = NULL;
    wrapperBuilder.len = 0;
    wrapperBuilder.cap = 0;
    if (wrapperSource == NULL) {
        ErrorSimple("out of memory");
        goto fail;
    }
    wrapperSourceLen = (uint32_t)strlen(wrapperSource);

    existingSourceFile = fopen(wrapperCPath, "rb");
    if (existingSourceFile != NULL && fseek(existingSourceFile, 0, SEEK_END) == 0) {
        existingSourceFileLen = ftell(existingSourceFile);
        if (existingSourceFileLen >= 0 && (uint64_t)existingSourceFileLen <= (uint64_t)UINT32_MAX
            && (uint32_t)existingSourceFileLen == wrapperSourceLen
            && fseek(existingSourceFile, 0, SEEK_SET) == 0)
        {
            if (wrapperSourceLen == 0) {
                sourceMatches = 1;
            } else {
                existingSource = (char*)malloc((size_t)wrapperSourceLen);
                if (existingSource != NULL) {
                    nread = fread(existingSource, 1u, (size_t)wrapperSourceLen, existingSourceFile);
                    if (nread == (size_t)wrapperSourceLen
                        && memcmp(existingSource, wrapperSource, wrapperSourceLen) == 0)
                    {
                        sourceMatches = 1;
                    }
                }
            }
        }
    }
    if (existingSourceFile != NULL) {
        fclose(existingSourceFile);
    }
    free(existingSource);
    existingSource = NULL;

    if (!sourceMatches) {
        if (WriteOutput(wrapperCPath, wrapperSource, wrapperSourceLen) != 0) {
            ErrorSimple("failed to write wrapper source");
            goto fail;
        }
    }

    if (sourceMatches && ToolchainSignatureMatches(wrapperSigPath, toolchainSignature)
        && GetFileMtimeNs(wrapperCPath, &srcMtimeNs) == 0
        && GetFileMtimeNs(wrapperOPath, &objMtimeNs) == 0 && srcMtimeNs <= objMtimeNs)
    {
        isUpToDate = 1;
    }

    if (!isUpToDate) {
        ccArgv[0] = "cc";
        ccArgv[1] = "-std=c11";
        ccArgv[2] = "-g";
        ccArgv[3] = "-w";
        ccArgv[4] = "-isystem";
        ccArgv[5] = libDir;
        ccArgv[6] = "-c";
        ccArgv[7] = wrapperCPath;
        ccArgv[8] = "-o";
        ccArgv[9] = wrapperOPath;
        ccArgv[10] = NULL;
        if (RunCommand(ccArgv) != 0) {
            ErrorSimple("C compilation failed");
            goto fail;
        }
        if (WriteOutput(wrapperSigPath, toolchainSignature, (uint32_t)strlen(toolchainSignature))
            != 0)
        {
            ErrorSimple("failed to write cache toolchain signature");
            goto fail;
        }
    }

    free(wrapperSource);
    free(wrapperSigPath);
    free(wrapperCPath);
    free(cacheWrapperEntryDir);
    free(wrapperKey);
    free(cacheWrapperTargetDir);
    free(cacheWrapperDir);
    free(cacheV1Dir);
    free(cacheRoot);
    *outWrapperObjPath = wrapperOPath;
    return 0;

fail:
    free(existingSource);
    free(wrapperSource);
    free(wrapperBuilder.v);
    free(wrapperSigPath);
    free(wrapperOPath);
    free(wrapperCPath);
    free(cacheWrapperEntryDir);
    free(wrapperKey);
    free(cacheWrapperTargetDir);
    free(cacheWrapperDir);
    free(cacheV1Dir);
    free(cacheRoot);
    return -1;
}

static int IsLinkedOutputUpToDate(
    const char*              outExe,
    const char*              wrapperObjPath,
    const char*              builtinObjPath,
    const char*              platformObjPath,
    const H2PackageArtifact* artifacts,
    uint32_t                 artifactLen) {
    struct stat outSt;
    uint64_t    outMtimeNs;
    uint64_t    inputMtimeNs;
    uint32_t    i;

    if (stat(outExe, &outSt) != 0 || !S_ISREG(outSt.st_mode)) {
        return 0;
    }
    outMtimeNs = StatMtimeNs(&outSt);

    if (GetFileMtimeNs(wrapperObjPath, &inputMtimeNs) != 0 || inputMtimeNs > outMtimeNs) {
        return 0;
    }
    if (GetFileMtimeNs(builtinObjPath, &inputMtimeNs) != 0 || inputMtimeNs > outMtimeNs) {
        return 0;
    }
    if (GetFileMtimeNs(platformObjPath, &inputMtimeNs) != 0 || inputMtimeNs > outMtimeNs) {
        return 0;
    }
    for (i = 0; i < artifactLen; i++) {
        if (StrEq(artifacts[i].pkg->name, "platform")) {
            continue;
        }
        if (GetFileMtimeNs(artifacts[i].oPath, &inputMtimeNs) != 0 || inputMtimeNs > outMtimeNs) {
            return 0;
        }
    }
    return 1;
}

static int IsEvalPlatformTarget(const char* _Nullable platformTarget) {
    return platformTarget != NULL && StrEq(platformTarget, H2_EVAL_PLATFORM_TARGET);
}

static int IsWasmMinPlatformTarget(const char* _Nullable platformTarget) {
    return platformTarget != NULL && StrEq(platformTarget, H2_WASM_MIN_PLATFORM_TARGET);
}

static int IsPlaybitPlatformTarget(const char* _Nullable platformTarget) {
    return platformTarget != NULL && StrEq(platformTarget, H2_PLAYBIT_PLATFORM_TARGET);
}

/* Embedded cli-libc platform source — compiled alongside the generated HopHop
 * package. Provides runtime platform functions via libc and defines main()
 * which calls hop_main(). */
int CompileProgramInput(
    const H2PackageInput* input,
    const char*           outExe,
    const char* _Nullable platformTarget,
    const char* _Nullable archTarget,
    int testingBuild,
    const char* _Nullable cacheDirArg) {
    H2PackageLoader    loader = { 0 };
    H2Package*         entryPkg = NULL;
    int                loaderReady = 0;
    char*              libDir = NULL;
    char*              platformPath = NULL;
    char*              builtinPath = NULL;
    char*              builtinHeaderPath = NULL;
    char*              platformObjPath = NULL;
    char*              builtinObjPath = NULL;
    char*              wrapperObjPath = NULL;
    char*              toolchainSignature = NULL;
    H2PackageArtifact* artifacts = NULL;
    uint32_t           artifactLen = 0;
    H2PackageArtifact* entryArtifact;
    const char**       ccLinkArgv = NULL;
    uint32_t           i;
    int                rc = -1;

    if (IsWasmMinPlatformTarget(platformTarget) || IsPlaybitPlatformTarget(platformTarget)) {
        return ErrorSimple(
            "platform target %s is run-only; use `hop run --platform %s`",
            platformTarget,
            platformTarget);
    }
    if (!HasCBackendBuild()) {
        return ErrorCBackendDisabled();
    }

    if (LoadAndCheckPackageInput(
            input, platformTarget, archTarget, testingBuild, &loader, &entryPkg)
        != 0)
    {
        goto end;
    }
    loaderReady = 1;
    if (ValidateEntryMainSignature(entryPkg) != 0) {
        goto end;
    }
    if (IsEvalPlatformTarget(loader.platformTarget)) {
        ErrorSimple(
            "platform target %s is evaluator-only; use `hop run --platform %s`",
            loader.platformTarget,
            loader.platformTarget);
        goto end;
    }
    if (LoaderUsesWasmImportDirective(&loader)) {
        ErrorSimple(
            "native C compilation does not support @wasm_import; use `hop build --output-format "
            "c` or a Wasm toolchain");
        goto end;
    }
    if (ResolveLibDir(&libDir) != 0) {
        goto end;
    }
    if (ResolvePlatformPath(libDir, loader.platformTarget, &platformPath) != 0) {
        goto end;
    }
    if (ResolveBuiltinPath(libDir, &builtinPath, &builtinHeaderPath) != 0) {
        goto end;
    }
    if (BuildToolchainSignature(&loader, libDir, &toolchainSignature) != 0) {
        ErrorSimple("out of memory");
        goto end;
    }
    if (BuildCachedPlatformObject(
            &loader, cacheDirArg, libDir, platformPath, toolchainSignature, &platformObjPath)
        != 0)
    {
        goto end;
    }
    if (BuildCachedBuiltinObject(
            &loader,
            cacheDirArg,
            libDir,
            builtinPath,
            builtinHeaderPath,
            toolchainSignature,
            &builtinObjPath)
        != 0)
    {
        goto end;
    }
    if (BuildCachedPackageArtifacts(&loader, cacheDirArg, libDir, &artifacts, &artifactLen) != 0) {
        goto end;
    }
    entryArtifact = FindArtifactByPkg(artifacts, artifactLen, entryPkg);
    if (entryArtifact == NULL) {
        ErrorSimple("internal error: entry package artifact missing");
        goto end;
    }
    if (BuildCachedWrapperObject(
            &loader,
            cacheDirArg,
            libDir,
            entryArtifact->linkPrefix,
            toolchainSignature,
            &wrapperObjPath)
        != 0)
    {
        goto end;
    }

    if (IsLinkedOutputUpToDate(
            outExe, wrapperObjPath, builtinObjPath, platformObjPath, artifacts, artifactLen))
    {
        rc = 0;
        goto end;
    }

    ccLinkArgv = (const char**)calloc((size_t)artifactLen + 12u, sizeof(char*));
    if (ccLinkArgv == NULL) {
        ErrorSimple("out of memory");
        goto end;
    }
    i = 0;
    ccLinkArgv[i++] = "cc";
    ccLinkArgv[i++] = "-std=c11";
    ccLinkArgv[i++] = "-g";
    ccLinkArgv[i++] = "-w";
    ccLinkArgv[i++] = "-isystem";
    ccLinkArgv[i++] = libDir;
    ccLinkArgv[i++] = "-o";
    ccLinkArgv[i++] = outExe;
    ccLinkArgv[i++] = wrapperObjPath;
    {
        uint32_t j;
        for (j = 0; j < artifactLen; j++) {
            if (StrEq(artifacts[j].pkg->name, "platform")) {
                continue;
            }
            ccLinkArgv[i++] = artifacts[j].oPath;
        }
    }
    ccLinkArgv[i++] = builtinObjPath;
    ccLinkArgv[i++] = platformObjPath;
    ccLinkArgv[i] = NULL;
    if (RunCommand(ccLinkArgv) != 0) {
        ErrorSimple("C link failed");
        goto end;
    }

    rc = 0;

end:
    free(ccLinkArgv);
    free(wrapperObjPath);
    free(builtinObjPath);
    free(toolchainSignature);
    free(platformObjPath);
    free(builtinHeaderPath);
    free(builtinPath);
    free(platformPath);
    free(libDir);
    FreePackageArtifacts(artifacts, artifactLen);
    if (loaderReady) {
        FreeLoader(&loader);
    }
    return rc;
}

int CompileProgram(
    const char* entryPath,
    const char* outExe,
    const char* _Nullable platformTarget,
    const char* _Nullable archTarget,
    int testingBuild,
    const char* _Nullable cacheDirArg) {
    H2PackageInput input = { 0 };
    input.paths = &entryPath;
    input.pathLen = 1;
    return CompileProgramInput(
        &input, outExe, platformTarget, archTarget, testingBuild, cacheDirArg);
}

static int RunProgramC(
    const char* entryPath,
    const char* _Nullable platformTarget,
    const char* _Nullable archTarget,
    int testingBuild,
    const char* _Nullable cacheDirArg) {
    const char* tmpBase = getenv("TMPDIR");
    char        exeTemplate[PATH_MAX];
    int         n;
    int         fd;
    char* const execArgv[2] = { exeTemplate, NULL };

    if (tmpBase == NULL || tmpBase[0] == '\0') {
        tmpBase = "/tmp";
    }
    n = snprintf(exeTemplate, sizeof(exeTemplate), "%s/hop-run.XXXXXX", tmpBase);
    if (n <= 0 || (size_t)n >= sizeof(exeTemplate)) {
        return ErrorSimple("temporary path too long");
    }
    fd = mkstemp(exeTemplate);
    if (fd < 0) {
        return ErrorSimple("failed to create temporary output file");
    }
    close(fd);
    unlink(exeTemplate);

    if (CompileProgram(
            entryPath, exeTemplate, platformTarget, archTarget, testingBuild, cacheDirArg)
        != 0)
    {
        unlink(exeTemplate);
        return -1;
    }

    execv(exeTemplate, execArgv);
    unlink(exeTemplate);
    return ErrorSimple("failed to execute compiled program");
}

static int RunProgramWasmMin(
    const char* entryPath,
    const char* _Nullable platformTarget,
    const char* _Nullable archTarget,
    int testingBuild,
    const char* _Nullable cacheDirArg) {
    const char* tmpBase = getenv("TMPDIR");
    char        wasmTemplate[PATH_MAX];
    int         n;
    int         fd;
    char*       runnerPath = NULL;
    const char* runArgv[4];
    int         exitCode = 0;

    if (!HasWasmBackendBuild()) {
        return ErrorSimple("this hop build was compiled without the Wasm backend");
    }
    if (tmpBase == NULL || tmpBase[0] == '\0') {
        tmpBase = "/tmp";
    }
    n = snprintf(wasmTemplate, sizeof(wasmTemplate), "%s/hop-run-wasm.XXXXXX", tmpBase);
    if (n <= 0 || (size_t)n >= sizeof(wasmTemplate)) {
        return ErrorSimple("temporary path too long");
    }
    fd = mkstemp(wasmTemplate);
    if (fd < 0) {
        return ErrorSimple("failed to create temporary wasm output file");
    }
    close(fd);

    if (GeneratePackage(
            entryPath, "wasm", wasmTemplate, platformTarget, archTarget, testingBuild, cacheDirArg)
        != 0)
    {
        unlink(wasmTemplate);
        return -1;
    }
    if (ResolveRepoToolPath("tools/wasm_min_runner.js", &runnerPath) != 0) {
        unlink(wasmTemplate);
        return -1;
    }

    runArgv[0] = "node";
    runArgv[1] = runnerPath;
    runArgv[2] = wasmTemplate;
    runArgv[3] = NULL;
    if (RunCommandExitCode(runArgv, &exitCode) != 0) {
        free(runnerPath);
        unlink(wasmTemplate);
        return ErrorSimple("failed to execute wasm-min runner");
    }

    free(runnerPath);
    unlink(wasmTemplate);
    return exitCode;
}

static int RunProgramPlaybit(
    const char* entryPath,
    const char* _Nullable platformTarget,
    const char* _Nullable archTarget,
    int testingBuild,
    const char* _Nullable cacheDirArg) {
    const char* tmpBase = getenv("TMPDIR");
    char        wasmTemplate[PATH_MAX];
    int         n;
    int         fd;
    char*       pbPath = NULL;
    const char* runArgv[4];
    int         exitCode = 0;

    if (!HasWasmBackendBuild()) {
        return ErrorSimple("this hop build was compiled without the Wasm backend");
    }
    if (tmpBase == NULL || tmpBase[0] == '\0') {
        tmpBase = "/tmp";
    }
    // `pb run` only takes the prebuilt-wasm path when the input filename ends in `.wasm`.
    n = snprintf(wasmTemplate, sizeof(wasmTemplate), "%s/hop-run-playbit.XXXXXX.wasm", tmpBase);
    if (n <= 0 || (size_t)n >= sizeof(wasmTemplate)) {
        return ErrorSimple("temporary path too long");
    }
    fd = mkstemps(wasmTemplate, 5);
    if (fd < 0) {
        return ErrorSimple("failed to create temporary wasm output file");
    }
    close(fd);

    if (GeneratePackage(
            entryPath, "wasm", wasmTemplate, platformTarget, archTarget, testingBuild, cacheDirArg)
        != 0)
    {
        unlink(wasmTemplate);
        return -1;
    }
    if (ResolvePlaybitPbPath(&pbPath) != 0) {
        unlink(wasmTemplate);
        return -1;
    }

    runArgv[0] = pbPath;
    runArgv[1] = "run";
    runArgv[2] = wasmTemplate;
    runArgv[3] = NULL;
    if (RunCommandExitCode(runArgv, &exitCode) != 0) {
        free(pbPath);
        unlink(wasmTemplate);
        return ErrorSimple("failed to execute pb run");
    }

    free(pbPath);
    unlink(wasmTemplate);
    return exitCode;
}

int RunProgram(
    const char* entryPath,
    const char* _Nullable platformTarget,
    const char* _Nullable archTarget,
    int testingBuild,
    const char* _Nullable cacheDirArg) {
    if (IsEvalPlatformTarget(platformTarget)) {
        return RunProgramEval(entryPath, platformTarget, archTarget, testingBuild);
    }
    if (IsWasmMinPlatformTarget(platformTarget)) {
        return RunProgramWasmMin(entryPath, platformTarget, archTarget, testingBuild, cacheDirArg);
    }
    if (IsPlaybitPlatformTarget(platformTarget)) {
        return RunProgramPlaybit(entryPath, platformTarget, archTarget, testingBuild, cacheDirArg);
    }
    if (!HasCBackendBuild()) {
        return ErrorCBackendDisabled();
    }
    return RunProgramC(entryPath, platformTarget, archTarget, testingBuild, cacheDirArg);
}

H2_API_END
