#pragma once

#include "../codegen.h"
#include "../libsl-impl.h"

SL_API_BEGIN
typedef struct {
    const char* baseName;
    int         ptrDepth;
    int         valid;
    int         containerKind; /* 0 scalar, 1 array, 2 ro-slice, 3 rw-slice */
    int         containerPtrDepth;
    uint32_t    arrayLen;
    int         hasArrayLen;
    int         readOnly;
    int         isOptional;
} SLTypeRef;

typedef struct {
    SLArena* _Nullable arena;
    char*    v;
    uint32_t len;
    uint32_t cap;
} SLBuf;

typedef struct {
    char*     name;
    char*     cName;
    SLAstKind kind;
    int       isExported;
} SLNameMap;

typedef struct {
    int32_t nodeId;
} SLNodeRef;

typedef struct {
    char*      slName;
    char*      cName;
    int32_t    nodeId;
    uint32_t   tcFuncIndex;
    SLTypeRef  returnType;
    SLTypeRef* paramTypes;
    char**     paramNames;
    uint8_t*   paramFlags;
    uint32_t   paramLen;
    uint32_t   packArgStart;
    uint32_t   packArgCount;
    char* _Nullable packParamName;
    uint16_t  flags;
    SLTypeRef contextType;
    int       hasContext;
    uint8_t   isVariadic;
    uint8_t   _reserved[1];
} SLFnSig;

enum {
    SLFnSigFlag_TEMPLATE_BASE = 1u << 0,
    SLFnSigFlag_TEMPLATE_INSTANCE = 1u << 1,
    SLFnSigFlag_EXPANDED_ANYPACK = 1u << 2,
};

typedef struct {
    char*      aliasName;
    SLTypeRef  returnType;
    SLTypeRef* paramTypes;
    uint32_t   paramLen;
    uint8_t    isVariadic;
    uint8_t    _reserved[3];
} SLFnTypeAlias;

typedef struct {
    char*     aliasName;
    SLTypeRef targetType;
} SLTypeAliasInfo;

typedef struct {
    char*     ownerType;
    char*     fieldName;
    char*     lenFieldName;
    int       isDependent;
    int       isEmbedded;
    int32_t   defaultExprNode;
    SLTypeRef type;
} SLFieldInfo;

typedef struct {
    char* cName;
    int   isUnion;
    int   isVarSize;
} SLVarSizeType;

typedef struct {
    char*    key;
    char*    cName;
    int      isUnion;
    uint32_t fieldStart;
    uint16_t fieldCount;
    uint16_t flags;
} SLAnonTypeInfo;

enum {
    SLAnonTypeFlag_EMITTED_GLOBAL = 1u << 0,
};

typedef struct {
    char*     name;
    SLTypeRef type;
} SLLocal;

typedef struct {
    int32_t  localIdx;
    char*    enumTypeName;
    uint32_t variantStart;
    uint32_t variantEnd;
} SLVariantNarrow;

typedef struct {
    int32_t nodeId;
    char*   cName;
} SLFnNodeName;

typedef struct {
    uint8_t* _Nullable bytes;
    uint32_t len;
} SLStringLiteral;

typedef struct {
    const SLCodegenUnit*    unit;
    const SLCodegenOptions* options;
    SLDiag*                 diag;

    SLArena arena;
    uint8_t arenaInlineStorage[16384];
    SLAst   ast;

    SLBuf out;

    SLNameMap* names;
    uint32_t   nameLen;
    uint32_t   nameCap;

    SLNodeRef* pubDecls;
    uint32_t   pubDeclLen;
    uint32_t   pubDeclCap;

    SLNodeRef* topDecls;
    uint32_t   topDeclLen;
    uint32_t   topDeclCap;

    SLFnSig* fnSigs;
    uint32_t fnSigLen;
    uint32_t fnSigCap;

    SLFnTypeAlias* fnTypeAliases;
    uint32_t       fnTypeAliasLen;
    uint32_t       fnTypeAliasCap;

    SLTypeAliasInfo* typeAliases;
    uint32_t         typeAliasLen;
    uint32_t         typeAliasCap;

    SLFieldInfo* fieldInfos;
    uint32_t     fieldInfoLen;
    uint32_t     fieldInfoCap;

    SLVarSizeType* varSizeTypes;
    uint32_t       varSizeTypeLen;
    uint32_t       varSizeTypeCap;

    SLAnonTypeInfo* anonTypes;
    uint32_t        anonTypeLen;
    uint32_t        anonTypeCap;

    SLLocal* locals;
    uint32_t localLen;
    uint32_t localCap;

    SLVariantNarrow* variantNarrows;
    uint32_t         variantNarrowLen;
    uint32_t         variantNarrowCap;
    int32_t          activeOptionalNarrowLocalIdx;
    uint8_t          hasActiveOptionalNarrow;
    uint8_t          _reserved_optional_narrow[3];
    SLTypeRef        activeOptionalNarrowStorageType;

    SLFnNodeName* fnNodeNames;
    uint32_t      fnNodeNameLen;
    uint32_t      fnNodeNameCap;

    uint32_t* localScopeMarks;
    uint32_t  localScopeLen;
    uint32_t  localScopeCap;

    char**   localAnonTypedefs;
    uint32_t localAnonTypedefLen;
    uint32_t localAnonTypedefCap;

    uint32_t* localAnonTypedefScopeMarks;
    uint32_t  localAnonTypedefScopeLen;
    uint32_t  localAnonTypedefScopeCap;

    int32_t* deferredStmtNodes;
    uint32_t deferredStmtLen;
    uint32_t deferredStmtCap;

    uint32_t* deferScopeMarks;
    uint32_t  deferScopeLen;
    uint32_t  deferScopeCap;

    SLStringLiteral* stringLits;
    uint32_t         stringLitLen;
    uint32_t         stringLitCap;

    int32_t* stringLitByNode;
    uint32_t stringLitByNodeLen;

    int       emitPrivateFnDeclStatic;
    SLTypeRef currentReturnType;
    int       hasCurrentReturnType;
    SLTypeRef currentContextType;
    int       hasCurrentContext;
    int       currentFunctionIsMain;
    int32_t   activeCallWithNode;
    const char* _Nullable activePackParamName;
    char** _Nullable activePackElemNames;
    SLTypeRef* _Nullable activePackElemTypes;
    uint32_t activePackElemCount;
    uint32_t fmtTempCounter;
    SLConstEvalSession* _Nullable constEval;
} SLCBackendC;

enum {
    SLTypeContainer_SCALAR = 0,
    SLTypeContainer_ARRAY = 1,
    SLTypeContainer_SLICE_RO = 2,
    SLTypeContainer_SLICE_MUT = 3,
};

typedef struct {
    int32_t  nameListNode;
    int32_t  typeNode;
    int32_t  initNode;
    uint32_t nameCount;
    uint8_t  grouped;
} SLCCGVarLikeParts;

#define SLCCG_MAX_CALL_ARGS       128u
#define SLCCG_MAX_CALL_CANDIDATES 256u

typedef struct {
    int32_t  argNode;
    int32_t  exprNode;
    uint32_t explicitNameStart;
    uint32_t explicitNameEnd;
    uint32_t implicitNameStart;
    uint32_t implicitNameEnd;
    uint8_t  spread;
    uint8_t  _reserved[3];
} SLCCallArgInfo;

typedef struct {
    int       isVariadic;
    uint32_t  fixedCount;
    uint32_t  fixedInputCount;
    uint32_t  spreadArgIndex;
    int32_t   fixedMappedArgNodes[SLCCG_MAX_CALL_ARGS];
    int32_t   explicitTailNodes[SLCCG_MAX_CALL_ARGS];
    int32_t   argParamIndices[SLCCG_MAX_CALL_ARGS];
    uint32_t  explicitTailCount;
    SLTypeRef argExpectedTypes[SLCCG_MAX_CALL_ARGS];
} SLCCallBinding;

enum {
    SLCCGParamFlag_CONST = 1u << 0,
    SLCCGParamFlag_ANYTYPE = 1u << 1,
    SLCCGParamFlag_ANYPACK = 1u << 2,
};

size_t StrLen(const char* s);

int StrEq(const char* a, const char* b);

int StrHasPrefix(const char* s, const char* prefix);

int StrHasSuffix(const char* s, const char* suffix);

int IsAlnumChar(char ch);

int IsAsciiSpaceChar(unsigned char c);

int IsIdentStartChar(unsigned char c);

int IsIdentContinueChar(unsigned char c);

char ToUpperChar(char ch);

void SetDiag(SLDiag* diag, SLDiagCode code, uint32_t start, uint32_t end);

int EnsureCapArena(
    SLArena* arena, void** ptr, uint32_t* cap, uint32_t need, size_t elemSize, uint32_t align);

int BufReserve(SLBuf* b, uint32_t extra);

int BufAppend(SLBuf* b, const char* s, uint32_t len);

int BufAppendCStr(SLBuf* b, const char* s);

int BufAppendChar(SLBuf* b, char c);

int BufAppendU32(SLBuf* b, uint32_t value);

int BufAppendSlice(SLBuf* b, const char* src, uint32_t start, uint32_t end);

char* _Nullable BufFinish(SLBuf* b);

void EmitIndent(SLCBackendC* c, uint32_t depth);

int IsBuiltinType(const char* s);

int IsIntegerCTypeName(const char* s);

int IsFloatCTypeName(const char* s);

int SliceEq(const char* src, uint32_t start, uint32_t end, const char* s);

int SliceEqName(const char* src, uint32_t start, uint32_t end, const char* s);

int SliceSpanEq(const char* src, uint32_t aStart, uint32_t aEnd, uint32_t bStart, uint32_t bEnd);

int NameEqPkgPrefixedMethod(
    const char* name,
    const char* src,
    uint32_t    pkgStart,
    uint32_t    pkgEnd,
    uint32_t    methodStart,
    uint32_t    methodEnd);

int TypeNamePkgPrefixLen(const char* typeName, uint32_t* outPkgLen);

void TypeRefSetInvalid(SLTypeRef* t);

void TypeRefSetScalar(SLTypeRef* t, const char* baseName);

void CanonicalizeTypeRefBaseName(const SLCBackendC* c, SLTypeRef* t);

int SliceStructPtrDepth(const SLTypeRef* t);

int ParseArrayLenLiteral(const char* src, uint32_t start, uint32_t end, uint32_t* outLen);

void SetDiagNode(SLCBackendC* c, int32_t nodeId, SLDiagCode code);

int BufAppendI64(SLBuf* b, int64_t value);

int EvalConstIntExpr(SLCBackendC* c, int32_t nodeId, int64_t* outValue, int* outIsConst);
int EvalConstFloatExpr(SLCBackendC* c, int32_t nodeId, double* outValue, int* outIsConst);

int ConstIntFitsIntegerType(const char* typeName, int64_t value);
int ConstIntFitsFloatType(const char* typeName, int64_t value);
int ConstFloatFitsFloatType(const char* typeName, double value);

int EmitConstEvaluatedScalar(
    SLCBackendC* c, const SLTypeRef* dstType, const SLCTFEValue* value, int* outEmitted);

char* _Nullable DupSlice(SLCBackendC* c, const char* src, uint32_t start, uint32_t end);

int SliceIsHoleName(const char* src, uint32_t start, uint32_t end);

char* _Nullable DupParamNameForEmit(
    SLCBackendC* c, const SLAstNode* paramNode, uint32_t paramIndex);

char* _Nullable DupAndReplaceDots(SLCBackendC* c, const char* src, uint32_t start, uint32_t end);

char* _Nullable DupCStr(SLCBackendC* c, const char* s);

int DecodeStringLiteralNode(
    SLCBackendC* c, const SLAstNode* n, uint8_t** outBytes, uint32_t* outLen);

int AppendDecodedStringExpr(
    SLCBackendC* c, int32_t nodeId, uint8_t** bytes, uint32_t* len, uint32_t* cap);

int DecodeStringExpr(
    SLCBackendC* c,
    int32_t      nodeId,
    uint8_t**    outBytes,
    uint32_t*    outLen,
    uint32_t*    outStart,
    uint32_t*    outEnd);

int GetOrAddStringLiteralExpr(SLCBackendC* c, int32_t nodeId, int32_t* outLiteralId);

int GetOrAddStringLiteralBytes(
    SLCBackendC* c, const uint8_t* bytes, uint32_t len, int32_t* outLiteralId);

int CollectStringLiterals(SLCBackendC* c);

int HasDoubleUnderscore(const char* s);

int IsTypeDeclKind(SLAstKind kind);

int IsDeclKind(SLAstKind kind);

int IsPubDeclNode(const SLAstNode* n);

int32_t AstFirstChild(const SLAst* ast, int32_t nodeId);

int32_t AstNextSibling(const SLAst* ast, int32_t nodeId);

const SLAstNode* _Nullable NodeAt(const SLCBackendC* c, int32_t nodeId);

int GetDeclNameSpan(const SLCBackendC* c, int32_t nodeId, uint32_t* outStart, uint32_t* outEnd);

int AddName(SLCBackendC* c, uint32_t nameStart, uint32_t nameEnd, SLAstKind kind, int isExported);

const SLNameMap* _Nullable FindNameBySlice(const SLCBackendC* c, uint32_t start, uint32_t end);

const SLNameMap* _Nullable FindNameByCString(const SLCBackendC* c, const char* name);

int NameHasPrefixSuffix(const char* name, const char* prefix, const char* suffix);

int ResolveMainSemanticContextType(SLCBackendC* c, SLTypeRef* outType);

const char* ResolveRuneTypeBaseName(SLCBackendC* c);

const SLNameMap* _Nullable FindNameByCName(const SLCBackendC* c, const char* cName);

int ResolveTypeValueNameExprTypeRef(
    SLCBackendC* c, uint32_t start, uint32_t end, SLTypeRef* outTypeRef);

uint64_t TypeTagHashAddByte(uint64_t h, uint8_t b);

uint64_t TypeTagHashAddU32(uint64_t h, uint32_t v);

uint64_t TypeTagHashAddStr(uint64_t h, const char* s);

uint8_t TypeTagKindFromTypeRef(const SLCBackendC* c, const SLTypeRef* t);

uint64_t TypeTagFromTypeRef(const SLCBackendC* c, const SLTypeRef* t);

int EmitTypeTagLiteralFromTypeRef(SLCBackendC* c, const SLTypeRef* t);

const SLTypeAliasInfo* _Nullable FindTypeAliasInfoByAliasName(
    const SLCBackendC* c, const char* aliasName);

int AddTypeAliasInfo(SLCBackendC* c, const char* aliasName, SLTypeRef targetType);

const char* ResolveScalarAliasBaseName(const SLCBackendC* c, const char* typeName);

int ResolveReflectedTypeValueExprTypeRef(SLCBackendC* c, int32_t exprNode, SLTypeRef* outTypeRef);

int TypeRefIsTypeValue(const SLTypeRef* t);

const char* _Nullable FindReflectKindTypeName(const SLCBackendC* c);

const char* TypeRefDisplayBaseName(const SLCBackendC* c, const char* baseName);

int EmitTypeNameStringLiteralFromTypeRef(SLCBackendC* c, const SLTypeRef* _Nullable t);

int EmitTypeTagKindLiteralFromTypeRef(SLCBackendC* c, const SLTypeRef* t);

int EmitRuntimeTypeTagKindFromExpr(SLCBackendC* c, int32_t exprNode);

int EmitTypeTagIsAliasLiteralFromTypeRef(SLCBackendC* c, const SLTypeRef* t);

int EmitRuntimeTypeTagIsAliasFromExpr(SLCBackendC* c, int32_t exprNode);

int EmitRuntimeTypeTagCtorUnary(SLCBackendC* c, uint32_t kindTag, uint64_t salt, int32_t argNode);

int EmitRuntimeTypeTagCtorArray(SLCBackendC* c, int32_t elemTagNode, int32_t lenNode);

int EmitTypeTagBaseLiteralFromTypeRef(SLCBackendC* c, const SLTypeRef* t);

int TypeRefIsRuneLike(const SLCBackendC* c, const SLTypeRef* typeRef);

const char* _Nullable ResolveTypeName(SLCBackendC* c, uint32_t start, uint32_t end);

void NormalizeCoreRuntimeTypeName(SLTypeRef* outType);

const char* _Nullable ConstEvalBuiltinCName(SLConstEvalBuiltinKind builtin);

int ParseTypeRefFromConstEvalTypeId(SLCBackendC* c, int32_t typeId, SLTypeRef* outType);

int ParseTypeRefFromConstEvalTypeTag(SLCBackendC* c, uint64_t typeTag, SLTypeRef* outType);

int AddNodeRef(SLCBackendC* c, SLNodeRef** arr, uint32_t* len, uint32_t* cap, int32_t nodeId);

int CollectDeclSets(SLCBackendC* c);

const SLFnTypeAlias* _Nullable FindFnTypeAliasByName(const SLCBackendC* c, const char* name);

int EnsureFnTypeAlias(
    SLCBackendC* c,
    SLTypeRef    returnType,
    SLTypeRef*   paramTypes,
    uint32_t     paramLen,
    int          isVariadic,
    const char** outAliasName);

const char* _Nullable TupleFieldName(SLCBackendC* c, uint32_t index);

int ParseTypeRef(SLCBackendC* c, int32_t nodeId, SLTypeRef* outType);

int AddFnSig(
    SLCBackendC* c,
    const char*  slName,
    const char*  baseCName,
    int32_t      nodeId,
    SLTypeRef    returnType,
    SLTypeRef*   paramTypes,
    char** _Nullable paramNames,
    uint8_t* _Nullable paramFlags,
    uint32_t  paramLen,
    int       isVariadic,
    int       hasContext,
    SLTypeRef contextType,
    uint16_t  sigFlags,
    uint32_t  tcFuncIndex,
    uint32_t  packArgStart,
    uint32_t  packArgCount,
    char* _Nullable packParamName);

int AddFieldInfo(
    SLCBackendC* c,
    const char*  ownerType,
    const char*  fieldName,
    const char* _Nullable lenFieldName,
    int32_t   defaultExprNode,
    int       isDependent,
    int       isEmbedded,
    SLTypeRef type);

int AppendTypeRefKey(SLBuf* b, const SLTypeRef* t);

const SLAnonTypeInfo* _Nullable FindAnonTypeByKey(const SLCBackendC* c, const char* key);

const SLAnonTypeInfo* _Nullable FindAnonTypeByCName(const SLCBackendC* c, const char* cName);

int IsTupleFieldName(const char* name, uint32_t index);

int TypeRefTupleInfo(const SLCBackendC* c, const SLTypeRef* t, const SLAnonTypeInfo** outInfo);

int IsLocalAnonTypedefVisible(const SLCBackendC* c, const char* cName);

int MarkLocalAnonTypedefVisible(SLCBackendC* c, const char* cName);

int IsAnonTypeNameVisible(const SLCBackendC* c, const char* cName);

int EmitAnonTypeDeclAtDepth(SLCBackendC* c, const SLAnonTypeInfo* t, uint32_t depth);

int EnsureAnonTypeVisible(SLCBackendC* c, const SLTypeRef* type, uint32_t depth);

int EnsureAnonTypeByFields(
    SLCBackendC*     c,
    int              isUnion,
    const char**     fieldNames,
    const SLTypeRef* fieldTypes,
    uint32_t         fieldCount,
    const char**     outCName);

const SLFnSig* _Nullable FindFnSigBySlice(const SLCBackendC* c, uint32_t start, uint32_t end);

uint32_t FindFnSigCandidatesBySlice(
    const SLCBackendC* c, uint32_t start, uint32_t end, const SLFnSig** out, uint32_t cap);

uint32_t FindFnSigCandidatesByName(
    const SLCBackendC* c, const char* slName, const SLFnSig** out, uint32_t cap);

const char* _Nullable FindFnCNameByNodeId(const SLCBackendC* c, int32_t nodeId);

const SLFnSig* _Nullable FindFnSigByNodeId(const SLCBackendC* c, int32_t nodeId);
uint32_t FindFnSigCandidatesByNodeId(
    const SLCBackendC* c, int32_t nodeId, const SLFnSig** out, uint32_t cap);

const SLFieldInfo* _Nullable FindFieldInfo(
    const SLCBackendC* c, const char* ownerType, uint32_t fieldStart, uint32_t fieldEnd);

const SLFieldInfo* _Nullable FindEmbeddedFieldInfo(const SLCBackendC* c, const char* ownerType);

const SLFieldInfo* _Nullable FindFieldInfoByName(
    const SLCBackendC* c, const char* ownerType, const char* fieldName);

const char* _Nullable CanonicalFieldOwnerType(
    const SLCBackendC* c, const char* _Nullable ownerType);

int ResolveCoreStrFieldBySlice(
    const SLCBackendC* c, uint32_t fieldStart, uint32_t fieldEnd, const SLFieldInfo** outField);

int ResolveFieldPathSingleSegment(
    const SLCBackendC*  c,
    const char*         ownerTypeIn,
    uint32_t            fieldStart,
    uint32_t            fieldEnd,
    const SLFieldInfo** outPath,
    uint32_t            cap,
    uint32_t*           outLen,
    const SLFieldInfo** _Nullable outField);

int FieldPathNextSegment(
    const char* src, uint32_t pathEnd, uint32_t* ioPos, uint32_t* outSegStart, uint32_t* outSegEnd);

int ResolveFieldPathBySlice(
    const SLCBackendC*  c,
    const char*         ownerType,
    uint32_t            fieldStart,
    uint32_t            fieldEnd,
    const SLFieldInfo** outPath,
    uint32_t            cap,
    uint32_t*           outLen,
    const SLFieldInfo** _Nullable outField);

int ResolveEmbeddedPathByNames(
    const SLCBackendC*  c,
    const char*         srcTypeName,
    const char*         dstTypeName,
    const SLFieldInfo** outPath,
    uint32_t            cap,
    uint32_t*           outLen);

int CollectFnAndFieldInfoFromNode(SLCBackendC* c, int32_t nodeId);

int CollectFnAndFieldInfo(SLCBackendC* c);

int CollectTypeAliasInfo(SLCBackendC* c);

int CollectFnTypeAliasesFromNode(SLCBackendC* c, int32_t nodeId);

int CollectFnTypeAliases(SLCBackendC* c);

int AddVarSizeType(SLCBackendC* c, const char* cName, int isUnion);

SLVarSizeType* _Nullable FindVarSizeType(SLCBackendC* c, const char* cName);

int CollectVarSizeTypesFromDeclSets(SLCBackendC* c);

int IsVarSizeTypeName(const SLCBackendC* c, const char* cName);

int PropagateVarSizeTypes(SLCBackendC* c);

int PushScope(SLCBackendC* c);

void PopScope(SLCBackendC* c);

int PushDeferScope(SLCBackendC* c);

void PopDeferScope(SLCBackendC* c);

int AddDeferredStmt(SLCBackendC* c, int32_t stmtNodeId);

int AddLocal(SLCBackendC* c, const char* name, SLTypeRef type);

void TrimVariantNarrowsToLocalLen(SLCBackendC* c);

int AddVariantNarrow(
    SLCBackendC* c,
    int32_t      localIdx,
    const char*  enumTypeName,
    uint32_t     variantStart,
    uint32_t     variantEnd);

int32_t FindLocalIndexBySlice(const SLCBackendC* c, uint32_t start, uint32_t end);

const SLVariantNarrow* _Nullable FindVariantNarrowByLocalIdx(
    const SLCBackendC* c, int32_t localIdx);

const SLLocal* _Nullable FindLocalBySlice(const SLCBackendC* c, uint32_t start, uint32_t end);

int FindEnumDeclNodeBySlice(const SLCBackendC* c, uint32_t start, uint32_t end, int32_t* outNodeId);

int EnumDeclHasMemberBySlice(
    const SLCBackendC* c, int32_t enumNodeId, uint32_t memberStart, uint32_t memberEnd);

int ResolveEnumSelectorByFieldExpr(
    const SLCBackendC* c,
    int32_t            fieldExprNode,
    const SLNameMap** _Nullable outEnumMap,
    int32_t* _Nullable outEnumDeclNode,
    int* _Nullable outEnumHasPayload,
    uint32_t* _Nullable outVariantStart,
    uint32_t* _Nullable outVariantEnd);

int EnumDeclHasPayload(const SLCBackendC* c, int32_t enumNodeId);

int32_t EnumVariantTagExprNode(const SLCBackendC* c, int32_t variantNode);

int FindEnumDeclNodeByCName(const SLCBackendC* c, const char* enumCName, int32_t* outNodeId);

int FindEnumVariantNodeBySlice(
    const SLCBackendC* c,
    int32_t            enumNodeId,
    uint32_t           variantStart,
    uint32_t           variantEnd,
    int32_t*           outVariantNode);

int ResolveEnumVariantPayloadFieldType(
    SLCBackendC* c,
    const char*  enumTypeName,
    uint32_t     variantStart,
    uint32_t     variantEnd,
    uint32_t     fieldStart,
    uint32_t     fieldEnd,
    SLTypeRef*   outType);

int ResolveEnumVariantTypeNameNode(
    const SLCBackendC* c,
    int32_t            typeNode,
    const char**       outEnumCName,
    uint32_t*          outVariantStart,
    uint32_t*          outVariantEnd);

int CasePatternParts(
    const SLCBackendC* c, int32_t caseLabelNode, int32_t* outExprNode, int32_t* outAliasNode);

int DecodeEnumVariantPatternExpr(
    const SLCBackendC* c,
    int32_t            exprNode,
    const SLNameMap** _Nullable outEnumMap,
    int32_t* _Nullable outEnumDeclNode,
    int* _Nullable outEnumHasPayload,
    uint32_t* _Nullable outVariantStart,
    uint32_t* _Nullable outVariantEnd);

int ResolvePayloadEnumType(const SLCBackendC* c, const SLTypeRef* t, const char** outEnumName);

int AppendMappedIdentifier(SLCBackendC* c, uint32_t start, uint32_t end);

int EmitTypeNameWithDepth(SLCBackendC* c, const SLTypeRef* type);

int EmitTypeWithName(SLCBackendC* c, int32_t typeNode, const char* name);

int EmitAnonInlineTypeWithName(
    SLCBackendC* c, const char* ownerType, int isUnion, const char* name);

int EmitTypeRefWithName(SLCBackendC* c, const SLTypeRef* t, const char* name);

int EmitTypeForCast(SLCBackendC* c, int32_t typeNode);

void SetPreferredAllocatorPtrType(SLTypeRef* outType);

int IsTypeNodeKind(SLAstKind kind);

int DecodeNewExprNodes(
    SLCBackendC* c,
    int32_t      nodeId,
    int32_t*     outTypeNode,
    int32_t*     outCountNode,
    int32_t*     outInitNode,
    int32_t*     outAllocNode);

uint32_t ListCount(const SLAst* ast, int32_t listNode);

int32_t ListItemAt(const SLAst* ast, int32_t listNode, uint32_t index);

int ResolveVarLikeParts(SLCBackendC* c, int32_t nodeId, SLCCGVarLikeParts* out);

int InferVarLikeDeclType(SLCBackendC* c, int32_t initNode, SLTypeRef* outType);

int FindTopLevelVarLikeNodeBySliceEx(
    const SLCBackendC* c,
    uint32_t           start,
    uint32_t           end,
    int32_t*           outNodeId,
    int32_t* _Nullable outNameIndex);

int FindTopLevelVarLikeNodeBySlice(
    const SLCBackendC* c, uint32_t start, uint32_t end, int32_t* outNodeId);

int InferTopLevelVarLikeType(
    SLCBackendC* c, int32_t nodeId, uint32_t nameStart, uint32_t nameEnd, SLTypeRef* outType);

int ResolveTopLevelConstTypeValueBySlice(
    SLCBackendC* c, uint32_t start, uint32_t end, SLTypeRef* outType);

int TypeRefEqual(const SLTypeRef* a, const SLTypeRef* b);

int ExpandAliasSourceType(const SLCBackendC* c, const SLTypeRef* src, SLTypeRef* outExpanded);

int TypeRefAssignableCost(
    SLCBackendC* c, const SLTypeRef* dst, const SLTypeRef* src, uint8_t* outCost);

int CostVecCmp(const uint8_t* a, const uint8_t* b, uint32_t len);

int ExprNeedsExpectedType(const SLCBackendC* c, int32_t exprNode);

int32_t UnwrapCallArgExprNode(const SLCBackendC* c, int32_t argNode);

int CollectCallArgInfo(
    SLCBackendC*    c,
    int32_t         callNode,
    int32_t         calleeNode,
    int             includeReceiver,
    int32_t         receiverNode,
    SLCCallArgInfo* outArgs,
    SLTypeRef*      outArgTypes,
    uint32_t*       outArgCount);

void GatherCallCandidatesBySlice(
    const SLCBackendC* c,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    const SLFnSig**    outCandidates,
    uint32_t*          outCandidateLen,
    int*               outNameFound);

void GatherCallCandidatesByPkgMethod(
    const SLCBackendC* c,
    uint32_t           pkgStart,
    uint32_t           pkgEnd,
    uint32_t           methodStart,
    uint32_t           methodEnd,
    const SLFnSig**    outCandidates,
    uint32_t*          outCandidateLen,
    int*               outNameFound);

int MapCallArgsToParams(
    const SLCBackendC*    c,
    const SLFnSig*        sig,
    const SLCCallArgInfo* callArgs,
    uint32_t              argCount,
    uint32_t              firstPositionalArgIndex,
    int32_t*              outMappedArgNodes,
    SLTypeRef*            outMappedArgTypes,
    const SLTypeRef*      argTypes);

int PrepareCallBinding(
    const SLCBackendC*    c,
    const SLFnSig*        sig,
    const SLCCallArgInfo* callArgs,
    const int32_t*        argNodes,
    const SLTypeRef*      argTypes,
    uint32_t              argCount,
    uint32_t              firstPositionalArgIndex,
    int                   allowNamedMapping,
    SLCCallBinding*       out);

int ResolveCallTargetFromCandidates(
    SLCBackendC*          c,
    const SLFnSig**       candidates,
    uint32_t              candidateLen,
    int                   nameFound,
    const SLCCallArgInfo* callArgs,
    const int32_t*        argNodes,
    const SLTypeRef*      argTypes,
    uint32_t              argCount,
    uint32_t              firstPositionalArgIndex,
    int                   autoRefFirstArg,
    SLCCallBinding* _Nullable outBinding,
    const SLFnSig** outSig,
    const char**    outCalleeName);

int ResolveCallTarget(
    SLCBackendC*          c,
    uint32_t              nameStart,
    uint32_t              nameEnd,
    const SLCCallArgInfo* callArgs,
    const int32_t*        argNodes,
    const SLTypeRef*      argTypes,
    uint32_t              argCount,
    uint32_t              firstPositionalArgIndex,
    int                   autoRefFirstArg,
    SLCCallBinding* _Nullable outBinding,
    const SLFnSig** outSig,
    const char**    outCalleeName);

int ResolveCallTargetByPkgMethod(
    SLCBackendC*          c,
    uint32_t              pkgStart,
    uint32_t              pkgEnd,
    uint32_t              methodStart,
    uint32_t              methodEnd,
    const SLCCallArgInfo* callArgs,
    const int32_t*        argNodes,
    const SLTypeRef*      argTypes,
    uint32_t              argCount,
    uint32_t              firstPositionalArgIndex,
    int                   autoRefFirstArg,
    SLCCallBinding* _Nullable outBinding,
    const SLFnSig** outSig,
    const char**    outCalleeName);

int InferCompoundLiteralType(
    SLCBackendC* c, int32_t nodeId, const SLTypeRef* _Nullable expectedType, SLTypeRef* outType);

int InferExprTypeExpected(
    SLCBackendC* c, int32_t nodeId, const SLTypeRef* _Nullable expectedType, SLTypeRef* outType);

int InferExprType_IDENT(SLCBackendC* c, int32_t nodeId, const SLAstNode* n, SLTypeRef* outType);

int InferExprType_COMPOUND_LIT(
    SLCBackendC* c, int32_t nodeId, const SLAstNode* n, SLTypeRef* outType);

int InferExprType_CALL_WITH_CONTEXT(
    SLCBackendC* c, int32_t nodeId, const SLAstNode* n, SLTypeRef* outType);

int InferExprType_CALL(SLCBackendC* c, int32_t nodeId, const SLAstNode* n, SLTypeRef* outType);

int InferExprType_NEW(SLCBackendC* c, int32_t nodeId, const SLAstNode* n, SLTypeRef* outType);

int InferExprType_UNARY(SLCBackendC* c, int32_t nodeId, const SLAstNode* n, SLTypeRef* outType);

int InferExprType_FIELD_EXPR(
    SLCBackendC* c, int32_t nodeId, const SLAstNode* n, SLTypeRef* outType);

int InferExprType_INDEX(SLCBackendC* c, int32_t nodeId, const SLAstNode* n, SLTypeRef* outType);

int InferExprType_CAST(SLCBackendC* c, int32_t nodeId, const SLAstNode* n, SLTypeRef* outType);

int InferExprType_SIZEOF(SLCBackendC* c, int32_t nodeId, const SLAstNode* n, SLTypeRef* outType);

int InferExprType_STRING(SLCBackendC* c, int32_t nodeId, const SLAstNode* n, SLTypeRef* outType);

int InferExprType_BOOL(SLCBackendC* c, int32_t nodeId, const SLAstNode* n, SLTypeRef* outType);

int InferExprType_INT(SLCBackendC* c, int32_t nodeId, const SLAstNode* n, SLTypeRef* outType);

int InferExprType_RUNE(SLCBackendC* c, int32_t nodeId, const SLAstNode* n, SLTypeRef* outType);

int InferExprType_FLOAT(SLCBackendC* c, int32_t nodeId, const SLAstNode* n, SLTypeRef* outType);

int InferExprType_NULL(SLCBackendC* c, int32_t nodeId, const SLAstNode* n, SLTypeRef* outType);

int InferExprType_UNWRAP(SLCBackendC* c, int32_t nodeId, const SLAstNode* n, SLTypeRef* outType);

int InferExprType_TUPLE_EXPR(
    SLCBackendC* c, int32_t nodeId, const SLAstNode* n, SLTypeRef* outType);

int InferExprType_CALL_ARG(SLCBackendC* c, int32_t nodeId, const SLAstNode* n, SLTypeRef* outType);

int InferExprType(SLCBackendC* c, int32_t nodeId, SLTypeRef* outType);

int InferNewExprType(SLCBackendC* c, int32_t nodeId, SLTypeRef* outType);

const char* UnaryOpString(SLTokenKind op);

const char* BinaryOpString(SLTokenKind op);

int EmitHexByte(SLBuf* b, uint8_t value);

int BufAppendHexU64Literal(SLBuf* b, uint64_t value);

int EmitStringLiteralRef(SLCBackendC* c, int32_t literalId, int writable);

int EmitStringLiteralPool(SLCBackendC* c);

int IsStrBaseName(const char* _Nullable s);

int TypeRefIsStr(const SLTypeRef* t);

int TypeRefContainerWritable(const SLTypeRef* t);

int EmitElementTypeName(SLCBackendC* c, const SLTypeRef* t, int asConst);

int EmitLenExprFromType(SLCBackendC* c, int32_t exprNode, const SLTypeRef* t);

int EmitElemPtrExpr(
    SLCBackendC* c, int32_t baseNode, const SLTypeRef* baseType, int wantWritableElem);

int EmitSliceExpr(SLCBackendC* c, int32_t nodeId);

int TypeRefIsPointerLike(const SLTypeRef* t);
int TypeRefIsPointerBackedOptional(const SLTypeRef* t);
int TypeRefIsTaggedOptional(const SLTypeRef* t);
int TypeRefLowerForStorage(SLCBackendC* c, const SLTypeRef* type, SLTypeRef* outType);

int TypeRefIsOwnedRuntimeArrayStruct(const SLTypeRef* t);

int TypeRefIsNamedDeclKind(const SLCBackendC* c, const SLTypeRef* t, SLAstKind wantKind);

int TypeRefDerefReadonlyRefLike(const SLTypeRef* in, SLTypeRef* outBase);

int ResolveComparisonHookArgCost(
    SLCBackendC*     c,
    const SLTypeRef* paramType,
    const SLTypeRef* argType,
    uint8_t*         outCost,
    int*             outAutoRef);

int ResolveComparisonHook(
    SLCBackendC*     c,
    const char*      hookName,
    const SLTypeRef* lhsType,
    const SLTypeRef* rhsType,
    const SLFnSig**  outSig,
    const char**     outCalleeName,
    int              outAutoRef[2]);

int EmitExprAutoRefCoerced(SLCBackendC* c, int32_t argNode, const SLTypeRef* paramType);

int EmitExprAsSliceRO(SLCBackendC* c, int32_t exprNode, const SLTypeRef* exprType);

int EmitComparisonHookCall(
    SLCBackendC*   c,
    const SLFnSig* sig,
    const char*    calleeName,
    int32_t        lhsNode,
    int32_t        rhsNode,
    const int      autoRef[2]);

int EmitPointerIdentityExpr(SLCBackendC* c, int32_t exprNode, const SLTypeRef* exprType);

int EmitNewAllocArgExpr(SLCBackendC* c, int32_t allocArg);

const char* _Nullable ResolveVarSizeValueBaseName(SLCBackendC* c, const SLTypeRef* valueType);

int EmitConcatCallExpr(SLCBackendC* c, int32_t calleeNode);

uint32_t FmtNextTempId(SLCBackendC* c);

int TypeRefIsSignedIntegerLike(const SLCBackendC* c, const SLTypeRef* t);

int TypeRefIsIntegerLike(const SLCBackendC* c, const SLTypeRef* t);

int TypeRefIsBoolLike(const SLCBackendC* c, const SLTypeRef* t);

int TypeRefIsNamedEnumLike(const SLCBackendC* c, const SLTypeRef* t);

int TypeRefIsFmtStringLike(const SLCBackendC* c, const SLTypeRef* t);

int TypeRefIsFmtValueType(const SLCBackendC* c, const SLTypeRef* t);

int EmitFmtAppendLiteralBytes(
    SLCBackendC* c, const char* builderName, const uint8_t* bytes, uint32_t len);

int EmitFmtAppendLiteralText(
    SLCBackendC* c, const char* builderName, const char* text, uint32_t len);

int EmitFmtBuilderInitStmt(SLCBackendC* c, const char* builderName, int32_t allocArgNode);

char* _Nullable FmtMakeExprField(SLCBackendC* c, const char* baseExpr, const char* fieldName);

char* _Nullable FmtMakeExprIndex(SLCBackendC* c, const char* baseExpr, const char* indexExpr);

int EmitFmtAppendReflectArray(
    SLCBackendC*     c,
    const char*      builderName,
    const char*      expr,
    const SLTypeRef* type,
    uint32_t         depth);

int EmitFmtAppendReflectSlice(
    SLCBackendC*     c,
    const char*      builderName,
    const char*      expr,
    const SLTypeRef* type,
    uint32_t         depth);

int EmitFmtAppendReflectStruct(
    SLCBackendC*     c,
    const char*      builderName,
    const char*      expr,
    const SLTypeRef* type,
    uint32_t         depth);

int EmitFmtAppendReflectExpr(
    SLCBackendC*     c,
    const char*      builderName,
    const char*      expr,
    const SLTypeRef* type,
    uint32_t         depth);

int EmitExprCoerceFmtValue(
    SLCBackendC* c, int32_t exprNode, const SLTypeRef* srcType, const SLTypeRef* dstType);

int EmitFreeCallExpr(SLCBackendC* c, int32_t allocArgNode, int32_t valueNode);

int EmitNewExpr(
    SLCBackendC* c, int32_t nodeId, const SLTypeRef* _Nullable dstType, int requireNonNull);

int EmitExprCoerced(SLCBackendC* c, int32_t exprNode, const SLTypeRef* dstType);

int32_t ActiveCallOverlayNode(const SLCBackendC* c);

int32_t FindActiveOverlayBindByName(const SLCBackendC* c, const char* fieldName);

int EmitCurrentContextFieldRaw(SLCBackendC* c, const char* fieldName);

int EmitCurrentContextFieldValue(
    SLCBackendC* c, const char* fieldName, const SLTypeRef* requiredType);

int EmitEffectiveContextFieldValue(
    SLCBackendC* c, const char* fieldName, const SLTypeRef* requiredType);

int EmitContextArgForSig(SLCBackendC* c, const SLFnSig* sig);

int EmitResolvedCall(
    SLCBackendC*          c,
    int32_t               callNode,
    const char*           calleeName,
    const SLFnSig*        sig,
    const SLCCallBinding* binding,
    int                   autoRefFirstArg);

int EmitFieldPathLValue(
    SLCBackendC* c, const char* base, const SLFieldInfo* const* path, uint32_t pathLen);

int EmitCompoundFieldValueCoerced(
    SLCBackendC* c, const SLAstNode* field, int32_t exprNode, const SLTypeRef* _Nullable dstType);

int EmitEnumVariantCompoundLiteral(
    SLCBackendC*     c,
    int32_t          nodeId,
    int32_t          firstField,
    const char*      enumTypeName,
    uint32_t         variantStart,
    uint32_t         variantEnd,
    const SLTypeRef* valueType);

int EmitCompoundLiteralDesignated(
    SLCBackendC* c, int32_t firstField, const char* ownerType, const SLTypeRef* valueType);

int EmitCompoundLiteralOrderedStruct(
    SLCBackendC* c, int32_t firstField, const char* ownerType, const SLTypeRef* valueType);

int StructHasFieldDefaults(const SLCBackendC* c, const char* ownerType);

int EmitCompoundLiteral(SLCBackendC* c, int32_t nodeId, const SLTypeRef* _Nullable expectedType);

int EmitExpr_IDENT(SLCBackendC* c, int32_t nodeId, const SLAstNode* n);

int EmitExpr_INT(SLCBackendC* c, int32_t nodeId, const SLAstNode* n);

int EmitExpr_RUNE(SLCBackendC* c, int32_t nodeId, const SLAstNode* n);

int EmitExpr_FLOAT(SLCBackendC* c, int32_t nodeId, const SLAstNode* n);

int EmitExpr_BOOL(SLCBackendC* c, int32_t nodeId, const SLAstNode* n);

int EmitExpr_COMPOUND_LIT(SLCBackendC* c, int32_t nodeId, const SLAstNode* n);

int EmitExpr_STRING(SLCBackendC* c, int32_t nodeId, const SLAstNode* n);

int EmitExpr_UNARY(SLCBackendC* c, int32_t nodeId, const SLAstNode* n);

int EmitExpr_BINARY(SLCBackendC* c, int32_t nodeId, const SLAstNode* n);

int EmitExpr_CALL_WITH_CONTEXT(SLCBackendC* c, int32_t nodeId, const SLAstNode* n);

int EmitExpr_CALL(SLCBackendC* c, int32_t nodeId, const SLAstNode* n);

int EmitExpr_NEW(SLCBackendC* c, int32_t nodeId, const SLAstNode* n);

int EmitExpr_INDEX(SLCBackendC* c, int32_t nodeId, const SLAstNode* n);

int EmitExpr_FIELD_EXPR(SLCBackendC* c, int32_t nodeId, const SLAstNode* n);

int EmitExpr_CAST(SLCBackendC* c, int32_t nodeId, const SLAstNode* n);

int EmitExpr_SIZEOF(SLCBackendC* c, int32_t nodeId, const SLAstNode* n);

int EmitExpr_NULL(SLCBackendC* c, int32_t nodeId, const SLAstNode* n);

int EmitExpr_UNWRAP(SLCBackendC* c, int32_t nodeId, const SLAstNode* n);

int EmitExpr_CALL_ARG(SLCBackendC* c, int32_t nodeId, const SLAstNode* n);

int EmitExpr_TUPLE_EXPR(SLCBackendC* c, int32_t nodeId, const SLAstNode* n);

int EmitExpr(SLCBackendC* c, int32_t nodeId);

int EmitDeferredRange(SLCBackendC* c, uint32_t start, uint32_t depth);

int EmitBlockImpl(SLCBackendC* c, int32_t nodeId, uint32_t depth, int inlineOpen);

int EmitBlock(SLCBackendC* c, int32_t nodeId, uint32_t depth);

int EmitBlockInline(SLCBackendC* c, int32_t nodeId, uint32_t depth);

int EmitVarLikeStmt(SLCBackendC* c, int32_t nodeId, uint32_t depth, int isConst);

int EmitMultiAssignStmt(SLCBackendC* c, int32_t nodeId, uint32_t depth);

int EmitForStmt(SLCBackendC* c, int32_t nodeId, uint32_t depth);

int EmitSwitchStmt(SLCBackendC* c, int32_t nodeId, uint32_t depth);

int EmitAssertFormatArg(SLCBackendC* c, int32_t nodeId);

int EmitStmt(SLCBackendC* c, int32_t nodeId, uint32_t depth);

int IsMainFunctionNode(const SLCBackendC* c, int32_t nodeId);

int IsExplicitlyExportedNode(const SLCBackendC* c, int32_t nodeId);

int IsExportedNode(const SLCBackendC* c, int32_t nodeId);

int IsExportedTypeNode(const SLCBackendC* c, int32_t nodeId);

int EmitEnumDecl(SLCBackendC* c, int32_t nodeId, uint32_t depth);

int NodeHasDirectDependentFields(SLCBackendC* c, int32_t nodeId);

int EmitVarSizeStructDecl(SLCBackendC* c, int32_t nodeId, uint32_t depth);

int EmitStructOrUnionDecl(SLCBackendC* c, int32_t nodeId, uint32_t depth, int isUnion);

int EmitForwardTypeDecls(SLCBackendC* c);

int EmitForwardAnonTypeDecls(SLCBackendC* c);

int EmitAnonTypeDecls(SLCBackendC* c);

int EmitHeaderTypeAliasDecls(SLCBackendC* c);

int EmitFnTypeAliasDecls(SLCBackendC* c);

int FnNodeHasBody(const SLCBackendC* c, int32_t nodeId);

int HasFunctionBodyForName(const SLCBackendC* c, int32_t nodeId);

int EmitFnDeclOrDef(
    SLCBackendC* c,
    int32_t      nodeId,
    uint32_t     depth,
    int          emitBody,
    int          isPrivate,
    const SLFnSig* _Nullable forcedSig);

int EmitConstDecl(
    SLCBackendC* c, int32_t nodeId, uint32_t depth, int declarationOnly, int isPrivate);

int EmitVarDecl(SLCBackendC* c, int32_t nodeId, uint32_t depth, int declarationOnly, int isPrivate);

int EmitTypeAliasDecl(
    SLCBackendC* c, int32_t nodeId, uint32_t depth, int declarationOnly, int isPrivate);

int EmitDeclNode(
    SLCBackendC* c,
    int32_t      nodeId,
    uint32_t     depth,
    int          declarationOnly,
    int          isPrivate,
    int          emitBody);

int EmitPrelude(SLCBackendC* c);

char* _Nullable BuildDefaultMacro(SLCBackendC* c, const char* pkgName, const char* suffix);

int EmitHeader(SLCBackendC* c);

int ShouldEmitDeclNode(const SLCBackendC* c, int32_t nodeId);

int InitAst(SLCBackendC* c);

char* _Nullable AllocOutputCopy(SLCBackendC* c);

void FreeContext(SLCBackendC* c);

int EmitCBackend(
    const SLCodegenBackend* backend,
    const SLCodegenUnit*    unit,
    const SLCodegenOptions* _Nullable options,
    char** outHeader,
    SLDiag* _Nullable diag);

SL_API_END
