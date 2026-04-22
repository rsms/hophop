#ifndef HOP_TYPECHECK_INTERNAL_H
#define HOP_TYPECHECK_INTERNAL_H

#include "../libhop-impl.h"
#include "../ctfe.h"
#include "../ctfe_exec.h"
#include "../fmt_parse.h"
#include "../mir.h"

HOP_API_BEGIN

typedef enum {
    HOPTCType_INVALID = 0,
    HOPTCType_BUILTIN,
    HOPTCType_NAMED,
    HOPTCType_ALIAS,
    HOPTCType_ANON_STRUCT,
    HOPTCType_ANON_UNION,
    HOPTCType_PTR,
    HOPTCType_REF,
    HOPTCType_ARRAY,
    HOPTCType_SLICE,
    HOPTCType_UNTYPED_INT,
    HOPTCType_UNTYPED_FLOAT,
    HOPTCType_FUNCTION,
    HOPTCType_TUPLE,
    HOPTCType_PACK,
    HOPTCType_TYPE_PARAM,
    HOPTCType_ANYTYPE,
    HOPTCType_OPTIONAL,
    HOPTCType_NULL,
} HOPTCTypeKind;

typedef enum {
    HOPBuiltin_INVALID = 0,
    HOPBuiltin_VOID,
    HOPBuiltin_BOOL,
    HOPBuiltin_STR,
    HOPBuiltin_TYPE,
    HOPBuiltin_U8,
    HOPBuiltin_U16,
    HOPBuiltin_U32,
    HOPBuiltin_U64,
    HOPBuiltin_I8,
    HOPBuiltin_I16,
    HOPBuiltin_I32,
    HOPBuiltin_I64,
    HOPBuiltin_USIZE,
    HOPBuiltin_ISIZE,
    HOPBuiltin_RAWPTR,
    HOPBuiltin_F32,
    HOPBuiltin_F64,
} HOPBuiltinKind;

typedef struct {
    HOPTCTypeKind  kind;
    HOPBuiltinKind builtin;
    int32_t        baseType;
    int32_t        declNode;
    int32_t        funcIndex;
    uint32_t       arrayLen;
    uint32_t       nameStart;
    uint32_t       nameEnd;
    uint32_t       fieldStart;
    uint16_t       fieldCount;
    uint16_t       flags;
} HOPTCType;

enum {
    HOPTCTypeFlag_VARSIZE = 1u << 0,
    HOPTCTypeFlag_MUTABLE = 1u << 1,
    HOPTCTypeFlag_FUNCTION_VARIADIC = 1u << 2,
    HOPTCTypeFlag_ALIAS_RESOLVING = 1u << 12,
    HOPTCTypeFlag_ALIAS_RESOLVED = 1u << 13,
    HOPTCTypeFlag_VISITING = 1u << 14,
    HOPTCTypeFlag_VISITED = 1u << 15,
};

typedef struct {
    uint32_t nameStart;
    uint32_t nameEnd;
    int32_t  typeId;
    uint32_t lenNameStart;
    uint32_t lenNameEnd;
    uint16_t flags;
} HOPTCField;

enum {
    HOPTCFieldFlag_DEPENDENT = 1u << 0,
    HOPTCFieldFlag_EMBEDDED = 1u << 1,
};

typedef struct {
    uint32_t nameStart;
    uint32_t nameEnd;
    int32_t  typeId;
    int32_t  declNode;
    int32_t  ownerTypeId;
    uint32_t templateArgStart;
    uint16_t templateArgCount;
    int16_t  templateRootNamedIndex;
} HOPTCNamedType;

typedef struct {
    uint32_t nameStart;
    uint32_t nameEnd;
    int32_t  returnType;
    uint32_t paramTypeStart;
    uint16_t paramCount;
    int32_t  contextType;
    int32_t  declNode;
    int32_t  defNode;
    int32_t  funcTypeId;
    uint32_t templateArgStart;
    uint16_t templateArgCount;
    int16_t  templateRootFuncIndex;
    uint16_t flags;
} HOPTCFunction;

enum {
    HOPTCFunctionFlag_VARIADIC = 1u << 0,
    HOPTCFunctionFlag_TEMPLATE = 1u << 1,
    HOPTCFunctionFlag_TEMPLATE_INSTANCE = 1u << 2,
    HOPTCFunctionFlag_TEMPLATE_HAS_ANYPACK = 1u << 3,
};

enum {
    HOPTCFuncParamFlag_CONST = 1u << 0,
};

typedef struct {
    uint32_t nameStart;
    uint32_t nameEnd;
    int32_t  typeId;
    int32_t  initExprNode;
    uint16_t flags;
    uint8_t  initState;
    uint8_t  _reserved;
    uint32_t useIndex;
} HOPTCLocal;

enum {
    HOPTCLocalFlag_CONST = 1u << 0,
    HOPTCLocalFlag_ANYPACK = 1u << 1,
};

enum {
    HOPTCLocalInit_UNTRACKED = 0,
    HOPTCLocalInit_UNINIT = 1,
    HOPTCLocalInit_INIT = 2,
    HOPTCLocalInit_MAYBE = 3,
};

enum {
    HOPTCLocalUseKind_LOCAL = 0,
    HOPTCLocalUseKind_PARAM = 1,
};

typedef struct {
    uint32_t nameStart;
    uint32_t nameEnd;
    int32_t  ownerFnIndex;
    uint32_t readCount;
    uint32_t writeCount;
    uint8_t  kind;
    uint8_t  suppressWarning;
    uint16_t _reserved;
} HOPTCLocalUse;

typedef struct {
    int32_t  localIdx;
    int32_t  enumTypeId;
    uint32_t variantStart;
    uint32_t variantEnd;
} HOPTCVariantNarrow;

typedef enum {
    HOPTCForInValueMode_VALUE = 0,
    HOPTCForInValueMode_REF,
    HOPTCForInValueMode_ANY,
} HOPTCForInValueMode;

typedef struct HOPTCConstEvalCtx HOPTCConstEvalCtx;
typedef struct {
    HOPTCConstEvalCtx*   evalCtx;
    HOPMirProgramBuilder builder;
    uint32_t*            tcToMir;
    uint8_t*             loweringFns;
    uint32_t*            topConstToMir;
    uint8_t*             loweringTopConsts;
    HOPDiag*             diag;
} HOPTCMirConstLowerCtx;

typedef struct {
    void* _Nullable ctx;
    HOPDiagSinkFn _Nullable onDiag;
} HOPTCDiagSink;

typedef struct {
    uint32_t start;
    uint32_t end;
    const char* _Nullable message;
} HOPTCWarningDedup;

typedef struct {
    int32_t nodeId;
    int32_t ownerFnIndex;
    uint8_t executed;
    uint8_t _reserved[3];
} HOPTCConstDiagUse;

typedef struct {
    int32_t callNode;
    int32_t ownerFnIndex;
    int32_t targetFnIndex;
} HOPTCCallTarget;

typedef struct {
    HOPArena*     arena;
    const HOPAst* ast;
    HOPStrView    src;
    HOPDiag* _Nullable diag;
    HOPTCDiagSink diagSink;

    HOPTCType* types;
    uint32_t   typeLen;
    uint32_t   typeCap;

    HOPTCField* fields;
    uint32_t    fieldLen;
    uint32_t    fieldCap;

    HOPTCNamedType* namedTypes;
    uint32_t        namedTypeLen;
    uint32_t        namedTypeCap;

    HOPTCFunction* funcs;
    uint32_t       funcLen;
    uint32_t       funcCap;
    uint8_t*       funcUsed;
    uint32_t       funcUsedCap;

    int32_t*  funcParamTypes;
    uint32_t* funcParamNameStarts;
    uint32_t* funcParamNameEnds;
    uint8_t*  funcParamFlags;
    uint32_t  funcParamLen;
    uint32_t  funcParamCap;

    int32_t* genericArgTypes;
    uint32_t genericArgLen;
    uint32_t genericArgCap;

    int32_t* scratchParamTypes;
    uint8_t* scratchParamFlags;
    uint32_t scratchParamCap;

    HOPTCLocal*    locals;
    uint32_t       localLen;
    uint32_t       localCap;
    HOPTCLocalUse* localUses;
    uint32_t       localUseLen;
    uint32_t       localUseCap;

    HOPTCVariantNarrow* variantNarrows;
    uint32_t            variantNarrowLen;
    uint32_t            variantNarrowCap;

    HOPCTFEValue* constEvalValues;
    uint8_t*      constEvalState;
    int32_t*      topVarLikeTypes;
    uint8_t*      topVarLikeTypeState;
    const char* _Nullable lastConstEvalReason;
    uint32_t lastConstEvalReasonStart;
    uint32_t lastConstEvalReasonEnd;

    int32_t typeVoid;
    int32_t typeBool;
    int32_t typeStr;
    int32_t typeType;
    int32_t typeRune;
    int32_t typeMemAllocator;
    int32_t typeUsize;
    int32_t typeRawptr;
    int32_t typeSourceLocation;
    int32_t typeFmtValue;
    int32_t typeUntypedInt;
    int32_t typeUntypedFloat;
    int32_t typeNull;
    int32_t typeAnytype;

    HOPTCWarningDedup* warningDedup;
    uint32_t           warningDedupLen;
    uint32_t           warningDedupCap;

    HOPTCConstDiagUse* constDiagUses;
    uint32_t           constDiagUseLen;
    uint32_t           constDiagUseCap;
    uint8_t*           constDiagFnInvoked;
    uint32_t           constDiagFnInvokedCap;
    HOPTCCallTarget*   callTargets;
    uint32_t           callTargetLen;
    uint32_t           callTargetCap;

    int32_t  currentContextType;
    int      hasImplicitMainRootContext;
    int32_t  implicitMainContextType;
    int32_t  activeExpectedCallType;
    int32_t  activeCallWithNode;
    int32_t  currentFunctionIndex;
    int      currentFunctionIsCompareHook;
    int32_t  activeTypeParamFnNode;
    int32_t  currentTypeOwnerTypeId;
    uint32_t activeGenericArgStart;
    uint16_t activeGenericArgCount;
    int32_t  activeGenericDeclNode;
    HOPTCConstEvalCtx* _Nullable activeConstEvalCtx;
    uint8_t compilerDiagPathProven;
    uint8_t allowAnytypeParamType;
    uint8_t allowConstNumericTypeName;

    const int32_t* _Nullable defaultFieldNodes;
    const int32_t* _Nullable defaultFieldTypes;
    uint32_t defaultFieldCount;
    uint32_t defaultFieldCurrentIndex;
} HOPTypeCheckCtx;

struct HOPConstEvalSession {
    HOPTypeCheckCtx tc;
};

enum {
    HOPTCConstEval_UNSEEN = 0,
    HOPTCConstEval_VISITING,
    HOPTCConstEval_READY,
    HOPTCConstEval_NONCONST,
};

enum {
    HOPTCTopVarLikeType_UNSEEN = 0,
    HOPTCTopVarLikeType_VISITING,
    HOPTCTopVarLikeType_READY,
};

#define HOPTC_MAX_ANON_FIELDS 256u

typedef struct {
    uint32_t nameStart;
    uint32_t nameEnd;
    int32_t  typeId;
} HOPTCAnonFieldSig;

#define HOPTC_DIAG_TEXT_CAP 128u

typedef struct {
    char* _Nullable ptr;
    uint32_t cap;
    uint32_t len;
} HOPTCTextBuf;

int32_t HOPTCInternPtrType(
    HOPTypeCheckCtx* c, int32_t baseType, uint32_t errStart, uint32_t errEnd);
int32_t HOPTCInternRefType(
    HOPTypeCheckCtx* c, int32_t baseType, int isMutable, uint32_t errStart, uint32_t errEnd);
int32_t HOPTCResolveAliasBaseType(HOPTypeCheckCtx* c, int32_t typeId);

int32_t HOPTCFindBuiltinByKind(HOPTypeCheckCtx* c, HOPBuiltinKind builtinKind);

enum {
    HOPTCTypeTagKind_INVALID = 0,
    HOPTCTypeTagKind_PRIMITIVE = 1,
    HOPTCTypeTagKind_ALIAS = 2,
    HOPTCTypeTagKind_STRUCT = 3,
    HOPTCTypeTagKind_UNION = 4,
    HOPTCTypeTagKind_ENUM = 5,
    HOPTCTypeTagKind_POINTER = 6,
    HOPTCTypeTagKind_REFERENCE = 7,
    HOPTCTypeTagKind_SLICE = 8,
    HOPTCTypeTagKind_ARRAY = 9,
    HOPTCTypeTagKind_OPTIONAL = 10,
    HOPTCTypeTagKind_FUNCTION = 11,
    HOPTCTypeTagKind_TUPLE = 12,
};

int32_t  HOPTCFindNamedTypeIndex(HOPTypeCheckCtx* c, uint32_t start, uint32_t end);
uint16_t HOPTCDeclTypeParamCount(HOPTypeCheckCtx* c, int32_t declNode);
int32_t  HOPTCDeclTypeParamIndex(
    HOPTypeCheckCtx* c, int32_t declNode, uint32_t nameStart, uint32_t nameEnd);
int HOPTCAppendDeclTypeParamPlaceholders(
    HOPTypeCheckCtx* c, int32_t declNode, uint32_t* outStart, uint16_t* outCount);
int32_t HOPTCInstantiateNamedType(
    HOPTypeCheckCtx* c, int32_t rootTypeId, const int32_t* argTypes, uint16_t argCount);
int32_t HOPTCSubstituteType(
    HOPTypeCheckCtx* c,
    int32_t          typeId,
    const int32_t*   paramTypes,
    const int32_t*   argTypes,
    uint16_t         argCount,
    uint32_t         errStart,
    uint32_t         errEnd);
int HOPTCEnsureNamedTypeFieldsResolved(HOPTypeCheckCtx* c, int32_t typeId);

typedef enum {
    HOPTCCompilerDiagOp_NONE = 0,
    HOPTCCompilerDiagOp_ERROR,
    HOPTCCompilerDiagOp_ERROR_AT,
    HOPTCCompilerDiagOp_WARN,
    HOPTCCompilerDiagOp_WARN_AT,
} HOPTCCompilerDiagOp;

int HOPTCResolveAliasTypeId(HOPTypeCheckCtx* c, int32_t typeId);
int HOPTCTypeExpr(HOPTypeCheckCtx* c, int32_t nodeId, int32_t* outType);
int HOPTCConstBoolExpr(HOPTypeCheckCtx* c, int32_t nodeId, int* out, int* isConst);
int HOPTCConstIntExpr(HOPTypeCheckCtx* c, int32_t nodeId, int64_t* out, int* isConst);
int HOPTCResolveReflectedTypeValueExpr(HOPTypeCheckCtx* c, int32_t exprNode, int32_t* outTypeId);

int HOPTCFieldLookup(
    HOPTypeCheckCtx* c,
    int32_t          typeId,
    uint32_t         fieldStart,
    uint32_t         fieldEnd,
    int32_t*         outType,
    uint32_t* _Nullable outFieldIndex);
int HOPTCIsTypeNodeKind(HOPAstKind kind);

typedef struct {
    int32_t  elemType;
    int      indexable;
    int      sliceable;
    int      sliceMutable;
    int      isStringLike;
    int      hasKnownLen;
    uint32_t knownLen;
} HOPTCIndexBaseInfo;

int32_t HOPTCFindTopLevelVarLikeNode(
    HOPTypeCheckCtx* c, uint32_t start, uint32_t end, int32_t* outNameIndex);
int HOPTCTypeTopLevelVarLikeNode(
    HOPTypeCheckCtx* c, int32_t nodeId, int32_t nameIndex, int32_t* outType);
int     HOPTCEnumTypeHasTagZero(HOPTypeCheckCtx* c, int32_t enumTypeId);
int     HOPTCIsTypeNodeKind(HOPAstKind kind);
int32_t HOPTCLocalFind(HOPTypeCheckCtx* c, uint32_t nameStart, uint32_t nameEnd);
int     HOPTCTypeExpr(HOPTypeCheckCtx* c, int32_t nodeId, int32_t* outType);
int     HOPTCTypeContainsVarSizeByValue(HOPTypeCheckCtx* c, int32_t typeId);
int     HOPTCResolveTypeNode(HOPTypeCheckCtx* c, int32_t nodeId, int32_t* outType);
int32_t HOPTCResolveAliasBaseType(HOPTypeCheckCtx* c, int32_t typeId);
int     HOPTCIsIntegerType(HOPTypeCheckCtx* c, int32_t typeId);
int     HOPTCIsFloatType(HOPTypeCheckCtx* c, int32_t typeId);
int     HOPTCIsBoolType(HOPTypeCheckCtx* c, int32_t typeId);

typedef struct {
    int32_t  nameListNode;
    int32_t  typeNode;
    int32_t  initNode;
    uint32_t nameCount;
    uint8_t  grouped;
} HOPTCVarLikeParts;

#define HOPTC_CONST_CALL_MAX_DEPTH 64u
#define HOPTC_CONST_FOR_MAX_ITERS  100000u

struct HOPTCConstEvalCtx {
    HOPTypeCheckCtx* tc;
    HOPCTFEExecCtx* _Nullable execCtx;
    const HOPMirProgram* _Nullable mirProgram;
    const HOPMirFunction* _Nullable mirFunction;
    const HOPCTFEValue* _Nullable mirLocals;
    uint32_t mirLocalCount;
    const HOPMirProgram* _Nullable mirSavedPrograms[HOPTC_CONST_CALL_MAX_DEPTH];
    const HOPMirFunction* _Nullable mirSavedFunctions[HOPTC_CONST_CALL_MAX_DEPTH];
    const HOPCTFEValue* _Nullable mirSavedLocals[HOPTC_CONST_CALL_MAX_DEPTH];
    uint32_t mirSavedLocalCounts[HOPTC_CONST_CALL_MAX_DEPTH];
    uint32_t mirFrameDepth;
    int32_t  fnStack[HOPTC_CONST_CALL_MAX_DEPTH];
    uint32_t fnDepth;
    const void* _Nullable callArgs;
    uint32_t callArgCount;
    const void* _Nullable callBinding;
    uint32_t callPackParamNameStart;
    uint32_t callPackParamNameEnd;
    const char* _Nullable nonConstReason;
    uint32_t nonConstStart;
    uint32_t nonConstEnd;
};

int HOPTCEvalTopLevelConstNode(
    HOPTypeCheckCtx*   c,
    HOPTCConstEvalCtx* evalCtx,
    int32_t            nodeId,
    HOPCTFEValue*      outValue,
    int*               outIsConst);
int HOPTCEvalTopLevelConstNodeAt(
    HOPTypeCheckCtx*   c,
    HOPTCConstEvalCtx* evalCtx,
    int32_t            nodeId,
    int32_t            nameIndex,
    HOPCTFEValue*      outValue,
    int*               outIsConst);
int HOPTCEvalConstExprNode(
    HOPTCConstEvalCtx* evalCtx, int32_t exprNode, HOPCTFEValue* outValue, int* outIsConst);
int HOPTCResolveConstCall(
    void*    ctx,
    uint32_t nameStart,
    uint32_t nameEnd,
    const HOPCTFEValue* _Nullable args,
    uint32_t      argCount,
    HOPCTFEValue* outValue,
    int*          outIsConst,
    HOPDiag* _Nullable diag);
int HOPTCResolveConstCallMir(
    void* _Nullable ctx,
    const HOPMirProgram* _Nullable program,
    const HOPMirFunction* _Nullable function,
    const HOPMirInst* _Nullable inst,
    uint32_t nameStart,
    uint32_t nameEnd,
    const HOPCTFEValue* _Nonnull args,
    uint32_t argCount,
    HOPCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);

/* Returns 1 if handled as span compound, 0 if not span-like, -1 on hard error. */

/* Returns 0 if handled, 1 if not a compiler diagnostic call, -1 on error */

/* Returns 0 if handled, 1 if not a reflection call, -1 on error */

int     HOPTCTypeContainsVarSizeByValue(HOPTypeCheckCtx* c, int32_t typeId);
int     HOPTCResolveTypeNode(HOPTypeCheckCtx* c, int32_t nodeId, int32_t* outType);
int32_t HOPTCResolveAliasBaseType(HOPTypeCheckCtx* c, int32_t typeId);
int     HOPTCTypeExpr(HOPTypeCheckCtx* c, int32_t nodeId, int32_t* outType);
int     HOPTCIsIntegerType(HOPTypeCheckCtx* c, int32_t typeId);

#define HOPTC_MAX_CALL_ARGS       128u
#define HOPTC_MAX_CALL_CANDIDATES 256u

int HOPTCTypeExpr(HOPTypeCheckCtx* c, int32_t nodeId, int32_t* outType);
int HOPTCTypeExprExpected(
    HOPTypeCheckCtx* c, int32_t nodeId, int32_t expectedType, int32_t* outType);
int HOPTCTypeExprExpected(
    HOPTypeCheckCtx* c, int32_t nodeId, int32_t expectedType, int32_t* outType);
int HOPTCExprIsCompoundTemporary(HOPTypeCheckCtx* c, int32_t exprNode);
int HOPTCExprNeedsExpectedType(HOPTypeCheckCtx* c, int32_t exprNode);
int HOPTCExprIsAssignable(HOPTypeCheckCtx* c, int32_t exprNode);

typedef struct {
    int32_t  argNode;
    int32_t  exprNode;
    uint32_t start;
    uint32_t end;
    uint32_t explicitNameStart;
    uint32_t explicitNameEnd;
    uint32_t implicitNameStart;
    uint32_t implicitNameEnd;
    uint8_t  spread;
    uint8_t  _reserved[3];
} HOPTCCallArgInfo;

typedef struct {
    HOPDiagCode code;
    uint32_t    start;
    uint32_t    end;
    uint32_t    argStart;
    uint32_t    argEnd;
} HOPTCCallMapError;

typedef struct {
    int      isVariadic;
    uint32_t fixedCount;
    uint32_t fixedInputCount;
    uint32_t spreadArgIndex;
    int32_t  variadicParamType;
    int32_t  variadicElemType;
    int32_t  fixedMappedArgExprNodes[HOPTC_MAX_CALL_ARGS];
    int32_t  argParamIndices[HOPTC_MAX_CALL_ARGS];
    int32_t  argExpectedTypes[HOPTC_MAX_CALL_ARGS];
} HOPTCCallBinding;

/* Returns 0 success, 1 no hook name, 2 no viable hook, 3 ambiguous */

/* Returns 0: success, 1: no name, 2: no match, 3: ambiguous, 4: writable-ref temporary,
 * 5: compound infer ambiguous, 6: named argument error, -1: error */

/* Returns 0: success, 1: no name, 2: no match, 3: ambiguous, 4: writable-ref temporary,
 * 5: compound infer ambiguous, 6: named argument error, -1: error */

/* Returns 0: success, 1: no name, 2: no match, 3: ambiguous, 4: writable-ref temporary,
 * 5: compound infer ambiguous, 6: named argument error, -1: error */

int HOPTCTypeExpr(HOPTypeCheckCtx* c, int32_t nodeId, int32_t* outType);
int HOPTCTypeExprExpected(
    HOPTypeCheckCtx* c, int32_t nodeId, int32_t expectedType, int32_t* outType);

/* Returns: 1 recognized variant pattern, 0 not a variant pattern, -1 error */

/* Returns: 1 resolved enum.variant type-name, 0 not an enum.variant type-name, -1 error */

/* Returns 0 for a segment, 1 for end-of-path, -1 for malformed path syntax. */

int HOPTCTypeExpr(HOPTypeCheckCtx* c, int32_t nodeId, int32_t* outType);

int HOPTCTypeStmt(
    HOPTypeCheckCtx* c, int32_t nodeId, int32_t returnType, int loopDepth, int switchDepth);

/* Describes a narrowable local found in a null-check condition. */
typedef struct {
    int32_t localIdx;  /* index in c->locals[] */
    int32_t innerType; /* T from ?T */
} HOPTCNullNarrow;

/*
 * Detects if condNode is a direct null check on a local optional variable:
 *   ident == null   ->  *outIsEq = 1
 *   ident != null   ->  *outIsEq = 0
 *   null == ident   ->  *outIsEq = 1  (symmetric)
 *   null != ident   ->  *outIsEq = 0  (symmetric)
 * Returns 1 if the pattern matched, 0 otherwise.
 */

/* Returns 1 if the last statement of blockNode is an unconditional terminator.
 */

/*
 * Saved narrowing: remembers the original type of a local so it can be restored
 * after the narrowed region ends.
 */
typedef struct {
    int32_t localIdx;
    int32_t savedType;
} HOPTCNarrowSave;

/* Shared declarations for multi-file typechecker internals. */

void HOPTCSetDiag(HOPDiag* diag, HOPDiagCode code, uint32_t start, uint32_t end);
void HOPTCSetDiagWithArg(
    HOPDiag*    diag,
    HOPDiagCode code,
    uint32_t    start,
    uint32_t    end,
    uint32_t    argStart,
    uint32_t    argEnd);
int HOPTCFailSpan(HOPTypeCheckCtx* c, HOPDiagCode code, uint32_t start, uint32_t end);
int HOPTCFailDuplicateDefinition(
    HOPTypeCheckCtx* c,
    uint32_t         nameStart,
    uint32_t         nameEnd,
    uint32_t         otherStart,
    uint32_t         otherEnd);
int HOPTCFailNode(HOPTypeCheckCtx* c, int32_t nodeId, HOPDiagCode code);
const char* _Nullable HOPTCAllocCStringBytes(
    HOPTypeCheckCtx* c, const uint8_t* bytes, uint32_t len);
int  HOPTCStrEqNullable(const char* _Nullable a, const char* _Nullable b);
int  HOPTCEmitWarningDiag(HOPTypeCheckCtx* c, const HOPDiag* diag);
int  HOPTCRecordConstDiagUse(HOPTypeCheckCtx* c, int32_t nodeId);
void HOPTCMarkConstDiagUseExecuted(HOPTypeCheckCtx* c, int32_t nodeId);
void HOPTCMarkConstDiagFnInvoked(HOPTypeCheckCtx* c, int32_t fnIndex);
int  HOPTCValidateConstDiagUses(HOPTypeCheckCtx* c);
void HOPTCMarkFunctionUsed(HOPTypeCheckCtx* c, int32_t fnIndex);
void HOPTCMarkLocalRead(HOPTypeCheckCtx* c, int32_t localIdx);
void HOPTCMarkLocalWrite(HOPTypeCheckCtx* c, int32_t localIdx);
void HOPTCUnmarkLocalRead(HOPTypeCheckCtx* c, int32_t localIdx);
void HOPTCMarkLocalInitialized(HOPTypeCheckCtx* c, int32_t localIdx);
int  HOPTCCheckLocalInitialized(HOPTypeCheckCtx* c, int32_t localIdx, uint32_t start, uint32_t end);
int  HOPTCTypeIsTrackedPtrRef(HOPTypeCheckCtx* c, int32_t typeId);
int  HOPTCFailTopLevelPtrRefMissingInitializer(
    HOPTypeCheckCtx* c, uint32_t start, uint32_t end, uint32_t nameStart, uint32_t nameEnd);
void HOPTCSetLocalUsageKind(HOPTypeCheckCtx* c, int32_t localIdx, uint8_t kind);
void HOPTCSetLocalUsageSuppress(HOPTypeCheckCtx* c, int32_t localIdx, int suppress);
int  HOPTCEmitUnusedSymbolWarnings(HOPTypeCheckCtx* c);
int  HOPTCRecordCallTarget(HOPTypeCheckCtx* c, int32_t callNode, int32_t targetFnIndex);
int  HOPTCFindCallTarget(
    const HOPTypeCheckCtx* c, int32_t ownerFnIndex, int32_t callNode, int32_t* outTargetFnIndex);
void HOPTCOffsetToLineCol(
    const char* src, uint32_t srcLen, uint32_t offset, uint32_t* outLine, uint32_t* outColumn);
int HOPTCLineColToOffset(
    const char* src, uint32_t srcLen, uint32_t line, uint32_t column, uint32_t* outOffset);
void        HOPTCTextBufInit(HOPTCTextBuf* b, char* ptr, uint32_t cap);
void        HOPTCTextBufAppendChar(HOPTCTextBuf* b, char ch);
void        HOPTCTextBufAppendCStr(HOPTCTextBuf* b, const char* s);
void        HOPTCTextBufAppendSlice(HOPTCTextBuf* b, HOPStrView src, uint32_t start, uint32_t end);
void        HOPTCTextBufAppendU32(HOPTCTextBuf* b, uint32_t v);
void        HOPTCTextBufAppendHexU64(HOPTCTextBuf* b, uint64_t v);
const char* HOPTCBuiltinName(HOPTypeCheckCtx* c, int32_t typeId, HOPBuiltinKind kind);
void        HOPTCFormatTypeRec(HOPTypeCheckCtx* c, int32_t typeId, HOPTCTextBuf* b, uint32_t depth);
int         HOPTCExprIsStringConstant(HOPTypeCheckCtx* c, int32_t nodeId);
void        HOPTCFormatExprSubject(HOPTypeCheckCtx* c, int32_t nodeId, HOPTCTextBuf* b);
char* _Nullable HOPTCAllocDiagText(HOPTypeCheckCtx* c, const char* text);
int HOPTCFailTypeMismatchDetail(
    HOPTypeCheckCtx* c, int32_t failNode, int32_t exprNode, int32_t srcType, int32_t dstType);
int HOPTCFailAssignToConst(HOPTypeCheckCtx* c, int32_t lhsNode);
int HOPTCFailSwitchMissingCases(
    HOPTypeCheckCtx* c,
    int32_t          failNode,
    int32_t          subjectType,
    int32_t          subjectEnumType,
    uint32_t         enumVariantCount,
    const uint32_t* _Nullable enumVariantStarts,
    const uint32_t* _Nullable enumVariantEnds,
    const uint8_t* _Nullable enumCovered,
    int boolCoveredTrue,
    int boolCoveredFalse);
int32_t HOPAstFirstChild(const HOPAst* ast, int32_t nodeId);
int32_t HOPAstNextSibling(const HOPAst* ast, int32_t nodeId);
int HOPNameEqSlice(HOPStrView src, uint32_t aStart, uint32_t aEnd, uint32_t bStart, uint32_t bEnd);
int HOPNameEqLiteral(HOPStrView src, uint32_t start, uint32_t end, const char* lit);
int HOPNameHasPrefix(HOPStrView src, uint32_t start, uint32_t end, const char* prefix);
int HOPNameHasSuffix(HOPStrView src, uint32_t start, uint32_t end, const char* suffix);
int32_t  HOPTCFindMemAllocatorType(HOPTypeCheckCtx* c);
int32_t  HOPTCGetStrRefType(HOPTypeCheckCtx* c, uint32_t start, uint32_t end);
int32_t  HOPTCGetStrPtrType(HOPTypeCheckCtx* c, uint32_t start, uint32_t end);
int32_t  HOPTCAddType(HOPTypeCheckCtx* c, const HOPTCType* t, uint32_t errStart, uint32_t errEnd);
int32_t  HOPTCAddBuiltinType(HOPTypeCheckCtx* c, const char* name, HOPBuiltinKind builtinKind);
int      HOPTCEnsureInitialized(HOPTypeCheckCtx* c);
int32_t  HOPTCFindBuiltinType(HOPTypeCheckCtx* c, uint32_t start, uint32_t end);
int32_t  HOPTCFindBuiltinByKind(HOPTypeCheckCtx* c, HOPBuiltinKind builtinKind);
uint8_t  HOPTCTypeTagKindOf(HOPTypeCheckCtx* c, int32_t typeId);
uint64_t HOPTCEncodeTypeTag(HOPTypeCheckCtx* c, int32_t typeId);
int      HOPTCDecodeTypeTag(HOPTypeCheckCtx* c, uint64_t typeTag, int32_t* outTypeId);
int32_t  HOPTCResolveTypeValueName(HOPTypeCheckCtx* c, uint32_t start, uint32_t end);
int32_t  HOPTCFindNamedTypeIndex(HOPTypeCheckCtx* c, uint32_t start, uint32_t end);
int32_t  HOPTCFindNamedTypeIndexOwned(
    HOPTypeCheckCtx* c, int32_t ownerTypeId, uint32_t start, uint32_t end);
int32_t HOPTCResolveTypeNamePath(
    HOPTypeCheckCtx* c, uint32_t start, uint32_t end, int32_t ownerTypeId);
int32_t HOPTCFindNamedTypeByLiteral(HOPTypeCheckCtx* c, const char* name);
int32_t HOPTCFindBuiltinQualifiedNamedType(HOPTypeCheckCtx* c, uint32_t start, uint32_t end);
int32_t HOPTCFindBuiltinNamedTypeBySuffix(HOPTypeCheckCtx* c, const char* suffix);
int32_t HOPTCFindNamedTypeBySuffix(HOPTypeCheckCtx* c, const char* suffix);
int32_t HOPTCFindReflectKindType(HOPTypeCheckCtx* c);
int     HOPTCNameEqLiteralOrPkgBuiltin(
    HOPTypeCheckCtx* c, uint32_t start, uint32_t end, const char* name, const char* pkgPrefix);
HOPTCCompilerDiagOp HOPTCCompilerDiagOpFromName(HOPTypeCheckCtx* c, uint32_t start, uint32_t end);
int                 HOPTCIsSourceLocationOfName(HOPTypeCheckCtx* c, uint32_t start, uint32_t end);
int32_t             HOPTCFindSourceLocationType(HOPTypeCheckCtx* c);
int32_t             HOPTCFindFmtValueType(HOPTypeCheckCtx* c);
int                 HOPTCTypeIsSourceLocation(HOPTypeCheckCtx* c, int32_t typeId);
int                 HOPTCTypeIsFmtValue(HOPTypeCheckCtx* c, int32_t typeId);
int32_t             HOPTCFindFunctionIndex(HOPTypeCheckCtx* c, uint32_t start, uint32_t end);
int32_t HOPTCFindBuiltinQualifiedFunctionIndex(HOPTypeCheckCtx* c, uint32_t start, uint32_t end);
int32_t HOPTCFindPlainFunctionValueIndex(HOPTypeCheckCtx* c, uint32_t start, uint32_t end);
int32_t HOPTCFindPkgQualifiedFunctionValueIndex(
    HOPTypeCheckCtx* c, uint32_t pkgStart, uint32_t pkgEnd, uint32_t nameStart, uint32_t nameEnd);
int HOPTCFunctionNameEq(const HOPTypeCheckCtx* c, uint32_t funcIndex, uint32_t start, uint32_t end);
int HOPTCNameEqPkgPrefixedMethod(
    HOPTypeCheckCtx* c,
    uint32_t         candidateStart,
    uint32_t         candidateEnd,
    uint32_t         pkgStart,
    uint32_t         pkgEnd,
    uint32_t         methodStart,
    uint32_t         methodEnd);
int HOPTCExtractPkgPrefixFromTypeName(
    HOPTypeCheckCtx* c,
    uint32_t         typeNameStart,
    uint32_t         typeNameEnd,
    uint32_t*        outPkgStart,
    uint32_t*        outPkgEnd);
int HOPTCImportDefaultAliasEq(
    HOPStrView src, uint32_t pathStart, uint32_t pathEnd, uint32_t aliasStart, uint32_t aliasEnd);
int HOPTCHasImportAlias(HOPTypeCheckCtx* c, uint32_t aliasStart, uint32_t aliasEnd);
int HOPTCResolveReceiverPkgPrefix(
    HOPTypeCheckCtx* c, int32_t typeId, uint32_t* outPkgStart, uint32_t* outPkgEnd);
int HOPTCResolveEnumMemberType(
    HOPTypeCheckCtx* c,
    int32_t          recvNode,
    uint32_t         memberStart,
    uint32_t         memberEnd,
    int32_t*         outType);
int32_t HOPTCInternPtrType(
    HOPTypeCheckCtx* c, int32_t baseType, uint32_t errStart, uint32_t errEnd);
int     HOPTCTypeIsMutable(const HOPTCType* t);
int     HOPTCIsMutableRefType(HOPTypeCheckCtx* c, int32_t typeId);
int32_t HOPTCInternRefType(
    HOPTypeCheckCtx* c, int32_t baseType, int isMutable, uint32_t errStart, uint32_t errEnd);
int32_t HOPTCInternArrayType(
    HOPTypeCheckCtx* c, int32_t baseType, uint32_t arrayLen, uint32_t errStart, uint32_t errEnd);
int32_t HOPTCInternSliceType(
    HOPTypeCheckCtx* c, int32_t baseType, int isMutable, uint32_t errStart, uint32_t errEnd);
int32_t HOPTCInternOptionalType(
    HOPTypeCheckCtx* c, int32_t baseType, uint32_t errStart, uint32_t errEnd);
int32_t HOPTCInternAnonAggregateType(
    HOPTypeCheckCtx*         c,
    int                      isUnion,
    const HOPTCAnonFieldSig* fields,
    uint32_t                 fieldCount,
    int32_t                  declNode,
    uint32_t                 errStart,
    uint32_t                 errEnd);
int HOPTCFunctionTypeMatchesSignature(
    HOPTypeCheckCtx* c,
    const HOPTCType* t,
    int32_t          returnType,
    const int32_t*   paramTypes,
    const uint8_t*   paramFlags,
    uint32_t         paramCount,
    int              isVariadic);
int32_t HOPTCInternFunctionType(
    HOPTypeCheckCtx* c,
    int32_t          returnType,
    const int32_t*   paramTypes,
    const uint8_t*   paramFlags,
    uint32_t         paramCount,
    int              isVariadic,
    int32_t          funcIndex,
    uint32_t         errStart,
    uint32_t         errEnd);
int HOPTCTupleTypeMatchesSignature(
    HOPTypeCheckCtx* c, const HOPTCType* t, const int32_t* elemTypes, uint32_t elemCount);
int32_t HOPTCInternTupleType(
    HOPTypeCheckCtx* c,
    const int32_t*   elemTypes,
    uint32_t         elemCount,
    uint32_t         errStart,
    uint32_t         errEnd);
int32_t HOPTCInternPackType(
    HOPTypeCheckCtx* c,
    const int32_t*   elemTypes,
    uint32_t         elemCount,
    uint32_t         errStart,
    uint32_t         errEnd);
int      HOPTCParseArrayLen(HOPTypeCheckCtx* c, const HOPAstNode* node, uint32_t* outLen);
int      HOPTCResolveIndexBaseInfo(HOPTypeCheckCtx* c, int32_t baseType, HOPTCIndexBaseInfo* out);
int32_t  HOPTCListItemAt(const HOPAst* ast, int32_t listNode, uint32_t index);
uint32_t HOPTCListCount(const HOPAst* ast, int32_t listNode);
int      HOPTCVarLikeGetParts(HOPTypeCheckCtx* c, int32_t nodeId, HOPTCVarLikeParts* out);
int      HOPTCHasForeignImportDirective(const HOPAst* ast, HOPStrView src, int32_t nodeId);
int32_t  HOPTCVarLikeNameIndexBySlice(
    HOPTypeCheckCtx* c, int32_t nodeId, uint32_t start, uint32_t end);
int32_t HOPTCVarLikeInitExprNodeAt(HOPTypeCheckCtx* c, int32_t nodeId, int32_t nameIndex);
int32_t HOPTCVarLikeInitExprNode(HOPTypeCheckCtx* c, int32_t nodeId);
void    HOPTCConstSetReason(
    HOPTCConstEvalCtx* evalCtx, uint32_t start, uint32_t end, const char* reason);
void HOPTCConstSetReasonNode(HOPTCConstEvalCtx* evalCtx, int32_t nodeId, const char* reason);
void HOPTCAttachConstEvalReason(HOPTypeCheckCtx* c);
int  HOPTCResolveConstIdent(
    void*         ctx,
    uint32_t      nameStart,
    uint32_t      nameEnd,
    HOPCTFEValue* outValue,
    int*          outIsConst,
    HOPDiag* _Nullable diag);
int HOPTCConstLookupExecBindingType(
    HOPTCConstEvalCtx* evalCtx, uint32_t nameStart, uint32_t nameEnd, int32_t* outType);
int HOPTCConstLookupMirLocalType(
    HOPTCConstEvalCtx* evalCtx, uint32_t nameStart, uint32_t nameEnd, int32_t* outType);
int      HOPTCConstBuiltinSizeBytes(HOPBuiltinKind b, uint64_t* outBytes);
int      HOPTCConstBuiltinAlignBytes(HOPBuiltinKind b, uint64_t* outAlign);
uint64_t HOPTCConstAlignUpU64(uint64_t v, uint64_t align);
int      HOPTCConstTypeLayout(
    HOPTypeCheckCtx* c, int32_t typeId, uint64_t* outSize, uint64_t* outAlign, uint32_t depth);
int HOPTCConstEvalSizeOf(
    HOPTCConstEvalCtx* evalCtx, int32_t exprNode, HOPCTFEValue* outValue, int* outIsConst);
int HOPTCConstEvalCast(
    HOPTCConstEvalCtx* evalCtx, int32_t exprNode, HOPCTFEValue* outValue, int* outIsConst);
int HOPTCConstEvalTypeOf(
    HOPTCConstEvalCtx* evalCtx, int32_t exprNode, HOPCTFEValue* outValue, int* outIsConst);
int HOPTCResolveReflectedTypeValueExpr(HOPTypeCheckCtx* c, int32_t exprNode, int32_t* outTypeId);
int HOPTCConstEvalTypeNameValue(
    HOPTypeCheckCtx* c, int32_t typeId, HOPCTFEValue* outValue, int* outIsConst);
void HOPTCConstEvalSetNullValue(HOPCTFEValue* outValue);
void HOPTCConstEvalSetSourceLocationFromOffsets(
    HOPTypeCheckCtx* c, uint32_t startOffset, uint32_t endOffset, HOPCTFEValue* outValue);
int HOPTCConstEvalSourceLocationOfCall(
    HOPTCConstEvalCtx* evalCtx, int32_t exprNode, HOPCTFEValue* outValue, int* outIsConst);
int HOPTCConstEvalU32Arg(
    HOPTCConstEvalCtx* evalCtx, int32_t exprNode, uint32_t* outValue, int* outIsConst);
int HOPTCConstEvalSourceLocationCompound(
    HOPTCConstEvalCtx* evalCtx,
    int32_t            exprNode,
    int                forceSourceLocation,
    HOPCTFEValue*      outValue,
    int*               outIsConst);
int HOPTCConstEvalSourceLocationExpr(
    HOPTCConstEvalCtx* evalCtx, int32_t exprNode, HOPCTFESpan* outSpan, int* outIsConst);
int HOPTCConstEvalCompilerDiagCall(
    HOPTCConstEvalCtx* evalCtx, int32_t exprNode, HOPCTFEValue* outValue, int* outIsConst);
int HOPTCConstEvalTypeReflectionCall(
    HOPTCConstEvalCtx* evalCtx, int32_t exprNode, HOPCTFEValue* outValue, int* outIsConst);
int HOPTCEvalConstExprNode(
    HOPTCConstEvalCtx* evalCtx, int32_t exprNode, HOPCTFEValue* outValue, int* outIsConst);
int HOPTCEvalConstExecExprCb(void* ctx, int32_t exprNode, HOPCTFEValue* outValue, int* outIsConst);
int HOPTCEvalConstExecResolveTypeCb(void* ctx, int32_t typeNode, int32_t* outTypeId);
int HOPTCEvalConstExecInferValueTypeCb(void* ctx, const HOPCTFEValue* value, int32_t* outTypeId);
int HOPTCMirConstZeroInitLocal(
    void* _Nullable ctx,
    const HOPMirTypeRef* _Nonnull typeRef,
    HOPCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);
int HOPTCMirConstCoerceValueForType(
    void* _Nullable ctx,
    const HOPMirTypeRef* _Nonnull typeRef,
    HOPCTFEValue* _Nonnull inOutValue,
    HOPDiag* _Nullable diag);
int HOPTCMirConstIndexValue(
    void* _Nullable ctx,
    const HOPCTFEValue* _Nonnull base,
    const HOPCTFEValue* _Nonnull index,
    HOPCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);
int HOPTCMirConstSequenceLen(
    void* _Nullable ctx,
    const HOPCTFEValue* _Nonnull base,
    HOPCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);
int HOPTCMirConstIterInit(
    void* _Nullable ctx,
    uint32_t sourceNode,
    const HOPCTFEValue* _Nonnull source,
    uint16_t flags,
    HOPCTFEValue* _Nonnull outIter,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);
int HOPTCMirConstIterNext(
    void* _Nullable ctx,
    const HOPCTFEValue* _Nonnull iterValue,
    uint16_t flags,
    int* _Nonnull outHasItem,
    HOPCTFEValue* _Nonnull outKey,
    int* _Nonnull outKeyIsConst,
    HOPCTFEValue* _Nonnull outValue,
    int* _Nonnull outValueIsConst,
    HOPDiag* _Nullable diag);
int HOPTCMirConstAggGetField(
    void* _Nullable ctx,
    const HOPCTFEValue* _Nonnull base,
    uint32_t nameStart,
    uint32_t nameEnd,
    HOPCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);
int HOPTCMirConstAggAddrField(
    void* _Nullable ctx,
    const HOPCTFEValue* _Nonnull base,
    uint32_t nameStart,
    uint32_t nameEnd,
    HOPCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);
int HOPTCMirConstMakeTuple(
    void* _Nullable ctx,
    const HOPCTFEValue* _Nonnull elems,
    uint32_t elemCount,
    uint32_t typeNodeHint,
    HOPCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);
int HOPTCEvalConstForInIndexCb(
    void* _Nullable ctx,
    HOPCTFEExecCtx* _Nonnull execCtx,
    const HOPCTFEValue* _Nonnull sourceValue,
    uint32_t index,
    int      byRef,
    HOPCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst);
int32_t HOPTCFindConstCallableFunction(
    HOPTypeCheckCtx* c, uint32_t nameStart, uint32_t nameEnd, uint32_t argCount);
int HOPTCResolveForInIterator(
    HOPTypeCheckCtx* c,
    int32_t          sourceNode,
    int32_t          sourceType,
    int32_t*         outFnIndex,
    int32_t*         outIterType);
int HOPTCResolveForInNextValue(
    HOPTypeCheckCtx*    c,
    int32_t             iterPtrType,
    HOPTCForInValueMode valueMode,
    int32_t*            outValueType,
    int32_t*            outFn);
int HOPTCResolveForInNextKey(
    HOPTypeCheckCtx* c, int32_t iterPtrType, int32_t* outKeyType, int32_t* outFn);
int HOPTCResolveForInNextKeyAndValue(
    HOPTypeCheckCtx*    c,
    int32_t             iterPtrType,
    HOPTCForInValueMode valueMode,
    int32_t*            outKeyType,
    int32_t*            outValueType,
    int32_t*            outFn);
int HOPTCResolveConstCall(
    void*    ctx,
    uint32_t nameStart,
    uint32_t nameEnd,
    const HOPCTFEValue* _Nullable args,
    uint32_t      argCount,
    HOPCTFEValue* outValue,
    int*          outIsConst,
    HOPDiag* _Nullable diag);
int HOPTCResolveConstCallMirPre(
    void* _Nullable ctx,
    const HOPMirProgram* _Nullable program,
    const HOPMirFunction* _Nullable function,
    const HOPMirInst* _Nullable inst,
    uint32_t      nameStart,
    uint32_t      nameEnd,
    HOPCTFEValue* outValue,
    int*          outIsConst,
    HOPDiag* _Nullable diag);
int HOPTCMirConstBindFrame(
    void* _Nullable ctx,
    const HOPMirProgram* _Nullable program,
    const HOPMirFunction* _Nullable function,
    const HOPCTFEValue* _Nullable locals,
    uint32_t localCount,
    HOPDiag* _Nullable diag);
void HOPTCMirConstUnbindFrame(void* _Nullable ctx);
void HOPTCMirConstAdoptLowerDiagReason(HOPTCConstEvalCtx* evalCtx, const HOPDiag* _Nullable diag);
int  HOPTCMirConstLowerConstExpr(
    void* _Nullable ctx, int32_t exprNode, HOPMirConst* _Nonnull outValue, HOPDiag* _Nullable diag);
int HOPTCEvalTopLevelConstNodeAt(
    HOPTypeCheckCtx*   c,
    HOPTCConstEvalCtx* evalCtx,
    int32_t            nodeId,
    int32_t            nameIndex,
    HOPCTFEValue*      outValue,
    int*               outIsConst);
int HOPTCEvalTopLevelConstNode(
    HOPTypeCheckCtx*   c,
    HOPTCConstEvalCtx* evalCtx,
    int32_t            nodeId,
    HOPCTFEValue*      outValue,
    int*               outIsConst);
int HOPTCConstBoolExpr(HOPTypeCheckCtx* c, int32_t nodeId, int* out, int* isConst);
int HOPTCConstIntExpr(HOPTypeCheckCtx* c, int32_t nodeId, int64_t* out, int* isConst);
int HOPTCConstFloatExpr(HOPTypeCheckCtx* c, int32_t nodeId, double* out, int* isConst);
int HOPTCConstStringExpr(
    HOPTypeCheckCtx* c,
    int32_t          nodeId,
    const uint8_t**  outBytes,
    uint32_t*        outLen,
    int*             outIsConst);
void HOPTCMarkRuntimeBoundsCheck(HOPTypeCheckCtx* c, int32_t nodeId);
int  HOPTCResolveAnonAggregateTypeNode(
    HOPTypeCheckCtx* c, int32_t nodeId, int isUnion, int32_t* outType);
int     HOPTCResolveAliasTypeId(HOPTypeCheckCtx* c, int32_t typeId);
int32_t HOPTCResolveAliasBaseType(HOPTypeCheckCtx* c, int32_t typeId);
int     HOPTCFnNodeHasTypeParamName(
    HOPTypeCheckCtx* c, int32_t fnNode, uint32_t nameStart, uint32_t nameEnd);
int HOPTCResolveActiveTypeParamType(
    HOPTypeCheckCtx* c, uint32_t nameStart, uint32_t nameEnd, int32_t* outType);
int HOPTCMirConstInitLowerCtx(HOPTCConstEvalCtx* evalCtx, HOPTCMirConstLowerCtx* _Nonnull outCtx);
int HOPTCMirConstLowerFunction(
    HOPTCMirConstLowerCtx* c, int32_t fnIndex, uint32_t* _Nullable outMirFnIndex);
int HOPTCMirConstRewriteDirectCalls(
    HOPTCMirConstLowerCtx* c, uint32_t mirFnIndex, int32_t rootNode);
int HOPTCResolveTypeNode(HOPTypeCheckCtx* c, int32_t nodeId, int32_t* outType);
int HOPTCAddNamedType(HOPTypeCheckCtx* c, int32_t nodeId, int32_t ownerTypeId, int32_t* outTypeId);
int HOPTCCollectTypeDeclsFromNode(HOPTypeCheckCtx* c, int32_t nodeId);
int HOPTCIsIntegerType(HOPTypeCheckCtx* c, int32_t typeId);
int HOPTCIsConstNumericType(HOPTypeCheckCtx* c, int32_t typeId);
int HOPTCTypeIsRuneLike(HOPTypeCheckCtx* c, int32_t typeId);
int HOPTCConstIntFitsType(HOPTypeCheckCtx* c, int64_t value, int32_t typeId);
int HOPTCConstIntFitsFloatType(HOPTypeCheckCtx* c, int64_t value, int32_t typeId);
int HOPTCConstFloatFitsType(HOPTypeCheckCtx* c, double value, int32_t typeId);
int HOPTCFailConstIntRange(HOPTypeCheckCtx* c, int32_t nodeId, int64_t value, int32_t expectedType);
int HOPTCFailConstFloatRange(HOPTypeCheckCtx* c, int32_t nodeId, int32_t expectedType);
int HOPTCIsFloatType(HOPTypeCheckCtx* c, int32_t typeId);
int HOPTCIsNumericType(HOPTypeCheckCtx* c, int32_t typeId);
int HOPTCIsBoolType(HOPTypeCheckCtx* c, int32_t typeId);
int HOPTCIsRawptrType(HOPTypeCheckCtx* c, int32_t typeId);
int HOPTCIsNamedDeclKind(HOPTypeCheckCtx* c, int32_t typeId, HOPAstKind kind);
int HOPTCIsStringLikeType(HOPTypeCheckCtx* c, int32_t typeId);
int HOPTCTypeSupportsFmtReflectRec(HOPTypeCheckCtx* c, int32_t typeId, uint32_t depth);
int HOPTCIsComparableTypeRec(HOPTypeCheckCtx* c, int32_t typeId, uint32_t depth);
int HOPTCIsOrderedTypeRec(HOPTypeCheckCtx* c, int32_t typeId, uint32_t depth);
int HOPTCIsComparableType(HOPTypeCheckCtx* c, int32_t typeId);
int HOPTCIsOrderedType(HOPTypeCheckCtx* c, int32_t typeId);
int HOPTCTypeSupportsLen(HOPTypeCheckCtx* c, int32_t typeId);
int HOPTCIsUntyped(HOPTypeCheckCtx* c, int32_t typeId);
int HOPTCIsTypeNodeKind(HOPAstKind kind);
int HOPTCConcretizeInferredType(HOPTypeCheckCtx* c, int32_t typeId, int32_t* outType);
int HOPTCTypeIsVarSize(HOPTypeCheckCtx* c, int32_t typeId);
int HOPTCTypeContainsVarSizeByValue(HOPTypeCheckCtx* c, int32_t typeId);
int HOPTCIsComparisonHookName(
    HOPTypeCheckCtx* c, uint32_t nameStart, uint32_t nameEnd, int* outIsEqualHook);
int     HOPTCTypeIsU8Slice(HOPTypeCheckCtx* c, int32_t typeId, int requireMutable);
int     HOPTCTypeIsFreeablePointer(HOPTypeCheckCtx* c, int32_t typeId);
int32_t HOPTCFindEmbeddedFieldIndex(HOPTypeCheckCtx* c, int32_t namedTypeId);
int     HOPTCEmbedDistanceToType(
    HOPTypeCheckCtx* c, int32_t srcType, int32_t dstType, uint32_t* outDistance);
int HOPTCIsTypeDerivedFromEmbedded(HOPTypeCheckCtx* c, int32_t srcType, int32_t dstType);
int HOPTCCanAssign(HOPTypeCheckCtx* c, int32_t dstType, int32_t srcType);
int HOPTCValidateMemAllocatorArg(HOPTypeCheckCtx* c, int32_t nodeId, int32_t allocBaseType);
int HOPTCCoerceForBinary(HOPTypeCheckCtx* c, int32_t leftType, int32_t rightType, int32_t* outType);
int HOPTCConversionCost(HOPTypeCheckCtx* c, int32_t dstType, int32_t srcType, uint8_t* outCost);
int HOPTCCostVectorCompare(const uint8_t* a, const uint8_t* b, uint32_t len);
int32_t HOPTCUnwrapCallArgExprNode(HOPTypeCheckCtx* c, int32_t argNode);
int     HOPTCCollectCallArgInfo(
    HOPTypeCheckCtx*  c,
    int32_t           callNode,
    int32_t           calleeNode,
    int               includeReceiver,
    int32_t           receiverNode,
    HOPTCCallArgInfo* outArgs,
    int32_t* _Nullable outArgTypes,
    uint32_t* outArgCount);
int     HOPTCIsMainFunction(HOPTypeCheckCtx* c, const HOPTCFunction* fn);
int32_t HOPTCResolveImplicitMainContextType(HOPTypeCheckCtx* c);
int     HOPTCCurrentContextFieldType(
    HOPTypeCheckCtx* c, uint32_t fieldStart, uint32_t fieldEnd, int32_t* outType);
int HOPTCCurrentContextFieldTypeByLiteral(
    HOPTypeCheckCtx* c, const char* fieldName, int32_t* outType);
int32_t HOPTCContextFindOverlayNode(HOPTypeCheckCtx* c);
int32_t HOPTCContextFindDirectNode(HOPTypeCheckCtx* c);
int32_t HOPTCContextFindOverlayBindMatch(
    HOPTypeCheckCtx* c, uint32_t fieldStart, uint32_t fieldEnd, const char* _Nullable fieldName);
int32_t HOPTCContextFindOverlayBindByLiteral(HOPTypeCheckCtx* c, const char* fieldName);
int     HOPTCGetEffectiveContextFieldType(
    HOPTypeCheckCtx* c, uint32_t fieldStart, uint32_t fieldEnd, int32_t* outType);
int HOPTCGetEffectiveContextFieldTypeByLiteral(
    HOPTypeCheckCtx* c, const char* fieldName, int32_t* outType);
int HOPTCValidateCurrentCallOverlay(HOPTypeCheckCtx* c);
int HOPTCValidateCallContextRequirements(HOPTypeCheckCtx* c, int32_t requiredContextType);
int HOPTCGetFunctionTypeSignature(
    HOPTypeCheckCtx* c,
    int32_t          typeId,
    int32_t*         outReturnType,
    uint32_t*        outParamStart,
    uint32_t*        outParamCount,
    int* _Nullable outIsVariadic);
void HOPTCCallMapErrorClear(HOPTCCallMapError* err);
int  HOPTCMapCallArgsToParams(
    HOPTypeCheckCtx*        c,
    const HOPTCCallArgInfo* callArgs,
    uint32_t                argCount,
    const uint32_t*         paramNameStarts,
    const uint32_t*         paramNameEnds,
    uint32_t                paramCount,
    uint32_t                firstPositionalArgIndex,
    int32_t*                outMappedArgExprNodes,
    HOPTCCallMapError* _Nullable outError);
int HOPTCPrepareCallBinding(
    HOPTypeCheckCtx*        c,
    const HOPTCCallArgInfo* callArgs,
    uint32_t                argCount,
    const uint32_t*         paramNameStarts,
    const uint32_t*         paramNameEnds,
    const int32_t*          paramTypes,
    uint32_t                paramCount,
    int                     isVariadic,
    int                     allowNamedMapping,
    uint32_t                firstPositionalArgIndex,
    HOPTCCallBinding*       outBinding,
    HOPTCCallMapError*      outError);
int HOPTCCheckConstParamArgs(
    HOPTypeCheckCtx*        c,
    const HOPTCCallArgInfo* callArgs,
    uint32_t                argCount,
    const HOPTCCallBinding* binding,
    const uint32_t*         paramNameStarts,
    const uint32_t*         paramNameEnds,
    const uint8_t*          paramFlags,
    uint32_t                paramCount,
    HOPTCCallMapError*      outError);
int HOPTCFunctionHasAnytypeParam(HOPTypeCheckCtx* c, int32_t fnIndex);
int HOPTCInstantiateAnytypeFunctionForCall(
    HOPTypeCheckCtx*        c,
    int32_t                 fnIndex,
    const HOPTCCallArgInfo* callArgs,
    uint32_t                argCount,
    uint32_t                firstPositionalArgIndex,
    int32_t                 autoRefFirstArgType,
    int32_t*                outFuncIndex,
    HOPTCCallMapError*      outError);
int HOPTCResolveComparisonHookArgCost(
    HOPTypeCheckCtx* c, int32_t paramType, int32_t argType, uint8_t* outCost);
int HOPTCResolveComparisonHook(
    HOPTypeCheckCtx* c,
    const char*      hookName,
    int32_t          lhsType,
    int32_t          rhsType,
    int32_t*         outFuncIndex);
void HOPTCGatherCallCandidates(
    HOPTypeCheckCtx* c,
    uint32_t         nameStart,
    uint32_t         nameEnd,
    int32_t*         outCandidates,
    uint32_t*        outCandidateCount,
    int*             outNameFound);
void HOPTCGatherCallCandidatesByPkgMethod(
    HOPTypeCheckCtx* c,
    uint32_t         pkgStart,
    uint32_t         pkgEnd,
    uint32_t         methodStart,
    uint32_t         methodEnd,
    int32_t*         outCandidates,
    uint32_t*        outCandidateCount,
    int*             outNameFound);
int HOPTCResolveCallFromCandidates(
    HOPTypeCheckCtx*        c,
    const int32_t*          candidates,
    uint32_t                candidateCount,
    int                     nameFound,
    const HOPTCCallArgInfo* callArgs,
    uint32_t                argCount,
    uint32_t                firstPositionalArgIndex,
    int                     autoRefFirstArg,
    int32_t*                outFuncIndex,
    int32_t*                outMutRefTempArgNode);
int HOPTCResolveCallByName(
    HOPTypeCheckCtx*        c,
    uint32_t                nameStart,
    uint32_t                nameEnd,
    const HOPTCCallArgInfo* callArgs,
    uint32_t                argCount,
    uint32_t                firstPositionalArgIndex,
    int                     autoRefFirstArg,
    int32_t*                outFuncIndex,
    int32_t*                outMutRefTempArgNode);
int HOPTCResolveCallByPkgMethod(
    HOPTypeCheckCtx*        c,
    uint32_t                pkgStart,
    uint32_t                pkgEnd,
    uint32_t                methodStart,
    uint32_t                methodEnd,
    const HOPTCCallArgInfo* callArgs,
    uint32_t                argCount,
    uint32_t                firstPositionalArgIndex,
    int                     autoRefFirstArg,
    int32_t*                outFuncIndex,
    int32_t*                outMutRefTempArgNode);
int HOPTCResolveDependentPtrReturnForCall(
    HOPTypeCheckCtx* c, int32_t fnIndex, int32_t argNode, int32_t* outType);
int HOPTCResolveNamedTypeFields(HOPTypeCheckCtx* c, uint32_t namedIndex);
int HOPTCResolveAllNamedTypeFields(HOPTypeCheckCtx* c);
int HOPTCResolveAllTypeAliases(HOPTypeCheckCtx* c);
int HOPTCCheckEmbeddedCycleFrom(HOPTypeCheckCtx* c, int32_t typeId);
int HOPTCCheckEmbeddedCycles(HOPTypeCheckCtx* c);
int HOPTCPropagateVarSizeNamedTypes(HOPTypeCheckCtx* c);
int HOPTCReadFunctionSig(
    HOPTypeCheckCtx* c,
    int32_t          funNode,
    int32_t*         outReturnType,
    uint32_t*        outParamCount,
    int*             outIsVariadic,
    int32_t*         outContextType,
    int*             outHasBody);
int     HOPTCCollectFunctionFromNode(HOPTypeCheckCtx* c, int32_t nodeId);
int     HOPTCFinalizeFunctionTypes(HOPTypeCheckCtx* c);
int32_t HOPTCFindTopLevelVarLikeNode(
    HOPTypeCheckCtx* c, uint32_t start, uint32_t end, int32_t* outNameIndex);
int HOPTCTypeTopLevelVarLikeNode(
    HOPTypeCheckCtx* c, int32_t nodeId, int32_t nameIndex, int32_t* outType);
int32_t HOPTCLocalFind(HOPTypeCheckCtx* c, uint32_t nameStart, uint32_t nameEnd);
int     HOPTCLocalAdd(
    HOPTypeCheckCtx* c,
    uint32_t         nameStart,
    uint32_t         nameEnd,
    int32_t          typeId,
    int              isConst,
    int32_t          initExprNode);
int HOPTCVariantNarrowPush(
    HOPTypeCheckCtx* c,
    int32_t          localIdx,
    int32_t          enumTypeId,
    uint32_t         variantStart,
    uint32_t         variantEnd);
int HOPTCVariantNarrowFind(
    HOPTypeCheckCtx* c, int32_t localIdx, const HOPTCVariantNarrow** outNarrow);
int32_t HOPTCEnumDeclFirstVariantNode(HOPTypeCheckCtx* c, int32_t enumDeclNode);
int32_t HOPTCEnumVariantTagExprNode(HOPTypeCheckCtx* c, int32_t variantNode);
int32_t HOPTCFindEnumVariantNodeByName(
    HOPTypeCheckCtx* c, int32_t enumTypeId, uint32_t variantStart, uint32_t variantEnd);
int HOPTCEnumVariantPayloadFieldType(
    HOPTypeCheckCtx* c,
    int32_t          enumTypeId,
    uint32_t         variantStart,
    uint32_t         variantEnd,
    uint32_t         fieldStart,
    uint32_t         fieldEnd,
    int32_t*         outType);
int HOPTCEnumTypeHasTagZero(HOPTypeCheckCtx* c, int32_t enumTypeId);
int HOPTCCasePatternParts(
    HOPTypeCheckCtx* c, int32_t caseLabelNode, int32_t* outExprNode, int32_t* outAliasNode);
int HOPTCDecodeVariantPatternExpr(
    HOPTypeCheckCtx* c,
    int32_t          exprNode,
    int32_t*         outEnumType,
    uint32_t*        outVariantStart,
    uint32_t*        outVariantEnd);
int HOPTCResolveEnumVariantTypeName(
    HOPTypeCheckCtx* c,
    int32_t          typeNameNode,
    int32_t*         outEnumType,
    uint32_t*        outVariantStart,
    uint32_t*        outVariantEnd);
int HOPTCFieldLookup(
    HOPTypeCheckCtx* c,
    int32_t          typeId,
    uint32_t         fieldStart,
    uint32_t         fieldEnd,
    int32_t*         outType,
    uint32_t* _Nullable outFieldIndex);
int HOPTCIsAsciiSpace(unsigned char ch);
int HOPTCIsIdentStartChar(unsigned char ch);
int HOPTCIsIdentContinueChar(unsigned char ch);
int HOPTCFieldPathNextSegment(
    HOPTypeCheckCtx* c,
    uint32_t         pathStart,
    uint32_t         pathEnd,
    uint32_t*        ioPos,
    uint32_t*        outSegStart,
    uint32_t*        outSegEnd);
int HOPTCFieldLookupPath(
    HOPTypeCheckCtx* c,
    int32_t          ownerTypeId,
    uint32_t         pathStart,
    uint32_t         pathEnd,
    int32_t*         outType);
int HOPTCTypeNewExpr(HOPTypeCheckCtx* c, int32_t nodeId, int32_t* outType);
int HOPTCExprIsCompoundTemporary(HOPTypeCheckCtx* c, int32_t exprNode);
int HOPTCExprNeedsExpectedType(HOPTypeCheckCtx* c, int32_t exprNode);
int HOPTCResolveIdentifierExprType(
    HOPTypeCheckCtx* c,
    uint32_t         nameStart,
    uint32_t         nameEnd,
    uint32_t         spanStart,
    uint32_t         spanEnd,
    int32_t*         outType);
int HOPTCInferAnonStructTypeFromCompound(
    HOPTypeCheckCtx* c, int32_t nodeId, int32_t firstField, int32_t* outType);
int HOPTCTypeCompoundLit(
    HOPTypeCheckCtx* c, int32_t nodeId, int32_t expectedType, int32_t* outType);
int HOPTCTypeExprExpected(
    HOPTypeCheckCtx* c, int32_t nodeId, int32_t expectedType, int32_t* outType);
int HOPTCTypeAssignTargetExpr(
    HOPTypeCheckCtx* c, int32_t nodeId, int skipDirectIdentRead, int32_t* outType);
void HOPTCMarkDirectIdentLocalWrite(HOPTypeCheckCtx* c, int32_t nodeId, int markInitialized);
int  HOPTCExprIsAssignable(HOPTypeCheckCtx* c, int32_t exprNode);
int  HOPTCExprIsConstAssignTarget(HOPTypeCheckCtx* c, int32_t exprNode);
int  HOPTCTypeExpr_IDENT(HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* n, int32_t* outType);
int  HOPTCTypeExpr_INT(HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* n, int32_t* outType);
int  HOPTCTypeExpr_FLOAT(HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* n, int32_t* outType);
int HOPTCTypeExpr_STRING(HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* n, int32_t* outType);
int HOPTCTypeExpr_RUNE(HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* n, int32_t* outType);
int HOPTCTypeExpr_BOOL(HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* n, int32_t* outType);
int HOPTCTypeExpr_COMPOUND_LIT(
    HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* n, int32_t* outType);
int HOPTCTypeExpr_CALL_WITH_CONTEXT(
    HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* n, int32_t* outType);
int HOPTCTypeExpr_NEW(HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* n, int32_t* outType);
int HOPTCTypeSourceLocationOfCall(
    HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* callee, int32_t* outType);
int HOPTCTypeCompilerDiagCall(
    HOPTypeCheckCtx*    c,
    int32_t             nodeId,
    const HOPAstNode*   callee,
    HOPTCCompilerDiagOp op,
    int32_t*            outType);
int HOPTCTypeExpr_CALL(HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* n, int32_t* outType);
int HOPTCTypeExpr_CAST(HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* n, int32_t* outType);
int HOPTCTypeExpr_SIZEOF(HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* n, int32_t* outType);
int HOPTCTypeExpr_FIELD_EXPR(
    HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* n, int32_t* outType);
int HOPTCTypeExpr_INDEX(HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* n, int32_t* outType);
int HOPTCTypeExpr_UNARY(HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* n, int32_t* outType);
int HOPTCTypeExpr_BINARY(HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* n, int32_t* outType);
int HOPTCTypeExpr_NULL(HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* n, int32_t* outType);
int HOPTCTypeExpr_UNWRAP(HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* n, int32_t* outType);
int HOPTCTypeExpr_TUPLE_EXPR(
    HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* n, int32_t* outType);
int HOPTCTypeExpr(HOPTypeCheckCtx* c, int32_t nodeId, int32_t* outType);
int HOPTCValidateConstInitializerExprNode(HOPTypeCheckCtx* c, int32_t initNode);
int HOPTCValidateLocalConstVarLikeInitializers(
    HOPTypeCheckCtx* c, int32_t nodeId, const HOPTCVarLikeParts* parts);
int HOPTCTypeVarLike(HOPTypeCheckCtx* c, int32_t nodeId);
int HOPTCTypeTopLevelVarLikes(HOPTypeCheckCtx* c, HOPAstKind wantKind);
int HOPTCTypeTopLevelConsts(HOPTypeCheckCtx* c);
int HOPTCTypeTopLevelVars(HOPTypeCheckCtx* c);
int HOPTCCheckTopLevelConstInitializers(HOPTypeCheckCtx* c);
int HOPTCValidateTopLevelConstEvaluable(HOPTypeCheckCtx* c);
int HOPTCGetNullNarrow(HOPTypeCheckCtx* c, int32_t condNode, int* outIsEq, HOPTCNullNarrow* out);
int HOPTCGetOptionalCondNarrow(
    HOPTypeCheckCtx* c, int32_t condNode, int* outThenIsSome, HOPTCNullNarrow* out);
int HOPTCBlockTerminates(HOPTypeCheckCtx* c, int32_t blockNode);
int HOPTCTypeBlock(
    HOPTypeCheckCtx* c, int32_t blockNode, int32_t returnType, int loopDepth, int switchDepth);
int HOPTCTypeForStmt(
    HOPTypeCheckCtx* c, int32_t nodeId, int32_t returnType, int loopDepth, int switchDepth);
int HOPTCTypeSwitchStmt(
    HOPTypeCheckCtx* c, int32_t nodeId, int32_t returnType, int loopDepth, int switchDepth);
int HOPTCExprIsBlankIdent(HOPTypeCheckCtx* c, int32_t exprNode);
int HOPTCTypeMultiAssignStmt(HOPTypeCheckCtx* c, int32_t nodeId);
int HOPTCTypeStmt(
    HOPTypeCheckCtx* c, int32_t nodeId, int32_t returnType, int loopDepth, int switchDepth);
int HOPTCTypeFunctionBody(HOPTypeCheckCtx* c, int32_t funcIndex);
int HOPTCMarkTemplateRootFunctionUses(HOPTypeCheckCtx* c);
int HOPTCCollectFunctionDecls(HOPTypeCheckCtx* c);
int HOPTCCollectTypeDecls(HOPTypeCheckCtx* c);
int HOPTCBuildCheckedContext(
    HOPArena*     arena,
    const HOPAst* ast,
    HOPStrView    src,
    const HOPTypeCheckOptions* _Nullable options,
    HOPDiag* _Nullable diag,
    HOPTypeCheckCtx* _Nullable outCtx);
int HOPTypeCheckEx(
    HOPArena*     arena,
    const HOPAst* ast,
    HOPStrView    src,
    const HOPTypeCheckOptions* _Nullable options,
    HOPDiag* _Nullable diag);
int HOPTypeCheck(HOPArena* arena, const HOPAst* ast, HOPStrView src, HOPDiag* _Nullable diag);
int HOPConstEvalSessionInit(
    HOPArena*             arena,
    const HOPAst*         ast,
    HOPStrView            src,
    HOPConstEvalSession** outSession,
    HOPDiag* _Nullable diag);
int HOPConstEvalSessionEvalExpr(
    HOPConstEvalSession* session, int32_t exprNode, HOPCTFEValue* outValue, int* outIsConst);
int HOPConstEvalSessionEvalIntExpr(
    HOPConstEvalSession* session, int32_t exprNode, int64_t* outValue, int* outIsConst);
int HOPConstEvalSessionEvalTopLevelConst(
    HOPConstEvalSession* session, int32_t constNode, HOPCTFEValue* outValue, int* outIsConst);
int HOPConstEvalSessionDecodeTypeTag(
    HOPConstEvalSession* session, uint64_t typeTag, int32_t* outTypeId);
int HOPConstEvalSessionGetTypeInfo(
    HOPConstEvalSession* session, int32_t typeId, HOPConstEvalTypeInfo* outTypeInfo);

HOP_API_END

#endif
