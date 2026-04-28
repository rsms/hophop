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

static const char* ProgramBasename(const char* _Nullable argv0) {
    const char* slash;
#if defined(_WIN32)
    const char* backslash;
#endif
    if (argv0 == NULL || argv0[0] == '\0') {
        return "hop";
    }
    slash = strrchr(argv0, '/');
#if defined(_WIN32)
    backslash = strrchr(argv0, '\\');
    if (backslash != NULL && (slash == NULL || backslash > slash)) {
        slash = backslash;
    }
#endif
    return slash != NULL && slash[1] != '\0' ? slash + 1 : argv0;
}

typedef enum {
    BuildOutputFormat_EXECUTABLE = 0,
    BuildOutputFormat_C,
    BuildOutputFormat_MIR,
    BuildOutputFormat_TOKENS,
    BuildOutputFormat_AST,
} BuildOutputFormat;

static int ParseBuildOutputFormat(const char* name, BuildOutputFormat* outFormat) {
    if (StrEq(name, "executable")) {
        *outFormat = BuildOutputFormat_EXECUTABLE;
        return 0;
    }
    if (StrEq(name, "c")) {
        *outFormat = BuildOutputFormat_C;
        return 0;
    }
    if (StrEq(name, "mir")) {
        *outFormat = BuildOutputFormat_MIR;
        return 0;
    }
    if (StrEq(name, "tokens")) {
        *outFormat = BuildOutputFormat_TOKENS;
        return 0;
    }
    if (StrEq(name, "ast")) {
        *outFormat = BuildOutputFormat_AST;
        return 0;
    }
    return -1;
}

static void PrintUsage(const char* argv0) {
    const char* progname = ProgramBasename(argv0);
    fprintf(
        stderr,
        "usage:\n"
        "    %s --version\n"
        "    %s run [--platform <target>] [--arch <name>] [--cache-dir <dir>] "
        "[--diag-format <text|jsonl>] <pkgdir|srcfile>\n"
        "    %s fmt [--check] [<file-or-dir> ...]\n"
        "    %s build [-o output] [options] [<package>]\n"
        "    %s build [-o output] [options] <source-file> ...\n"
        "    %s check [--platform <target>] [--arch <name>] [--cache-dir <dir>] "
        "[--diag-format <text|jsonl>] "
        "[--no-import] "
        "<pkgdir|srcfile>\n"
        "    %s --version\n"
        "    %s --help\n"
        "\n"
        "<target> is one of: cli-libc (default), cli-eval, wasm-min, playbit\n"
        "`hop run` defaults to cli-eval when --platform is omitted\n",
        progname,
        progname,
        progname,
        progname,
        progname,
        progname,
        progname,
        progname);
}

static void PrintPlatformTargetList(void) {
    fprintf(stderr, "\n<target> is one of: cli-libc (default), cli-eval, wasm-min, playbit\n");
}

static void PrintSharedPlatformOptions(void) {
    fprintf(
        stderr,
        "\noptions:\n"
        "    --platform <target>   target platform\n"
        "    --arch <name>         target architecture\n"
        "    --cache-dir <dir>     compiler cache directory\n"
        "    --diag-format <fmt>   diagnostics output format (`text` or `jsonl`)\n");
}

static void PrintRunHelp(const char* argv0) {
    const char* progname = ProgramBasename(argv0);
    fprintf(
        stderr,
        "usage:\n"
        "    %s run [--platform <target>] [--arch <name>] [--cache-dir <dir>] "
        "[--diag-format <text|jsonl>] <pkgdir|srcfile>\n",
        progname);
    PrintSharedPlatformOptions();
    PrintPlatformTargetList();
    fprintf(stderr, "`%s run` defaults to cli-eval when --platform is omitted\n", progname);
}

static void PrintFmtHelp(const char* argv0) {
    const char* progname = ProgramBasename(argv0);
    fprintf(
        stderr,
        "usage:\n"
        "    %s fmt [--check] [<file-or-dir> ...]\n"
        "\n"
        "options:\n"
        "    --check               report files that need formatting without rewriting them\n",
        progname);
}

static void PrintBuildHelp(const char* argv0) {
    const char* progname = ProgramBasename(argv0);
    fprintf(
        stderr,
        "Build a program\n"
        "Usage: %s build [-o output] [options] [<package>]\n"
        "       %s build [-o output] [options] <source-file> ...\n"
        "Options:\n"
        "    -o, --output <file>        Write output here (`-` for stdout)\n"
        "    --platform <platform>      Target platform\n"
        "    --arch <name>              Target architecture\n"
        "    --output-format <format>   Output format (one of: executable, c, mir, tokens, ast)\n"
        "    --cache-dir <dir>          Use this cache directory instead of the default\n"
        "    --diag-format <fmt>        Diagnostics output format (one of: text, jsonl)\n"
        "    -h, --help                 Show help and exit\n"
        "<package>\n"
        "    If no package or <source-file> is specified, the current directory is\n"
        "    assumed to contain the package to build.\n"
        "<platform>\n"
        "    One of: cli-libc (default), cli-eval, wasm-min, playbit\n"
        "<format>\n"
        "    executable builds a usable program and is the default.\n",
        progname,
        progname);
}

static void PrintCheckHelp(const char* argv0) {
    const char* progname = ProgramBasename(argv0);
    fprintf(
        stderr,
        "usage:\n"
        "    %s check [--platform <target>] [--arch <name>] [--cache-dir <dir>] "
        "[--diag-format <text|jsonl>] "
        "[--no-import] "
        "<pkgdir|srcfile>\n",
        progname);
    PrintSharedPlatformOptions();
    fprintf(
        stderr, "    --no-import           typecheck one source file without package imports\n");
    PrintPlatformTargetList();
}

static int PrintCommandHelp(const char* argv0, const char* mode) {
    if (StrEq(mode, "run")) {
        PrintRunHelp(argv0);
        return 1;
    }
    if (StrEq(mode, "fmt")) {
        PrintFmtHelp(argv0);
        return 1;
    }
    if (StrEq(mode, "build")) {
        PrintBuildHelp(argv0);
        return 1;
    }
    if (StrEq(mode, "check")) {
        PrintCheckHelp(argv0);
        return 1;
    }
    return 0;
}

static int IsHelpFlag(const char* arg) {
    return StrEq(arg, "--help") || StrEq(arg, "-h");
}

static void PrintVersion(void) {
    fprintf(
        stdout,
        "HopHop compiler version %d (%s) eval%s\n",
        H2_VERSION,
        H2_SOURCE_HASH,
        H2_WITH_C_BACKEND ? " c11" : "");
}

static int ParseSharedCommandOptions(
    int          argc,
    char*        argv[],
    int          argi,
    const char** outPlatformTarget,
    const char** outArchTarget,
    const char** outCacheDirArg,
    const char** outDiagFormat,
    int* _Nullable outTestingBuild,
    int* _Nullable outHasPlatformTarget,
    int* _Nullable outNoImport) {
    while (argi < argc) {
        if (StrEq(argv[argi], "--platform")) {
            if (argi + 1 >= argc) {
                return -1;
            }
            *outPlatformTarget = argv[argi + 1];
            if (outHasPlatformTarget != NULL) {
                *outHasPlatformTarget = 1;
            }
            argi += 2;
            continue;
        }
        if (StrEq(argv[argi], "--arch")) {
            if (argi + 1 >= argc) {
                return -1;
            }
            *outArchTarget = argv[argi + 1];
            argi += 2;
            continue;
        }
        if (StrEq(argv[argi], "--cache-dir")) {
            if (argi + 1 >= argc) {
                return -1;
            }
            *outCacheDirArg = argv[argi + 1];
            argi += 2;
            continue;
        }
        if (StrEq(argv[argi], "--diag-format")) {
            if (argi + 1 >= argc) {
                return -1;
            }
            *outDiagFormat = argv[argi + 1];
            argi += 2;
            continue;
        }
        if (StrEq(argv[argi], "--testing")) {
            if (outTestingBuild != NULL) {
                *outTestingBuild = 1;
            }
            argi++;
            continue;
        }
        if (StrEq(argv[argi], "--no-import")) {
            if (outNoImport == NULL) {
                break;
            }
            *outNoImport = 1;
            argi++;
            continue;
        }
        break;
    }
    return argi;
}

static int ValidatePlatformTargetOrUsage(const char* platformTarget) {
    if (IsValidPlatformTargetName(platformTarget)) {
        return 0;
    }
    fprintf(stderr, "invalid platform target: %s\n", platformTarget);
    return 2;
}

static int ValidateArchTargetOrUsage(const char* archTarget) {
    if (IsValidPlatformTargetName(archTarget)) {
        return 0;
    }
    fprintf(stderr, "invalid arch target: %s\n", archTarget);
    return 2;
}

static int ParseDiagOutputFormatOrUsage(const char* name, H2DiagOutputFormat* outFormat) {
    if (name == NULL || outFormat == NULL) {
        return -1;
    }
    if (StrEq(name, "text")) {
        *outFormat = H2DiagOutputFormat_TEXT;
        return 0;
    }
    if (StrEq(name, "jsonl")) {
        *outFormat = H2DiagOutputFormat_JSONL;
        return 0;
    }
    fprintf(stderr, "invalid diagnostic format: %s\n", name);
    return 2;
}

static int IsKnownCommand(const char* mode) {
    return StrEq(mode, "run") || StrEq(mode, "fmt") || StrEq(mode, "build") || StrEq(mode, "check");
}

static int ErrorUnknownCommand(const char* argv0, const char* mode) {
    const char* progname = ProgramBasename(argv0);
    fprintf(stderr, "%s: unknown command: %s (see %s --help)\n", progname, mode, progname);
    return 2;
}

static int PathIsExistingDirectory(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int IsStdoutOutputPath(const char* _Nullable path) {
    return path != NULL && StrEq(path, "-");
}

static char* _Nullable InferBuildInputPackageName(const H2PackageInput* input) {
    const char* firstPath;
    char*       canonical = NULL;
    char*       dirPath = NULL;
    char*       dirName = NULL;
    char*       fileName = NULL;
    char*       baseName = NULL;
    struct stat st;

    if (input == NULL || input->paths == NULL || input->pathLen == 0) {
        return NULL;
    }
    firstPath = input->paths[0];
    canonical = realpath(firstPath, NULL);
    if (canonical == NULL) {
        return NULL;
    }
    if (stat(canonical, &st) != 0) {
        free(canonical);
        return NULL;
    }
    if (input->pathLen == 1 && S_ISDIR(st.st_mode)) {
        dirName = LastPathComponentDup(canonical);
        free(canonical);
        return dirName;
    }
    dirPath = DirNameDup(canonical);
    if (dirPath == NULL) {
        free(canonical);
        return NULL;
    }
    dirName = LastPathComponentDup(dirPath);
    if (input->pathLen > 1 || (dirName != NULL && IsValidIdentifier(dirName))) {
        free(canonical);
        free(dirPath);
        return dirName;
    }
    free(dirName);
    fileName = BaseNameDup(canonical);
    free(canonical);
    free(dirPath);
    if (fileName == NULL) {
        return NULL;
    }
    baseName = StripHOPExtensionDup(fileName);
    free(fileName);
    if (baseName != NULL && IsValidIdentifier(baseName)) {
        return baseName;
    }
    free(baseName);
    return NULL;
}

static int IsSingleBuildSourceFile(const H2PackageInput* input) {
    struct stat st;
    if (input == NULL || input->paths == NULL || input->pathLen != 1) {
        return 0;
    }
    if (!HasSuffix(input->paths[0], ".hop")) {
        return 0;
    }
    return stat(input->paths[0], &st) == 0 && S_ISREG(st.st_mode);
}

static const char* BuildOutputFormatSourceExtension(
    BuildOutputFormat outputFormat, const char* platformTarget) {
    switch (outputFormat) {
        case BuildOutputFormat_EXECUTABLE:
            return StrEq(platformTarget, H2_WASM_MIN_PLATFORM_TARGET)
                        || StrEq(platformTarget, H2_PLAYBIT_PLATFORM_TARGET)
                     ? ".wasm"
                     : "";
        case BuildOutputFormat_MIR:    return ".mir";
        case BuildOutputFormat_TOKENS: return ".tokens";
        case BuildOutputFormat_AST:    return ".ast";
        case BuildOutputFormat_C:      return ".c";
    }
    return "";
}

static int ReplacePathExtensionDup(const char* path, const char* extension, char** outPath) {
    const char* slash = strrchr(path, '/');
    const char* dot = strrchr(path, '.');
    size_t      stemLen = strlen(path);
    size_t      extensionLen = strlen(extension);
    char*       out;
    if (dot != NULL && (slash == NULL || dot > slash)) {
        stemLen = (size_t)(dot - path);
    }
    out = (char*)malloc(stemLen + extensionLen + 1u);
    if (out == NULL) {
        return -1;
    }
    memcpy(out, path, stemLen);
    memcpy(out + stemLen, extension, extensionLen);
    out[stemLen + extensionLen] = '\0';
    *outPath = out;
    return 0;
}

static int CheckDefaultOutputPathOrError(const char* argv0, const char* outPath) {
    if (PathIsExistingDirectory(outPath)) {
        fprintf(
            stderr,
            "%s: error: Default output file \"%s\" is a directory. Specify output with -o\n",
            ProgramBasename(argv0),
            outPath);
        return 1;
    }
    return 0;
}

static int DefaultPackageExecutableOutputPath(
    const char* argv0, const H2PackageInput* input, const char* platformTarget, char** outPath) {
    char*       pkgName = InferBuildInputPackageName(input);
    const char* fallback =
        StrEq(platformTarget, H2_WASM_MIN_PLATFORM_TARGET)
                || StrEq(platformTarget, H2_PLAYBIT_PLATFORM_TARGET)
            ? "a.wasm"
            : "a.out";
    char* out;
    *outPath = NULL;
    if (pkgName == NULL || !IsValidIdentifier(pkgName)) {
        free(pkgName);
        *outPath = H2CDupCStr(fallback);
        if (*outPath == NULL) {
            return -1;
        }
        return CheckDefaultOutputPathOrError(argv0, *outPath);
    }
    if (StrEq(platformTarget, H2_WASM_MIN_PLATFORM_TARGET)
        || StrEq(platformTarget, H2_PLAYBIT_PLATFORM_TARGET))
    {
        size_t len = strlen(pkgName);
        out = (char*)malloc(len + 5u + 1u);
        if (out == NULL) {
            free(pkgName);
            return -1;
        }
        memcpy(out, pkgName, len);
        memcpy(out + len, ".wasm", 6u);
        free(pkgName);
    } else {
        out = pkgName;
    }
    if (CheckDefaultOutputPathOrError(argv0, out) != 0) {
        free(out);
        return 1;
    }
    *outPath = out;
    return 0;
}

static int DefaultBuildOutputPath(
    const char*           argv0,
    const H2PackageInput* input,
    BuildOutputFormat     outputFormat,
    const char*           platformTarget,
    char**                outPath) {
    *outPath = NULL;
    if (outputFormat == BuildOutputFormat_C) {
        return 0;
    }
    if (IsSingleBuildSourceFile(input)) {
        const char* extension = BuildOutputFormatSourceExtension(outputFormat, platformTarget);
        if (ReplacePathExtensionDup(input->paths[0], extension, outPath) != 0) {
            return -1;
        }
        if (CheckDefaultOutputPathOrError(argv0, *outPath) != 0) {
            free(*outPath);
            *outPath = NULL;
            return 1;
        }
        return 0;
    }
    if (outputFormat != BuildOutputFormat_EXECUTABLE) {
        return 0;
    }
    return DefaultPackageExecutableOutputPath(argv0, input, platformTarget, outPath);
}

static int OpenBuildOutputFile(
    const char* _Nullable outFilename, FILE** outFile, int* outShouldClose) {
    *outFile = stdout;
    *outShouldClose = 0;
    if (outFilename == NULL || IsStdoutOutputPath(outFilename)) {
        return 0;
    }
    *outFile = fopen(outFilename, "wb");
    if (*outFile == NULL) {
        return ErrorSimple("failed to open output file: %s", outFilename);
    }
    *outShouldClose = 1;
    return 0;
}

static int CloseBuildOutputFile(FILE* outFile, int shouldClose) {
    if (shouldClose && fclose(outFile) != 0) {
        return ErrorSimple("failed to write output file");
    }
    return 0;
}

static int StreamFileToStdout(const char* path) {
    FILE*         in = fopen(path, "rb");
    unsigned char buf[8192];
    size_t        n;
    if (in == NULL) {
        return ErrorSimple("failed to read output file");
    }
    while ((n = fread(buf, 1u, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1u, n, stdout) != n) {
            fclose(in);
            return ErrorSimple("failed to write output");
        }
    }
    if (ferror(in)) {
        fclose(in);
        return ErrorSimple("failed to read output file");
    }
    fclose(in);
    return 0;
}

static int CompileProgramInputToStdout(
    const H2PackageInput* input,
    const char* _Nullable platformTarget,
    const char* _Nullable archTarget,
    int testingBuild,
    const char* _Nullable cacheDirArg) {
    const char* tmpBase = getenv("TMPDIR");
    char        exeTemplate[PATH_MAX];
    int         n;
    int         fd;
    int         rc;
    if (tmpBase == NULL || tmpBase[0] == '\0') {
        tmpBase = "/tmp";
    }
    n = snprintf(exeTemplate, sizeof(exeTemplate), "%s/hop-build-stdout.XXXXXX", tmpBase);
    if (n <= 0 || (size_t)n >= sizeof(exeTemplate)) {
        return ErrorSimple("temporary path too long");
    }
    fd = mkstemp(exeTemplate);
    if (fd < 0) {
        return ErrorSimple("failed to create temporary output file");
    }
    close(fd);

    rc = CompileProgramInput(
        input, exeTemplate, platformTarget, archTarget, testingBuild, cacheDirArg);
    if (rc == 0) {
        rc = StreamFileToStdout(exeTemplate);
    }
    unlink(exeTemplate);
    return rc;
}

static int ReadSingleBuildSource(
    const H2PackageInput* input, char** outFilename, char** outSource, uint32_t* outSourceLen) {
    struct stat st;
    if (input == NULL || input->paths == NULL || input->pathLen != 1) {
        return ErrorSimple("output format expects exactly one source file");
    }
    if (stat(input->paths[0], &st) != 0 || !S_ISREG(st.st_mode)
        || !HasSuffix(input->paths[0], ".hop"))
    {
        return ErrorSimple("output format expects exactly one source file");
    }
    *outFilename = H2CDupCStr(input->paths[0]);
    if (*outFilename == NULL) {
        return ErrorSimple("out of memory");
    }
    if (ReadFile(input->paths[0], outSource, outSourceLen) != 0) {
        free(*outFilename);
        *outFilename = NULL;
        return -1;
    }
    return 0;
}

static int RunBuildCommand(int argc, char* argv[]) {
    const char*        platformTarget = H2_DEFAULT_PLATFORM_TARGET;
    const char*        archTarget = NULL;
    const char*        cacheDirArg = NULL;
    const char*        diagFormatArg = "text";
    const char*        outFilenameArg = NULL;
    const char*        defaultPath = ".";
    const char**       inputPaths = NULL;
    uint32_t           inputLen = 0;
    int                testingBuild = 0;
    H2DiagOutputFormat diagOutputFormat = H2DiagOutputFormat_TEXT;
    BuildOutputFormat  outputFormat = BuildOutputFormat_EXECUTABLE;
    H2PackageInput     input = { 0 };
    char*              defaultOutFilename = NULL;
    int                i;
    int                rc = 2;

    inputPaths = (const char**)calloc((size_t)argc, sizeof(char*));
    if (inputPaths == NULL) {
        return ErrorSimple("out of memory") == 0 ? 0 : 1;
    }

    for (i = 2; i < argc; i++) {
        const char* arg = argv[i];
        if (IsHelpFlag(arg)) {
            PrintBuildHelp(argv[0]);
            rc = 0;
            goto end;
        }
        if (StrEq(arg, "-o-")) {
            outFilenameArg = "-";
            continue;
        }
        if (StrEq(arg, "-o") || StrEq(arg, "--output")) {
            if (i + 1 >= argc) {
                PrintBuildHelp(argv[0]);
                goto end;
            }
            outFilenameArg = argv[++i];
            continue;
        }
        if (StrEq(arg, "--platform")) {
            if (i + 1 >= argc) {
                PrintBuildHelp(argv[0]);
                goto end;
            }
            platformTarget = argv[++i];
            continue;
        }
        if (StrEq(arg, "--arch")) {
            if (i + 1 >= argc) {
                PrintBuildHelp(argv[0]);
                goto end;
            }
            archTarget = argv[++i];
            continue;
        }
        if (StrEq(arg, "--cache-dir")) {
            if (i + 1 >= argc) {
                PrintBuildHelp(argv[0]);
                goto end;
            }
            cacheDirArg = argv[++i];
            continue;
        }
        if (StrEq(arg, "--diag-format")) {
            if (i + 1 >= argc) {
                PrintBuildHelp(argv[0]);
                goto end;
            }
            diagFormatArg = argv[++i];
            continue;
        }
        if (StrEq(arg, "--output-format")) {
            if (i + 1 >= argc || ParseBuildOutputFormat(argv[i + 1], &outputFormat) != 0) {
                fprintf(stderr, "invalid output format: %s\n", i + 1 < argc ? argv[i + 1] : "");
                rc = 2;
                goto end;
            }
            i++;
            continue;
        }
        if (StrEq(arg, "--testing")) {
            testingBuild = 1;
            continue;
        }
        if (arg[0] == '-') {
            fprintf(stderr, "unknown build option: %s\n", arg);
            rc = 2;
            goto end;
        }
        inputPaths[inputLen++] = arg;
    }

    if (inputLen == 0) {
        inputPaths[inputLen++] = defaultPath;
    }
    input.paths = inputPaths;
    input.pathLen = inputLen;

    if (ParseDiagOutputFormatOrUsage(diagFormatArg, &diagOutputFormat) != 0) {
        rc = 2;
        goto end;
    }
    SetDiagOutputFormat(diagOutputFormat);
    if (ValidatePlatformTargetOrUsage(platformTarget) != 0) {
        rc = 2;
        goto end;
    }
    if (ValidateArchTargetOrUsage(archTarget != NULL ? archTarget : H2_DEFAULT_ARCH_TARGET) != 0) {
        rc = 2;
        goto end;
    }

    if (StrEq(platformTarget, H2_EVAL_PLATFORM_TARGET) && outputFormat != BuildOutputFormat_TOKENS
        && outputFormat != BuildOutputFormat_AST && outputFormat != BuildOutputFormat_MIR)
    {
        ErrorSimple(
            "platform target %s is evaluator-only; use `hop run --platform %s`",
            platformTarget,
            platformTarget);
        rc = 1;
        goto end;
    }

    if (outFilenameArg == NULL) {
        int outPathRc = DefaultBuildOutputPath(
            argv[0], &input, outputFormat, platformTarget, &defaultOutFilename);
        if (outPathRc != 0) {
            if (outPathRc < 0) {
                ErrorSimple("out of memory");
            }
            rc = 1;
            goto end;
        }
        if (defaultOutFilename != NULL) {
            outFilenameArg = defaultOutFilename;
        }
    }

    if (outputFormat == BuildOutputFormat_TOKENS || outputFormat == BuildOutputFormat_AST) {
        char*    filename = NULL;
        char*    source = NULL;
        uint32_t sourceLen = 0;
        FILE*    outFile = NULL;
        int      closeOutFile = 0;
        if (ReadSingleBuildSource(&input, &filename, &source, &sourceLen) != 0) {
            rc = 1;
            goto end;
        }
        if (OpenBuildOutputFile(outFilenameArg, &outFile, &closeOutFile) != 0) {
            rc = 1;
            free(source);
            free(filename);
            goto end;
        }
        rc = (outputFormat == BuildOutputFormat_TOKENS
                  ? DumpTokens(outFile, filename, source, sourceLen)
                  : DumpAST(outFile, filename, source, sourceLen))
                  == 0
               ? 0
               : 1;
        if (CloseBuildOutputFile(outFile, closeOutFile) != 0 && rc == 0) {
            rc = 1;
        }
        free(source);
        free(filename);
        goto end;
    }

    if (outputFormat == BuildOutputFormat_MIR) {
        FILE* outFile = NULL;
        int   closeOutFile = 0;
        if (OpenBuildOutputFile(outFilenameArg, &outFile, &closeOutFile) != 0) {
            rc = 1;
            goto end;
        }
        rc = DumpMIRInput(&input, outFile, platformTarget, archTarget, testingBuild) == 0 ? 0 : 1;
        if (CloseBuildOutputFile(outFile, closeOutFile) != 0 && rc == 0) {
            rc = 1;
        }
        goto end;
    }

    if (outputFormat == BuildOutputFormat_C) {
        rc = GeneratePackageInput(
                 &input, "c", outFilenameArg, platformTarget, archTarget, testingBuild, cacheDirArg)
                  == 0
               ? 0
               : 1;
        goto end;
    }

    if (outFilenameArg == NULL) {
        if (defaultOutFilename == NULL) {
            ErrorSimple("out of memory");
            rc = 1;
            goto end;
        }
        outFilenameArg = defaultOutFilename;
    }
    if (StrEq(platformTarget, H2_WASM_MIN_PLATFORM_TARGET)
        || StrEq(platformTarget, H2_PLAYBIT_PLATFORM_TARGET))
    {
        rc = GeneratePackageInput(
                 &input,
                 "wasm",
                 outFilenameArg,
                 platformTarget,
                 archTarget,
                 testingBuild,
                 cacheDirArg)
                  == 0
               ? 0
               : 1;
    } else if (IsStdoutOutputPath(outFilenameArg)) {
        rc = CompileProgramInputToStdout(
                 &input, platformTarget, archTarget, testingBuild, cacheDirArg)
                  == 0
               ? 0
               : 1;
    } else {
        rc = CompileProgramInput(
                 &input, outFilenameArg, platformTarget, archTarget, testingBuild, cacheDirArg)
                  == 0
               ? 0
               : 1;
    }

end:
    free(defaultOutFilename);
    free(inputPaths);
    return rc;
}

int main(int argc, char* argv[]) {
    const char*        mode;
    const char*        filename = NULL;
    const char*        platformTarget = NULL;
    const char*        archTarget = NULL;
    const char*        cacheDirArg = NULL;
    const char*        diagFormatArg = "text";
    int                hasPlatformTarget = 0;
    int                noImport = 0;
    int                testingBuild = 0;
    H2DiagOutputFormat diagOutputFormat = H2DiagOutputFormat_TEXT;
    char*              source;
    uint32_t           sourceLen;
    int                argi;

    if (argc == 1) {
        PrintUsage(argv[0]);
        return 2;
    }
    if (argc == 2) {
        if (StrEq(argv[1], "--version")) {
            PrintVersion();
            return 0;
        }
        if (IsHelpFlag(argv[1])) {
            PrintUsage(argv[0]);
            return 0;
        }
    }

    mode = argv[1];
    if (mode[0] == '-') {
        PrintUsage(argv[0]);
        return 2;
    }
    if (!IsKnownCommand(mode)) {
        return ErrorUnknownCommand(argv[0], mode);
    }
    if (argc == 3 && IsHelpFlag(argv[2])) {
        if (PrintCommandHelp(argv[0], mode)) {
            return 0;
        }
    }

    if (StrEq(mode, "run")) {
        platformTarget = H2_EVAL_PLATFORM_TARGET;
        argi = ParseSharedCommandOptions(
            argc,
            argv,
            2,
            &platformTarget,
            &archTarget,
            &cacheDirArg,
            &diagFormatArg,
            &testingBuild,
            NULL,
            NULL);
        if (argi < 0 || argc - argi != 1) {
            PrintCommandHelp(argv[0], mode);
            return 2;
        }
        if (ParseDiagOutputFormatOrUsage(diagFormatArg, &diagOutputFormat) != 0) {
            return 2;
        }
        SetDiagOutputFormat(diagOutputFormat);
        if (ValidatePlatformTargetOrUsage(platformTarget) != 0) {
            return 2;
        }
        if (ValidateArchTargetOrUsage(archTarget != NULL ? archTarget : H2_DEFAULT_ARCH_TARGET)
            != 0)
        {
            return 2;
        }
        {
            int runRc = RunProgram(
                argv[argi], platformTarget, archTarget, testingBuild, cacheDirArg);
            if (runRc < 0) {
                return 1;
            }
            return runRc;
        }
    }
    if (StrEq(mode, "fmt")) {
        return RunFmtCommand(argc - 2, (const char* const*)&argv[2]);
    }
    if (StrEq(mode, "build")) {
        return RunBuildCommand(argc, argv);
    }

    argi = ParseSharedCommandOptions(
        argc,
        argv,
        2,
        &platformTarget,
        &archTarget,
        &cacheDirArg,
        &diagFormatArg,
        &testingBuild,
        &hasPlatformTarget,
        &noImport);
    if (argi < 0) {
        PrintCommandHelp(argv[0], mode);
        return 2;
    }
    if (noImport && argc - argi != 1) {
        fprintf(stderr, "--no-import expects exactly one source file\n");
        return 2;
    }
    if (argc - argi != 1) {
        PrintCommandHelp(argv[0], mode);
        return 2;
    }
    if (noImport
        && (platformTarget != NULL || archTarget != NULL || cacheDirArg != NULL || testingBuild
            || hasPlatformTarget))
    {
        PrintCommandHelp(argv[0], mode);
        return 2;
    }
    if (platformTarget == NULL) {
        platformTarget = H2_DEFAULT_PLATFORM_TARGET;
    }
    if (ValidatePlatformTargetOrUsage(platformTarget) != 0) {
        return 2;
    }
    if (ValidateArchTargetOrUsage(archTarget != NULL ? archTarget : H2_DEFAULT_ARCH_TARGET) != 0) {
        return 2;
    }
    filename = argv[argi];

    if (ParseDiagOutputFormatOrUsage(diagFormatArg, &diagOutputFormat) != 0) {
        return 2;
    }
    SetDiagOutputFormat(diagOutputFormat);

    if (!noImport) {
        return CheckPackageDir(filename, platformTarget, archTarget, testingBuild) == 0 ? 0 : 1;
    }

    if (noImport) {
        struct stat st;
        if (stat(filename, &st) == 0 && S_ISDIR(st.st_mode)) {
            fprintf(stderr, "--no-import expects a source file, got directory: %s\n", filename);
            return 2;
        }
    }
    if (ReadFile(filename, &source, &sourceLen) != 0) {
        return 1;
    }
    if (CheckSource(filename, source, sourceLen) != 0) {
        free(source);
        return 1;
    }

    free(source);
    return 0;
}

H2_API_END
