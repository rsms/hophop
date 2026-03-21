#ifndef SL_TYPECHECK_INTERNAL_H
#define SL_TYPECHECK_INTERNAL_H

#include "../libsl-impl.h"
#include "../ctfe.h"
#include "../ctfe_exec.h"
#include "../fmt_parse.h"
#include "../mir.h"

SL_API_BEGIN

typedef enum {
    SLTCType_INVALID = 0,
    SLTCType_BUILTIN,
    SLTCType_NAMED,
    SLTCType_ALIAS,
    SLTCType_ANON_STRUCT,
    SLTCType_ANON_UNION,
    SLTCType_PTR,
    SLTCType_REF,
    SLTCType_ARRAY,
    SLTCType_SLICE,
    SLTCType_UNTYPED_INT,
    SLTCType_UNTYPED_FLOAT,
    SLTCType_FUNCTION,
    SLTCType_TUPLE,
    SLTCType_PACK,
    SLTCType_ANYTYPE,
    SLTCType_OPTIONAL,
    SLTCType_NULL,
} SLTCTypeKind;

typedef enum {
    SLBuiltin_INVALID = 0,
    SLBuiltin_VOID,
    SLBuiltin_BOOL,
    SLBuiltin_STR,
    SLBuiltin_TYPE,
    SLBuiltin_U8,
    SLBuiltin_U16,
    SLBuiltin_U32,
    SLBuiltin_U64,
    SLBuiltin_I8,
    SLBuiltin_I16,
    SLBuiltin_I32,
    SLBuiltin_I64,
    SLBuiltin_USIZE,
    SLBuiltin_ISIZE,
    SLBuiltin_F32,
    SLBuiltin_F64,
} SLBuiltinKind;

typedef struct {
    SLTCTypeKind  kind;
    SLBuiltinKind builtin;
    int32_t       baseType;
    int32_t       declNode;
    int32_t       funcIndex;
    uint32_t      arrayLen;
    uint32_t      nameStart;
    uint32_t      nameEnd;
    uint32_t      fieldStart;
    uint16_t      fieldCount;
    uint16_t      flags;
} SLTCType;

enum {
    SLTCTypeFlag_VARSIZE = 1u << 0,
    SLTCTypeFlag_MUTABLE = 1u << 1,
    SLTCTypeFlag_FUNCTION_VARIADIC = 1u << 2,
    SLTCTypeFlag_ALIAS_RESOLVING = 1u << 12,
    SLTCTypeFlag_ALIAS_RESOLVED = 1u << 13,
    SLTCTypeFlag_VISITING = 1u << 14,
    SLTCTypeFlag_VISITED = 1u << 15,
};

typedef struct {
    uint32_t nameStart;
    uint32_t nameEnd;
    int32_t  typeId;
    uint32_t lenNameStart;
    uint32_t lenNameEnd;
    uint16_t flags;
} SLTCField;

enum {
    SLTCFieldFlag_DEPENDENT = 1u << 0,
    SLTCFieldFlag_EMBEDDED = 1u << 1,
};

typedef struct {
    uint32_t nameStart;
    uint32_t nameEnd;
    int32_t  typeId;
    int32_t  declNode;
    int32_t  ownerTypeId;
} SLTCNamedType;

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
    uint16_t flags;
} SLTCFunction;

enum {
    SLTCFunctionFlag_VARIADIC = 1u << 0,
    SLTCFunctionFlag_TEMPLATE = 1u << 1,
    SLTCFunctionFlag_TEMPLATE_INSTANCE = 1u << 2,
    SLTCFunctionFlag_TEMPLATE_HAS_ANYPACK = 1u << 3,
};

enum {
    SLTCFuncParamFlag_CONST = 1u << 0,
};

typedef struct {
    uint32_t nameStart;
    uint32_t nameEnd;
    int32_t  typeId;
    int32_t  initExprNode;
    uint16_t flags;
    uint16_t _reserved;
    uint32_t useIndex;
} SLTCLocal;

enum {
    SLTCLocalFlag_CONST = 1u << 0,
    SLTCLocalFlag_ANYPACK = 1u << 1,
};

enum {
    SLTCLocalUseKind_LOCAL = 0,
    SLTCLocalUseKind_PARAM = 1,
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
} SLTCLocalUse;

typedef struct {
    int32_t  localIdx;
    int32_t  enumTypeId;
    uint32_t variantStart;
    uint32_t variantEnd;
} SLTCVariantNarrow;

typedef enum {
    SLTCForInValueMode_VALUE = 0,
    SLTCForInValueMode_REF,
    SLTCForInValueMode_ANY,
} SLTCForInValueMode;

typedef struct SLTCConstEvalCtx SLTCConstEvalCtx;
typedef struct {
    SLTCConstEvalCtx*   evalCtx;
    SLMirProgramBuilder builder;
    uint32_t*           tcToMir;
    uint8_t*            loweringFns;
    uint32_t*           topConstToMir;
    uint8_t*            loweringTopConsts;
    SLDiag*             diag;
} SLTCMirConstLowerCtx;

typedef struct {
    void* _Nullable ctx;
    SLDiagSinkFn _Nullable onDiag;
} SLTCDiagSink;

typedef struct {
    uint32_t start;
    uint32_t end;
    const char* _Nullable message;
} SLTCWarningDedup;

typedef struct {
    int32_t nodeId;
    int32_t ownerFnIndex;
    uint8_t executed;
    uint8_t _reserved[3];
} SLTCConstDiagUse;

typedef struct {
    SLArena*     arena;
    const SLAst* ast;
    SLStrView    src;
    SLDiag*      diag;
    SLTCDiagSink diagSink;

    SLTCType* types;
    uint32_t  typeLen;
    uint32_t  typeCap;

    SLTCField* fields;
    uint32_t   fieldLen;
    uint32_t   fieldCap;

    SLTCNamedType* namedTypes;
    uint32_t       namedTypeLen;
    uint32_t       namedTypeCap;

    SLTCFunction* funcs;
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

    int32_t* scratchParamTypes;
    uint8_t* scratchParamFlags;
    uint32_t scratchParamCap;

    SLTCLocal*    locals;
    uint32_t      localLen;
    uint32_t      localCap;
    SLTCLocalUse* localUses;
    uint32_t      localUseLen;
    uint32_t      localUseCap;

    SLTCVariantNarrow* variantNarrows;
    uint32_t           variantNarrowLen;
    uint32_t           variantNarrowCap;

    SLCTFEValue* constEvalValues;
    uint8_t*     constEvalState;
    int32_t*     topVarLikeTypes;
    uint8_t*     topVarLikeTypeState;
    const char*  lastConstEvalReason;
    uint32_t     lastConstEvalReasonStart;
    uint32_t     lastConstEvalReasonEnd;

    int32_t typeVoid;
    int32_t typeBool;
    int32_t typeStr;
    int32_t typeType;
    int32_t typeRune;
    int32_t typeMemAllocator;
    int32_t typeUsize;
    int32_t typeReflectSpan;
    int32_t typeFmtValue;
    int32_t typeUntypedInt;
    int32_t typeUntypedFloat;
    int32_t typeNull;
    int32_t typeAnytype;

    SLTCWarningDedup* warningDedup;
    uint32_t          warningDedupLen;
    uint32_t          warningDedupCap;

    SLTCConstDiagUse* constDiagUses;
    uint32_t          constDiagUseLen;
    uint32_t          constDiagUseCap;
    uint8_t*          constDiagFnInvoked;
    uint32_t          constDiagFnInvokedCap;

    int32_t           currentContextType;
    int               hasImplicitMainRootContext;
    int32_t           implicitMainContextType;
    int32_t           activeCallWithNode;
    int32_t           currentFunctionIndex;
    int               currentFunctionIsCompareHook;
    int32_t           activeTypeParamFnNode;
    int32_t           currentTypeOwnerTypeId;
    SLTCConstEvalCtx* activeConstEvalCtx;
    uint8_t           compilerDiagPathProven;
    uint8_t           allowAnytypeParamType;
    uint8_t           allowConstNumericTypeName;

    const int32_t* defaultFieldNodes;
    const int32_t* defaultFieldTypes;
    uint32_t       defaultFieldCount;
    uint32_t       defaultFieldCurrentIndex;
} SLTypeCheckCtx;

struct SLConstEvalSession {
    SLTypeCheckCtx tc;
};

enum {
    SLTCConstEval_UNSEEN = 0,
    SLTCConstEval_VISITING,
    SLTCConstEval_READY,
    SLTCConstEval_NONCONST,
};

enum {
    SLTCTopVarLikeType_UNSEEN = 0,
    SLTCTopVarLikeType_VISITING,
    SLTCTopVarLikeType_READY,
};

#define SLTC_MAX_ANON_FIELDS 256u

typedef struct {
    uint32_t nameStart;
    uint32_t nameEnd;
    int32_t  typeId;
} SLTCAnonFieldSig;

#define SLTC_DIAG_TEXT_CAP 128u

typedef struct {
    char*    ptr;
    uint32_t cap;
    uint32_t len;
} SLTCTextBuf;

int32_t SLTCInternPtrType(SLTypeCheckCtx* c, int32_t baseType, uint32_t errStart, uint32_t errEnd);
int32_t SLTCInternRefType(
    SLTypeCheckCtx* c, int32_t baseType, int isMutable, uint32_t errStart, uint32_t errEnd);
int32_t SLTCResolveAliasBaseType(SLTypeCheckCtx* c, int32_t typeId);

int32_t SLTCFindBuiltinByKind(SLTypeCheckCtx* c, SLBuiltinKind builtinKind);

enum {
    SLTCTypeTagKind_INVALID = 0,
    SLTCTypeTagKind_PRIMITIVE = 1,
    SLTCTypeTagKind_ALIAS = 2,
    SLTCTypeTagKind_STRUCT = 3,
    SLTCTypeTagKind_UNION = 4,
    SLTCTypeTagKind_ENUM = 5,
    SLTCTypeTagKind_POINTER = 6,
    SLTCTypeTagKind_REFERENCE = 7,
    SLTCTypeTagKind_SLICE = 8,
    SLTCTypeTagKind_ARRAY = 9,
    SLTCTypeTagKind_OPTIONAL = 10,
    SLTCTypeTagKind_FUNCTION = 11,
    SLTCTypeTagKind_TUPLE = 12,
};

int32_t SLTCFindNamedTypeIndex(SLTypeCheckCtx* c, uint32_t start, uint32_t end);

typedef enum {
    SLTCCompilerDiagOp_NONE = 0,
    SLTCCompilerDiagOp_ERROR,
    SLTCCompilerDiagOp_ERROR_AT,
    SLTCCompilerDiagOp_WARN,
    SLTCCompilerDiagOp_WARN_AT,
} SLTCCompilerDiagOp;

int SLTCResolveAliasTypeId(SLTypeCheckCtx* c, int32_t typeId);
int SLTCTypeExpr(SLTypeCheckCtx* c, int32_t nodeId, int32_t* outType);
int SLTCConstBoolExpr(SLTypeCheckCtx* c, int32_t nodeId, int* out, int* isConst);
int SLTCConstIntExpr(SLTypeCheckCtx* c, int32_t nodeId, int64_t* out, int* isConst);
int SLTCResolveReflectedTypeValueExpr(SLTypeCheckCtx* c, int32_t exprNode, int32_t* outTypeId);

int SLTCFieldLookup(
    SLTypeCheckCtx* c,
    int32_t         typeId,
    uint32_t        fieldStart,
    uint32_t        fieldEnd,
    int32_t*        outType,
    uint32_t* _Nullable outFieldIndex);
int SLTCIsTypeNodeKind(SLAstKind kind);

typedef struct {
    int32_t  elemType;
    int      indexable;
    int      sliceable;
    int      sliceMutable;
    int      isStringLike;
    int      hasKnownLen;
    uint32_t knownLen;
} SLTCIndexBaseInfo;

int32_t SLTCFindTopLevelVarLikeNode(
    SLTypeCheckCtx* c, uint32_t start, uint32_t end, int32_t* outNameIndex);
int SLTCTypeTopLevelVarLikeNode(
    SLTypeCheckCtx* c, int32_t nodeId, int32_t nameIndex, int32_t* outType);
int     SLTCEnumTypeHasTagZero(SLTypeCheckCtx* c, int32_t enumTypeId);
int     SLTCIsTypeNodeKind(SLAstKind kind);
int32_t SLTCLocalFind(SLTypeCheckCtx* c, uint32_t nameStart, uint32_t nameEnd);
int     SLTCTypeExpr(SLTypeCheckCtx* c, int32_t nodeId, int32_t* outType);
int     SLTCTypeContainsVarSizeByValue(SLTypeCheckCtx* c, int32_t typeId);
int     SLTCResolveTypeNode(SLTypeCheckCtx* c, int32_t nodeId, int32_t* outType);
int32_t SLTCResolveAliasBaseType(SLTypeCheckCtx* c, int32_t typeId);
int     SLTCIsIntegerType(SLTypeCheckCtx* c, int32_t typeId);
int     SLTCIsFloatType(SLTypeCheckCtx* c, int32_t typeId);
int     SLTCIsBoolType(SLTypeCheckCtx* c, int32_t typeId);

typedef struct {
    int32_t  nameListNode;
    int32_t  typeNode;
    int32_t  initNode;
    uint32_t nameCount;
    uint8_t  grouped;
} SLTCVarLikeParts;

#define SLTC_CONST_CALL_MAX_DEPTH 64u
#define SLTC_CONST_FOR_MAX_ITERS  100000u

struct SLTCConstEvalCtx {
    SLTypeCheckCtx* tc;
    SLCTFEExecCtx*  execCtx;
    int32_t         fnStack[SLTC_CONST_CALL_MAX_DEPTH];
    uint32_t        fnDepth;
    const void*     callArgs;
    uint32_t        callArgCount;
    const void*     callBinding;
    uint32_t        callPackParamNameStart;
    uint32_t        callPackParamNameEnd;
    const char*     nonConstReason;
    uint32_t        nonConstStart;
    uint32_t        nonConstEnd;
};

int SLTCEvalTopLevelConstNode(
    SLTypeCheckCtx*   c,
    SLTCConstEvalCtx* evalCtx,
    int32_t           nodeId,
    SLCTFEValue*      outValue,
    int*              outIsConst);
int SLTCEvalTopLevelConstNodeAt(
    SLTypeCheckCtx*   c,
    SLTCConstEvalCtx* evalCtx,
    int32_t           nodeId,
    int32_t           nameIndex,
    SLCTFEValue*      outValue,
    int*              outIsConst);
int SLTCEvalConstExprNode(
    SLTCConstEvalCtx* evalCtx, int32_t exprNode, SLCTFEValue* outValue, int* outIsConst);
int SLTCResolveConstCall(
    void*              ctx,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    const SLCTFEValue* args,
    uint32_t           argCount,
    SLCTFEValue*       outValue,
    int*               outIsConst,
    SLDiag* _Nullable diag);
int SLTCResolveConstCallMir(
    void* _Nullable ctx,
    const SLMirProgram* _Nullable program,
    const SLMirFunction* _Nullable function,
    const SLMirInst* _Nullable inst,
    uint32_t nameStart,
    uint32_t nameEnd,
    const SLCTFEValue* _Nonnull args,
    uint32_t argCount,
    SLCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag);

/* Returns 1 if handled as span compound, 0 if not span-like, -1 on hard error. */

/* Returns 0 if handled, 1 if not a compiler diagnostic call, -1 on error */

/* Returns 0 if handled, 1 if not a reflection call, -1 on error */

int     SLTCTypeContainsVarSizeByValue(SLTypeCheckCtx* c, int32_t typeId);
int     SLTCResolveTypeNode(SLTypeCheckCtx* c, int32_t nodeId, int32_t* outType);
int32_t SLTCResolveAliasBaseType(SLTypeCheckCtx* c, int32_t typeId);
int     SLTCTypeExpr(SLTypeCheckCtx* c, int32_t nodeId, int32_t* outType);
int     SLTCIsIntegerType(SLTypeCheckCtx* c, int32_t typeId);

#define SLTC_MAX_CALL_ARGS       128u
#define SLTC_MAX_CALL_CANDIDATES 256u

int SLTCTypeExpr(SLTypeCheckCtx* c, int32_t nodeId, int32_t* outType);
int SLTCTypeExprExpected(SLTypeCheckCtx* c, int32_t nodeId, int32_t expectedType, int32_t* outType);
int SLTCTypeExprExpected(SLTypeCheckCtx* c, int32_t nodeId, int32_t expectedType, int32_t* outType);
int SLTCExprIsCompoundTemporary(SLTypeCheckCtx* c, int32_t exprNode);
int SLTCExprNeedsExpectedType(SLTypeCheckCtx* c, int32_t exprNode);
int SLTCExprIsAssignable(SLTypeCheckCtx* c, int32_t exprNode);

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
} SLTCCallArgInfo;

typedef struct {
    SLDiagCode code;
    uint32_t   start;
    uint32_t   end;
    uint32_t   argStart;
    uint32_t   argEnd;
} SLTCCallMapError;

typedef struct {
    int      isVariadic;
    uint32_t fixedCount;
    uint32_t fixedInputCount;
    uint32_t spreadArgIndex;
    int32_t  variadicParamType;
    int32_t  variadicElemType;
    int32_t  fixedMappedArgExprNodes[SLTC_MAX_CALL_ARGS];
    int32_t  argParamIndices[SLTC_MAX_CALL_ARGS];
    int32_t  argExpectedTypes[SLTC_MAX_CALL_ARGS];
} SLTCCallBinding;

/* Returns 0 success, 1 no hook name, 2 no viable hook, 3 ambiguous */

/* Returns 0: success, 1: no name, 2: no match, 3: ambiguous, 4: writable-ref temporary,
 * 5: compound infer ambiguous, 6: named argument error, -1: error */

/* Returns 0: success, 1: no name, 2: no match, 3: ambiguous, 4: writable-ref temporary,
 * 5: compound infer ambiguous, 6: named argument error, -1: error */

/* Returns 0: success, 1: no name, 2: no match, 3: ambiguous, 4: writable-ref temporary,
 * 5: compound infer ambiguous, 6: named argument error, -1: error */

int SLTCTypeExpr(SLTypeCheckCtx* c, int32_t nodeId, int32_t* outType);
int SLTCTypeExprExpected(SLTypeCheckCtx* c, int32_t nodeId, int32_t expectedType, int32_t* outType);

/* Returns: 1 recognized variant pattern, 0 not a variant pattern, -1 error */

/* Returns: 1 resolved enum.variant type-name, 0 not an enum.variant type-name, -1 error */

/* Returns 0 for a segment, 1 for end-of-path, -1 for malformed path syntax. */

int SLTCTypeExpr(SLTypeCheckCtx* c, int32_t nodeId, int32_t* outType);

int SLTCTypeStmt(
    SLTypeCheckCtx* c, int32_t nodeId, int32_t returnType, int loopDepth, int switchDepth);

/* Describes a narrowable local found in a null-check condition. */
typedef struct {
    int32_t localIdx;  /* index in c->locals[] */
    int32_t innerType; /* T from ?T */
} SLTCNullNarrow;

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
} SLTCNarrowSave;

/* Shared declarations for multi-file typechecker internals. */

void SLTCSetDiag(SLDiag* diag, SLDiagCode code, uint32_t start, uint32_t end);
void SLTCSetDiagWithArg(
    SLDiag*    diag,
    SLDiagCode code,
    uint32_t   start,
    uint32_t   end,
    uint32_t   argStart,
    uint32_t   argEnd);
int SLTCFailSpan(SLTypeCheckCtx* c, SLDiagCode code, uint32_t start, uint32_t end);
int SLTCFailNode(SLTypeCheckCtx* c, int32_t nodeId, SLDiagCode code);
const char* _Nullable SLTCAllocCStringBytes(SLTypeCheckCtx* c, const uint8_t* bytes, uint32_t len);
int  SLTCStrEqNullable(const char* _Nullable a, const char* _Nullable b);
int  SLTCEmitWarningDiag(SLTypeCheckCtx* c, const SLDiag* diag);
int  SLTCRecordConstDiagUse(SLTypeCheckCtx* c, int32_t nodeId);
void SLTCMarkConstDiagUseExecuted(SLTypeCheckCtx* c, int32_t nodeId);
void SLTCMarkConstDiagFnInvoked(SLTypeCheckCtx* c, int32_t fnIndex);
int  SLTCValidateConstDiagUses(SLTypeCheckCtx* c);
void SLTCMarkFunctionUsed(SLTypeCheckCtx* c, int32_t fnIndex);
void SLTCMarkLocalRead(SLTypeCheckCtx* c, int32_t localIdx);
void SLTCMarkLocalWrite(SLTypeCheckCtx* c, int32_t localIdx);
void SLTCUnmarkLocalRead(SLTypeCheckCtx* c, int32_t localIdx);
void SLTCSetLocalUsageKind(SLTypeCheckCtx* c, int32_t localIdx, uint8_t kind);
void SLTCSetLocalUsageSuppress(SLTypeCheckCtx* c, int32_t localIdx, int suppress);
int  SLTCEmitUnusedSymbolWarnings(SLTypeCheckCtx* c);
void SLTCOffsetToLineCol(
    const char* src, uint32_t srcLen, uint32_t offset, uint32_t* outLine, uint32_t* outColumn);
int SLTCLineColToOffset(
    const char* src, uint32_t srcLen, uint32_t line, uint32_t column, uint32_t* outOffset);
void        SLTCTextBufInit(SLTCTextBuf* b, char* ptr, uint32_t cap);
void        SLTCTextBufAppendChar(SLTCTextBuf* b, char ch);
void        SLTCTextBufAppendCStr(SLTCTextBuf* b, const char* s);
void        SLTCTextBufAppendSlice(SLTCTextBuf* b, SLStrView src, uint32_t start, uint32_t end);
void        SLTCTextBufAppendU32(SLTCTextBuf* b, uint32_t v);
void        SLTCTextBufAppendHexU64(SLTCTextBuf* b, uint64_t v);
const char* SLTCBuiltinName(SLTypeCheckCtx* c, int32_t typeId, SLBuiltinKind kind);
void        SLTCFormatTypeRec(SLTypeCheckCtx* c, int32_t typeId, SLTCTextBuf* b, uint32_t depth);
int         SLTCExprIsStringConstant(SLTypeCheckCtx* c, int32_t nodeId);
void        SLTCFormatExprSubject(SLTypeCheckCtx* c, int32_t nodeId, SLTCTextBuf* b);
char* _Nullable SLTCAllocDiagText(SLTypeCheckCtx* c, const char* text);
int SLTCFailTypeMismatchDetail(
    SLTypeCheckCtx* c, int32_t failNode, int32_t exprNode, int32_t srcType, int32_t dstType);
int SLTCFailAssignToConst(SLTypeCheckCtx* c, int32_t lhsNode);
int SLTCFailSwitchMissingCases(
    SLTypeCheckCtx* c,
    int32_t         failNode,
    int32_t         subjectType,
    int32_t         subjectEnumType,
    uint32_t        enumVariantCount,
    const uint32_t* enumVariantStarts,
    const uint32_t* enumVariantEnds,
    const uint8_t*  enumCovered,
    int             boolCoveredTrue,
    int             boolCoveredFalse);
int32_t SLAstFirstChild(const SLAst* ast, int32_t nodeId);
int32_t SLAstNextSibling(const SLAst* ast, int32_t nodeId);
int SLNameEqSlice(SLStrView src, uint32_t aStart, uint32_t aEnd, uint32_t bStart, uint32_t bEnd);
int SLNameEqLiteral(SLStrView src, uint32_t start, uint32_t end, const char* lit);
int SLNameHasPrefix(SLStrView src, uint32_t start, uint32_t end, const char* prefix);
int SLNameHasSuffix(SLStrView src, uint32_t start, uint32_t end, const char* suffix);
int32_t  SLTCFindMemAllocatorType(SLTypeCheckCtx* c);
int32_t  SLTCGetStrRefType(SLTypeCheckCtx* c, uint32_t start, uint32_t end);
int32_t  SLTCGetStrPtrType(SLTypeCheckCtx* c, uint32_t start, uint32_t end);
int32_t  SLTCAddType(SLTypeCheckCtx* c, const SLTCType* t, uint32_t errStart, uint32_t errEnd);
int32_t  SLTCAddBuiltinType(SLTypeCheckCtx* c, const char* name, SLBuiltinKind builtinKind);
int      SLTCEnsureInitialized(SLTypeCheckCtx* c);
int32_t  SLTCFindBuiltinType(SLTypeCheckCtx* c, uint32_t start, uint32_t end);
int32_t  SLTCFindBuiltinByKind(SLTypeCheckCtx* c, SLBuiltinKind builtinKind);
uint8_t  SLTCTypeTagKindOf(SLTypeCheckCtx* c, int32_t typeId);
uint64_t SLTCEncodeTypeTag(SLTypeCheckCtx* c, int32_t typeId);
int      SLTCDecodeTypeTag(SLTypeCheckCtx* c, uint64_t typeTag, int32_t* outTypeId);
int32_t  SLTCResolveTypeValueName(SLTypeCheckCtx* c, uint32_t start, uint32_t end);
int32_t  SLTCFindNamedTypeIndex(SLTypeCheckCtx* c, uint32_t start, uint32_t end);
int32_t  SLTCFindNamedTypeIndexOwned(
     SLTypeCheckCtx* c, int32_t ownerTypeId, uint32_t start, uint32_t end);
int32_t SLTCResolveTypeNamePath(
    SLTypeCheckCtx* c, uint32_t start, uint32_t end, int32_t ownerTypeId);
int32_t SLTCFindNamedTypeByLiteral(SLTypeCheckCtx* c, const char* name);
int32_t SLTCFindBuiltinNamedTypeBySuffix(SLTypeCheckCtx* c, const char* suffix);
int32_t SLTCFindNamedTypeBySuffix(SLTypeCheckCtx* c, const char* suffix);
int32_t SLTCFindReflectKindType(SLTypeCheckCtx* c);
int     SLTCNameEqLiteralOrPkgBuiltin(
        SLTypeCheckCtx* c, uint32_t start, uint32_t end, const char* name, const char* pkgPrefix);
SLTCCompilerDiagOp SLTCCompilerDiagOpFromName(SLTypeCheckCtx* c, uint32_t start, uint32_t end);
int                SLTCIsSpanOfName(SLTypeCheckCtx* c, uint32_t start, uint32_t end);
int32_t            SLTCFindReflectSpanType(SLTypeCheckCtx* c);
int32_t            SLTCFindFmtValueType(SLTypeCheckCtx* c);
int                SLTCTypeIsReflectSpan(SLTypeCheckCtx* c, int32_t typeId);
int                SLTCTypeIsFmtValue(SLTypeCheckCtx* c, int32_t typeId);
int32_t            SLTCFindFunctionIndex(SLTypeCheckCtx* c, uint32_t start, uint32_t end);
int32_t            SLTCFindPlainFunctionValueIndex(SLTypeCheckCtx* c, uint32_t start, uint32_t end);
int32_t            SLTCFindPkgQualifiedFunctionValueIndex(
               SLTypeCheckCtx* c, uint32_t pkgStart, uint32_t pkgEnd, uint32_t nameStart, uint32_t nameEnd);
int SLTCFunctionNameEq(const SLTypeCheckCtx* c, uint32_t funcIndex, uint32_t start, uint32_t end);
int SLTCNameEqPkgPrefixedMethod(
    SLTypeCheckCtx* c,
    uint32_t        candidateStart,
    uint32_t        candidateEnd,
    uint32_t        pkgStart,
    uint32_t        pkgEnd,
    uint32_t        methodStart,
    uint32_t        methodEnd);
int SLTCExtractPkgPrefixFromTypeName(
    SLTypeCheckCtx* c,
    uint32_t        typeNameStart,
    uint32_t        typeNameEnd,
    uint32_t*       outPkgStart,
    uint32_t*       outPkgEnd);
int SLTCImportDefaultAliasEq(
    SLStrView src, uint32_t pathStart, uint32_t pathEnd, uint32_t aliasStart, uint32_t aliasEnd);
int SLTCHasImportAlias(SLTypeCheckCtx* c, uint32_t aliasStart, uint32_t aliasEnd);
int SLTCResolveReceiverPkgPrefix(
    SLTypeCheckCtx* c, int32_t typeId, uint32_t* outPkgStart, uint32_t* outPkgEnd);
int SLTCResolveEnumMemberType(
    SLTypeCheckCtx* c,
    int32_t         recvNode,
    uint32_t        memberStart,
    uint32_t        memberEnd,
    int32_t*        outType);
int32_t SLTCInternPtrType(SLTypeCheckCtx* c, int32_t baseType, uint32_t errStart, uint32_t errEnd);
int     SLTCTypeIsMutable(const SLTCType* t);
int     SLTCIsMutableRefType(SLTypeCheckCtx* c, int32_t typeId);
int32_t SLTCInternRefType(
    SLTypeCheckCtx* c, int32_t baseType, int isMutable, uint32_t errStart, uint32_t errEnd);
int32_t SLTCInternArrayType(
    SLTypeCheckCtx* c, int32_t baseType, uint32_t arrayLen, uint32_t errStart, uint32_t errEnd);
int32_t SLTCInternSliceType(
    SLTypeCheckCtx* c, int32_t baseType, int isMutable, uint32_t errStart, uint32_t errEnd);
int32_t SLTCInternOptionalType(
    SLTypeCheckCtx* c, int32_t baseType, uint32_t errStart, uint32_t errEnd);
int32_t SLTCInternAnonAggregateType(
    SLTypeCheckCtx*         c,
    int                     isUnion,
    const SLTCAnonFieldSig* fields,
    uint32_t                fieldCount,
    int32_t                 declNode,
    uint32_t                errStart,
    uint32_t                errEnd);
int SLTCFunctionTypeMatchesSignature(
    SLTypeCheckCtx* c,
    const SLTCType* t,
    int32_t         returnType,
    const int32_t*  paramTypes,
    const uint8_t*  paramFlags,
    uint32_t        paramCount,
    int             isVariadic);
int32_t SLTCInternFunctionType(
    SLTypeCheckCtx* c,
    int32_t         returnType,
    const int32_t*  paramTypes,
    const uint8_t*  paramFlags,
    uint32_t        paramCount,
    int             isVariadic,
    int32_t         funcIndex,
    uint32_t        errStart,
    uint32_t        errEnd);
int SLTCTupleTypeMatchesSignature(
    SLTypeCheckCtx* c, const SLTCType* t, const int32_t* elemTypes, uint32_t elemCount);
int32_t SLTCInternTupleType(
    SLTypeCheckCtx* c,
    const int32_t*  elemTypes,
    uint32_t        elemCount,
    uint32_t        errStart,
    uint32_t        errEnd);
int32_t SLTCInternPackType(
    SLTypeCheckCtx* c,
    const int32_t*  elemTypes,
    uint32_t        elemCount,
    uint32_t        errStart,
    uint32_t        errEnd);
int      SLTCParseArrayLen(SLTypeCheckCtx* c, const SLAstNode* node, uint32_t* outLen);
int      SLTCResolveIndexBaseInfo(SLTypeCheckCtx* c, int32_t baseType, SLTCIndexBaseInfo* out);
int32_t  SLTCListItemAt(const SLAst* ast, int32_t listNode, uint32_t index);
uint32_t SLTCListCount(const SLAst* ast, int32_t listNode);
int      SLTCVarLikeGetParts(SLTypeCheckCtx* c, int32_t nodeId, SLTCVarLikeParts* out);
int32_t  SLTCVarLikeNameIndexBySlice(
     SLTypeCheckCtx* c, int32_t nodeId, uint32_t start, uint32_t end);
int32_t SLTCVarLikeInitExprNodeAt(SLTypeCheckCtx* c, int32_t nodeId, int32_t nameIndex);
int32_t SLTCVarLikeInitExprNode(SLTypeCheckCtx* c, int32_t nodeId);
void    SLTCConstSetReason(
       SLTCConstEvalCtx* evalCtx, uint32_t start, uint32_t end, const char* reason);
void SLTCConstSetReasonNode(SLTCConstEvalCtx* evalCtx, int32_t nodeId, const char* reason);
void SLTCAttachConstEvalReason(SLTypeCheckCtx* c);
int  SLTCResolveConstIdent(
     void*        ctx,
     uint32_t     nameStart,
     uint32_t     nameEnd,
     SLCTFEValue* outValue,
     int*         outIsConst,
     SLDiag* _Nullable diag);
int SLTCConstLookupExecBindingType(
    SLTCConstEvalCtx* evalCtx, uint32_t nameStart, uint32_t nameEnd, int32_t* outType);
int      SLTCConstBuiltinSizeBytes(SLBuiltinKind b, uint64_t* outBytes);
int      SLTCConstBuiltinAlignBytes(SLBuiltinKind b, uint64_t* outAlign);
uint64_t SLTCConstAlignUpU64(uint64_t v, uint64_t align);
int      SLTCConstTypeLayout(
         SLTypeCheckCtx* c, int32_t typeId, uint64_t* outSize, uint64_t* outAlign, uint32_t depth);
int SLTCConstEvalSizeOf(
    SLTCConstEvalCtx* evalCtx, int32_t exprNode, SLCTFEValue* outValue, int* outIsConst);
int SLTCConstEvalCast(
    SLTCConstEvalCtx* evalCtx, int32_t exprNode, SLCTFEValue* outValue, int* outIsConst);
int SLTCConstEvalTypeOf(
    SLTCConstEvalCtx* evalCtx, int32_t exprNode, SLCTFEValue* outValue, int* outIsConst);
int SLTCResolveReflectedTypeValueExpr(SLTypeCheckCtx* c, int32_t exprNode, int32_t* outTypeId);
int SLTCConstEvalTypeNameValue(
    SLTypeCheckCtx* c, int32_t typeId, SLCTFEValue* outValue, int* outIsConst);
void SLTCConstEvalSetNullValue(SLCTFEValue* outValue);
void SLTCConstEvalSetSpanFromOffsets(
    SLTypeCheckCtx* c, uint32_t startOffset, uint32_t endOffset, SLCTFEValue* outValue);
int SLTCConstEvalSpanOfCall(
    SLTCConstEvalCtx* evalCtx, int32_t exprNode, SLCTFEValue* outValue, int* outIsConst);
int SLTCConstEvalU32Arg(
    SLTCConstEvalCtx* evalCtx, int32_t exprNode, uint32_t* outValue, int* outIsConst);
int SLTCConstEvalPosCompound(
    SLTCConstEvalCtx* evalCtx, int32_t nodeId, uint32_t* ioLine, uint32_t* ioColumn);
int SLTCConstEvalSpanCompound(
    SLTCConstEvalCtx* evalCtx,
    int32_t           exprNode,
    int               forceSpan,
    SLCTFEValue*      outValue,
    int*              outIsConst);
int SLTCConstEvalSpanExpr(
    SLTCConstEvalCtx* evalCtx, int32_t exprNode, SLCTFESpan* outSpan, int* outIsConst);
int SLTCConstEvalCompilerDiagCall(
    SLTCConstEvalCtx* evalCtx, int32_t exprNode, SLCTFEValue* outValue, int* outIsConst);
int SLTCConstEvalTypeReflectionCall(
    SLTCConstEvalCtx* evalCtx, int32_t exprNode, SLCTFEValue* outValue, int* outIsConst);
int SLTCEvalConstExprNode(
    SLTCConstEvalCtx* evalCtx, int32_t exprNode, SLCTFEValue* outValue, int* outIsConst);
int SLTCEvalConstExecExprCb(void* ctx, int32_t exprNode, SLCTFEValue* outValue, int* outIsConst);
int SLTCEvalConstExecResolveTypeCb(void* ctx, int32_t typeNode, int32_t* outTypeId);
int SLTCEvalConstExecInferValueTypeCb(void* ctx, const SLCTFEValue* value, int32_t* outTypeId);
int SLTCMirConstZeroInitLocal(
    void* _Nullable ctx,
    const SLMirTypeRef* _Nonnull typeRef,
    SLCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag);
int SLTCMirConstCoerceValueForType(
    void* _Nullable ctx,
    const SLMirTypeRef* _Nonnull typeRef,
    SLCTFEValue* _Nonnull inOutValue,
    SLDiag* _Nullable diag);
int SLTCMirConstIndexValue(
    void* _Nullable ctx,
    const SLCTFEValue* _Nonnull base,
    const SLCTFEValue* _Nonnull index,
    SLCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag);
int SLTCMirConstSequenceLen(
    void* _Nullable ctx,
    const SLCTFEValue* _Nonnull base,
    SLCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag);
int SLTCMirConstIterInit(
    void* _Nullable ctx,
    uint32_t sourceNode,
    const SLCTFEValue* _Nonnull source,
    uint16_t flags,
    SLCTFEValue* _Nonnull outIter,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag);
int SLTCMirConstIterNext(
    void* _Nullable ctx,
    const SLCTFEValue* _Nonnull iterValue,
    uint16_t flags,
    int* _Nonnull outHasItem,
    SLCTFEValue* _Nonnull outKey,
    int* _Nonnull outKeyIsConst,
    SLCTFEValue* _Nonnull outValue,
    int* _Nonnull outValueIsConst,
    SLDiag* _Nullable diag);
int SLTCMirConstAggGetField(
    void* _Nullable ctx,
    const SLCTFEValue* _Nonnull base,
    uint32_t nameStart,
    uint32_t nameEnd,
    SLCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag);
int SLTCMirConstAggAddrField(
    void* _Nullable ctx,
    const SLCTFEValue* _Nonnull base,
    uint32_t nameStart,
    uint32_t nameEnd,
    SLCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag);
int SLTCMirConstMakeTuple(
    void* _Nullable ctx,
    const SLCTFEValue* _Nonnull elems,
    uint32_t elemCount,
    uint32_t typeNodeHint,
    SLCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag);
int SLTCEvalConstForInIndexCb(
    void* _Nullable ctx,
    SLCTFEExecCtx* _Nonnull execCtx,
    const SLCTFEValue* _Nonnull sourceValue,
    uint32_t index,
    int      byRef,
    SLCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst);
int32_t SLTCFindConstCallableFunction(
    SLTypeCheckCtx* c, uint32_t nameStart, uint32_t nameEnd, uint32_t argCount);
int SLTCResolveForInIterator(
    SLTypeCheckCtx* c,
    int32_t         sourceNode,
    int32_t         sourceType,
    int32_t*        outFnIndex,
    int32_t*        outIterType);
int SLTCResolveForInNextValue(
    SLTypeCheckCtx*    c,
    int32_t            iterPtrType,
    SLTCForInValueMode valueMode,
    int32_t*           outValueType,
    int32_t*           outFn);
int SLTCResolveForInNextKey(
    SLTypeCheckCtx* c, int32_t iterPtrType, int32_t* outKeyType, int32_t* outFn);
int SLTCResolveForInNextKeyAndValue(
    SLTypeCheckCtx*    c,
    int32_t            iterPtrType,
    SLTCForInValueMode valueMode,
    int32_t*           outKeyType,
    int32_t*           outValueType,
    int32_t*           outFn);
int SLTCResolveConstCall(
    void*              ctx,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    const SLCTFEValue* args,
    uint32_t           argCount,
    SLCTFEValue*       outValue,
    int*               outIsConst,
    SLDiag* _Nullable diag);
int SLTCResolveConstCallMirPre(
    void* _Nullable ctx,
    const SLMirProgram* _Nullable program,
    const SLMirFunction* _Nullable function,
    const SLMirInst* _Nullable inst,
    uint32_t     nameStart,
    uint32_t     nameEnd,
    SLCTFEValue* outValue,
    int*         outIsConst,
    SLDiag* _Nullable diag);
int SLTCEvalTopLevelConstNodeAt(
    SLTypeCheckCtx*   c,
    SLTCConstEvalCtx* evalCtx,
    int32_t           nodeId,
    int32_t           nameIndex,
    SLCTFEValue*      outValue,
    int*              outIsConst);
int SLTCEvalTopLevelConstNode(
    SLTypeCheckCtx*   c,
    SLTCConstEvalCtx* evalCtx,
    int32_t           nodeId,
    SLCTFEValue*      outValue,
    int*              outIsConst);
int SLTCConstBoolExpr(SLTypeCheckCtx* c, int32_t nodeId, int* out, int* isConst);
int SLTCConstIntExpr(SLTypeCheckCtx* c, int32_t nodeId, int64_t* out, int* isConst);
int SLTCConstFloatExpr(SLTypeCheckCtx* c, int32_t nodeId, double* out, int* isConst);
int SLTCConstStringExpr(
    SLTypeCheckCtx* c, int32_t nodeId, const uint8_t** outBytes, uint32_t* outLen, int* outIsConst);
void SLTCMarkRuntimeBoundsCheck(SLTypeCheckCtx* c, int32_t nodeId);
int  SLTCResolveAnonAggregateTypeNode(
     SLTypeCheckCtx* c, int32_t nodeId, int isUnion, int32_t* outType);
int     SLTCResolveAliasTypeId(SLTypeCheckCtx* c, int32_t typeId);
int32_t SLTCResolveAliasBaseType(SLTypeCheckCtx* c, int32_t typeId);
int     SLTCFnNodeHasTypeParamName(
        SLTypeCheckCtx* c, int32_t fnNode, uint32_t nameStart, uint32_t nameEnd);
int SLTCResolveActiveTypeParamType(
    SLTypeCheckCtx* c, uint32_t nameStart, uint32_t nameEnd, int32_t* outType);
int SLTCMirConstInitLowerCtx(SLTCConstEvalCtx* evalCtx, SLTCMirConstLowerCtx* _Nonnull outCtx);
int SLTCMirConstLowerFunction(
    SLTCMirConstLowerCtx* c, int32_t fnIndex, uint32_t* _Nullable outMirFnIndex);
int SLTCMirConstRewriteDirectCalls(SLTCMirConstLowerCtx* c, uint32_t mirFnIndex, int32_t rootNode);
int SLTCResolveTypeNode(SLTypeCheckCtx* c, int32_t nodeId, int32_t* outType);
int SLTCAddNamedType(SLTypeCheckCtx* c, int32_t nodeId, int32_t ownerTypeId, int32_t* outTypeId);
int SLTCCollectTypeDeclsFromNode(SLTypeCheckCtx* c, int32_t nodeId);
int SLTCIsIntegerType(SLTypeCheckCtx* c, int32_t typeId);
int SLTCIsConstNumericType(SLTypeCheckCtx* c, int32_t typeId);
int SLTCTypeIsRuneLike(SLTypeCheckCtx* c, int32_t typeId);
int SLTCConstIntFitsType(SLTypeCheckCtx* c, int64_t value, int32_t typeId);
int SLTCConstIntFitsFloatType(SLTypeCheckCtx* c, int64_t value, int32_t typeId);
int SLTCConstFloatFitsType(SLTypeCheckCtx* c, double value, int32_t typeId);
int SLTCFailConstIntRange(SLTypeCheckCtx* c, int32_t nodeId, int64_t value, int32_t expectedType);
int SLTCFailConstFloatRange(SLTypeCheckCtx* c, int32_t nodeId, int32_t expectedType);
int SLTCIsFloatType(SLTypeCheckCtx* c, int32_t typeId);
int SLTCIsNumericType(SLTypeCheckCtx* c, int32_t typeId);
int SLTCIsBoolType(SLTypeCheckCtx* c, int32_t typeId);
int SLTCIsNamedDeclKind(SLTypeCheckCtx* c, int32_t typeId, SLAstKind kind);
int SLTCIsStringLikeType(SLTypeCheckCtx* c, int32_t typeId);
int SLTCTypeSupportsFmtReflectRec(SLTypeCheckCtx* c, int32_t typeId, uint32_t depth);
int SLTCIsComparableTypeRec(SLTypeCheckCtx* c, int32_t typeId, uint32_t depth);
int SLTCIsOrderedTypeRec(SLTypeCheckCtx* c, int32_t typeId, uint32_t depth);
int SLTCIsComparableType(SLTypeCheckCtx* c, int32_t typeId);
int SLTCIsOrderedType(SLTypeCheckCtx* c, int32_t typeId);
int SLTCTypeSupportsLen(SLTypeCheckCtx* c, int32_t typeId);
int SLTCIsUntyped(SLTypeCheckCtx* c, int32_t typeId);
int SLTCIsTypeNodeKind(SLAstKind kind);
int SLTCConcretizeInferredType(SLTypeCheckCtx* c, int32_t typeId, int32_t* outType);
int SLTCTypeIsVarSize(SLTypeCheckCtx* c, int32_t typeId);
int SLTCTypeContainsVarSizeByValue(SLTypeCheckCtx* c, int32_t typeId);
int SLTCIsComparisonHookName(
    SLTypeCheckCtx* c, uint32_t nameStart, uint32_t nameEnd, int* outIsEqualHook);
int     SLTCTypeIsU8Slice(SLTypeCheckCtx* c, int32_t typeId, int requireMutable);
int     SLTCTypeIsFreeablePointer(SLTypeCheckCtx* c, int32_t typeId);
int32_t SLTCFindEmbeddedFieldIndex(SLTypeCheckCtx* c, int32_t namedTypeId);
int     SLTCEmbedDistanceToType(
        SLTypeCheckCtx* c, int32_t srcType, int32_t dstType, uint32_t* outDistance);
int SLTCIsTypeDerivedFromEmbedded(SLTypeCheckCtx* c, int32_t srcType, int32_t dstType);
int SLTCCanAssign(SLTypeCheckCtx* c, int32_t dstType, int32_t srcType);
int SLTCCoerceForBinary(SLTypeCheckCtx* c, int32_t leftType, int32_t rightType, int32_t* outType);
int SLTCConversionCost(SLTypeCheckCtx* c, int32_t dstType, int32_t srcType, uint8_t* outCost);
int SLTCCostVectorCompare(const uint8_t* a, const uint8_t* b, uint32_t len);
int32_t SLTCUnwrapCallArgExprNode(SLTypeCheckCtx* c, int32_t argNode);
int     SLTCCollectCallArgInfo(
        SLTypeCheckCtx*  c,
        int32_t          callNode,
        int32_t          calleeNode,
        int              includeReceiver,
        int32_t          receiverNode,
        SLTCCallArgInfo* outArgs,
        int32_t* _Nullable outArgTypes,
        uint32_t* outArgCount);
int     SLTCIsMainFunction(SLTypeCheckCtx* c, const SLTCFunction* fn);
int32_t SLTCResolveImplicitMainContextType(SLTypeCheckCtx* c);
int     SLTCCurrentContextFieldType(
        SLTypeCheckCtx* c, uint32_t fieldStart, uint32_t fieldEnd, int32_t* outType);
int SLTCCurrentContextFieldTypeByLiteral(
    SLTypeCheckCtx* c, const char* fieldName, int32_t* outType);
int32_t SLTCContextFindOverlayNode(SLTypeCheckCtx* c);
int32_t SLTCContextFindOverlayBindMatch(
    SLTypeCheckCtx* c, uint32_t fieldStart, uint32_t fieldEnd, const char* _Nullable fieldName);
int32_t SLTCContextFindOverlayBindByLiteral(SLTypeCheckCtx* c, const char* fieldName);
int     SLTCGetEffectiveContextFieldType(
        SLTypeCheckCtx* c, uint32_t fieldStart, uint32_t fieldEnd, int32_t* outType);
int SLTCGetEffectiveContextFieldTypeByLiteral(
    SLTypeCheckCtx* c, const char* fieldName, int32_t* outType);
int SLTCValidateCurrentCallOverlay(SLTypeCheckCtx* c);
int SLTCValidateCallContextRequirements(SLTypeCheckCtx* c, int32_t requiredContextType);
int SLTCGetFunctionTypeSignature(
    SLTypeCheckCtx* c,
    int32_t         typeId,
    int32_t*        outReturnType,
    uint32_t*       outParamStart,
    uint32_t*       outParamCount,
    int* _Nullable outIsVariadic);
void SLTCCallMapErrorClear(SLTCCallMapError* err);
int  SLTCMapCallArgsToParams(
     SLTypeCheckCtx*        c,
     const SLTCCallArgInfo* callArgs,
     uint32_t               argCount,
     const uint32_t*        paramNameStarts,
     const uint32_t*        paramNameEnds,
     uint32_t               paramCount,
     uint32_t               firstPositionalArgIndex,
     int32_t*               outMappedArgExprNodes,
     SLTCCallMapError* _Nullable outError);
int SLTCPrepareCallBinding(
    SLTypeCheckCtx*        c,
    const SLTCCallArgInfo* callArgs,
    uint32_t               argCount,
    const uint32_t*        paramNameStarts,
    const uint32_t*        paramNameEnds,
    const int32_t*         paramTypes,
    uint32_t               paramCount,
    int                    isVariadic,
    int                    allowNamedMapping,
    uint32_t               firstPositionalArgIndex,
    SLTCCallBinding*       outBinding,
    SLTCCallMapError*      outError);
int SLTCCheckConstParamArgs(
    SLTypeCheckCtx*        c,
    const SLTCCallArgInfo* callArgs,
    uint32_t               argCount,
    const SLTCCallBinding* binding,
    const uint32_t*        paramNameStarts,
    const uint32_t*        paramNameEnds,
    const uint8_t*         paramFlags,
    uint32_t               paramCount,
    SLTCCallMapError*      outError);
int SLTCFunctionHasAnytypeParam(SLTypeCheckCtx* c, int32_t fnIndex);
int SLTCInstantiateAnytypeFunctionForCall(
    SLTypeCheckCtx*        c,
    int32_t                fnIndex,
    const SLTCCallArgInfo* callArgs,
    uint32_t               argCount,
    uint32_t               firstPositionalArgIndex,
    int32_t                autoRefFirstArgType,
    int32_t*               outFuncIndex,
    SLTCCallMapError*      outError);
int SLTCResolveComparisonHookArgCost(
    SLTypeCheckCtx* c, int32_t paramType, int32_t argType, uint8_t* outCost);
int SLTCResolveComparisonHook(
    SLTypeCheckCtx* c,
    const char*     hookName,
    int32_t         lhsType,
    int32_t         rhsType,
    int32_t*        outFuncIndex);
void SLTCGatherCallCandidates(
    SLTypeCheckCtx* c,
    uint32_t        nameStart,
    uint32_t        nameEnd,
    int32_t*        outCandidates,
    uint32_t*       outCandidateCount,
    int*            outNameFound);
void SLTCGatherCallCandidatesByPkgMethod(
    SLTypeCheckCtx* c,
    uint32_t        pkgStart,
    uint32_t        pkgEnd,
    uint32_t        methodStart,
    uint32_t        methodEnd,
    int32_t*        outCandidates,
    uint32_t*       outCandidateCount,
    int*            outNameFound);
int SLTCResolveCallFromCandidates(
    SLTypeCheckCtx*        c,
    const int32_t*         candidates,
    uint32_t               candidateCount,
    int                    nameFound,
    const SLTCCallArgInfo* callArgs,
    uint32_t               argCount,
    uint32_t               firstPositionalArgIndex,
    int                    autoRefFirstArg,
    int32_t*               outFuncIndex,
    int32_t*               outMutRefTempArgNode);
int SLTCResolveCallByName(
    SLTypeCheckCtx*        c,
    uint32_t               nameStart,
    uint32_t               nameEnd,
    const SLTCCallArgInfo* callArgs,
    uint32_t               argCount,
    uint32_t               firstPositionalArgIndex,
    int                    autoRefFirstArg,
    int32_t*               outFuncIndex,
    int32_t*               outMutRefTempArgNode);
int SLTCResolveCallByPkgMethod(
    SLTypeCheckCtx*        c,
    uint32_t               pkgStart,
    uint32_t               pkgEnd,
    uint32_t               methodStart,
    uint32_t               methodEnd,
    const SLTCCallArgInfo* callArgs,
    uint32_t               argCount,
    uint32_t               firstPositionalArgIndex,
    int                    autoRefFirstArg,
    int32_t*               outFuncIndex,
    int32_t*               outMutRefTempArgNode);
int SLTCResolveDependentPtrReturnForCall(
    SLTypeCheckCtx* c, int32_t fnIndex, int32_t argNode, int32_t* outType);
int SLTCResolveNamedTypeFields(SLTypeCheckCtx* c, uint32_t namedIndex);
int SLTCResolveAllNamedTypeFields(SLTypeCheckCtx* c);
int SLTCResolveAllTypeAliases(SLTypeCheckCtx* c);
int SLTCCheckEmbeddedCycleFrom(SLTypeCheckCtx* c, int32_t typeId);
int SLTCCheckEmbeddedCycles(SLTypeCheckCtx* c);
int SLTCPropagateVarSizeNamedTypes(SLTypeCheckCtx* c);
int SLTCReadFunctionSig(
    SLTypeCheckCtx* c,
    int32_t         funNode,
    int32_t*        outReturnType,
    uint32_t*       outParamCount,
    int*            outIsVariadic,
    int32_t*        outContextType,
    int*            outHasBody);
int     SLTCCollectFunctionFromNode(SLTypeCheckCtx* c, int32_t nodeId);
int     SLTCFinalizeFunctionTypes(SLTypeCheckCtx* c);
int32_t SLTCFindTopLevelVarLikeNode(
    SLTypeCheckCtx* c, uint32_t start, uint32_t end, int32_t* outNameIndex);
int SLTCTypeTopLevelVarLikeNode(
    SLTypeCheckCtx* c, int32_t nodeId, int32_t nameIndex, int32_t* outType);
int32_t SLTCLocalFind(SLTypeCheckCtx* c, uint32_t nameStart, uint32_t nameEnd);
int     SLTCLocalAdd(
        SLTypeCheckCtx* c,
        uint32_t        nameStart,
        uint32_t        nameEnd,
        int32_t         typeId,
        int             isConst,
        int32_t         initExprNode);
int SLTCVariantNarrowPush(
    SLTypeCheckCtx* c,
    int32_t         localIdx,
    int32_t         enumTypeId,
    uint32_t        variantStart,
    uint32_t        variantEnd);
int SLTCVariantNarrowFind(SLTypeCheckCtx* c, int32_t localIdx, const SLTCVariantNarrow** outNarrow);
int32_t SLTCEnumDeclFirstVariantNode(SLTypeCheckCtx* c, int32_t enumDeclNode);
int32_t SLTCEnumVariantTagExprNode(SLTypeCheckCtx* c, int32_t variantNode);
int32_t SLTCFindEnumVariantNodeByName(
    SLTypeCheckCtx* c, int32_t enumTypeId, uint32_t variantStart, uint32_t variantEnd);
int SLTCEnumVariantPayloadFieldType(
    SLTypeCheckCtx* c,
    int32_t         enumTypeId,
    uint32_t        variantStart,
    uint32_t        variantEnd,
    uint32_t        fieldStart,
    uint32_t        fieldEnd,
    int32_t*        outType);
int SLTCEnumTypeHasTagZero(SLTypeCheckCtx* c, int32_t enumTypeId);
int SLTCCasePatternParts(
    SLTypeCheckCtx* c, int32_t caseLabelNode, int32_t* outExprNode, int32_t* outAliasNode);
int SLTCDecodeVariantPatternExpr(
    SLTypeCheckCtx* c,
    int32_t         exprNode,
    int32_t*        outEnumType,
    uint32_t*       outVariantStart,
    uint32_t*       outVariantEnd);
int SLTCResolveEnumVariantTypeName(
    SLTypeCheckCtx* c,
    int32_t         typeNameNode,
    int32_t*        outEnumType,
    uint32_t*       outVariantStart,
    uint32_t*       outVariantEnd);
int SLTCFieldLookup(
    SLTypeCheckCtx* c,
    int32_t         typeId,
    uint32_t        fieldStart,
    uint32_t        fieldEnd,
    int32_t*        outType,
    uint32_t* _Nullable outFieldIndex);
int SLTCIsAsciiSpace(unsigned char ch);
int SLTCIsIdentStartChar(unsigned char ch);
int SLTCIsIdentContinueChar(unsigned char ch);
int SLTCFieldPathNextSegment(
    SLTypeCheckCtx* c,
    uint32_t        pathStart,
    uint32_t        pathEnd,
    uint32_t*       ioPos,
    uint32_t*       outSegStart,
    uint32_t*       outSegEnd);
int SLTCFieldLookupPath(
    SLTypeCheckCtx* c, int32_t ownerTypeId, uint32_t pathStart, uint32_t pathEnd, int32_t* outType);
int SLTCTypeNewExpr(SLTypeCheckCtx* c, int32_t nodeId, int32_t* outType);
int SLTCExprIsCompoundTemporary(SLTypeCheckCtx* c, int32_t exprNode);
int SLTCExprNeedsExpectedType(SLTypeCheckCtx* c, int32_t exprNode);
int SLTCResolveIdentifierExprType(
    SLTypeCheckCtx* c,
    uint32_t        nameStart,
    uint32_t        nameEnd,
    uint32_t        spanStart,
    uint32_t        spanEnd,
    int32_t*        outType);
int SLTCInferAnonStructTypeFromCompound(
    SLTypeCheckCtx* c, int32_t nodeId, int32_t firstField, int32_t* outType);
int SLTCTypeCompoundLit(SLTypeCheckCtx* c, int32_t nodeId, int32_t expectedType, int32_t* outType);
int SLTCTypeExprExpected(SLTypeCheckCtx* c, int32_t nodeId, int32_t expectedType, int32_t* outType);
int SLTCExprIsAssignable(SLTypeCheckCtx* c, int32_t exprNode);
int SLTCExprIsConstAssignTarget(SLTypeCheckCtx* c, int32_t exprNode);
int SLTCTypeExpr_IDENT(SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* n, int32_t* outType);
int SLTCTypeExpr_INT(SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* n, int32_t* outType);
int SLTCTypeExpr_FLOAT(SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* n, int32_t* outType);
int SLTCTypeExpr_STRING(SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* n, int32_t* outType);
int SLTCTypeExpr_RUNE(SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* n, int32_t* outType);
int SLTCTypeExpr_BOOL(SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* n, int32_t* outType);
int SLTCTypeExpr_COMPOUND_LIT(
    SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* n, int32_t* outType);
int SLTCTypeExpr_CALL_WITH_CONTEXT(
    SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* n, int32_t* outType);
int SLTCTypeExpr_NEW(SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* n, int32_t* outType);
int SLTCTypeSpanOfCall(
    SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* callee, int32_t* outType);
int SLTCTypeCompilerDiagCall(
    SLTypeCheckCtx*    c,
    int32_t            nodeId,
    const SLAstNode*   callee,
    SLTCCompilerDiagOp op,
    int32_t*           outType);
int SLTCTypeExpr_CALL(SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* n, int32_t* outType);
int SLTCTypeExpr_CAST(SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* n, int32_t* outType);
int SLTCTypeExpr_SIZEOF(SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* n, int32_t* outType);
int SLTCTypeExpr_FIELD_EXPR(
    SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* n, int32_t* outType);
int SLTCTypeExpr_INDEX(SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* n, int32_t* outType);
int SLTCTypeExpr_UNARY(SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* n, int32_t* outType);
int SLTCTypeExpr_BINARY(SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* n, int32_t* outType);
int SLTCTypeExpr_NULL(SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* n, int32_t* outType);
int SLTCTypeExpr_UNWRAP(SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* n, int32_t* outType);
int SLTCTypeExpr_TUPLE_EXPR(
    SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* n, int32_t* outType);
int SLTCTypeExpr(SLTypeCheckCtx* c, int32_t nodeId, int32_t* outType);
int SLTCValidateConstInitializerExprNode(SLTypeCheckCtx* c, int32_t initNode);
int SLTCValidateLocalConstVarLikeInitializers(
    SLTypeCheckCtx* c, int32_t nodeId, const SLTCVarLikeParts* parts);
int SLTCTypeVarLike(SLTypeCheckCtx* c, int32_t nodeId);
int SLTCTypeTopLevelVarLikes(SLTypeCheckCtx* c, SLAstKind wantKind);
int SLTCTypeTopLevelConsts(SLTypeCheckCtx* c);
int SLTCTypeTopLevelVars(SLTypeCheckCtx* c);
int SLTCCheckTopLevelConstInitializers(SLTypeCheckCtx* c);
int SLTCValidateTopLevelConstEvaluable(SLTypeCheckCtx* c);
int SLTCGetNullNarrow(SLTypeCheckCtx* c, int32_t condNode, int* outIsEq, SLTCNullNarrow* out);
int SLTCGetOptionalCondNarrow(
    SLTypeCheckCtx* c, int32_t condNode, int* outThenIsSome, SLTCNullNarrow* out);
int SLTCBlockTerminates(SLTypeCheckCtx* c, int32_t blockNode);
int SLTCTypeBlock(
    SLTypeCheckCtx* c, int32_t blockNode, int32_t returnType, int loopDepth, int switchDepth);
int SLTCTypeForStmt(
    SLTypeCheckCtx* c, int32_t nodeId, int32_t returnType, int loopDepth, int switchDepth);
int SLTCTypeSwitchStmt(
    SLTypeCheckCtx* c, int32_t nodeId, int32_t returnType, int loopDepth, int switchDepth);
int SLTCExprIsBlankIdent(SLTypeCheckCtx* c, int32_t exprNode);
int SLTCTypeMultiAssignStmt(SLTypeCheckCtx* c, int32_t nodeId);
int SLTCTypeStmt(
    SLTypeCheckCtx* c, int32_t nodeId, int32_t returnType, int loopDepth, int switchDepth);
int SLTCTypeFunctionBody(SLTypeCheckCtx* c, int32_t funcIndex);
int SLTCMarkTemplateRootFunctionUses(SLTypeCheckCtx* c);
int SLTCCollectFunctionDecls(SLTypeCheckCtx* c);
int SLTCCollectTypeDecls(SLTypeCheckCtx* c);
int SLTCBuildCheckedContext(
    SLArena*     arena,
    const SLAst* ast,
    SLStrView    src,
    const SLTypeCheckOptions* _Nullable options,
    SLDiag* diag,
    SLTypeCheckCtx* _Nullable outCtx);
int SLTypeCheckEx(
    SLArena*     arena,
    const SLAst* ast,
    SLStrView    src,
    const SLTypeCheckOptions* _Nullable options,
    SLDiag* diag);
int SLTypeCheck(SLArena* arena, const SLAst* ast, SLStrView src, SLDiag* diag);
int SLConstEvalSessionInit(
    SLArena*             arena,
    const SLAst*         ast,
    SLStrView            src,
    SLConstEvalSession** outSession,
    SLDiag* _Nullable diag);
int SLConstEvalSessionEvalExpr(
    SLConstEvalSession* session, int32_t exprNode, SLCTFEValue* outValue, int* outIsConst);
int SLConstEvalSessionEvalIntExpr(
    SLConstEvalSession* session, int32_t exprNode, int64_t* outValue, int* outIsConst);
int SLConstEvalSessionEvalTopLevelConst(
    SLConstEvalSession* session, int32_t constNode, SLCTFEValue* outValue, int* outIsConst);
int SLConstEvalSessionDecodeTypeTag(
    SLConstEvalSession* session, uint64_t typeTag, int32_t* outTypeId);
int SLConstEvalSessionGetTypeInfo(
    SLConstEvalSession* session, int32_t typeId, SLConstEvalTypeInfo* outTypeInfo);

SL_API_END

#endif
