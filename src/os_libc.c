#define H2_OS_LIBC_IMPL 1

#include "os.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
    #include <mach-o/dyld.h>
#endif

#include "hop_internal.h"

H2_API_BEGIN

struct H2OSOutput {
    FILE* f;
};

static H2OSOutput gStdoutOutput = { NULL };

void* _Nullable H2OSAlloc(size_t size) {
    return malloc(size);
}

void* _Nullable H2OSCalloc(size_t count, size_t size) {
    return calloc(count, size);
}

void* _Nullable H2OSRealloc(void* _Nullable ptr, size_t size) {
    return realloc(ptr, size);
}

void H2OSFree(void* _Nullable ptr) {
    free(ptr);
}

const char* _Nullable H2OSGetEnv(const char* name) {
    return getenv(name);
}

int H2OSGetCwd(char* buf, size_t bufSize) {
    return getcwd(buf, bufSize) != NULL ? 0 : -1;
}

char* _Nullable H2OSRealPathDup(const char* path) {
    return realpath(path, NULL);
}

char* _Nullable H2OSGetExeDir(void) {
#if defined(__APPLE__)
    char     buf[PATH_MAX];
    char     resolved[PATH_MAX];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) {
        return NULL;
    }
    if (realpath(buf, resolved) == NULL) {
        return NULL;
    }
    return DirNameDup(resolved);
#elif defined(__linux__)
    char    buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) {
        return NULL;
    }
    buf[n] = '\0';
    return DirNameDup(buf);
#else
    return NULL;
#endif
}

static uint64_t H2OSStatMtimeNs(const struct stat* st) {
#if defined(__APPLE__)
    return (uint64_t)st->st_mtimespec.tv_sec * 1000000000ull + (uint64_t)st->st_mtimespec.tv_nsec;
#elif defined(__linux__)
    return (uint64_t)st->st_mtim.tv_sec * 1000000000ull + (uint64_t)st->st_mtim.tv_nsec;
#else
    return (uint64_t)st->st_mtime * 1000000000ull;
#endif
}

int H2OSPathInfo(const char* path, H2OSFileInfo* outInfo) {
    struct stat st;
    if (outInfo == NULL) {
        return -1;
    }
    memset(outInfo, 0, sizeof(*outInfo));
    if (stat(path, &st) != 0) {
        outInfo->kind = H2OSPathKind_MISSING;
        return -1;
    }
    if (S_ISREG(st.st_mode)) {
        outInfo->kind = H2OSPathKind_FILE;
    } else if (S_ISDIR(st.st_mode)) {
        outInfo->kind = H2OSPathKind_DIR;
    } else {
        outInfo->kind = H2OSPathKind_OTHER;
    }
    outInfo->mtimeNs = H2OSStatMtimeNs(&st);
    return 0;
}

int H2OSAccessRead(const char* path) {
    return access(path, R_OK) == 0 ? 0 : -1;
}

int H2OSAccessExec(const char* path) {
    return access(path, X_OK) == 0 ? 0 : -1;
}

int H2OSEnsureDir(const char* path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }
    if (mkdir(path, 0777) == 0) {
        return 0;
    }
    if (errno == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        return 0;
    }
    return -1;
}

int H2OSEnsureDirRecursive(const char* path) {
    char   tmp[PATH_MAX];
    char*  p;
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp)) {
        return -1;
    }
    memcpy(tmp, path, len + 1u);
    for (p = tmp + 1; *p != '\0'; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        if (H2OSEnsureDir(tmp) != 0) {
            return -1;
        }
        *p = '/';
    }
    return H2OSEnsureDir(tmp);
}

int H2OSListDir(const char* dirPath, H2OSFileList* outList) {
    DIR*           dir;
    struct dirent* ent;
    char**         items = NULL;
    uint32_t       len = 0;
    uint32_t       cap = 0;
    if (outList == NULL) {
        return -1;
    }
    outList->items = NULL;
    outList->len = 0;
    dir = opendir(dirPath);
    if (dir == NULL) {
        return -1;
    }
    while ((ent = readdir(dir)) != NULL) {
        char* name;
        if (EnsureCap((void**)&items, &cap, len + 1u, sizeof(char*)) != 0) {
            closedir(dir);
            H2OSFileList tmp = { items, len };
            H2OSFileListFree(&tmp);
            return -1;
        }
        name = H2CDupCStr(ent->d_name);
        if (name == NULL) {
            closedir(dir);
            H2OSFileList tmp = { items, len };
            H2OSFileListFree(&tmp);
            return -1;
        }
        items[len++] = name;
    }
    closedir(dir);
    outList->items = items;
    outList->len = len;
    return 0;
}

void H2OSFileListFree(H2OSFileList* list) {
    uint32_t i;
    if (list == NULL) {
        return;
    }
    for (i = 0; i < list->len; i++) {
        free(list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->len = 0;
}

int H2OSReadFile(const char* filename, char** outData, uint32_t* outLen) {
    FILE*  f;
    long   size;
    char*  data;
    size_t nread;
    *outData = NULL;
    *outLen = 0;
    f = fopen(filename, "rb");
    if (f == NULL) {
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    size = ftell(f);
    if (size < 0 || (unsigned long)size > UINT32_MAX) {
        fclose(f);
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    data = (char*)malloc((size_t)size + 1u);
    if (data == NULL) {
        fclose(f);
        return -1;
    }
    nread = fread(data, 1u, (size_t)size, f);
    fclose(f);
    if (nread != (size_t)size) {
        free(data);
        return -1;
    }
    data[size] = '\0';
    *outData = data;
    *outLen = (uint32_t)size;
    return 0;
}

int H2OSWriteFile(const char* filename, const char* data, uint32_t len) {
    FILE*  out;
    size_t nwritten;
    if (filename == NULL || strcmp(filename, "-") == 0) {
        nwritten = fwrite(data, 1u, (size_t)len, stdout);
        return nwritten == (size_t)len ? 0 : -1;
    }
    out = fopen(filename, "wb");
    if (out == NULL) {
        return -1;
    }
    nwritten = fwrite(data, 1u, (size_t)len, out);
    if (fclose(out) != 0) {
        return -1;
    }
    return nwritten == (size_t)len ? 0 : -1;
}

int H2OSWriteFileAtomic(const char* filename, const char* data, uint32_t len) {
    size_t  filenameLen = strlen(filename);
    size_t  tmpCap = filenameLen + 32u;
    char*   tmpPath;
    int     fd;
    ssize_t nwritten;
    int     rc = -1;
    tmpPath = (char*)malloc(tmpCap);
    if (tmpPath == NULL) {
        return -1;
    }
    snprintf(tmpPath, tmpCap, "%s.tmp.XXXXXX", filename);
    fd = mkstemp(tmpPath);
    if (fd < 0) {
        free(tmpPath);
        return -1;
    }
    nwritten = write(fd, data, (size_t)len);
    if (nwritten != (ssize_t)len) {
        close(fd);
        unlink(tmpPath);
        free(tmpPath);
        return -1;
    }
    if (close(fd) != 0) {
        unlink(tmpPath);
        free(tmpPath);
        return -1;
    }
    if (rename(tmpPath, filename) == 0) {
        rc = 0;
    } else {
        unlink(tmpPath);
    }
    free(tmpPath);
    return rc;
}

void H2OSWriteStdout(const void* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(STDOUT_FILENO, (const unsigned char*)data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        off += (size_t)n;
    }
}

void H2OSWriteStderr(const void* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(STDERR_FILENO, (const unsigned char*)data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        off += (size_t)n;
    }
}

int H2OSStreamFileToStdout(const char* path) {
    int           in = open(path, O_RDONLY);
    unsigned char buf[8192];
    if (in < 0) {
        return -1;
    }
    for (;;) {
        ssize_t n = read(in, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(in);
            return -1;
        }
        if (n == 0) {
            break;
        }
        H2OSWriteStdout(buf, (size_t)n);
    }
    close(in);
    return 0;
}

int H2OSCreateTempPath(char* pathTemplate) {
    int fd = mkstemp(pathTemplate);
    if (fd < 0) {
        return -1;
    }
    if (close(fd) != 0) {
        unlink(pathTemplate);
        return -1;
    }
    return 0;
}

int H2OSCreateTempPathWithSuffix(char* pathTemplate, int suffixLen) {
    int fd = mkstemps(pathTemplate, suffixLen);
    if (fd < 0) {
        return -1;
    }
    if (close(fd) != 0) {
        unlink(pathTemplate);
        return -1;
    }
    return 0;
}

int H2OSUnlink(const char* path) {
    return unlink(path);
}

int H2OSRename(const char* oldPath, const char* newPath) {
    return rename(oldPath, newPath);
}

int H2OSOpenOutput(const char* _Nullable filename, H2OSOutput** outFile, int* outShouldClose) {
    H2OSOutput* output;
    if (outFile == NULL || outShouldClose == NULL) {
        return -1;
    }
    if (filename == NULL || strcmp(filename, "-") == 0) {
        gStdoutOutput.f = stdout;
        *outFile = &gStdoutOutput;
        *outShouldClose = 0;
        return 0;
    }
    output = (H2OSOutput*)malloc(sizeof(*output));
    if (output == NULL) {
        return -1;
    }
    output->f = fopen(filename, "wb");
    if (output->f == NULL) {
        free(output);
        return -1;
    }
    *outFile = output;
    *outShouldClose = 1;
    return 0;
}

int H2OSCloseOutput(H2OSOutput* outFile, int shouldClose) {
    int rc = 0;
    if (outFile == NULL || !shouldClose) {
        return 0;
    }
    if (fclose(outFile->f) != 0) {
        rc = -1;
    }
    free(outFile);
    return rc;
}

void H2OSOutputWrite(H2OSOutput* outFile, const char* data, uint32_t len) {
    if (outFile == NULL || outFile->f == NULL || len == 0) {
        return;
    }
    fwrite(data, 1u, (size_t)len, outFile->f);
}

int H2OSOutputPrintf(H2OSOutput* outFile, const char* fmt, ...) {
    int     rc;
    va_list ap;
    if (outFile == NULL || outFile->f == NULL) {
        return -1;
    }
    va_start(ap, fmt);
    rc = vfprintf(outFile->f, fmt, ap);
    va_end(ap);
    return rc;
}

int H2OSOutputPutc(H2OSOutput* outFile, int ch) {
    if (outFile == NULL || outFile->f == NULL) {
        return -1;
    }
    return fputc(ch, outFile->f);
}

int H2OSOutputPuts(H2OSOutput* outFile, const char* s) {
    if (outFile == NULL || outFile->f == NULL) {
        return -1;
    }
    return fputs(s, outFile->f);
}

int H2OSPrintStdout(const char* fmt, ...) {
    int     rc;
    va_list ap;
    va_start(ap, fmt);
    rc = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return rc;
}

int H2OSPrintStderr(const char* fmt, ...) {
    int     rc;
    va_list ap;
    va_start(ap, fmt);
    rc = vfprintf(stderr, fmt, ap);
    va_end(ap);
    return rc;
}

int H2OSVPrintStderr(const char* fmt, va_list ap) {
    return vfprintf(stderr, fmt, ap);
}

int H2OSPutcStderr(int ch) {
    return fputc(ch, stderr);
}

int H2OSRunCommand(const char* const* argv) {
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

int H2OSRunCommandExitCode(const char* const* argv, int* outExitCode) {
    pid_t pid;
    int   status;
    if (outExitCode == NULL) {
        return -1;
    }
    *outExitCode = -1;
    if (argv == NULL || argv[0] == NULL) {
        return -1;
    }
    pid = fork();
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

int H2OSExecReplace(const char* path, char* const argv[]) {
    execv(path, argv);
    return -1;
}

H2_API_END
