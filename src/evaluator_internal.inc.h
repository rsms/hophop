#pragma once

#include "ctfe.h"
#include "ctfe_exec.h"
#include "evaluator.h"
#include "libhop-impl.h"
#include "mir_exec.h"
#include "mir_lower.h"
#include "mir_lower_pkg.h"
#include "mir_lower_stmt.h"
#include "hop_internal.h"

H2_API_BEGIN

enum {
    HOP_EVAL_MIR_HOST_INVALID = H2MirHostTarget_INVALID,
    HOP_EVAL_MIR_HOST_PRINT = H2MirHostTarget_PRINT,
    HOP_EVAL_MIR_HOST_PLATFORM_EXIT = H2MirHostTarget_PLATFORM_EXIT,
    HOP_EVAL_MIR_HOST_FREE = H2MirHostTarget_FREE,
    HOP_EVAL_MIR_HOST_CONCAT = H2MirHostTarget_CONCAT,
    HOP_EVAL_MIR_HOST_COPY = H2MirHostTarget_COPY,
    HOP_EVAL_MIR_HOST_PLATFORM_CONSOLE_LOG = H2MirHostTarget_PLATFORM_CONSOLE_LOG,
};

typedef struct HOPEvalProgram HOPEvalProgram;
typedef struct HOPEvalContext HOPEvalContext;

typedef struct {
    const H2Package*    pkg;
    const H2ParsedFile* file;
    int32_t             fnNode;
    int32_t             bodyNode;
    uint32_t            nameStart;
    uint32_t            nameEnd;
    uint32_t            paramCount;
    uint8_t             hasReturnType;
    uint8_t             hasContextClause;
    uint8_t             isBuiltinPackageFn;
    uint8_t             isVariadic;
} HOPEvalFunction;

enum {
    HOPEvalTopConstState_UNSEEN = 0,
    HOPEvalTopConstState_VISITING = 1,
    HOPEvalTopConstState_READY = 2,
    HOPEvalTopConstState_FAILED = 3,
};

typedef struct {
    const H2ParsedFile* file;
    int32_t             nodeId;
    int32_t             initExprNode;
    uint32_t            nameStart;
    uint32_t            nameEnd;
    uint8_t             state;
    uint8_t             _reserved[3];
    H2CTFEValue         value;
} HOPEvalTopConst;

typedef struct {
    const H2ParsedFile* file;
    int32_t             nodeId;
    int32_t             initExprNode;
    int32_t             declTypeNode;
    uint32_t            nameStart;
    uint32_t            nameEnd;
    uint8_t             state;
    uint8_t             _reserved[3];
    H2CTFEValue         value;
} HOPEvalTopVar;

typedef struct {
    uint32_t    nameStart;
    uint32_t    nameEnd;
    uint16_t    flags;
    uint16_t    _reserved;
    int32_t     typeNode;
    int32_t     defaultExprNode;
    H2CTFEValue value;
} HOPEvalAggregateField;

typedef struct {
    const H2ParsedFile*    file;
    int32_t                nodeId;
    HOPEvalAggregateField* fields;
    uint32_t               fieldLen;
} HOPEvalAggregate;

typedef struct {
    const H2ParsedFile* file;
    int32_t             typeNode;
    int32_t             elemTypeNode;
    H2CTFEValue* _Nullable elems;
    uint32_t len;
} HOPEvalArray;

typedef struct H2EvalRuntimeAlloc H2EvalRuntimeAlloc;

typedef struct {
    uint32_t            magic;
    H2CTFEValue         token;
    H2EvalRuntimeAlloc* allocations;
} H2EvalRuntimeAllocatorState;

struct H2EvalRuntimeAlloc {
    uint32_t                     magic;
    uint32_t                     align;
    H2CTFEValue                  token;
    H2EvalRuntimeAllocatorState* owner;
    void* _Nullable bytes;
    int64_t             size;
    H2EvalRuntimeAlloc* next;
};

struct HOPEvalContext {
    H2CTFEValue allocator;
    H2CTFEValue tempAllocator;
    H2CTFEValue logger;
};

typedef struct {
    const H2ParsedFile* file;
    int32_t             enumNode;
    uint32_t            variantNameStart;
    uint32_t            variantNameEnd;
    uint32_t            tagIndex;
    HOPEvalAggregate* _Nullable payload;
} HOPEvalTaggedEnum;

typedef struct {
    const H2ParsedFile* activeTemplateParamFile;
    uint32_t            activeTemplateParamNameStart;
    uint32_t            activeTemplateParamNameEnd;
    const H2ParsedFile* activeTemplateTypeFile;
    int32_t             activeTemplateTypeNode;
    H2CTFEValue         activeTemplateTypeValue;
    uint8_t             hasActiveTemplateTypeValue;
} HOPEvalTemplateBindingState;

typedef struct HOPEvalReflectedType HOPEvalReflectedType;
struct HOPEvalReflectedType {
    uint8_t             kind;
    uint8_t             namedKind;
    uint16_t            _reserved;
    const H2ParsedFile* file;
    int32_t             nodeId;
    uint32_t            arrayLen;
    H2CTFEValue         elemType;
};

#define HOP_EVAL_PACKAGE_REF_TAG_FLAG  (UINT64_C(1) << 63)
#define HOP_EVAL_NULL_FIXED_LEN_TAG    (UINT64_C(1) << 62)
#define HOP_EVAL_FUNCTION_REF_TAG_FLAG (UINT64_C(1) << 61)
#define HOP_EVAL_TAGGED_ENUM_TAG_FLAG  (UINT64_C(1) << 60)
#define HOP_EVAL_SIMPLE_TYPE_TAG_FLAG  (UINT64_C(1) << 59)
#define HOP_EVAL_REFLECT_TYPE_TAG_FLAG (UINT64_C(1) << 58)
#define HOP_EVAL_RUNTIME_TYPE_MAGIC    0x534c4556u

enum {
    HOPEvalReflectType_NAMED = 1,
    HOPEvalReflectType_PTR = 2,
    HOPEvalReflectType_SLICE = 3,
    HOPEvalReflectType_ARRAY = 4,
};

enum {
    HOPEvalTypeKind_PRIMITIVE = 1,
    HOPEvalTypeKind_ALIAS = 2,
    HOPEvalTypeKind_STRUCT = 3,
    HOPEvalTypeKind_UNION = 4,
    HOPEvalTypeKind_ENUM = 5,
    HOPEvalTypeKind_POINTER = 6,
    HOPEvalTypeKind_REFERENCE = 7,
    HOPEvalTypeKind_SLICE = 8,
    HOPEvalTypeKind_ARRAY = 9,
    HOPEvalTypeKind_OPTIONAL = 10,
    HOPEvalTypeKind_FUNCTION = 11,
};

struct HOPEvalProgram {
    H2Arena* _Nonnull arena;
    const H2PackageLoader* loader;
    const H2Package*       entryPkg;
    const H2ParsedFile*    currentFile;
    H2CTFEExecCtx*         currentExecCtx;
    struct HOPEvalMirExecCtx* _Nullable currentMirExecCtx;
    HOPEvalFunction* funcs;
    uint32_t         funcLen;
    uint32_t         funcCap;
    HOPEvalTopConst* topConsts;
    uint32_t         topConstLen;
    uint32_t         topConstCap;
    HOPEvalTopVar*   topVars;
    uint32_t         topVarLen;
    uint32_t         topVarCap;
    uint32_t         callDepth;
    uint32_t         callStack[H2_EVAL_CALL_MAX_DEPTH];
    HOPEvalContext   rootContext;
    const HOPEvalContext* _Nullable currentContext;
    H2CTFEValue                 loggerPrefix;
    const H2ParsedFile*         activeTemplateParamFile;
    uint32_t                    activeTemplateParamNameStart;
    uint32_t                    activeTemplateParamNameEnd;
    const H2ParsedFile*         activeTemplateTypeFile;
    int32_t                     activeTemplateTypeNode;
    H2CTFEValue                 activeTemplateTypeValue;
    uint8_t                     hasActiveTemplateTypeValue;
    const H2ParsedFile*         expectedCallExprFile;
    int32_t                     expectedCallExprNode;
    const H2ParsedFile*         expectedCallTypeFile;
    int32_t                     expectedCallTypeNode;
    const H2ParsedFile*         activeCallExpectedTypeFile;
    int32_t                     activeCallExpectedTypeNode;
    int                         exitCalled;
    int                         exitCode;
    uint8_t                     reportedFailure;
    H2EvalRuntimeAllocatorState rootAllocatorState;
    H2EvalRuntimeAllocatorState rootTempAllocatorState;
};

typedef struct HOPEvalMirExecCtx {
    HOPEvalProgram*             p;
    uint32_t*                   evalToMir;
    uint32_t                    evalToMirLen;
    uint32_t*                   mirToEval;
    uint32_t                    mirToEvalLen;
    const H2ParsedFile**        sourceFiles;
    uint32_t                    sourceFileCap;
    const H2ParsedFile*         savedFiles[H2_EVAL_CALL_MAX_DEPTH];
    uint8_t                     pushedFrames[H2_EVAL_CALL_MAX_DEPTH];
    struct HOPEvalMirExecCtx*   savedMirExecCtxs[H2_EVAL_CALL_MAX_DEPTH];
    uint32_t                    savedFileLen;
    uint32_t                    rootMirFnIndex;
    const H2MirProgram*         mirProgram;
    const H2MirFunction*        mirFunction;
    const H2CTFEValue*          mirLocals;
    uint32_t                    mirLocalCount;
    const H2MirProgram*         savedMirPrograms[H2_EVAL_CALL_MAX_DEPTH];
    const H2MirFunction*        savedMirFunctions[H2_EVAL_CALL_MAX_DEPTH];
    const H2CTFEValue*          savedMirLocals[H2_EVAL_CALL_MAX_DEPTH];
    uint32_t                    savedMirLocalCounts[H2_EVAL_CALL_MAX_DEPTH];
    uint32_t                    mirFrameDepth;
    HOPEvalTemplateBindingState savedTemplateBindings[H2_EVAL_CALL_MAX_DEPTH];
    uint8_t                     restoresTemplateBinding[H2_EVAL_CALL_MAX_DEPTH];
    HOPEvalTemplateBindingState pendingTemplateBinding;
    uint8_t                     hasPendingTemplateBinding;
} HOPEvalMirExecCtx;

int HOPEvalRunProgramInternal(
    const char* entryPath,
    const char* _Nullable platformTarget,
    const char* _Nullable archTarget,
    int testingBuild);

int32_t HOPEvalFindAnyFunctionBySlice(
    const HOPEvalProgram* p, const H2ParsedFile* callerFile, uint32_t nameStart, uint32_t nameEnd);
int32_t HOPEvalFindAnyFunctionBySliceInPackage(
    const HOPEvalProgram* p,
    const H2Package*      pkg,
    const H2ParsedFile*   callerFile,
    uint32_t              nameStart,
    uint32_t              nameEnd);
const H2Package* _Nullable HOPEvalFindPackageByFile(
    const HOPEvalProgram* p, const H2ParsedFile* file);
int32_t HOPEvalFindTopConstBySliceInPackage(
    const HOPEvalProgram* p,
    const H2Package*      pkg,
    const H2ParsedFile*   callerFile,
    uint32_t              nameStart,
    uint32_t              nameEnd);
int32_t HOPEvalFindTopVarBySliceInPackage(
    const HOPEvalProgram* p,
    const H2Package*      pkg,
    const H2ParsedFile*   callerFile,
    uint32_t              nameStart,
    uint32_t              nameEnd);
int32_t HOPEvalResolveFunctionBySlice(
    const HOPEvalProgram* p,
    const H2Package* _Nullable targetPkg,
    const H2ParsedFile* callerFile,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    const H2CTFEValue* _Nullable args,
    uint32_t argCount);
int HOPEvalTypeValueFromTypeNode(
    HOPEvalProgram* p, const H2ParsedFile* file, int32_t typeNode, H2CTFEValue* outValue);
void HOPEvalValueSetFunctionRef(H2CTFEValue* value, uint32_t fnIndex);
HOPEvalAggregate* _Nullable HOPEvalValueAsAggregate(const H2CTFEValue* value);
HOPEvalArray* _Nullable HOPEvalValueAsArray(const H2CTFEValue* value);
HOPEvalTaggedEnum* _Nullable HOPEvalValueAsTaggedEnum(const H2CTFEValue* value);
H2CTFEValue* _Nullable HOPEvalValueReferenceTarget(const H2CTFEValue* value);

int HOPEvalMirLookupLocalTypeNode(
    HOPEvalProgram*      p,
    uint32_t             nameStart,
    uint32_t             nameEnd,
    const H2ParsedFile** outFile,
    int32_t*             outTypeNode);
int HOPEvalMirLookupLocalValue(
    HOPEvalProgram* p, uint32_t nameStart, uint32_t nameEnd, H2CTFEValue* outValue);
int HOPEvalTryMirZeroInitType(
    HOPEvalProgram*     p,
    const H2ParsedFile* file,
    int32_t             typeNode,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    H2CTFEValue*        outValue,
    int*                outIsConst);
int HOPEvalTryMirEvalTopInit(
    HOPEvalProgram*     p,
    const H2ParsedFile* file,
    int32_t             initExprNode,
    int32_t             typeNode,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    const H2ParsedFile* typeFile,
    int32_t             expectedTypeNode,
    H2CTFEValue*        outValue,
    int*                outIsConst,
    int*                outSupported);
int HOPEvalMirBuildTopInitProgram(
    HOPEvalProgram*     p,
    const H2ParsedFile* file,
    int32_t             initExprNode,
    int32_t             typeNode,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    H2MirProgram*       outProgram,
    HOPEvalMirExecCtx*  outExecCtx,
    uint32_t*           outRootMirFnIndex,
    int*                outSupported);
void HOPEvalMirAdaptOutValue(
    const HOPEvalMirExecCtx* c, H2CTFEValue* _Nullable value, int* _Nullable inOutIsConst);
int HOPEvalTryMirEvalExprWithType(
    HOPEvalProgram*     p,
    const H2ParsedFile* file,
    int32_t             exprNode,
    const H2ParsedFile* typeFile,
    int32_t             typeNode,
    H2CTFEValue*        outValue,
    int*                outIsConst);
void HOPEvalMirInitExecEnv(
    HOPEvalProgram*     p,
    const H2ParsedFile* file,
    H2MirExecEnv*       env,
    HOPEvalMirExecCtx* _Nullable functionCtx);
int HOPEvalTryMirInvokeFunction(
    HOPEvalProgram*        p,
    const HOPEvalFunction* fn,
    int32_t                fnIndex,
    const H2CTFEValue*     args,
    uint32_t               argCount,
    H2CTFEValue*           outValue,
    int*                   outDidReturn,
    int*                   outIsConst);

H2_API_END
