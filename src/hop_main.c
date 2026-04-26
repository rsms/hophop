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

static const char* DefaultGenpkgBackendName(const char* _Nullable platformTarget) {
    if (platformTarget != NULL
        && (StrEq(platformTarget, H2_WASM_MIN_PLATFORM_TARGET)
            || StrEq(platformTarget, H2_PLAYBIT_PLATFORM_TARGET)))
    {
        return "wasm";
    }
    return "c";
}

static int ParseGenpkgMode(
    const char* mode,
    const char* _Nullable platformTarget,
    char*    outBackend,
    uint32_t outBackendCap) {
    const char* defaultBackend;
    uint32_t    i;
    if (StrEq(mode, "genpkg")) {
        defaultBackend = DefaultGenpkgBackendName(platformTarget);
        i = (uint32_t)strlen(defaultBackend);
        if (i + 1u > outBackendCap) {
            return -1;
        }
        memcpy(outBackend, defaultBackend, (size_t)i + 1u);
        return 1;
    }
    if (mode[0] != 'g' || mode[1] != 'e' || mode[2] != 'n' || mode[3] != 'p' || mode[4] != 'k'
        || mode[5] != 'g')
    {
        return 0;
    }
    if (mode[6] != ':') {
        return -1;
    }
    i = 0;
    while (mode[7u + i] != '\0') {
        if (i + 1u >= outBackendCap) {
            return -1;
        }
        outBackend[i] = mode[7u + i];
        i++;
    }
    if (i == 0) {
        return -1;
    }
    outBackend[i] = '\0';
    return 1;
}

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

static void PrintUsage(const char* argv0) {
    const char* progname = ProgramBasename(argv0);
    fprintf(
        stderr,
        "usage:\n"
        "    %s --version\n"
        "    %s run [--platform <target>] [--arch <name>] [--cache-dir <dir>] "
        "[--diag-format <text|jsonl>] <pkgdir|srcfile>\n"
        "    %s fmt [--check] [<file-or-dir> ...]\n"
        "    %s compile [--platform <target>] [--arch <name>] [--cache-dir <dir>] "
        "[--diag-format <text|jsonl>] <pkgdir|srcfile> [-o <output>]\n"
        "    %s genpkg[:backend] [--platform <target>] [--arch <name>] [--cache-dir <dir>] "
        "[--diag-format <text|jsonl>] "
        "<pkgdir|srcfile> "
        "[out]\n"
        "    %s check [--platform <target>] [--arch <name>] [--cache-dir <dir>] "
        "[--diag-format <text|jsonl>] "
        "[--no-import] "
        "<pkgdir|srcfile>\n"
        "    %s lex [--diag-format <text|jsonl>] <srcfile>\n"
        "    %s ast [--diag-format <text|jsonl>] <srcfile>\n"
        "    %s mir [--platform <target>] [--arch <name>] [--cache-dir <dir>] "
        "[--diag-format <text|jsonl>] <pkgdir|srcfile>\n"
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

static void PrintCompileHelp(const char* argv0) {
    const char* progname = ProgramBasename(argv0);
    fprintf(
        stderr,
        "usage:\n"
        "    %s compile [--platform <target>] [--arch <name>] [--cache-dir <dir>] "
        "[--diag-format <text|jsonl>] "
        "<pkgdir|srcfile> [-o <output>]\n",
        progname);
    PrintSharedPlatformOptions();
    fprintf(stderr, "    -o <output>           output executable path\n");
    PrintPlatformTargetList();
}

static void PrintGenpkgHelp(const char* argv0, const char* mode) {
    const char* progname = ProgramBasename(argv0);
    fprintf(stderr, "usage:\n");
    if (StrEq(mode, "genpkg")) {
        fprintf(
            stderr,
            "    %s genpkg [--platform <target>] [--arch <name>] [--cache-dir <dir>] "
            "[--diag-format <text|jsonl>] "
            "<pkgdir|srcfile> [out]\n"
            "    %s genpkg:<backend> [--platform <target>] [--arch <name>] [--cache-dir <dir>] "
            "[--diag-format <text|jsonl>] "
            "<pkgdir|srcfile> [out]\n",
            progname,
            progname);
    } else {
        fprintf(
            stderr,
            "    %s %s [--platform <target>] [--arch <name>] [--cache-dir <dir>] "
            "[--diag-format <text|jsonl>] "
            "<pkgdir|srcfile> [out]\n",
            progname,
            mode);
    }
    PrintSharedPlatformOptions();
    PrintPlatformTargetList();
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

static void PrintLexHelp(const char* argv0) {
    const char* progname = ProgramBasename(argv0);
    fprintf(stderr, "usage:\n    %s lex [--diag-format <text|jsonl>] <srcfile>\n", progname);
}

static void PrintAstHelp(const char* argv0) {
    const char* progname = ProgramBasename(argv0);
    fprintf(stderr, "usage:\n    %s ast [--diag-format <text|jsonl>] <srcfile>\n", progname);
}

static void PrintMirHelp(const char* argv0) {
    const char* progname = ProgramBasename(argv0);
    fprintf(
        stderr,
        "usage:\n"
        "    %s mir [--platform <target>] [--arch <name>] [--cache-dir <dir>] "
        "[--diag-format <text|jsonl>] <pkgdir|srcfile>\n",
        progname);
    PrintSharedPlatformOptions();
    PrintPlatformTargetList();
}

static int PrintCommandHelp(const char* argv0, const char* mode) {
    char backendName[32];
    int  genpkgMode = ParseGenpkgMode(mode, NULL, backendName, sizeof(backendName));
    if (StrEq(mode, "run")) {
        PrintRunHelp(argv0);
        return 1;
    }
    if (StrEq(mode, "fmt")) {
        PrintFmtHelp(argv0);
        return 1;
    }
    if (StrEq(mode, "compile")) {
        PrintCompileHelp(argv0);
        return 1;
    }
    if (genpkgMode == 1) {
        PrintGenpkgHelp(argv0, mode);
        return 1;
    }
    if (StrEq(mode, "check")) {
        PrintCheckHelp(argv0);
        return 1;
    }
    if (StrEq(mode, "lex")) {
        PrintLexHelp(argv0);
        return 1;
    }
    if (StrEq(mode, "ast")) {
        PrintAstHelp(argv0);
        return 1;
    }
    if (StrEq(mode, "mir")) {
        PrintMirHelp(argv0);
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
    if (StrEq(mode, "run") || StrEq(mode, "fmt") || StrEq(mode, "compile") || StrEq(mode, "check")
        || StrEq(mode, "lex") || StrEq(mode, "ast") || StrEq(mode, "mir"))
    {
        return 1;
    }
    return mode[0] == 'g' && mode[1] == 'e' && mode[2] == 'n' && mode[3] == 'p' && mode[4] == 'k'
        && mode[5] == 'g';
}

static int ErrorUnknownCommand(const char* argv0, const char* mode) {
    const char* progname = ProgramBasename(argv0);
    fprintf(stderr, "%s: unknown command: %s (see %s --help)\n", progname, mode, progname);
    return 2;
}

int main(int argc, char* argv[]) {
    const char*        mode;
    const char*        filename = NULL;
    const char*        outFilename = NULL;
    const char*        platformTarget = NULL;
    const char*        archTarget = NULL;
    const char*        cacheDirArg = NULL;
    const char*        diagFormatArg = "text";
    char               backendName[32];
    int                genpkgMode;
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

    if (StrEq(mode, "compile")) {
        platformTarget = H2_DEFAULT_PLATFORM_TARGET;
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
        if (argi < 0) {
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
        if (argc - argi == 1) {
            return CompileProgram(
                       argv[argi], "a.out", platformTarget, archTarget, testingBuild, cacheDirArg)
                        == 0
                     ? 0
                     : 1;
        }
        if (argc - argi != 3 || !StrEq(argv[argi + 1], "-o")) {
            PrintCommandHelp(argv[0], mode);
            return 2;
        }
        return CompileProgram(
                   argv[argi],
                   argv[argi + 2],
                   platformTarget,
                   archTarget,
                   testingBuild,
                   cacheDirArg)
                    == 0
                 ? 0
                 : 1;
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

    argi = 2;
    if (StrEq(mode, "check") || StrEq(mode, "mir")) {
        argi = ParseSharedCommandOptions(
            argc,
            argv,
            argi,
            &platformTarget,
            &archTarget,
            &cacheDirArg,
            &diagFormatArg,
            &testingBuild,
            &hasPlatformTarget,
            StrEq(mode, "check") ? &noImport : NULL);
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
        if (ValidateArchTargetOrUsage(archTarget != NULL ? archTarget : H2_DEFAULT_ARCH_TARGET)
            != 0)
        {
            return 2;
        }
    } else if (StrEq(mode, "check") || StrEq(mode, "lex") || StrEq(mode, "ast")) {
        argi = ParseSharedCommandOptions(
            argc,
            argv,
            argi,
            &platformTarget,
            &archTarget,
            &cacheDirArg,
            &diagFormatArg,
            &testingBuild,
            &hasPlatformTarget,
            NULL);
        if (argi < 0 || argc - argi != 1 || platformTarget != NULL || archTarget != NULL
            || cacheDirArg != NULL || testingBuild || hasPlatformTarget)
        {
            PrintCommandHelp(argv[0], mode);
            return 2;
        }
    } else {
        argi = ParseSharedCommandOptions(
            argc,
            argv,
            argi,
            &platformTarget,
            &archTarget,
            &cacheDirArg,
            &diagFormatArg,
            &testingBuild,
            &hasPlatformTarget,
            NULL);
        if (argi < 0) {
            PrintCommandHelp(argv[0], mode);
            return 2;
        }
        if (archTarget != NULL && ValidateArchTargetOrUsage(archTarget) != 0) {
            return 2;
        }
    }

    if (StrEq(mode, "check") || StrEq(mode, "lex") || StrEq(mode, "ast") || StrEq(mode, "mir")) {
        filename = argv[argi];
    } else {
        if (argc - argi == 1) {
            filename = argv[argi];
        } else if (argc - argi == 2) {
            filename = argv[argi];
            outFilename = argv[argi + 1];
        } else {
            PrintCommandHelp(argv[0], mode);
            return 2;
        }
    }

    genpkgMode = ParseGenpkgMode(mode, platformTarget, backendName, sizeof(backendName));
    if (genpkgMode < 0) {
        fprintf(stderr, "unknown mode: %s\n", mode);
        return 2;
    }
    if (ParseDiagOutputFormatOrUsage(diagFormatArg, &diagOutputFormat) != 0) {
        return 2;
    }
    SetDiagOutputFormat(diagOutputFormat);
    if (genpkgMode == 1) {
        const char* genpkgPlatformTarget = hasPlatformTarget ? platformTarget : NULL;
        if (!hasPlatformTarget && StrEq(backendName, "c")) {
            genpkgPlatformTarget = H2_DEFAULT_PLATFORM_TARGET;
        }
        return GeneratePackage(
                   filename,
                   backendName,
                   outFilename,
                   genpkgPlatformTarget,
                   archTarget,
                   testingBuild,
                   cacheDirArg)
                    == 0
                 ? 0
                 : 1;
    }

    if (StrEq(mode, "check") && !noImport) {
        if (outFilename != NULL) {
            fprintf(stderr, "unexpected output argument for mode check\n");
            return 2;
        }
        return CheckPackageDir(
                   filename,
                   hasPlatformTarget ? platformTarget : H2_DEFAULT_PLATFORM_TARGET,
                   archTarget,
                   testingBuild)
                    == 0
                 ? 0
                 : 1;
    }
    if (StrEq(mode, "mir")) {
        if (outFilename != NULL) {
            fprintf(stderr, "unexpected output argument for mode mir\n");
            return 2;
        }
        return DumpMIR(
                   filename,
                   hasPlatformTarget ? platformTarget : H2_DEFAULT_PLATFORM_TARGET,
                   archTarget,
                   testingBuild)
                    == 0
                 ? 0
                 : 1;
    }

    if (outFilename != NULL) {
        fprintf(stderr, "unexpected output argument for mode %s\n", mode);
        return 2;
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

    if (StrEq(mode, "lex")) {
        if (DumpTokens(filename, source, sourceLen) != 0) {
            free(source);
            return 1;
        }
    } else if (StrEq(mode, "ast")) {
        if (DumpAST(filename, source, sourceLen) != 0) {
            free(source);
            return 1;
        }
    } else if (StrEq(mode, "check")) {
        if (CheckSource(filename, source, sourceLen) != 0) {
            free(source);
            return 1;
        }
    } else {
        fprintf(stderr, "unknown mode: %s\n", mode);
        free(source);
        return 2;
    }

    free(source);
    return 0;
}

H2_API_END
