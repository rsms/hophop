#pragma once

#include "libhop.h"

#include <stdarg.h>
#include <stddef.h>

H2_API_BEGIN

typedef enum {
    H2OSPathKind_MISSING = 0,
    H2OSPathKind_FILE,
    H2OSPathKind_DIR,
    H2OSPathKind_OTHER,
} H2OSPathKind;

typedef struct {
    H2OSPathKind kind;
    uint64_t     mtimeNs;
} H2OSFileInfo;

typedef struct {
    char** _Nullable items;
    uint32_t len;
} H2OSFileList;

typedef struct H2OSOutput H2OSOutput;

void* _Nullable H2OSAlloc(size_t size);
void* _Nullable H2OSCalloc(size_t count, size_t size);
void* _Nullable H2OSRealloc(void* _Nullable ptr, size_t size);
void H2OSFree(void* _Nullable ptr);

const char* _Nullable H2OSGetEnv(const char* name);
int H2OSGetCwd(char* buf, size_t bufSize);
char* _Nullable H2OSRealPathDup(const char* path);
char* _Nullable H2OSGetExeDir(void);
int  H2OSPathInfo(const char* path, H2OSFileInfo* outInfo);
int  H2OSAccessRead(const char* path);
int  H2OSAccessExec(const char* path);
int  H2OSEnsureDir(const char* path);
int  H2OSEnsureDirRecursive(const char* path);
int  H2OSListDir(const char* dirPath, H2OSFileList* outList);
void H2OSFileListFree(H2OSFileList* list);

int H2OSReadFile(const char* filename, char** outData, uint32_t* outLen);
int H2OSWriteFile(const char* filename, const char* data, uint32_t len);
int H2OSWriteFileAtomic(const char* filename, const char* data, uint32_t len);
int H2OSStreamFileToStdout(const char* path);
int H2OSCreateTempPath(char* pathTemplate);
int H2OSCreateTempPathWithSuffix(char* pathTemplate, int suffixLen);
int H2OSUnlink(const char* path);
int H2OSRename(const char* oldPath, const char* newPath);

int  H2OSOpenOutput(const char* _Nullable filename, H2OSOutput** outFile, int* outShouldClose);
int  H2OSCloseOutput(H2OSOutput* outFile, int shouldClose);
void H2OSOutputWrite(H2OSOutput* outFile, const char* data, uint32_t len);
int  H2OSOutputPrintf(H2OSOutput* outFile, const char* fmt, ...) H2_FMT_ATTR(printf, 2, 3);
int  H2OSOutputPutc(H2OSOutput* outFile, int ch);
int  H2OSOutputPuts(H2OSOutput* outFile, const char* s);

void H2OSWriteStdout(const void* data, size_t len);
void H2OSWriteStderr(const void* data, size_t len);
int  H2OSPrintStdout(const char* fmt, ...) H2_FMT_ATTR(printf, 1, 2);
int  H2OSPrintStderr(const char* fmt, ...) H2_FMT_ATTR(printf, 1, 2);
int  H2OSVPrintStderr(const char* fmt, va_list ap) H2_FMT_ATTR(printf, 1, 0);
int  H2OSPutcStderr(int ch);

int H2OSRunCommand(const char* const* argv);
int H2OSRunCommandExitCode(const char* const* argv, int* outExitCode);
int H2OSExecReplace(const char* path, char* const argv[]);

H2_API_END
