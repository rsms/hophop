#ifndef H2_TYPECHECK_INTERNAL_H
#define H2_TYPECHECK_INTERNAL_H

#include "../libhop-impl.h"
#include "../ctfe.h"
#include "../ctfe_exec.h"
#include "../fmt_parse.h"
#include "../mir.h"

H2_API_BEGIN

typedef enum {
    H2TCType_INVALID = 0,
    H2TCType_BUILTIN,
    H2TCType_NAMED,
    H2TCType_ALIAS,
    H2TCType_ANON_STRUCT,
    H2TCType_ANON_UNION,
    H2TCType_PTR,
    H2TCType_REF,
    H2TCType_ARRAY,
    H2TCType_SLICE,
    H2TCType_UNTYPED_INT,
    H2TCType_UNTYPED_FLOAT,
    H2TCType_FUNCTION,
    H2TCType_TUPLE,
    H2TCType_PACK,
    H2TCType_TYPE_PARAM,
    H2TCType_ANYTYPE,
    H2TCType_OPTIONAL,
    H2TCType_NULL,
} H2TCTypeKind;

typedef enum {
    H2Builtin_INVALID = 0,
    H2Builtin_VOID,
    H2Builtin_BOOL,
    H2Builtin_STR,
    H2Builtin_TYPE,
    H2Builtin_U8,
    H2Builtin_U16,
    H2Builtin_U32,
    H2Builtin_U64,
    H2Builtin_I8,
    H2Builtin_I16,
    H2Builtin_I32,
    H2Builtin_I64,
    H2Builtin_USIZE,
    H2Builtin_ISIZE,
    H2Builtin_RAWPTR,
    H2Builtin_F32,
    H2Builtin_F64,
} H2BuiltinKind;

typedef struct {
    H2TCTypeKind  kind;
    H2BuiltinKind builtin;
    int32_t       baseType;
    int32_t       declNode;
    int32_t       funcIndex;
    uint32_t      arrayLen;
    uint32_t      nameStart;
    uint32_t      nameEnd;
    uint32_t      fieldStart;
    uint16_t      fieldCount;
    uint16_t      flags;
} H2TCType;

enum {
    H2TCTypeFlag_VARSIZE = 1u << 0,
    H2TCTypeFlag_MUTABLE = 1u << 1,
    H2TCTypeFlag_FUNCTION_VARIADIC = 1u << 2,
    H2TCTypeFlag_ALIAS_RESOLVING = 1u << 12,
    H2TCTypeFlag_ALIAS_RESOLVED = 1u << 13,
    H2TCTypeFlag_VISITING = 1u << 14,
    H2TCTypeFlag_VISITED = 1u << 15,
};

typedef struct {
    uint32_t nameStart;
    uint32_t nameEnd;
    int32_t  typeId;
    uint32_t lenNameStart;
    uint32_t lenNameEnd;
    uint16_t flags;
} H2TCField;

enum {
    H2TCFieldFlag_DEPENDENT = 1u << 0,
    H2TCFieldFlag_EMBEDDED = 1u << 1,
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
} H2TCNamedType;

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
} H2TCFunction;

enum {
    H2TCFunctionFlag_VARIADIC = 1u << 0,
    H2TCFunctionFlag_TEMPLATE = 1u << 1,
    H2TCFunctionFlag_TEMPLATE_INSTANCE = 1u << 2,
    H2TCFunctionFlag_TEMPLATE_HAS_ANYPACK = 1u << 3,
};

enum {
    H2TCFuncParamFlag_CONST = 1u << 0,
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
} H2TCLocal;

enum {
    H2TCLocalFlag_CONST = 1u << 0,
    H2TCLocalFlag_ANYPACK = 1u << 1,
};

enum {
    H2TCLocalInit_UNTRACKED = 0,
    H2TCLocalInit_UNINIT = 1,
    H2TCLocalInit_INIT = 2,
    H2TCLocalInit_MAYBE = 3,
};

enum {
    H2TCLocalUseKind_LOCAL = 0,
    H2TCLocalUseKind_PARAM = 1,
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
} H2TCLocalUse;

typedef struct {
    int32_t  localIdx;
    int32_t  enumTypeId;
    uint32_t variantStart;
    uint32_t variantEnd;
} H2TCVariantNarrow;

typedef enum {
    H2TCForInValueMode_VALUE = 0,
    H2TCForInValueMode_REF,
    H2TCForInValueMode_ANY,
} H2TCForInValueMode;

typedef struct H2TCConstEvalCtx H2TCConstEvalCtx;
typedef struct {
    H2TCConstEvalCtx*   evalCtx;
    H2MirProgramBuilder builder;
    uint32_t*           tcToMir;
    uint8_t*            loweringFns;
    uint32_t*           topConstToMir;
    uint8_t*            loweringTopConsts;
    H2Diag*             diag;
} H2TCMirConstLowerCtx;

typedef struct {
    void* _Nullable ctx;
    H2DiagSinkFn _Nullable onDiag;
} H2TCDiagSink;

typedef struct {
    uint32_t start;
    uint32_t end;
    const char* _Nullable message;
} H2TCWarningDedup;

typedef struct {
    int32_t nodeId;
    int32_t ownerFnIndex;
    uint8_t executed;
    uint8_t _reserved[3];
} H2TCConstDiagUse;

typedef struct {
    int32_t callNode;
    int32_t ownerFnIndex;
    int32_t targetFnIndex;
} H2TCCallTarget;

typedef struct {
    H2Arena*     arena;
    const H2Ast* ast;
    H2StrView    src;
    H2Diag* _Nullable diag;
    H2TCDiagSink diagSink;

    H2TCType* types;
    uint32_t  typeLen;
    uint32_t  typeCap;

    H2TCField* fields;
    uint32_t   fieldLen;
    uint32_t   fieldCap;

    H2TCNamedType* namedTypes;
    uint32_t       namedTypeLen;
    uint32_t       namedTypeCap;

    H2TCFunction* funcs;
    uint32_t      funcLen;
    uint32_t      funcCap;
    uint8_t*      funcUsed;
    uint32_t      funcUsedCap;

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

    H2TCLocal*    locals;
    uint32_t      localLen;
    uint32_t      localCap;
    H2TCLocalUse* localUses;
    uint32_t      localUseLen;
    uint32_t      localUseCap;

    H2TCVariantNarrow* variantNarrows;
    uint32_t           variantNarrowLen;
    uint32_t           variantNarrowCap;

    H2CTFEValue* constEvalValues;
    uint8_t*     constEvalState;
    int32_t*     topVarLikeTypes;
    uint8_t*     topVarLikeTypeState;
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

    H2TCWarningDedup* warningDedup;
    uint32_t          warningDedupLen;
    uint32_t          warningDedupCap;

    H2TCConstDiagUse* constDiagUses;
    uint32_t          constDiagUseLen;
    uint32_t          constDiagUseCap;
    uint8_t*          constDiagFnInvoked;
    uint32_t          constDiagFnInvokedCap;
    H2TCCallTarget*   callTargets;
    uint32_t          callTargetLen;
    uint32_t          callTargetCap;

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
    H2TCConstEvalCtx* _Nullable activeConstEvalCtx;
    uint8_t compilerDiagPathProven;
    uint8_t allowAnytypeParamType;
    uint8_t allowConstNumericTypeName;

    const int32_t* _Nullable defaultFieldNodes;
    const int32_t* _Nullable defaultFieldTypes;
    uint32_t defaultFieldCount;
    uint32_t defaultFieldCurrentIndex;
} H2TypeCheckCtx;

struct H2ConstEvalSession {
    H2TypeCheckCtx tc;
};

enum {
    H2TCConstEval_UNSEEN = 0,
    H2TCConstEval_VISITING,
    H2TCConstEval_READY,
    H2TCConstEval_NONCONST,
};

enum {
    H2TCTopVarLikeType_UNSEEN = 0,
    H2TCTopVarLikeType_VISITING,
    H2TCTopVarLikeType_READY,
};

#define H2TC_MAX_ANON_FIELDS 256u

typedef struct {
    uint32_t nameStart;
    uint32_t nameEnd;
    int32_t  typeId;
} H2TCAnonFieldSig;

#define H2TC_DIAG_TEXT_CAP 128u

typedef struct {
    char* _Nullable ptr;
    uint32_t cap;
    uint32_t len;
} H2TCTextBuf;

int32_t H2TCInternPtrType(H2TypeCheckCtx* c, int32_t baseType, uint32_t errStart, uint32_t errEnd);
int32_t H2TCInternRefType(
    H2TypeCheckCtx* c, int32_t baseType, int isMutable, uint32_t errStart, uint32_t errEnd);
int32_t H2TCResolveAliasBaseType(H2TypeCheckCtx* c, int32_t typeId);

int32_t H2TCFindBuiltinByKind(H2TypeCheckCtx* c, H2BuiltinKind builtinKind);

enum {
    H2TCTypeTagKind_INVALID = 0,
    H2TCTypeTagKind_PRIMITIVE = 1,
    H2TCTypeTagKind_ALIAS = 2,
    H2TCTypeTagKind_STRUCT = 3,
    H2TCTypeTagKind_UNION = 4,
    H2TCTypeTagKind_ENUM = 5,
    H2TCTypeTagKind_POINTER = 6,
    H2TCTypeTagKind_REFERENCE = 7,
    H2TCTypeTagKind_SLICE = 8,
    H2TCTypeTagKind_ARRAY = 9,
    H2TCTypeTagKind_OPTIONAL = 10,
    H2TCTypeTagKind_FUNCTION = 11,
    H2TCTypeTagKind_TUPLE = 12,
};

int32_t  H2TCFindNamedTypeIndex(H2TypeCheckCtx* c, uint32_t start, uint32_t end);
uint16_t H2TCDeclTypeParamCount(H2TypeCheckCtx* c, int32_t declNode);
int32_t  H2TCDeclTypeParamIndex(
    H2TypeCheckCtx* c, int32_t declNode, uint32_t nameStart, uint32_t nameEnd);
int H2TCAppendDeclTypeParamPlaceholders(
    H2TypeCheckCtx* c, int32_t declNode, uint32_t* outStart, uint16_t* outCount);
int32_t H2TCInstantiateNamedType(
    H2TypeCheckCtx* c, int32_t rootTypeId, const int32_t* argTypes, uint16_t argCount);
int32_t H2TCSubstituteType(
    H2TypeCheckCtx* c,
    int32_t         typeId,
    const int32_t*  paramTypes,
    const int32_t*  argTypes,
    uint16_t        argCount,
    uint32_t        errStart,
    uint32_t        errEnd);
int H2TCEnsureNamedTypeFieldsResolved(H2TypeCheckCtx* c, int32_t typeId);

typedef enum {
    H2TCCompilerDiagOp_NONE = 0,
    H2TCCompilerDiagOp_ERROR,
    H2TCCompilerDiagOp_ERROR_AT,
    H2TCCompilerDiagOp_WARN,
    H2TCCompilerDiagOp_WARN_AT,
} H2TCCompilerDiagOp;

int H2TCResolveAliasTypeId(H2TypeCheckCtx* c, int32_t typeId);
int H2TCTypeExpr(H2TypeCheckCtx* c, int32_t nodeId, int32_t* outType);
int H2TCConstBoolExpr(H2TypeCheckCtx* c, int32_t nodeId, int* out, int* isConst);
int H2TCConstIntExpr(H2TypeCheckCtx* c, int32_t nodeId, int64_t* out, int* isConst);
int H2TCResolveReflectedTypeValueExpr(H2TypeCheckCtx* c, int32_t exprNode, int32_t* outTypeId);

int H2TCFieldLookup(
    H2TypeCheckCtx* c,
    int32_t         typeId,
    uint32_t        fieldStart,
    uint32_t        fieldEnd,
    int32_t*        outType,
    uint32_t* _Nullable outFieldIndex);
int H2TCIsTypeNodeKind(H2AstKind kind);

typedef struct {
    int32_t  elemType;
    int      indexable;
    int      sliceable;
    int      sliceMutable;
    int      isStringLike;
    int      hasKnownLen;
    uint32_t knownLen;
} H2TCIndexBaseInfo;

int32_t H2TCFindTopLevelVarLikeNode(
    H2TypeCheckCtx* c, uint32_t start, uint32_t end, int32_t* outNameIndex);
int H2TCTypeTopLevelVarLikeNode(
    H2TypeCheckCtx* c, int32_t nodeId, int32_t nameIndex, int32_t* outType);
int     H2TCEnumTypeHasTagZero(H2TypeCheckCtx* c, int32_t enumTypeId);
int     H2TCIsTypeNodeKind(H2AstKind kind);
int32_t H2TCLocalFind(H2TypeCheckCtx* c, uint32_t nameStart, uint32_t nameEnd);
int     H2TCTypeExpr(H2TypeCheckCtx* c, int32_t nodeId, int32_t* outType);
int     H2TCTypeContainsVarSizeByValue(H2TypeCheckCtx* c, int32_t typeId);
int     H2TCResolveTypeNode(H2TypeCheckCtx* c, int32_t nodeId, int32_t* outType);
int32_t H2TCResolveAliasBaseType(H2TypeCheckCtx* c, int32_t typeId);
int     H2TCIsIntegerType(H2TypeCheckCtx* c, int32_t typeId);
int     H2TCIsFloatType(H2TypeCheckCtx* c, int32_t typeId);
int     H2TCIsBoolType(H2TypeCheckCtx* c, int32_t typeId);

typedef struct {
    int32_t  nameListNode;
    int32_t  typeNode;
    int32_t  initNode;
    uint32_t nameCount;
    uint8_t  grouped;
} H2TCVarLikeParts;

#define H2TC_CONST_CALL_MAX_DEPTH 64u
#define H2TC_CONST_FOR_MAX_ITERS  100000u

struct H2TCConstEvalCtx {
    H2TypeCheckCtx* tc;
    H2CTFEExecCtx* _Nullable execCtx;
    const H2MirProgram* _Nullable mirProgram;
    const H2MirFunction* _Nullable mirFunction;
    const H2CTFEValue* _Nullable mirLocals;
    uint32_t mirLocalCount;
    const H2MirProgram* _Nullable mirSavedPrograms[H2TC_CONST_CALL_MAX_DEPTH];
    const H2MirFunction* _Nullable mirSavedFunctions[H2TC_CONST_CALL_MAX_DEPTH];
    const H2CTFEValue* _Nullable mirSavedLocals[H2TC_CONST_CALL_MAX_DEPTH];
    uint32_t mirSavedLocalCounts[H2TC_CONST_CALL_MAX_DEPTH];
    uint32_t mirFrameDepth;
    int32_t  fnStack[H2TC_CONST_CALL_MAX_DEPTH];
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

int H2TCEvalTopLevelConstNode(
    H2TypeCheckCtx*   c,
    H2TCConstEvalCtx* evalCtx,
    int32_t           nodeId,
    H2CTFEValue*      outValue,
    int*              outIsConst);
int H2TCEvalTopLevelConstNodeAt(
    H2TypeCheckCtx*   c,
    H2TCConstEvalCtx* evalCtx,
    int32_t           nodeId,
    int32_t           nameIndex,
    H2CTFEValue*      outValue,
    int*              outIsConst);
int H2TCEvalConstExprNode(
    H2TCConstEvalCtx* evalCtx, int32_t exprNode, H2CTFEValue* outValue, int* outIsConst);
int H2TCResolveConstCall(
    void*    ctx,
    uint32_t nameStart,
    uint32_t nameEnd,
    const H2CTFEValue* _Nullable args,
    uint32_t     argCount,
    H2CTFEValue* outValue,
    int*         outIsConst,
    H2Diag* _Nullable diag);
int H2TCResolveConstCallMir(
    void* _Nullable ctx,
    const H2MirProgram* _Nullable program,
    const H2MirFunction* _Nullable function,
    const H2MirInst* _Nullable inst,
    uint32_t nameStart,
    uint32_t nameEnd,
    const H2CTFEValue* _Nonnull args,
    uint32_t argCount,
    H2CTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);

/* Returns 1 if handled as span compound, 0 if not span-like, -1 on hard error. */

/* Returns 0 if handled, 1 if not a compiler diagnostic call, -1 on error */

/* Returns 0 if handled, 1 if not a reflection call, -1 on error */

int     H2TCTypeContainsVarSizeByValue(H2TypeCheckCtx* c, int32_t typeId);
int     H2TCResolveTypeNode(H2TypeCheckCtx* c, int32_t nodeId, int32_t* outType);
int32_t H2TCResolveAliasBaseType(H2TypeCheckCtx* c, int32_t typeId);
int     H2TCTypeExpr(H2TypeCheckCtx* c, int32_t nodeId, int32_t* outType);
int     H2TCIsIntegerType(H2TypeCheckCtx* c, int32_t typeId);

#define H2TC_MAX_CALL_ARGS       128u
#define H2TC_MAX_CALL_CANDIDATES 256u

int H2TCTypeExpr(H2TypeCheckCtx* c, int32_t nodeId, int32_t* outType);
int H2TCTypeExprExpected(H2TypeCheckCtx* c, int32_t nodeId, int32_t expectedType, int32_t* outType);
int H2TCTypeExprExpected(H2TypeCheckCtx* c, int32_t nodeId, int32_t expectedType, int32_t* outType);
int H2TCExprIsCompoundTemporary(H2TypeCheckCtx* c, int32_t exprNode);
int H2TCExprNeedsExpectedType(H2TypeCheckCtx* c, int32_t exprNode);
int H2TCExprIsAssignable(H2TypeCheckCtx* c, int32_t exprNode);

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
} H2TCCallArgInfo;

typedef struct {
    H2DiagCode code;
    uint32_t   start;
    uint32_t   end;
    uint32_t   argStart;
    uint32_t   argEnd;
} H2TCCallMapError;

typedef struct {
    int      isVariadic;
    uint32_t fixedCount;
    uint32_t fixedInputCount;
    uint32_t spreadArgIndex;
    int32_t  variadicParamType;
    int32_t  variadicElemType;
    int32_t  fixedMappedArgExprNodes[H2TC_MAX_CALL_ARGS];
    int32_t  argParamIndices[H2TC_MAX_CALL_ARGS];
    int32_t  argExpectedTypes[H2TC_MAX_CALL_ARGS];
} H2TCCallBinding;

/* Returns 0 success, 1 no hook name, 2 no viable hook, 3 ambiguous */

/* Returns 0: success, 1: no name, 2: no match, 3: ambiguous, 4: writable-ref temporary,
 * 5: compound infer ambiguous, 6: named argument error, -1: error */

/* Returns 0: success, 1: no name, 2: no match, 3: ambiguous, 4: writable-ref temporary,
 * 5: compound infer ambiguous, 6: named argument error, -1: error */

/* Returns 0: success, 1: no name, 2: no match, 3: ambiguous, 4: writable-ref temporary,
 * 5: compound infer ambiguous, 6: named argument error, -1: error */

int H2TCTypeExpr(H2TypeCheckCtx* c, int32_t nodeId, int32_t* outType);
int H2TCTypeExprExpected(H2TypeCheckCtx* c, int32_t nodeId, int32_t expectedType, int32_t* outType);

/* Returns: 1 recognized variant pattern, 0 not a variant pattern, -1 error */

/* Returns: 1 resolved enum.variant type-name, 0 not an enum.variant type-name, -1 error */

/* Returns 0 for a segment, 1 for end-of-path, -1 for malformed path syntax. */

int H2TCTypeExpr(H2TypeCheckCtx* c, int32_t nodeId, int32_t* outType);

int H2TCTypeStmt(
    H2TypeCheckCtx* c, int32_t nodeId, int32_t returnType, int loopDepth, int switchDepth);

/* Describes a narrowable local found in a null-check condition. */
typedef struct {
    int32_t localIdx;  /* index in c->locals[] */
    int32_t innerType; /* T from ?T */
} H2TCNullNarrow;

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
} H2TCNarrowSave;

/* Shared declarations for multi-file typechecker internals. */

void H2TCSetDiag(H2Diag* diag, H2DiagCode code, uint32_t start, uint32_t end);
void H2TCSetDiagWithArg(
    H2Diag*    diag,
    H2DiagCode code,
    uint32_t   start,
    uint32_t   end,
    uint32_t   argStart,
    uint32_t   argEnd);
int H2TCFailSpan(H2TypeCheckCtx* c, H2DiagCode code, uint32_t start, uint32_t end);
int H2TCFailDuplicateDefinition(
    H2TypeCheckCtx* c,
    uint32_t        nameStart,
    uint32_t        nameEnd,
    uint32_t        otherStart,
    uint32_t        otherEnd);
int H2TCFailNode(H2TypeCheckCtx* c, int32_t nodeId, H2DiagCode code);
const char* _Nullable H2TCAllocCStringBytes(H2TypeCheckCtx* c, const uint8_t* bytes, uint32_t len);
int  H2TCStrEqNullable(const char* _Nullable a, const char* _Nullable b);
int  H2TCEmitWarningDiag(H2TypeCheckCtx* c, const H2Diag* diag);
int  H2TCRecordConstDiagUse(H2TypeCheckCtx* c, int32_t nodeId);
void H2TCMarkConstDiagUseExecuted(H2TypeCheckCtx* c, int32_t nodeId);
void H2TCMarkConstDiagFnInvoked(H2TypeCheckCtx* c, int32_t fnIndex);
int  H2TCValidateConstDiagUses(H2TypeCheckCtx* c);
void H2TCMarkFunctionUsed(H2TypeCheckCtx* c, int32_t fnIndex);
void H2TCMarkLocalRead(H2TypeCheckCtx* c, int32_t localIdx);
void H2TCMarkLocalWrite(H2TypeCheckCtx* c, int32_t localIdx);
void H2TCUnmarkLocalRead(H2TypeCheckCtx* c, int32_t localIdx);
void H2TCMarkLocalInitialized(H2TypeCheckCtx* c, int32_t localIdx);
int  H2TCCheckLocalInitialized(H2TypeCheckCtx* c, int32_t localIdx, uint32_t start, uint32_t end);
int  H2TCTypeIsTrackedPtrRef(H2TypeCheckCtx* c, int32_t typeId);
int  H2TCFailTopLevelPtrRefMissingInitializer(
    H2TypeCheckCtx* c, uint32_t start, uint32_t end, uint32_t nameStart, uint32_t nameEnd);
void H2TCSetLocalUsageKind(H2TypeCheckCtx* c, int32_t localIdx, uint8_t kind);
void H2TCSetLocalUsageSuppress(H2TypeCheckCtx* c, int32_t localIdx, int suppress);
int  H2TCEmitUnusedSymbolWarnings(H2TypeCheckCtx* c);
int  H2TCRecordCallTarget(H2TypeCheckCtx* c, int32_t callNode, int32_t targetFnIndex);
int  H2TCFindCallTarget(
    const H2TypeCheckCtx* c, int32_t ownerFnIndex, int32_t callNode, int32_t* outTargetFnIndex);
void H2TCOffsetToLineCol(
    const char* src, uint32_t srcLen, uint32_t offset, uint32_t* outLine, uint32_t* outColumn);
int H2TCLineColToOffset(
    const char* src, uint32_t srcLen, uint32_t line, uint32_t column, uint32_t* outOffset);
void        H2TCTextBufInit(H2TCTextBuf* b, char* ptr, uint32_t cap);
void        H2TCTextBufAppendChar(H2TCTextBuf* b, char ch);
void        H2TCTextBufAppendCStr(H2TCTextBuf* b, const char* s);
void        H2TCTextBufAppendSlice(H2TCTextBuf* b, H2StrView src, uint32_t start, uint32_t end);
void        H2TCTextBufAppendU32(H2TCTextBuf* b, uint32_t v);
void        H2TCTextBufAppendHexU64(H2TCTextBuf* b, uint64_t v);
const char* H2TCBuiltinName(H2TypeCheckCtx* c, int32_t typeId, H2BuiltinKind kind);
void        H2TCFormatTypeRec(H2TypeCheckCtx* c, int32_t typeId, H2TCTextBuf* b, uint32_t depth);
int         H2TCExprIsStringConstant(H2TypeCheckCtx* c, int32_t nodeId);
void        H2TCFormatExprSubject(H2TypeCheckCtx* c, int32_t nodeId, H2TCTextBuf* b);
char* _Nullable H2TCAllocDiagText(H2TypeCheckCtx* c, const char* text);
int H2TCFailTypeMismatchDetail(
    H2TypeCheckCtx* c, int32_t failNode, int32_t exprNode, int32_t srcType, int32_t dstType);
int H2TCFailDiagText(H2TypeCheckCtx* c, int32_t nodeId, H2DiagCode code, const char* detail);
int H2TCFailTypeMismatchText(H2TypeCheckCtx* c, int32_t nodeId, const char* detail);
int H2TCFailVarSizeByValue(H2TypeCheckCtx* c, int32_t nodeId, int32_t typeId, const char* position);
int H2TCFailAssignToConst(H2TypeCheckCtx* c, int32_t lhsNode);
int H2TCFailAssignTargetNotAssignable(H2TypeCheckCtx* c, int32_t lhsNode);
int H2TCFailSwitchMissingCases(
    H2TypeCheckCtx* c,
    int32_t         failNode,
    int32_t         subjectType,
    int32_t         subjectEnumType,
    uint32_t        enumVariantCount,
    const uint32_t* _Nullable enumVariantStarts,
    const uint32_t* _Nullable enumVariantEnds,
    const uint8_t* _Nullable enumCovered,
    int boolCoveredTrue,
    int boolCoveredFalse);
int32_t H2AstFirstChild(const H2Ast* ast, int32_t nodeId);
int32_t H2AstNextSibling(const H2Ast* ast, int32_t nodeId);
int H2NameEqSlice(H2StrView src, uint32_t aStart, uint32_t aEnd, uint32_t bStart, uint32_t bEnd);
int H2NameEqLiteral(H2StrView src, uint32_t start, uint32_t end, const char* lit);
int H2NameHasPrefix(H2StrView src, uint32_t start, uint32_t end, const char* prefix);
int H2NameHasSuffix(H2StrView src, uint32_t start, uint32_t end, const char* suffix);
int32_t  H2TCFindMemAllocatorType(H2TypeCheckCtx* c);
int32_t  H2TCGetStrRefType(H2TypeCheckCtx* c, uint32_t start, uint32_t end);
int32_t  H2TCGetStrPtrType(H2TypeCheckCtx* c, uint32_t start, uint32_t end);
int32_t  H2TCAddType(H2TypeCheckCtx* c, const H2TCType* t, uint32_t errStart, uint32_t errEnd);
int32_t  H2TCAddBuiltinType(H2TypeCheckCtx* c, const char* name, H2BuiltinKind builtinKind);
int      H2TCEnsureInitialized(H2TypeCheckCtx* c);
int32_t  H2TCFindBuiltinType(H2TypeCheckCtx* c, uint32_t start, uint32_t end);
int32_t  H2TCFindBuiltinByKind(H2TypeCheckCtx* c, H2BuiltinKind builtinKind);
uint8_t  H2TCTypeTagKindOf(H2TypeCheckCtx* c, int32_t typeId);
uint64_t H2TCEncodeTypeTag(H2TypeCheckCtx* c, int32_t typeId);
int      H2TCDecodeTypeTag(H2TypeCheckCtx* c, uint64_t typeTag, int32_t* outTypeId);
int32_t  H2TCResolveTypeValueName(H2TypeCheckCtx* c, uint32_t start, uint32_t end);
int32_t  H2TCFindNamedTypeIndex(H2TypeCheckCtx* c, uint32_t start, uint32_t end);
int32_t  H2TCFindNamedTypeIndexOwned(
    H2TypeCheckCtx* c, int32_t ownerTypeId, uint32_t start, uint32_t end);
int32_t H2TCResolveTypeNamePath(
    H2TypeCheckCtx* c, uint32_t start, uint32_t end, int32_t ownerTypeId);
int32_t H2TCFindNamedTypeByLiteral(H2TypeCheckCtx* c, const char* name);
int32_t H2TCFindBuiltinQualifiedNamedType(H2TypeCheckCtx* c, uint32_t start, uint32_t end);
int32_t H2TCFindBuiltinNamedTypeBySuffix(H2TypeCheckCtx* c, const char* suffix);
int32_t H2TCFindNamedTypeBySuffix(H2TypeCheckCtx* c, const char* suffix);
int32_t H2TCFindReflectKindType(H2TypeCheckCtx* c);
int     H2TCNameEqLiteralOrPkgBuiltin(
    H2TypeCheckCtx* c, uint32_t start, uint32_t end, const char* name, const char* pkgPrefix);
H2TCCompilerDiagOp H2TCCompilerDiagOpFromName(H2TypeCheckCtx* c, uint32_t start, uint32_t end);
int                H2TCIsSourceLocationOfName(H2TypeCheckCtx* c, uint32_t start, uint32_t end);
int32_t            H2TCFindSourceLocationType(H2TypeCheckCtx* c);
int32_t            H2TCFindFmtValueType(H2TypeCheckCtx* c);
int                H2TCTypeIsSourceLocation(H2TypeCheckCtx* c, int32_t typeId);
int                H2TCTypeIsFmtValue(H2TypeCheckCtx* c, int32_t typeId);
int32_t            H2TCFindFunctionIndex(H2TypeCheckCtx* c, uint32_t start, uint32_t end);
int32_t H2TCFindBuiltinQualifiedFunctionIndex(H2TypeCheckCtx* c, uint32_t start, uint32_t end);
int32_t H2TCFindPlainFunctionValueIndex(H2TypeCheckCtx* c, uint32_t start, uint32_t end);
int32_t H2TCFindPkgQualifiedFunctionValueIndex(
    H2TypeCheckCtx* c, uint32_t pkgStart, uint32_t pkgEnd, uint32_t nameStart, uint32_t nameEnd);
int H2TCFunctionNameEq(const H2TypeCheckCtx* c, uint32_t funcIndex, uint32_t start, uint32_t end);
int H2TCNameEqPkgPrefixedMethod(
    H2TypeCheckCtx* c,
    uint32_t        candidateStart,
    uint32_t        candidateEnd,
    uint32_t        pkgStart,
    uint32_t        pkgEnd,
    uint32_t        methodStart,
    uint32_t        methodEnd);
int H2TCExtractPkgPrefixFromTypeName(
    H2TypeCheckCtx* c,
    uint32_t        typeNameStart,
    uint32_t        typeNameEnd,
    uint32_t*       outPkgStart,
    uint32_t*       outPkgEnd);
int H2TCImportDefaultAliasEq(
    H2StrView src, uint32_t pathStart, uint32_t pathEnd, uint32_t aliasStart, uint32_t aliasEnd);
int H2TCHasImportAlias(H2TypeCheckCtx* c, uint32_t aliasStart, uint32_t aliasEnd);
int H2TCResolveReceiverPkgPrefix(
    H2TypeCheckCtx* c, int32_t typeId, uint32_t* outPkgStart, uint32_t* outPkgEnd);
int H2TCResolveEnumMemberType(
    H2TypeCheckCtx* c,
    int32_t         recvNode,
    uint32_t        memberStart,
    uint32_t        memberEnd,
    int32_t*        outType);
int32_t H2TCInternPtrType(H2TypeCheckCtx* c, int32_t baseType, uint32_t errStart, uint32_t errEnd);
int     H2TCTypeIsMutable(const H2TCType* t);
int     H2TCIsMutableRefType(H2TypeCheckCtx* c, int32_t typeId);
int32_t H2TCInternRefType(
    H2TypeCheckCtx* c, int32_t baseType, int isMutable, uint32_t errStart, uint32_t errEnd);
int32_t H2TCInternArrayType(
    H2TypeCheckCtx* c, int32_t baseType, uint32_t arrayLen, uint32_t errStart, uint32_t errEnd);
int32_t H2TCInternSliceType(
    H2TypeCheckCtx* c, int32_t baseType, int isMutable, uint32_t errStart, uint32_t errEnd);
int32_t H2TCInternOptionalType(
    H2TypeCheckCtx* c, int32_t baseType, uint32_t errStart, uint32_t errEnd);
int32_t H2TCInternAnonAggregateType(
    H2TypeCheckCtx*         c,
    int                     isUnion,
    const H2TCAnonFieldSig* fields,
    uint32_t                fieldCount,
    int32_t                 declNode,
    uint32_t                errStart,
    uint32_t                errEnd);
int H2TCFunctionTypeMatchesSignature(
    H2TypeCheckCtx* c,
    const H2TCType* t,
    int32_t         returnType,
    const int32_t*  paramTypes,
    const uint8_t*  paramFlags,
    uint32_t        paramCount,
    int             isVariadic);
int32_t H2TCInternFunctionType(
    H2TypeCheckCtx* c,
    int32_t         returnType,
    const int32_t*  paramTypes,
    const uint8_t*  paramFlags,
    uint32_t        paramCount,
    int             isVariadic,
    int32_t         funcIndex,
    uint32_t        errStart,
    uint32_t        errEnd);
int H2TCTupleTypeMatchesSignature(
    H2TypeCheckCtx* c, const H2TCType* t, const int32_t* elemTypes, uint32_t elemCount);
int32_t H2TCInternTupleType(
    H2TypeCheckCtx* c,
    const int32_t*  elemTypes,
    uint32_t        elemCount,
    uint32_t        errStart,
    uint32_t        errEnd);
int32_t H2TCInternPackType(
    H2TypeCheckCtx* c,
    const int32_t*  elemTypes,
    uint32_t        elemCount,
    uint32_t        errStart,
    uint32_t        errEnd);
int      H2TCParseArrayLen(H2TypeCheckCtx* c, const H2AstNode* node, uint32_t* outLen);
int      H2TCResolveIndexBaseInfo(H2TypeCheckCtx* c, int32_t baseType, H2TCIndexBaseInfo* out);
int32_t  H2TCListItemAt(const H2Ast* ast, int32_t listNode, uint32_t index);
uint32_t H2TCListCount(const H2Ast* ast, int32_t listNode);
int      H2TCVarLikeGetParts(H2TypeCheckCtx* c, int32_t nodeId, H2TCVarLikeParts* out);
int      H2TCHasForeignImportDirective(const H2Ast* ast, H2StrView src, int32_t nodeId);
int32_t  H2TCVarLikeNameIndexBySlice(
    H2TypeCheckCtx* c, int32_t nodeId, uint32_t start, uint32_t end);
int32_t H2TCVarLikeInitExprNodeAt(H2TypeCheckCtx* c, int32_t nodeId, int32_t nameIndex);
int32_t H2TCVarLikeInitExprNode(H2TypeCheckCtx* c, int32_t nodeId);
void    H2TCConstSetReason(
    H2TCConstEvalCtx* evalCtx, uint32_t start, uint32_t end, const char* reason);
void H2TCConstSetReasonNode(H2TCConstEvalCtx* evalCtx, int32_t nodeId, const char* reason);
void H2TCAttachConstEvalReason(H2TypeCheckCtx* c);
int  H2TCResolveConstIdent(
    void*        ctx,
    uint32_t     nameStart,
    uint32_t     nameEnd,
    H2CTFEValue* outValue,
    int*         outIsConst,
    H2Diag* _Nullable diag);
int H2TCConstLookupExecBindingType(
    H2TCConstEvalCtx* evalCtx, uint32_t nameStart, uint32_t nameEnd, int32_t* outType);
int H2TCConstLookupMirLocalType(
    H2TCConstEvalCtx* evalCtx, uint32_t nameStart, uint32_t nameEnd, int32_t* outType);
int      H2TCConstBuiltinSizeBytes(H2BuiltinKind b, uint64_t* outBytes);
int      H2TCConstBuiltinAlignBytes(H2BuiltinKind b, uint64_t* outAlign);
uint64_t H2TCConstAlignUpU64(uint64_t v, uint64_t align);
int      H2TCConstTypeLayout(
    H2TypeCheckCtx* c, int32_t typeId, uint64_t* outSize, uint64_t* outAlign, uint32_t depth);
int H2TCConstEvalSizeOf(
    H2TCConstEvalCtx* evalCtx, int32_t exprNode, H2CTFEValue* outValue, int* outIsConst);
int H2TCConstEvalCast(
    H2TCConstEvalCtx* evalCtx, int32_t exprNode, H2CTFEValue* outValue, int* outIsConst);
int H2TCConstEvalTypeOf(
    H2TCConstEvalCtx* evalCtx, int32_t exprNode, H2CTFEValue* outValue, int* outIsConst);
int H2TCResolveReflectedTypeValueExpr(H2TypeCheckCtx* c, int32_t exprNode, int32_t* outTypeId);
int H2TCConstEvalTypeNameValue(
    H2TypeCheckCtx* c, int32_t typeId, H2CTFEValue* outValue, int* outIsConst);
void H2TCConstEvalSetNullValue(H2CTFEValue* outValue);
void H2TCConstEvalSetSourceLocationFromOffsets(
    H2TypeCheckCtx* c, uint32_t startOffset, uint32_t endOffset, H2CTFEValue* outValue);
int H2TCConstEvalSourceLocationOfCall(
    H2TCConstEvalCtx* evalCtx, int32_t exprNode, H2CTFEValue* outValue, int* outIsConst);
int H2TCConstEvalU32Arg(
    H2TCConstEvalCtx* evalCtx, int32_t exprNode, uint32_t* outValue, int* outIsConst);
int H2TCConstEvalSourceLocationCompound(
    H2TCConstEvalCtx* evalCtx,
    int32_t           exprNode,
    int               forceSourceLocation,
    H2CTFEValue*      outValue,
    int*              outIsConst);
int H2TCConstEvalSourceLocationExpr(
    H2TCConstEvalCtx* evalCtx, int32_t exprNode, H2CTFESpan* outSpan, int* outIsConst);
int H2TCConstEvalCompilerDiagCall(
    H2TCConstEvalCtx* evalCtx, int32_t exprNode, H2CTFEValue* outValue, int* outIsConst);
int H2TCConstEvalTypeReflectionCall(
    H2TCConstEvalCtx* evalCtx, int32_t exprNode, H2CTFEValue* outValue, int* outIsConst);
int H2TCEvalConstExprNode(
    H2TCConstEvalCtx* evalCtx, int32_t exprNode, H2CTFEValue* outValue, int* outIsConst);
int H2TCEvalConstExecExprCb(void* ctx, int32_t exprNode, H2CTFEValue* outValue, int* outIsConst);
int H2TCEvalConstExecResolveTypeCb(void* ctx, int32_t typeNode, int32_t* outTypeId);
int H2TCEvalConstExecInferValueTypeCb(void* ctx, const H2CTFEValue* value, int32_t* outTypeId);
int H2TCMirConstZeroInitLocal(
    void* _Nullable ctx,
    const H2MirTypeRef* _Nonnull typeRef,
    H2CTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);
int H2TCMirConstCoerceValueForType(
    void* _Nullable ctx,
    const H2MirTypeRef* _Nonnull typeRef,
    H2CTFEValue* _Nonnull inOutValue,
    H2Diag* _Nullable diag);
int H2TCMirConstIndexValue(
    void* _Nullable ctx,
    const H2CTFEValue* _Nonnull base,
    const H2CTFEValue* _Nonnull index,
    H2CTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);
int H2TCMirConstSequenceLen(
    void* _Nullable ctx,
    const H2CTFEValue* _Nonnull base,
    H2CTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);
int H2TCMirConstIterInit(
    void* _Nullable ctx,
    uint32_t sourceNode,
    const H2CTFEValue* _Nonnull source,
    uint16_t flags,
    H2CTFEValue* _Nonnull outIter,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);
int H2TCMirConstIterNext(
    void* _Nullable ctx,
    const H2CTFEValue* _Nonnull iterValue,
    uint16_t flags,
    int* _Nonnull outHasItem,
    H2CTFEValue* _Nonnull outKey,
    int* _Nonnull outKeyIsConst,
    H2CTFEValue* _Nonnull outValue,
    int* _Nonnull outValueIsConst,
    H2Diag* _Nullable diag);
int H2TCMirConstAggGetField(
    void* _Nullable ctx,
    const H2CTFEValue* _Nonnull base,
    uint32_t nameStart,
    uint32_t nameEnd,
    H2CTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);
int H2TCMirConstAggAddrField(
    void* _Nullable ctx,
    const H2CTFEValue* _Nonnull base,
    uint32_t nameStart,
    uint32_t nameEnd,
    H2CTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);
int H2TCMirConstMakeTuple(
    void* _Nullable ctx,
    const H2CTFEValue* _Nonnull elems,
    uint32_t elemCount,
    uint32_t typeNodeHint,
    H2CTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);
int H2TCEvalConstForInIndexCb(
    void* _Nullable ctx,
    H2CTFEExecCtx* _Nonnull execCtx,
    const H2CTFEValue* _Nonnull sourceValue,
    uint32_t index,
    int      byRef,
    H2CTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst);
int32_t H2TCFindConstCallableFunction(
    H2TypeCheckCtx* c, uint32_t nameStart, uint32_t nameEnd, uint32_t argCount);
int H2TCResolveForInIterator(
    H2TypeCheckCtx* c,
    int32_t         sourceNode,
    int32_t         sourceType,
    int32_t*        outFnIndex,
    int32_t*        outIterType);
int H2TCResolveForInNextValue(
    H2TypeCheckCtx*    c,
    int32_t            iterPtrType,
    H2TCForInValueMode valueMode,
    int32_t*           outValueType,
    int32_t*           outFn);
int H2TCResolveForInNextKey(
    H2TypeCheckCtx* c, int32_t iterPtrType, int32_t* outKeyType, int32_t* outFn);
int H2TCResolveForInNextKeyAndValue(
    H2TypeCheckCtx*    c,
    int32_t            iterPtrType,
    H2TCForInValueMode valueMode,
    int32_t*           outKeyType,
    int32_t*           outValueType,
    int32_t*           outFn);
int H2TCResolveConstCall(
    void*    ctx,
    uint32_t nameStart,
    uint32_t nameEnd,
    const H2CTFEValue* _Nullable args,
    uint32_t     argCount,
    H2CTFEValue* outValue,
    int*         outIsConst,
    H2Diag* _Nullable diag);
int H2TCResolveConstCallMirPre(
    void* _Nullable ctx,
    const H2MirProgram* _Nullable program,
    const H2MirFunction* _Nullable function,
    const H2MirInst* _Nullable inst,
    uint32_t     nameStart,
    uint32_t     nameEnd,
    H2CTFEValue* outValue,
    int*         outIsConst,
    H2Diag* _Nullable diag);
int H2TCMirConstBindFrame(
    void* _Nullable ctx,
    const H2MirProgram* _Nullable program,
    const H2MirFunction* _Nullable function,
    const H2CTFEValue* _Nullable locals,
    uint32_t localCount,
    H2Diag* _Nullable diag);
void H2TCMirConstUnbindFrame(void* _Nullable ctx);
void H2TCMirConstAdoptLowerDiagReason(H2TCConstEvalCtx* evalCtx, const H2Diag* _Nullable diag);
int  H2TCMirConstLowerConstExpr(
    void* _Nullable ctx, int32_t exprNode, H2MirConst* _Nonnull outValue, H2Diag* _Nullable diag);
int H2TCEvalTopLevelConstNodeAt(
    H2TypeCheckCtx*   c,
    H2TCConstEvalCtx* evalCtx,
    int32_t           nodeId,
    int32_t           nameIndex,
    H2CTFEValue*      outValue,
    int*              outIsConst);
int H2TCEvalTopLevelConstNode(
    H2TypeCheckCtx*   c,
    H2TCConstEvalCtx* evalCtx,
    int32_t           nodeId,
    H2CTFEValue*      outValue,
    int*              outIsConst);
int H2TCConstBoolExpr(H2TypeCheckCtx* c, int32_t nodeId, int* out, int* isConst);
int H2TCConstIntExpr(H2TypeCheckCtx* c, int32_t nodeId, int64_t* out, int* isConst);
int H2TCConstFloatExpr(H2TypeCheckCtx* c, int32_t nodeId, double* out, int* isConst);
int H2TCConstStringExpr(
    H2TypeCheckCtx* c, int32_t nodeId, const uint8_t** outBytes, uint32_t* outLen, int* outIsConst);
void H2TCMarkRuntimeBoundsCheck(H2TypeCheckCtx* c, int32_t nodeId);
int  H2TCResolveAnonAggregateTypeNode(
    H2TypeCheckCtx* c, int32_t nodeId, int isUnion, int32_t* outType);
int     H2TCResolveAliasTypeId(H2TypeCheckCtx* c, int32_t typeId);
int32_t H2TCResolveAliasBaseType(H2TypeCheckCtx* c, int32_t typeId);
int     H2TCFnNodeHasTypeParamName(
    H2TypeCheckCtx* c, int32_t fnNode, uint32_t nameStart, uint32_t nameEnd);
int H2TCResolveActiveTypeParamType(
    H2TypeCheckCtx* c, uint32_t nameStart, uint32_t nameEnd, int32_t* outType);
int H2TCMirConstInitLowerCtx(H2TCConstEvalCtx* evalCtx, H2TCMirConstLowerCtx* _Nonnull outCtx);
int H2TCMirConstLowerFunction(
    H2TCMirConstLowerCtx* c, int32_t fnIndex, uint32_t* _Nullable outMirFnIndex);
int H2TCMirConstRewriteDirectCalls(H2TCMirConstLowerCtx* c, uint32_t mirFnIndex, int32_t rootNode);
int H2TCResolveTypeNode(H2TypeCheckCtx* c, int32_t nodeId, int32_t* outType);
int H2TCAddNamedType(H2TypeCheckCtx* c, int32_t nodeId, int32_t ownerTypeId, int32_t* outTypeId);
int H2TCCollectTypeDeclsFromNode(H2TypeCheckCtx* c, int32_t nodeId);
int H2TCIsIntegerType(H2TypeCheckCtx* c, int32_t typeId);
int H2TCIsConstNumericType(H2TypeCheckCtx* c, int32_t typeId);
int H2TCTypeIsRuneLike(H2TypeCheckCtx* c, int32_t typeId);
int H2TCConstIntFitsType(H2TypeCheckCtx* c, int64_t value, int32_t typeId);
int H2TCConstIntFitsFloatType(H2TypeCheckCtx* c, int64_t value, int32_t typeId);
int H2TCConstFloatFitsType(H2TypeCheckCtx* c, double value, int32_t typeId);
int H2TCFailConstIntRange(H2TypeCheckCtx* c, int32_t nodeId, int64_t value, int32_t expectedType);
int H2TCFailConstFloatRange(H2TypeCheckCtx* c, int32_t nodeId, int32_t expectedType);
int H2TCIsFloatType(H2TypeCheckCtx* c, int32_t typeId);
int H2TCIsNumericType(H2TypeCheckCtx* c, int32_t typeId);
int H2TCIsBoolType(H2TypeCheckCtx* c, int32_t typeId);
int H2TCIsRawptrType(H2TypeCheckCtx* c, int32_t typeId);
int H2TCIsNamedDeclKind(H2TypeCheckCtx* c, int32_t typeId, H2AstKind kind);
int H2TCIsStringLikeType(H2TypeCheckCtx* c, int32_t typeId);
int H2TCTypeSupportsFmtReflectRec(H2TypeCheckCtx* c, int32_t typeId, uint32_t depth);
int H2TCIsComparableTypeRec(H2TypeCheckCtx* c, int32_t typeId, uint32_t depth);
int H2TCIsOrderedTypeRec(H2TypeCheckCtx* c, int32_t typeId, uint32_t depth);
int H2TCIsComparableType(H2TypeCheckCtx* c, int32_t typeId);
int H2TCIsOrderedType(H2TypeCheckCtx* c, int32_t typeId);
int H2TCTypeSupportsLen(H2TypeCheckCtx* c, int32_t typeId);
int H2TCIsUntyped(H2TypeCheckCtx* c, int32_t typeId);
int H2TCIsTypeNodeKind(H2AstKind kind);
int H2TCConcretizeInferredType(H2TypeCheckCtx* c, int32_t typeId, int32_t* outType);
int H2TCTypeIsVarSize(H2TypeCheckCtx* c, int32_t typeId);
int H2TCTypeContainsVarSizeByValue(H2TypeCheckCtx* c, int32_t typeId);
int H2TCIsComparisonHookName(
    H2TypeCheckCtx* c, uint32_t nameStart, uint32_t nameEnd, int* outIsEqualHook);
int     H2TCTypeIsU8Slice(H2TypeCheckCtx* c, int32_t typeId, int requireMutable);
int     H2TCTypeIsFreeablePointer(H2TypeCheckCtx* c, int32_t typeId);
int32_t H2TCFindEmbeddedFieldIndex(H2TypeCheckCtx* c, int32_t namedTypeId);
int     H2TCEmbedDistanceToType(
    H2TypeCheckCtx* c, int32_t srcType, int32_t dstType, uint32_t* outDistance);
int H2TCIsTypeDerivedFromEmbedded(H2TypeCheckCtx* c, int32_t srcType, int32_t dstType);
int H2TCCanAssign(H2TypeCheckCtx* c, int32_t dstType, int32_t srcType);
int H2TCValidateMemAllocatorArg(H2TypeCheckCtx* c, int32_t nodeId, int32_t allocBaseType);
int H2TCCoerceForBinary(H2TypeCheckCtx* c, int32_t leftType, int32_t rightType, int32_t* outType);
int H2TCConversionCost(H2TypeCheckCtx* c, int32_t dstType, int32_t srcType, uint8_t* outCost);
int H2TCCostVectorCompare(const uint8_t* a, const uint8_t* b, uint32_t len);
int32_t H2TCUnwrapCallArgExprNode(H2TypeCheckCtx* c, int32_t argNode);
int     H2TCCollectCallArgInfo(
    H2TypeCheckCtx*  c,
    int32_t          callNode,
    int32_t          calleeNode,
    int              includeReceiver,
    int32_t          receiverNode,
    H2TCCallArgInfo* outArgs,
    int32_t* _Nullable outArgTypes,
    uint32_t* outArgCount);
int     H2TCIsMainFunction(H2TypeCheckCtx* c, const H2TCFunction* fn);
int32_t H2TCResolveImplicitMainContextType(H2TypeCheckCtx* c);
int     H2TCCurrentContextFieldType(
    H2TypeCheckCtx* c, uint32_t fieldStart, uint32_t fieldEnd, int32_t* outType);
int H2TCCurrentContextFieldTypeByLiteral(
    H2TypeCheckCtx* c, const char* fieldName, int32_t* outType);
int32_t H2TCContextFindOverlayNode(H2TypeCheckCtx* c);
int32_t H2TCContextFindDirectNode(H2TypeCheckCtx* c);
int32_t H2TCContextFindOverlayBindMatch(
    H2TypeCheckCtx* c, uint32_t fieldStart, uint32_t fieldEnd, const char* _Nullable fieldName);
int32_t H2TCContextFindOverlayBindByLiteral(H2TypeCheckCtx* c, const char* fieldName);
int     H2TCGetEffectiveContextFieldType(
    H2TypeCheckCtx* c, uint32_t fieldStart, uint32_t fieldEnd, int32_t* outType);
int H2TCGetEffectiveContextFieldTypeByLiteral(
    H2TypeCheckCtx* c, const char* fieldName, int32_t* outType);
int H2TCValidateCurrentCallOverlay(H2TypeCheckCtx* c);
int H2TCValidateCallContextRequirements(H2TypeCheckCtx* c, int32_t requiredContextType);
int H2TCGetFunctionTypeSignature(
    H2TypeCheckCtx* c,
    int32_t         typeId,
    int32_t*        outReturnType,
    uint32_t*       outParamStart,
    uint32_t*       outParamCount,
    int* _Nullable outIsVariadic);
void H2TCCallMapErrorClear(H2TCCallMapError* err);
int  H2TCMapCallArgsToParams(
    H2TypeCheckCtx*        c,
    const H2TCCallArgInfo* callArgs,
    uint32_t               argCount,
    const uint32_t*        paramNameStarts,
    const uint32_t*        paramNameEnds,
    uint32_t               paramCount,
    uint32_t               firstPositionalArgIndex,
    int32_t*               outMappedArgExprNodes,
    H2TCCallMapError* _Nullable outError);
int H2TCPrepareCallBinding(
    H2TypeCheckCtx*        c,
    const H2TCCallArgInfo* callArgs,
    uint32_t               argCount,
    const uint32_t*        paramNameStarts,
    const uint32_t*        paramNameEnds,
    const int32_t*         paramTypes,
    uint32_t               paramCount,
    int                    isVariadic,
    int                    allowNamedMapping,
    uint32_t               firstPositionalArgIndex,
    H2TCCallBinding*       outBinding,
    H2TCCallMapError*      outError);
int H2TCCheckConstParamArgs(
    H2TypeCheckCtx*        c,
    const H2TCCallArgInfo* callArgs,
    uint32_t               argCount,
    const H2TCCallBinding* binding,
    const uint32_t*        paramNameStarts,
    const uint32_t*        paramNameEnds,
    const uint8_t*         paramFlags,
    uint32_t               paramCount,
    H2TCCallMapError*      outError);
int H2TCFunctionHasAnytypeParam(H2TypeCheckCtx* c, int32_t fnIndex);
int H2TCInstantiateAnytypeFunctionForCall(
    H2TypeCheckCtx*        c,
    int32_t                fnIndex,
    const H2TCCallArgInfo* callArgs,
    uint32_t               argCount,
    uint32_t               firstPositionalArgIndex,
    int32_t                autoRefFirstArgType,
    int32_t*               outFuncIndex,
    H2TCCallMapError*      outError);
int H2TCResolveComparisonHookArgCost(
    H2TypeCheckCtx* c, int32_t paramType, int32_t argType, uint8_t* outCost);
int H2TCResolveComparisonHook(
    H2TypeCheckCtx* c,
    const char*     hookName,
    int32_t         lhsType,
    int32_t         rhsType,
    int32_t*        outFuncIndex);
void H2TCGatherCallCandidates(
    H2TypeCheckCtx* c,
    uint32_t        nameStart,
    uint32_t        nameEnd,
    int32_t*        outCandidates,
    uint32_t*       outCandidateCount,
    int*            outNameFound);
void H2TCGatherCallCandidatesByPkgMethod(
    H2TypeCheckCtx* c,
    uint32_t        pkgStart,
    uint32_t        pkgEnd,
    uint32_t        methodStart,
    uint32_t        methodEnd,
    int32_t*        outCandidates,
    uint32_t*       outCandidateCount,
    int*            outNameFound);
int H2TCResolveCallFromCandidates(
    H2TypeCheckCtx*        c,
    const int32_t*         candidates,
    uint32_t               candidateCount,
    int                    nameFound,
    const H2TCCallArgInfo* callArgs,
    uint32_t               argCount,
    uint32_t               firstPositionalArgIndex,
    int                    autoRefFirstArg,
    int32_t*               outFuncIndex,
    int32_t*               outMutRefTempArgNode);
int H2TCResolveCallByName(
    H2TypeCheckCtx*        c,
    uint32_t               nameStart,
    uint32_t               nameEnd,
    const H2TCCallArgInfo* callArgs,
    uint32_t               argCount,
    uint32_t               firstPositionalArgIndex,
    int                    autoRefFirstArg,
    int32_t*               outFuncIndex,
    int32_t*               outMutRefTempArgNode);
int H2TCResolveCallByPkgMethod(
    H2TypeCheckCtx*        c,
    uint32_t               pkgStart,
    uint32_t               pkgEnd,
    uint32_t               methodStart,
    uint32_t               methodEnd,
    const H2TCCallArgInfo* callArgs,
    uint32_t               argCount,
    uint32_t               firstPositionalArgIndex,
    int                    autoRefFirstArg,
    int32_t*               outFuncIndex,
    int32_t*               outMutRefTempArgNode);
int H2TCResolveDependentPtrReturnForCall(
    H2TypeCheckCtx* c, int32_t fnIndex, int32_t argNode, int32_t* outType);
int H2TCResolveNamedTypeFields(H2TypeCheckCtx* c, uint32_t namedIndex);
int H2TCResolveAllNamedTypeFields(H2TypeCheckCtx* c);
int H2TCResolveAllTypeAliases(H2TypeCheckCtx* c);
int H2TCCheckEmbeddedCycleFrom(H2TypeCheckCtx* c, int32_t typeId);
int H2TCCheckEmbeddedCycles(H2TypeCheckCtx* c);
int H2TCPropagateVarSizeNamedTypes(H2TypeCheckCtx* c);
int H2TCReadFunctionSig(
    H2TypeCheckCtx* c,
    int32_t         funNode,
    int32_t*        outReturnType,
    uint32_t*       outParamCount,
    int*            outIsVariadic,
    int32_t*        outContextType,
    int*            outHasBody);
int     H2TCCollectFunctionFromNode(H2TypeCheckCtx* c, int32_t nodeId);
int     H2TCFinalizeFunctionTypes(H2TypeCheckCtx* c);
int32_t H2TCFindTopLevelVarLikeNode(
    H2TypeCheckCtx* c, uint32_t start, uint32_t end, int32_t* outNameIndex);
int H2TCTypeTopLevelVarLikeNode(
    H2TypeCheckCtx* c, int32_t nodeId, int32_t nameIndex, int32_t* outType);
int32_t H2TCLocalFind(H2TypeCheckCtx* c, uint32_t nameStart, uint32_t nameEnd);
int     H2TCLocalAdd(
    H2TypeCheckCtx* c,
    uint32_t        nameStart,
    uint32_t        nameEnd,
    int32_t         typeId,
    int             isConst,
    int32_t         initExprNode);
int H2TCVariantNarrowPush(
    H2TypeCheckCtx* c,
    int32_t         localIdx,
    int32_t         enumTypeId,
    uint32_t        variantStart,
    uint32_t        variantEnd);
int H2TCVariantNarrowFind(H2TypeCheckCtx* c, int32_t localIdx, const H2TCVariantNarrow** outNarrow);
int32_t H2TCEnumDeclFirstVariantNode(H2TypeCheckCtx* c, int32_t enumDeclNode);
int32_t H2TCEnumVariantTagExprNode(H2TypeCheckCtx* c, int32_t variantNode);
int32_t H2TCFindEnumVariantNodeByName(
    H2TypeCheckCtx* c, int32_t enumTypeId, uint32_t variantStart, uint32_t variantEnd);
int H2TCEnumVariantPayloadFieldType(
    H2TypeCheckCtx* c,
    int32_t         enumTypeId,
    uint32_t        variantStart,
    uint32_t        variantEnd,
    uint32_t        fieldStart,
    uint32_t        fieldEnd,
    int32_t*        outType);
int H2TCEnumTypeHasTagZero(H2TypeCheckCtx* c, int32_t enumTypeId);
int H2TCCasePatternParts(
    H2TypeCheckCtx* c, int32_t caseLabelNode, int32_t* outExprNode, int32_t* outAliasNode);
int H2TCDecodeVariantPatternExpr(
    H2TypeCheckCtx* c,
    int32_t         exprNode,
    int32_t*        outEnumType,
    uint32_t*       outVariantStart,
    uint32_t*       outVariantEnd);
int H2TCResolveEnumVariantTypeName(
    H2TypeCheckCtx* c,
    int32_t         typeNameNode,
    int32_t*        outEnumType,
    uint32_t*       outVariantStart,
    uint32_t*       outVariantEnd);
int H2TCFieldLookup(
    H2TypeCheckCtx* c,
    int32_t         typeId,
    uint32_t        fieldStart,
    uint32_t        fieldEnd,
    int32_t*        outType,
    uint32_t* _Nullable outFieldIndex);
int H2TCIsAsciiSpace(unsigned char ch);
int H2TCIsIdentStartChar(unsigned char ch);
int H2TCIsIdentContinueChar(unsigned char ch);
int H2TCFieldPathNextSegment(
    H2TypeCheckCtx* c,
    uint32_t        pathStart,
    uint32_t        pathEnd,
    uint32_t*       ioPos,
    uint32_t*       outSegStart,
    uint32_t*       outSegEnd);
int H2TCFieldLookupPath(
    H2TypeCheckCtx* c, int32_t ownerTypeId, uint32_t pathStart, uint32_t pathEnd, int32_t* outType);
int H2TCTypeNewExpr(H2TypeCheckCtx* c, int32_t nodeId, int32_t* outType);
int H2TCExprIsCompoundTemporary(H2TypeCheckCtx* c, int32_t exprNode);
int H2TCExprNeedsExpectedType(H2TypeCheckCtx* c, int32_t exprNode);
int H2TCResolveIdentifierExprType(
    H2TypeCheckCtx* c,
    uint32_t        nameStart,
    uint32_t        nameEnd,
    uint32_t        spanStart,
    uint32_t        spanEnd,
    int32_t*        outType);
int H2TCInferAnonStructTypeFromCompound(
    H2TypeCheckCtx* c, int32_t nodeId, int32_t firstField, int32_t* outType);
int H2TCTypeCompoundLit(H2TypeCheckCtx* c, int32_t nodeId, int32_t expectedType, int32_t* outType);
int H2TCTypeExprExpected(H2TypeCheckCtx* c, int32_t nodeId, int32_t expectedType, int32_t* outType);
int H2TCTypeAssignTargetExpr(
    H2TypeCheckCtx* c, int32_t nodeId, int skipDirectIdentRead, int32_t* outType);
void H2TCMarkDirectIdentLocalWrite(H2TypeCheckCtx* c, int32_t nodeId, int markInitialized);
int  H2TCExprIsAssignable(H2TypeCheckCtx* c, int32_t exprNode);
int  H2TCExprIsConstAssignTarget(H2TypeCheckCtx* c, int32_t exprNode);
int  H2TCTypeExpr_IDENT(H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* n, int32_t* outType);
int  H2TCTypeExpr_INT(H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* n, int32_t* outType);
int  H2TCTypeExpr_FLOAT(H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* n, int32_t* outType);
int  H2TCTypeExpr_STRING(H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* n, int32_t* outType);
int  H2TCTypeExpr_RUNE(H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* n, int32_t* outType);
int  H2TCTypeExpr_BOOL(H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* n, int32_t* outType);
int  H2TCTypeExpr_COMPOUND_LIT(
    H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* n, int32_t* outType);
int H2TCTypeExpr_CALL_WITH_CONTEXT(
    H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* n, int32_t* outType);
int H2TCTypeExpr_NEW(H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* n, int32_t* outType);
int H2TCTypeSourceLocationOfCall(
    H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* callee, int32_t* outType);
int H2TCTypeCompilerDiagCall(
    H2TypeCheckCtx*    c,
    int32_t            nodeId,
    const H2AstNode*   callee,
    H2TCCompilerDiagOp op,
    int32_t*           outType);
int H2TCTypeExpr_CALL(H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* n, int32_t* outType);
int H2TCTypeExpr_CAST(H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* n, int32_t* outType);
int H2TCTypeExpr_SIZEOF(H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* n, int32_t* outType);
int H2TCTypeExpr_FIELD_EXPR(
    H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* n, int32_t* outType);
int H2TCTypeExpr_INDEX(H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* n, int32_t* outType);
int H2TCTypeExpr_UNARY(H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* n, int32_t* outType);
int H2TCTypeExpr_BINARY(H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* n, int32_t* outType);
int H2TCTypeExpr_NULL(H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* n, int32_t* outType);
int H2TCTypeExpr_UNWRAP(H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* n, int32_t* outType);
int H2TCTypeExpr_TUPLE_EXPR(
    H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* n, int32_t* outType);
int H2TCTypeExpr(H2TypeCheckCtx* c, int32_t nodeId, int32_t* outType);
int H2TCValidateConstInitializerExprNode(H2TypeCheckCtx* c, int32_t initNode);
int H2TCValidateLocalConstVarLikeInitializers(
    H2TypeCheckCtx* c, int32_t nodeId, const H2TCVarLikeParts* parts);
int H2TCTypeVarLike(H2TypeCheckCtx* c, int32_t nodeId);
int H2TCTypeTopLevelVarLikes(H2TypeCheckCtx* c, H2AstKind wantKind);
int H2TCTypeTopLevelConsts(H2TypeCheckCtx* c);
int H2TCTypeTopLevelVars(H2TypeCheckCtx* c);
int H2TCCheckTopLevelConstInitializers(H2TypeCheckCtx* c);
int H2TCValidateTopLevelConstEvaluable(H2TypeCheckCtx* c);
int H2TCGetNullNarrow(H2TypeCheckCtx* c, int32_t condNode, int* outIsEq, H2TCNullNarrow* out);
int H2TCGetOptionalCondNarrow(
    H2TypeCheckCtx* c, int32_t condNode, int* outThenIsSome, H2TCNullNarrow* out);
int H2TCBlockTerminates(H2TypeCheckCtx* c, int32_t blockNode);
int H2TCTypeBlock(
    H2TypeCheckCtx* c, int32_t blockNode, int32_t returnType, int loopDepth, int switchDepth);
int H2TCTypeForStmt(
    H2TypeCheckCtx* c, int32_t nodeId, int32_t returnType, int loopDepth, int switchDepth);
int H2TCTypeSwitchStmt(
    H2TypeCheckCtx* c, int32_t nodeId, int32_t returnType, int loopDepth, int switchDepth);
int H2TCExprIsBlankIdent(H2TypeCheckCtx* c, int32_t exprNode);
int H2TCTypeMultiAssignStmt(H2TypeCheckCtx* c, int32_t nodeId);
int H2TCTypeStmt(
    H2TypeCheckCtx* c, int32_t nodeId, int32_t returnType, int loopDepth, int switchDepth);
int H2TCTypeFunctionBody(H2TypeCheckCtx* c, int32_t funcIndex);
int H2TCMarkTemplateRootFunctionUses(H2TypeCheckCtx* c);
int H2TCCollectFunctionDecls(H2TypeCheckCtx* c);
int H2TCCollectTypeDecls(H2TypeCheckCtx* c);
int H2TCBuildCheckedContext(
    H2Arena*     arena,
    const H2Ast* ast,
    H2StrView    src,
    const H2TypeCheckOptions* _Nullable options,
    H2Diag* _Nullable diag,
    H2TypeCheckCtx* _Nullable outCtx);
int H2TypeCheckEx(
    H2Arena*     arena,
    const H2Ast* ast,
    H2StrView    src,
    const H2TypeCheckOptions* _Nullable options,
    H2Diag* _Nullable diag);
int H2TypeCheck(H2Arena* arena, const H2Ast* ast, H2StrView src, H2Diag* _Nullable diag);
int H2ConstEvalSessionInit(
    H2Arena*             arena,
    const H2Ast*         ast,
    H2StrView            src,
    H2ConstEvalSession** outSession,
    H2Diag* _Nullable diag);
int H2ConstEvalSessionEvalExpr(
    H2ConstEvalSession* session, int32_t exprNode, H2CTFEValue* outValue, int* outIsConst);
int H2ConstEvalSessionEvalIntExpr(
    H2ConstEvalSession* session, int32_t exprNode, int64_t* outValue, int* outIsConst);
int H2ConstEvalSessionEvalTopLevelConst(
    H2ConstEvalSession* session, int32_t constNode, H2CTFEValue* outValue, int* outIsConst);
int H2ConstEvalSessionDecodeTypeTag(
    H2ConstEvalSession* session, uint64_t typeTag, int32_t* outTypeId);
int H2ConstEvalSessionGetTypeInfo(
    H2ConstEvalSession* session, int32_t typeId, H2ConstEvalTypeInfo* outTypeInfo);

H2_API_END

#endif
