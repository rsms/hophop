#pragma once

#include "../codegen.h"
#include "../libhop-impl.h"

HOP_API_BEGIN
typedef struct {
    const char* _Nullable baseName;
    int      ptrDepth;
    int      valid;
    int      containerKind; /* 0 scalar, 1 array, 2 ro-slice, 3 rw-slice */
    int      containerPtrDepth;
    uint32_t arrayLen;
    int      hasArrayLen;
    int      readOnly;
    int      isOptional;
} HOPTypeRef;

typedef struct {
    HOPArena* _Nullable arena;
    char* _Nullable v;
    uint32_t len;
    uint32_t cap;
} HOPBuf;

typedef struct {
    char*      name;
    char*      cName;
    HOPAstKind kind;
    int        isExported;
} HOPNameMap;

typedef struct {
    int32_t nodeId;
} HOPNodeRef;

typedef struct {
    char*      hopName;
    char*      cName;
    int32_t    nodeId;
    uint32_t   tcFuncIndex;
    HOPTypeRef returnType;
    HOPTypeRef* _Nullable paramTypes;
    char** _Nullable paramNames;
    uint8_t* _Nullable paramFlags;
    uint32_t paramLen;
    uint32_t packArgStart;
    uint32_t packArgCount;
    char* _Nullable packParamName;
    uint16_t   flags;
    HOPTypeRef contextType;
    int        hasContext;
    uint8_t    isVariadic;
    uint8_t    _reserved[1];
} HOPFnSig;

enum {
    HOPFnSigFlag_TEMPLATE_BASE = 1u << 0,
    HOPFnSigFlag_TEMPLATE_INSTANCE = 1u << 1,
    HOPFnSigFlag_EXPANDED_ANYPACK = 1u << 2,
};

typedef struct {
    char*      aliasName;
    HOPTypeRef returnType;
    HOPTypeRef* _Nullable paramTypes;
    uint32_t paramLen;
    uint8_t  isVariadic;
    uint8_t  _reserved[3];
} HOPFnTypeAlias;

typedef struct {
    char*      aliasName;
    HOPTypeRef targetType;
} HOPTypeAliasInfo;

typedef struct {
    char* _Nullable ownerType;
    char* _Nullable fieldName;
    char* _Nullable lenFieldName;
    int        isDependent;
    int        isEmbedded;
    int32_t    defaultExprNode;
    HOPTypeRef type;
} HOPFieldInfo;

typedef struct {
    char* cName;
    int   isUnion;
    int   isVarSize;
} HOPVarSizeType;

typedef struct {
    char*    key;
    char*    cName;
    int      isUnion;
    uint32_t fieldStart;
    uint16_t fieldCount;
    uint16_t flags;
} HOPAnonTypeInfo;

enum {
    HOPAnonTypeFlag_EMITTED_GLOBAL = 1u << 0,
};

typedef struct {
    char*      name;
    HOPTypeRef type;
} HOPLocal;

typedef struct {
    int32_t  localIdx;
    char*    enumTypeName;
    uint32_t variantStart;
    uint32_t variantEnd;
} HOPVariantNarrow;

typedef struct {
    int32_t nodeId;
    char*   cName;
} HOPFnNodeName;

typedef struct {
    uint8_t* _Nullable bytes;
    uint32_t len;
} HOPStringLiteral;

typedef struct {
    const HOPCodegenUnit*    unit;
    const HOPCodegenOptions* options;
    HOPDiag* _Nullable diag;

    HOPArena arena;
    uint8_t  arenaInlineStorage[16384];
    HOPAst   ast;

    HOPBuf out;

    HOPNameMap* names;
    uint32_t    nameLen;
    uint32_t    nameCap;

    HOPNodeRef* pubDecls;
    uint32_t    pubDeclLen;
    uint32_t    pubDeclCap;

    HOPNodeRef* topDecls;
    uint32_t    topDeclLen;
    uint32_t    topDeclCap;

    HOPFnSig* fnSigs;
    uint32_t  fnSigLen;
    uint32_t  fnSigCap;

    HOPFnTypeAlias* fnTypeAliases;
    uint32_t        fnTypeAliasLen;
    uint32_t        fnTypeAliasCap;

    HOPTypeAliasInfo* typeAliases;
    uint32_t          typeAliasLen;
    uint32_t          typeAliasCap;

    HOPFieldInfo* fieldInfos;
    uint32_t      fieldInfoLen;
    uint32_t      fieldInfoCap;

    HOPVarSizeType* varSizeTypes;
    uint32_t        varSizeTypeLen;
    uint32_t        varSizeTypeCap;

    HOPAnonTypeInfo* anonTypes;
    uint32_t         anonTypeLen;
    uint32_t         anonTypeCap;

    HOPLocal* locals;
    uint32_t  localLen;
    uint32_t  localCap;

    HOPVariantNarrow* variantNarrows;
    uint32_t          variantNarrowLen;
    uint32_t          variantNarrowCap;
    int32_t           activeOptionalNarrowLocalIdx;
    uint8_t           hasActiveOptionalNarrow;
    uint8_t           _reserved_optional_narrow[3];
    HOPTypeRef        activeOptionalNarrowStorageType;

    HOPFnNodeName* fnNodeNames;
    uint32_t       fnNodeNameLen;
    uint32_t       fnNodeNameCap;

    uint32_t* localScopeMarks;
    uint32_t  localScopeLen;
    uint32_t  localScopeCap;

    uint8_t*  contextCowScopeActive;
    uint32_t* contextCowScopeTempIds;
    uint32_t  contextCowScopeActiveCap;
    uint32_t  contextCowScopeTempCap;

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

    HOPStringLiteral* stringLits;
    uint32_t          stringLitLen;
    uint32_t          stringLitCap;

    int32_t* stringLitByNode;
    uint32_t stringLitByNodeLen;

    int        emitPrivateFnDeclStatic;
    HOPTypeRef currentReturnType;
    int        hasCurrentReturnType;
    HOPTypeRef currentContextType;
    int        hasCurrentContext;
    int        currentFunctionIsMain;
    int32_t    activeCallWithNode;
    const char* _Nullable activePackParamName;
    char** _Nullable activePackElemNames;
    HOPTypeRef* _Nullable activePackElemTypes;
    uint32_t activePackElemCount;
    uint32_t fmtTempCounter;
    HOPConstEvalSession* _Nullable constEval;
    uint32_t activeTcFuncIndex;
    int32_t  activeTcNamedTypeIndex;
} HOPCBackendC;

enum {
    HOPTypeContainer_SCALAR = 0,
    HOPTypeContainer_ARRAY = 1,
    HOPTypeContainer_SLICE_RO = 2,
    HOPTypeContainer_SLICE_MUT = 3,
};

typedef struct {
    int32_t  nameListNode;
    int32_t  typeNode;
    int32_t  initNode;
    uint32_t nameCount;
    uint8_t  grouped;
} HOPCCGVarLikeParts;

#define HOPCCG_MAX_CALL_ARGS       128u
#define HOPCCG_MAX_CALL_CANDIDATES 256u

typedef struct {
    int32_t  argNode;
    int32_t  exprNode;
    uint32_t explicitNameStart;
    uint32_t explicitNameEnd;
    uint32_t implicitNameStart;
    uint32_t implicitNameEnd;
    uint8_t  spread;
    uint8_t  _reserved[3];
} HOPCCallArgInfo;

typedef struct {
    int        isVariadic;
    uint32_t   fixedCount;
    uint32_t   fixedInputCount;
    uint32_t   spreadArgIndex;
    int32_t    fixedMappedArgNodes[HOPCCG_MAX_CALL_ARGS];
    int32_t    explicitTailNodes[HOPCCG_MAX_CALL_ARGS];
    int32_t    argParamIndices[HOPCCG_MAX_CALL_ARGS];
    uint32_t   explicitTailCount;
    HOPTypeRef argExpectedTypes[HOPCCG_MAX_CALL_ARGS];
} HOPCCallBinding;

enum {
    HOPCCGParamFlag_CONST = 1u << 0,
    HOPCCGParamFlag_ANYTYPE = 1u << 1,
    HOPCCGParamFlag_ANYPACK = 1u << 2,
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

void SetDiag(HOPDiag* _Nullable diag, HOPDiagCode code, uint32_t start, uint32_t end);

int EnsureCapArena(
    HOPArena* arena, void** ptr, uint32_t* cap, uint32_t need, size_t elemSize, uint32_t align);

int BufReserve(HOPBuf* b, uint32_t extra);

int BufAppend(HOPBuf* b, const char* s, uint32_t len);

int BufAppendCStr(HOPBuf* b, const char* s);

int BufAppendChar(HOPBuf* b, char c);

int BufAppendU32(HOPBuf* b, uint32_t value);

int BufAppendSlice(HOPBuf* b, const char* src, uint32_t start, uint32_t end);

char* _Nullable BufFinish(HOPBuf* b);

void EmitIndent(HOPCBackendC* c, uint32_t depth);

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

void TypeRefSetInvalid(HOPTypeRef* t);

void TypeRefSetScalar(HOPTypeRef* t, const char* baseName);

void CanonicalizeTypeRefBaseName(const HOPCBackendC* c, HOPTypeRef* t);

int SliceStructPtrDepth(const HOPTypeRef* t);

int ParseArrayLenLiteral(const char* src, uint32_t start, uint32_t end, uint32_t* outLen);

void SetDiagNode(HOPCBackendC* c, int32_t nodeId, HOPDiagCode code);

int BufAppendI64(HOPBuf* b, int64_t value);

int EvalConstIntExpr(HOPCBackendC* c, int32_t nodeId, int64_t* outValue, int* outIsConst);
int EvalConstFloatExpr(HOPCBackendC* c, int32_t nodeId, double* outValue, int* outIsConst);

int ConstIntFitsIntegerType(const char* typeName, int64_t value);
int ConstIntFitsFloatType(const char* typeName, int64_t value);
int ConstFloatFitsFloatType(const char* typeName, double value);

int EmitConstEvaluatedScalar(
    HOPCBackendC* c, const HOPTypeRef* dstType, const HOPCTFEValue* value, int* outEmitted);

char* _Nullable DupSlice(HOPCBackendC* c, const char* src, uint32_t start, uint32_t end);

int SliceIsHoleName(const char* src, uint32_t start, uint32_t end);

char* _Nullable DupParamNameForEmit(
    HOPCBackendC* c, const HOPAstNode* paramNode, uint32_t paramIndex);

char* _Nullable DupAndReplaceDots(HOPCBackendC* c, const char* src, uint32_t start, uint32_t end);

char* _Nullable DupCStr(HOPCBackendC* c, const char* s);

int DecodeStringLiteralNode(
    HOPCBackendC* c, const HOPAstNode* n, uint8_t** outBytes, uint32_t* outLen);

int AppendDecodedStringExpr(
    HOPCBackendC* c, int32_t nodeId, uint8_t** bytes, uint32_t* len, uint32_t* cap);

int DecodeStringExpr(
    HOPCBackendC* c,
    int32_t       nodeId,
    uint8_t**     outBytes,
    uint32_t*     outLen,
    uint32_t*     outStart,
    uint32_t*     outEnd);

int GetOrAddStringLiteralExpr(HOPCBackendC* c, int32_t nodeId, int32_t* outLiteralId);

int GetOrAddStringLiteralBytes(
    HOPCBackendC* c, const uint8_t* bytes, uint32_t len, int32_t* outLiteralId);

int CollectStringLiterals(HOPCBackendC* c);

int HasDoubleUnderscore(const char* s);

int IsTypeDeclKind(HOPAstKind kind);

int IsDeclKind(HOPAstKind kind);

int IsPubDeclNode(const HOPAstNode* n);

int32_t AstFirstChild(const HOPAst* ast, int32_t nodeId);

int32_t AstNextSibling(const HOPAst* ast, int32_t nodeId);

const HOPAstNode* _Nullable NodeAt(const HOPCBackendC* c, int32_t nodeId);

int GetDeclNameSpan(const HOPCBackendC* c, int32_t nodeId, uint32_t* outStart, uint32_t* outEnd);

int AddName(HOPCBackendC* c, uint32_t nameStart, uint32_t nameEnd, HOPAstKind kind, int isExported);
int BuildTypeDeclFlatName(HOPCBackendC* c, int32_t nodeId, char** outName);

const HOPNameMap* _Nullable FindNameBySlice(const HOPCBackendC* c, uint32_t start, uint32_t end);
const HOPNameMap* _Nullable FindTypeDeclMapByNode(HOPCBackendC* c, int32_t nodeId);

const HOPNameMap* _Nullable FindNameByCString(const HOPCBackendC* c, const char* name);

int NameHasPrefixSuffix(const char* name, const char* prefix, const char* suffix);

int ResolveMainSemanticContextType(HOPCBackendC* c, HOPTypeRef* outType);

const char* ResolveRuneTypeBaseName(HOPCBackendC* c);

const HOPNameMap* _Nullable FindNameByCName(const HOPCBackendC* c, const char* cName);

int ResolveTypeValueNameExprTypeRef(
    HOPCBackendC* c, uint32_t start, uint32_t end, HOPTypeRef* outTypeRef);

uint64_t TypeTagHashAddByte(uint64_t h, uint8_t b);

uint64_t TypeTagHashAddU32(uint64_t h, uint32_t v);

uint64_t TypeTagHashAddStr(uint64_t h, const char* s);

uint8_t TypeTagKindFromTypeRef(const HOPCBackendC* c, const HOPTypeRef* t);

uint64_t TypeTagFromTypeRef(const HOPCBackendC* c, const HOPTypeRef* t);

int EmitTypeTagLiteralFromTypeRef(HOPCBackendC* c, const HOPTypeRef* t);

const HOPTypeAliasInfo* _Nullable FindTypeAliasInfoByAliasName(
    const HOPCBackendC* c, const char* aliasName);

int AddTypeAliasInfo(HOPCBackendC* c, const char* aliasName, HOPTypeRef targetType);

const char* ResolveScalarAliasBaseName(const HOPCBackendC* c, const char* typeName);

int ResolveReflectedTypeValueExprTypeRef(HOPCBackendC* c, int32_t exprNode, HOPTypeRef* outTypeRef);

int TypeRefIsTypeValue(const HOPTypeRef* t);

const char* _Nullable FindReflectKindTypeName(const HOPCBackendC* c);

const char* TypeRefDisplayBaseName(const HOPCBackendC* c, const char* baseName);

int EmitTypeNameStringLiteralFromTypeRef(HOPCBackendC* c, const HOPTypeRef* _Nullable t);

int EmitTypeTagKindLiteralFromTypeRef(HOPCBackendC* c, const HOPTypeRef* t);

int EmitRuntimeTypeTagKindFromExpr(HOPCBackendC* c, int32_t exprNode);

int EmitTypeTagIsAliasLiteralFromTypeRef(HOPCBackendC* c, const HOPTypeRef* t);

int EmitRuntimeTypeTagIsAliasFromExpr(HOPCBackendC* c, int32_t exprNode);

int EmitRuntimeTypeTagCtorUnary(HOPCBackendC* c, uint32_t kindTag, uint64_t salt, int32_t argNode);

int EmitRuntimeTypeTagCtorArray(HOPCBackendC* c, int32_t elemTagNode, int32_t lenNode);

int EmitTypeTagBaseLiteralFromTypeRef(HOPCBackendC* c, const HOPTypeRef* t);

int TypeRefIsRuneLike(const HOPCBackendC* c, const HOPTypeRef* typeRef);

const char* _Nullable ResolveTypeName(HOPCBackendC* c, uint32_t start, uint32_t end);

void NormalizeCoreRuntimeTypeName(HOPTypeRef* outType);

const char* _Nullable ConstEvalBuiltinCName(HOPConstEvalBuiltinKind builtin);

int ParseTypeRefFromConstEvalTypeId(HOPCBackendC* c, int32_t typeId, HOPTypeRef* outType);

int ParseTypeRefFromConstEvalTypeTag(HOPCBackendC* c, uint64_t typeTag, HOPTypeRef* outType);

char* _Nullable BuildTemplateNamedTypeCName(
    HOPCBackendC* c, const char* baseCName, uint32_t tcNamedIndex);

int CollectTemplateInstanceNamedTypes(HOPCBackendC* c);

int CodegenCNodeHasTypeParams(const HOPCBackendC* c, int32_t nodeId);

int CodegenCPushActiveFunctionTypeContext(HOPCBackendC* c, uint32_t tcFuncIndex);

int CodegenCPushActiveNamedTypeContext(HOPCBackendC* c, uint32_t tcNamedIndex);

void CodegenCPopActiveTypeContext(
    HOPCBackendC* c,
    uint32_t      savedFuncIndex,
    int32_t       savedNamedTypeIndex,
    uint32_t      savedArgStart,
    uint16_t      savedArgCount,
    int32_t       savedDeclNode);

int AddNodeRef(HOPCBackendC* c, HOPNodeRef** arr, uint32_t* len, uint32_t* cap, int32_t nodeId);

int CollectDeclSets(HOPCBackendC* c);

const HOPFnTypeAlias* _Nullable FindFnTypeAliasByName(const HOPCBackendC* c, const char* name);

int EnsureFnTypeAlias(
    HOPCBackendC* c,
    HOPTypeRef    returnType,
    HOPTypeRef* _Nullable paramTypes,
    uint32_t     paramLen,
    int          isVariadic,
    const char** outAliasName);

const char* _Nullable TupleFieldName(HOPCBackendC* c, uint32_t index);

int ParseTypeRef(HOPCBackendC* c, int32_t nodeId, HOPTypeRef* outType);

int AddFnSig(
    HOPCBackendC* c,
    const char*   hopName,
    const char*   baseCName,
    int32_t       nodeId,
    HOPTypeRef    returnType,
    HOPTypeRef* _Nullable paramTypes,
    char** _Nullable paramNames,
    uint8_t* _Nullable paramFlags,
    uint32_t   paramLen,
    int        isVariadic,
    int        hasContext,
    HOPTypeRef contextType,
    uint16_t   sigFlags,
    uint32_t   tcFuncIndex,
    uint32_t   packArgStart,
    uint32_t   packArgCount,
    char* _Nullable packParamName);

int AddFieldInfo(
    HOPCBackendC* c,
    const char*   ownerType,
    const char*   fieldName,
    const char* _Nullable lenFieldName,
    int32_t    defaultExprNode,
    int        isDependent,
    int        isEmbedded,
    HOPTypeRef type);

int AppendTypeRefKey(HOPBuf* b, const HOPTypeRef* t);

const HOPAnonTypeInfo* _Nullable FindAnonTypeByKey(const HOPCBackendC* c, const char* key);

const HOPAnonTypeInfo* _Nullable FindAnonTypeByCName(const HOPCBackendC* c, const char* cName);

int IsTupleFieldName(const char* name, uint32_t index);

int TypeRefTupleInfo(const HOPCBackendC* c, const HOPTypeRef* t, const HOPAnonTypeInfo** outInfo);

int IsLocalAnonTypedefVisible(const HOPCBackendC* c, const char* cName);

int MarkLocalAnonTypedefVisible(HOPCBackendC* c, const char* cName);

int IsAnonTypeNameVisible(const HOPCBackendC* c, const char* cName);

int EmitAnonTypeDeclAtDepth(HOPCBackendC* c, const HOPAnonTypeInfo* t, uint32_t depth);

int EnsureAnonTypeVisible(HOPCBackendC* c, const HOPTypeRef* type, uint32_t depth);

int EnsureAnonTypeByFields(
    HOPCBackendC*     c,
    int               isUnion,
    const char**      fieldNames,
    const HOPTypeRef* fieldTypes,
    uint32_t          fieldCount,
    const char**      outCName);

const HOPFnSig* _Nullable FindFnSigBySlice(const HOPCBackendC* c, uint32_t start, uint32_t end);

uint32_t FindFnSigCandidatesBySlice(
    const HOPCBackendC* c, uint32_t start, uint32_t end, const HOPFnSig** out, uint32_t cap);

uint32_t FindFnSigCandidatesByName(
    const HOPCBackendC* c, const char* hopName, const HOPFnSig** out, uint32_t cap);

const char* _Nullable FindFnCNameByNodeId(const HOPCBackendC* c, int32_t nodeId);

const HOPFnSig* _Nullable FindFnSigByNodeId(const HOPCBackendC* c, int32_t nodeId);
uint32_t FindFnSigCandidatesByNodeId(
    const HOPCBackendC* c, int32_t nodeId, const HOPFnSig** out, uint32_t cap);

const HOPFieldInfo* _Nullable FindFieldInfo(
    const HOPCBackendC* c, const char* ownerType, uint32_t fieldStart, uint32_t fieldEnd);

const HOPFieldInfo* _Nullable FindEmbeddedFieldInfo(const HOPCBackendC* c, const char* ownerType);

const HOPFieldInfo* _Nullable FindFieldInfoByName(
    const HOPCBackendC* c, const char* ownerType, const char* fieldName);

const char* _Nullable CanonicalFieldOwnerType(
    const HOPCBackendC* c, const char* _Nullable ownerType);

int ResolveCoreStrFieldBySlice(
    const HOPCBackendC* c, uint32_t fieldStart, uint32_t fieldEnd, const HOPFieldInfo** outField);

int ResolveFieldPathSingleSegment(
    const HOPCBackendC*  c,
    const char*          ownerTypeIn,
    uint32_t             fieldStart,
    uint32_t             fieldEnd,
    const HOPFieldInfo** outPath,
    uint32_t             cap,
    uint32_t*            outLen,
    const HOPFieldInfo** _Nullable outField);

int FieldPathNextSegment(
    const char* src, uint32_t pathEnd, uint32_t* ioPos, uint32_t* outSegStart, uint32_t* outSegEnd);

int ResolveFieldPathBySlice(
    const HOPCBackendC*  c,
    const char*          ownerType,
    uint32_t             fieldStart,
    uint32_t             fieldEnd,
    const HOPFieldInfo** outPath,
    uint32_t             cap,
    uint32_t*            outLen,
    const HOPFieldInfo** _Nullable outField);

int ResolveEmbeddedPathByNames(
    const HOPCBackendC*  c,
    const char*          srcTypeName,
    const char*          dstTypeName,
    const HOPFieldInfo** outPath,
    uint32_t             cap,
    uint32_t*            outLen);

int CollectFnAndFieldInfoFromNode(HOPCBackendC* c, int32_t nodeId);

int CollectFnAndFieldInfo(HOPCBackendC* c);

int CollectTypeAliasInfo(HOPCBackendC* c);

int CollectFnTypeAliasesFromNode(HOPCBackendC* c, int32_t nodeId);

int CollectFnTypeAliases(HOPCBackendC* c);

int AddVarSizeType(HOPCBackendC* c, const char* cName, int isUnion);

HOPVarSizeType* _Nullable FindVarSizeType(HOPCBackendC* c, const char* cName);

int CollectVarSizeTypesFromDeclSets(HOPCBackendC* c);

int IsVarSizeTypeName(const HOPCBackendC* c, const char* cName);

int PropagateVarSizeTypes(HOPCBackendC* c);

int PushScope(HOPCBackendC* c);

void PopScope(HOPCBackendC* c);

int PushDeferScope(HOPCBackendC* c);

void PopDeferScope(HOPCBackendC* c);

int AddDeferredStmt(HOPCBackendC* c, int32_t stmtNodeId);

int AddLocal(HOPCBackendC* c, const char* name, HOPTypeRef type);

void TrimVariantNarrowsToLocalLen(HOPCBackendC* c);

int AddVariantNarrow(
    HOPCBackendC* c,
    int32_t       localIdx,
    const char*   enumTypeName,
    uint32_t      variantStart,
    uint32_t      variantEnd);

int32_t FindLocalIndexBySlice(const HOPCBackendC* c, uint32_t start, uint32_t end);

const HOPVariantNarrow* _Nullable FindVariantNarrowByLocalIdx(
    const HOPCBackendC* c, int32_t localIdx);

const HOPLocal* _Nullable FindLocalBySlice(const HOPCBackendC* c, uint32_t start, uint32_t end);

int FindEnumDeclNodeBySlice(
    const HOPCBackendC* c, uint32_t start, uint32_t end, int32_t* outNodeId);

int EnumDeclHasMemberBySlice(
    const HOPCBackendC* c, int32_t enumNodeId, uint32_t memberStart, uint32_t memberEnd);

int ResolveEnumSelectorByFieldExpr(
    const HOPCBackendC* c,
    int32_t             fieldExprNode,
    const HOPNameMap** _Nullable outEnumMap,
    int32_t* _Nullable outEnumDeclNode,
    int* _Nullable outEnumHasPayload,
    uint32_t* _Nullable outVariantStart,
    uint32_t* _Nullable outVariantEnd);

int EnumDeclHasPayload(const HOPCBackendC* c, int32_t enumNodeId);

int32_t EnumVariantTagExprNode(const HOPCBackendC* c, int32_t variantNode);

int FindEnumDeclNodeByCName(const HOPCBackendC* c, const char* enumCName, int32_t* outNodeId);

int FindEnumVariantNodeBySlice(
    const HOPCBackendC* c,
    int32_t             enumNodeId,
    uint32_t            variantStart,
    uint32_t            variantEnd,
    int32_t*            outVariantNode);

int ResolveEnumVariantPayloadFieldType(
    HOPCBackendC* c,
    const char*   enumTypeName,
    uint32_t      variantStart,
    uint32_t      variantEnd,
    uint32_t      fieldStart,
    uint32_t      fieldEnd,
    HOPTypeRef*   outType);

int ResolveEnumVariantTypeNameNode(
    const HOPCBackendC* c,
    int32_t             typeNode,
    const char**        outEnumCName,
    uint32_t*           outVariantStart,
    uint32_t*           outVariantEnd);

int CasePatternParts(
    const HOPCBackendC* c, int32_t caseLabelNode, int32_t* outExprNode, int32_t* outAliasNode);

int DecodeEnumVariantPatternExpr(
    const HOPCBackendC* c,
    int32_t             exprNode,
    const HOPNameMap** _Nullable outEnumMap,
    int32_t* _Nullable outEnumDeclNode,
    int* _Nullable outEnumHasPayload,
    uint32_t* _Nullable outVariantStart,
    uint32_t* _Nullable outVariantEnd);

int ResolvePayloadEnumType(const HOPCBackendC* c, const HOPTypeRef* t, const char** outEnumName);

int AppendMappedIdentifier(HOPCBackendC* c, uint32_t start, uint32_t end);

int EmitTypeNameWithDepth(HOPCBackendC* c, const HOPTypeRef* type);

int EmitTypeWithName(HOPCBackendC* c, int32_t typeNode, const char* name);

int EmitAnonInlineTypeWithName(
    HOPCBackendC* c, const char* ownerType, int isUnion, const char* name);

int EmitTypeRefWithName(HOPCBackendC* c, const HOPTypeRef* t, const char* name);

int EmitTypeForCast(HOPCBackendC* c, int32_t typeNode);

void SetPreferredAllocatorPtrType(HOPTypeRef* outType);

int IsTypeNodeKind(HOPAstKind kind);

int DecodeNewExprNodes(
    HOPCBackendC* c,
    int32_t       nodeId,
    int32_t*      outTypeNode,
    int32_t*      outCountNode,
    int32_t*      outInitNode,
    int32_t*      outAllocNode);

uint32_t ListCount(const HOPAst* ast, int32_t listNode);

int32_t ListItemAt(const HOPAst* ast, int32_t listNode, uint32_t index);

int ResolveVarLikeParts(HOPCBackendC* c, int32_t nodeId, HOPCCGVarLikeParts* out);

int InferVarLikeDeclType(HOPCBackendC* c, int32_t initNode, HOPTypeRef* outType);

int FindTopLevelVarLikeNodeBySliceEx(
    const HOPCBackendC* c,
    uint32_t            start,
    uint32_t            end,
    int32_t*            outNodeId,
    int32_t* _Nullable outNameIndex);

int FindTopLevelVarLikeNodeBySlice(
    const HOPCBackendC* c, uint32_t start, uint32_t end, int32_t* outNodeId);

int InferTopLevelVarLikeType(
    HOPCBackendC* c, int32_t nodeId, uint32_t nameStart, uint32_t nameEnd, HOPTypeRef* outType);

int ResolveTopLevelConstTypeValueBySlice(
    HOPCBackendC* c, uint32_t start, uint32_t end, HOPTypeRef* outType);

int TypeRefEqual(const HOPTypeRef* a, const HOPTypeRef* b);

int ExpandAliasSourceType(const HOPCBackendC* c, const HOPTypeRef* src, HOPTypeRef* outExpanded);

int TypeRefAssignableCost(
    HOPCBackendC* c, const HOPTypeRef* dst, const HOPTypeRef* src, uint8_t* outCost);

int CostVecCmp(const uint8_t* a, const uint8_t* b, uint32_t len);

int ExprNeedsExpectedType(const HOPCBackendC* c, int32_t exprNode);

int32_t UnwrapCallArgExprNode(const HOPCBackendC* c, int32_t argNode);

int CollectCallArgInfo(
    HOPCBackendC*    c,
    int32_t          callNode,
    int32_t          calleeNode,
    int              includeReceiver,
    int32_t          receiverNode,
    HOPCCallArgInfo* outArgs,
    HOPTypeRef*      outArgTypes,
    uint32_t*        outArgCount);

void GatherCallCandidatesBySlice(
    const HOPCBackendC* c,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    const HOPFnSig**    outCandidates,
    uint32_t*           outCandidateLen,
    int*                outNameFound);

void GatherCallCandidatesByPkgMethod(
    const HOPCBackendC* c,
    uint32_t            pkgStart,
    uint32_t            pkgEnd,
    uint32_t            methodStart,
    uint32_t            methodEnd,
    const HOPFnSig**    outCandidates,
    uint32_t*           outCandidateLen,
    int*                outNameFound);

int MapCallArgsToParams(
    const HOPCBackendC*    c,
    const HOPFnSig*        sig,
    const HOPCCallArgInfo* callArgs,
    uint32_t               argCount,
    uint32_t               firstPositionalArgIndex,
    int32_t*               outMappedArgNodes,
    HOPTypeRef*            outMappedArgTypes,
    const HOPTypeRef*      argTypes);

int PrepareCallBinding(
    const HOPCBackendC*    c,
    const HOPFnSig*        sig,
    const HOPCCallArgInfo* callArgs,
    const int32_t*         argNodes,
    const HOPTypeRef*      argTypes,
    uint32_t               argCount,
    uint32_t               firstPositionalArgIndex,
    int                    allowNamedMapping,
    HOPCCallBinding*       out);

int ResolveCallTargetFromCandidates(
    HOPCBackendC*          c,
    const HOPFnSig**       candidates,
    uint32_t               candidateLen,
    int                    nameFound,
    const HOPCCallArgInfo* callArgs,
    const int32_t*         argNodes,
    const HOPTypeRef*      argTypes,
    uint32_t               argCount,
    uint32_t               firstPositionalArgIndex,
    int                    autoRefFirstArg,
    HOPCCallBinding* _Nullable outBinding,
    const HOPFnSig** outSig,
    const char**     outCalleeName);

int ResolveCallTarget(
    HOPCBackendC*          c,
    uint32_t               nameStart,
    uint32_t               nameEnd,
    const HOPCCallArgInfo* callArgs,
    const int32_t*         argNodes,
    const HOPTypeRef*      argTypes,
    uint32_t               argCount,
    uint32_t               firstPositionalArgIndex,
    int                    autoRefFirstArg,
    HOPCCallBinding* _Nullable outBinding,
    const HOPFnSig** outSig,
    const char**     outCalleeName);

int ResolveCallTargetByPkgMethod(
    HOPCBackendC*          c,
    uint32_t               pkgStart,
    uint32_t               pkgEnd,
    uint32_t               methodStart,
    uint32_t               methodEnd,
    const HOPCCallArgInfo* callArgs,
    const int32_t*         argNodes,
    const HOPTypeRef*      argTypes,
    uint32_t               argCount,
    uint32_t               firstPositionalArgIndex,
    int                    autoRefFirstArg,
    HOPCCallBinding* _Nullable outBinding,
    const HOPFnSig** outSig,
    const char**     outCalleeName);

int InferCompoundLiteralType(
    HOPCBackendC* c, int32_t nodeId, const HOPTypeRef* _Nullable expectedType, HOPTypeRef* outType);

int InferExprTypeExpected(
    HOPCBackendC* c, int32_t nodeId, const HOPTypeRef* _Nullable expectedType, HOPTypeRef* outType);

int InferExprType_IDENT(HOPCBackendC* c, int32_t nodeId, const HOPAstNode* n, HOPTypeRef* outType);

int InferExprType_COMPOUND_LIT(
    HOPCBackendC* c, int32_t nodeId, const HOPAstNode* n, HOPTypeRef* outType);

int InferExprType_CALL_WITH_CONTEXT(
    HOPCBackendC* c, int32_t nodeId, const HOPAstNode* n, HOPTypeRef* outType);

int InferExprType_CALL(HOPCBackendC* c, int32_t nodeId, const HOPAstNode* n, HOPTypeRef* outType);

int InferExprType_NEW(HOPCBackendC* c, int32_t nodeId, const HOPAstNode* n, HOPTypeRef* outType);

int InferExprType_UNARY(HOPCBackendC* c, int32_t nodeId, const HOPAstNode* n, HOPTypeRef* outType);

int InferExprType_FIELD_EXPR(
    HOPCBackendC* c, int32_t nodeId, const HOPAstNode* n, HOPTypeRef* outType);

int InferExprType_INDEX(HOPCBackendC* c, int32_t nodeId, const HOPAstNode* n, HOPTypeRef* outType);

int InferExprType_CAST(HOPCBackendC* c, int32_t nodeId, const HOPAstNode* n, HOPTypeRef* outType);

int InferExprType_SIZEOF(HOPCBackendC* c, int32_t nodeId, const HOPAstNode* n, HOPTypeRef* outType);

int InferExprType_STRING(HOPCBackendC* c, int32_t nodeId, const HOPAstNode* n, HOPTypeRef* outType);

int InferExprType_BOOL(HOPCBackendC* c, int32_t nodeId, const HOPAstNode* n, HOPTypeRef* outType);

int InferExprType_INT(HOPCBackendC* c, int32_t nodeId, const HOPAstNode* n, HOPTypeRef* outType);

int InferExprType_RUNE(HOPCBackendC* c, int32_t nodeId, const HOPAstNode* n, HOPTypeRef* outType);

int InferExprType_FLOAT(HOPCBackendC* c, int32_t nodeId, const HOPAstNode* n, HOPTypeRef* outType);

int InferExprType_NULL(HOPCBackendC* c, int32_t nodeId, const HOPAstNode* n, HOPTypeRef* outType);

int InferExprType_UNWRAP(HOPCBackendC* c, int32_t nodeId, const HOPAstNode* n, HOPTypeRef* outType);

int InferExprType_TUPLE_EXPR(
    HOPCBackendC* c, int32_t nodeId, const HOPAstNode* n, HOPTypeRef* outType);

int InferExprType_CALL_ARG(
    HOPCBackendC* c, int32_t nodeId, const HOPAstNode* n, HOPTypeRef* outType);

int InferExprType(HOPCBackendC* c, int32_t nodeId, HOPTypeRef* outType);

int InferNewExprType(HOPCBackendC* c, int32_t nodeId, HOPTypeRef* outType);

const char* UnaryOpString(HOPTokenKind op);

const char* BinaryOpString(HOPTokenKind op);

int EmitHexByte(HOPBuf* b, uint8_t value);

int BufAppendHexU64Literal(HOPBuf* b, uint64_t value);

int EmitStringLiteralValue(HOPCBackendC* c, int32_t literalId, int writable);

int EmitStringLiteralPool(HOPCBackendC* c);

int IsStrBaseName(const char* _Nullable s);

int TypeRefIsStr(const HOPTypeRef* t);

int TypeRefContainerWritable(const HOPTypeRef* t);

int EmitElementTypeName(HOPCBackendC* c, const HOPTypeRef* t, int asConst);

int EmitLenExprFromType(HOPCBackendC* c, int32_t exprNode, const HOPTypeRef* t);

int EmitElemPtrExpr(
    HOPCBackendC* c, int32_t baseNode, const HOPTypeRef* baseType, int wantWritableElem);

int EmitSliceExpr(HOPCBackendC* c, int32_t nodeId);

int TypeRefIsPointerLike(const HOPTypeRef* t);
int TypeRefIsPointerBackedOptional(const HOPTypeRef* t);
int TypeRefIsTaggedOptional(const HOPTypeRef* t);
int TypeRefLowerForStorage(HOPCBackendC* c, const HOPTypeRef* type, HOPTypeRef* outType);

int TypeRefIsOwnedRuntimeArrayStruct(const HOPTypeRef* t);

int TypeRefIsNamedDeclKind(const HOPCBackendC* c, const HOPTypeRef* t, HOPAstKind wantKind);

int TypeRefDerefReadonlyRefLike(const HOPTypeRef* in, HOPTypeRef* outBase);

int ResolveComparisonHookArgCost(
    HOPCBackendC*     c,
    const HOPTypeRef* paramType,
    const HOPTypeRef* argType,
    uint8_t*          outCost,
    int*              outAutoRef);

int ResolveComparisonHook(
    HOPCBackendC*     c,
    const char*       hookName,
    const HOPTypeRef* lhsType,
    const HOPTypeRef* rhsType,
    const HOPFnSig**  outSig,
    const char**      outCalleeName,
    int               outAutoRef[2]);

int EmitExprAutoRefCoerced(HOPCBackendC* c, int32_t argNode, const HOPTypeRef* paramType);

int EmitExprAsSliceRO(HOPCBackendC* c, int32_t exprNode, const HOPTypeRef* exprType);

int EmitComparisonHookCall(
    HOPCBackendC*   c,
    const HOPFnSig* sig,
    const char*     calleeName,
    int32_t         lhsNode,
    int32_t         rhsNode,
    const int       autoRef[2]);

int EmitPointerIdentityExpr(HOPCBackendC* c, int32_t exprNode, const HOPTypeRef* exprType);

int EmitNewAllocArgExpr(HOPCBackendC* c, int32_t allocArg);

const char* _Nullable ResolveVarSizeValueBaseName(HOPCBackendC* c, const HOPTypeRef* valueType);

int EmitConcatCallExpr(HOPCBackendC* c, int32_t calleeNode);

uint32_t FmtNextTempId(HOPCBackendC* c);

int TypeRefIsSignedIntegerLike(const HOPCBackendC* c, const HOPTypeRef* t);

int TypeRefIsIntegerLike(const HOPCBackendC* c, const HOPTypeRef* t);

int TypeRefIsBoolLike(const HOPCBackendC* c, const HOPTypeRef* t);

int TypeRefIsNamedEnumLike(const HOPCBackendC* c, const HOPTypeRef* t);

int TypeRefIsFmtStringLike(const HOPCBackendC* c, const HOPTypeRef* t);

int TypeRefIsFmtValueType(const HOPCBackendC* c, const HOPTypeRef* t);

int EmitFmtAppendLiteralBytes(
    HOPCBackendC* c, const char* builderName, const uint8_t* bytes, uint32_t len);

int EmitFmtAppendLiteralText(
    HOPCBackendC* c, const char* builderName, const char* text, uint32_t len);

int EmitFmtBuilderInitStmt(HOPCBackendC* c, const char* builderName, int32_t allocArgNode);

char* _Nullable FmtMakeExprField(HOPCBackendC* c, const char* baseExpr, const char* fieldName);

char* _Nullable FmtMakeExprIndex(HOPCBackendC* c, const char* baseExpr, const char* indexExpr);

int EmitFmtAppendReflectArray(
    HOPCBackendC*     c,
    const char*       builderName,
    const char*       expr,
    const HOPTypeRef* type,
    uint32_t          depth);

int EmitFmtAppendReflectSlice(
    HOPCBackendC*     c,
    const char*       builderName,
    const char*       expr,
    const HOPTypeRef* type,
    uint32_t          depth);

int EmitFmtAppendReflectStruct(
    HOPCBackendC*     c,
    const char*       builderName,
    const char*       expr,
    const HOPTypeRef* type,
    uint32_t          depth);

int EmitFmtAppendReflectExpr(
    HOPCBackendC*     c,
    const char*       builderName,
    const char*       expr,
    const HOPTypeRef* type,
    uint32_t          depth);

int EmitExprCoerceFmtValue(
    HOPCBackendC* c, int32_t exprNode, const HOPTypeRef* srcType, const HOPTypeRef* dstType);

int EmitFreeCallExpr(HOPCBackendC* c, int32_t allocArgNode, int32_t valueNode);

int EmitNewExpr(
    HOPCBackendC* c, int32_t nodeId, const HOPTypeRef* _Nullable dstType, int requireNonNull);

int EmitExprCoerced(HOPCBackendC* c, int32_t exprNode, const HOPTypeRef* _Nullable dstType);

int32_t ActiveCallOverlayNode(const HOPCBackendC* c);

int32_t FindActiveOverlayBindByName(const HOPCBackendC* c, const char* fieldName);

int EmitCurrentContextFieldRaw(HOPCBackendC* c, const char* fieldName);

int EmitCurrentContextFieldValue(
    HOPCBackendC* c, const char* fieldName, const HOPTypeRef* requiredType);

int EmitEffectiveContextFieldValue(
    HOPCBackendC* c, const char* fieldName, const HOPTypeRef* requiredType);

int EmitContextArgForSig(HOPCBackendC* c, const HOPFnSig* sig);

int EmitResolvedCall(
    HOPCBackendC*          c,
    int32_t                callNode,
    const char*            calleeName,
    const HOPFnSig*        sig,
    const HOPCCallBinding* binding,
    int                    autoRefFirstArg);

int EmitFieldPathLValue(
    HOPCBackendC* c, const char* base, const HOPFieldInfo* const* path, uint32_t pathLen);

int EmitCompoundFieldValueCoerced(
    HOPCBackendC*     c,
    const HOPAstNode* field,
    int32_t           exprNode,
    const HOPTypeRef* _Nullable dstType);

int EmitEnumVariantCompoundLiteral(
    HOPCBackendC*     c,
    int32_t           nodeId,
    int32_t           firstField,
    const char*       enumTypeName,
    uint32_t          variantStart,
    uint32_t          variantEnd,
    const HOPTypeRef* valueType);

int EmitCompoundLiteralDesignated(
    HOPCBackendC* c, int32_t firstField, const char* ownerType, const HOPTypeRef* valueType);

int EmitCompoundLiteralOrderedStruct(
    HOPCBackendC* c, int32_t firstField, const char* ownerType, const HOPTypeRef* valueType);

int StructHasFieldDefaults(const HOPCBackendC* c, const char* ownerType);

int EmitCompoundLiteral(HOPCBackendC* c, int32_t nodeId, const HOPTypeRef* _Nullable expectedType);

int EmitExpr_IDENT(HOPCBackendC* c, int32_t nodeId, const HOPAstNode* n);

int EmitExpr_INT(HOPCBackendC* c, int32_t nodeId, const HOPAstNode* n);

int EmitExpr_RUNE(HOPCBackendC* c, int32_t nodeId, const HOPAstNode* n);

int EmitExpr_FLOAT(HOPCBackendC* c, int32_t nodeId, const HOPAstNode* n);

int EmitExpr_BOOL(HOPCBackendC* c, int32_t nodeId, const HOPAstNode* n);

int EmitExpr_COMPOUND_LIT(HOPCBackendC* c, int32_t nodeId, const HOPAstNode* n);

int EmitExpr_STRING(HOPCBackendC* c, int32_t nodeId, const HOPAstNode* n);

int EmitExpr_UNARY(HOPCBackendC* c, int32_t nodeId, const HOPAstNode* n);

int EmitExpr_BINARY(HOPCBackendC* c, int32_t nodeId, const HOPAstNode* n);

int EmitExpr_CALL_WITH_CONTEXT(HOPCBackendC* c, int32_t nodeId, const HOPAstNode* n);

int EmitExpr_CALL(HOPCBackendC* c, int32_t nodeId, const HOPAstNode* n);

int EmitExpr_NEW(HOPCBackendC* c, int32_t nodeId, const HOPAstNode* n);

int EmitExpr_INDEX(HOPCBackendC* c, int32_t nodeId, const HOPAstNode* n);

int EmitExpr_FIELD_EXPR(HOPCBackendC* c, int32_t nodeId, const HOPAstNode* n);

int EmitExpr_CAST(HOPCBackendC* c, int32_t nodeId, const HOPAstNode* n);

int EmitExpr_SIZEOF(HOPCBackendC* c, int32_t nodeId, const HOPAstNode* n);

int EmitExpr_NULL(HOPCBackendC* c, int32_t nodeId, const HOPAstNode* n);

int EmitExpr_UNWRAP(HOPCBackendC* c, int32_t nodeId, const HOPAstNode* n);

int EmitExpr_CALL_ARG(HOPCBackendC* c, int32_t nodeId, const HOPAstNode* n);

int EmitExpr_TUPLE_EXPR(HOPCBackendC* c, int32_t nodeId, const HOPAstNode* n);

int EmitExpr(HOPCBackendC* c, int32_t nodeId);

int EmitDeferredRange(HOPCBackendC* c, uint32_t start, uint32_t depth);

int EmitBlockImpl(HOPCBackendC* c, int32_t nodeId, uint32_t depth, int inlineOpen);

int EmitBlock(HOPCBackendC* c, int32_t nodeId, uint32_t depth);

int EmitBlockInline(HOPCBackendC* c, int32_t nodeId, uint32_t depth);

int EmitVarLikeStmt(HOPCBackendC* c, int32_t nodeId, uint32_t depth, int isConst);

int EmitMultiAssignStmt(HOPCBackendC* c, int32_t nodeId, uint32_t depth);

int EmitShortAssignStmt(HOPCBackendC* c, int32_t nodeId, uint32_t depth);

int EmitForStmt(HOPCBackendC* c, int32_t nodeId, uint32_t depth);

int EmitSwitchStmt(HOPCBackendC* c, int32_t nodeId, uint32_t depth);

int EmitAssertFormatArg(HOPCBackendC* c, int32_t nodeId);

int EmitStmt(HOPCBackendC* c, int32_t nodeId, uint32_t depth);

int IsMainFunctionNode(const HOPCBackendC* c, int32_t nodeId);

int IsExplicitlyExportedNode(const HOPCBackendC* c, int32_t nodeId);

int IsExportedNode(const HOPCBackendC* c, int32_t nodeId);

int IsExportedTypeNode(const HOPCBackendC* c, int32_t nodeId);

int EmitEnumDecl(HOPCBackendC* c, int32_t nodeId, uint32_t depth);

int NodeHasDirectDependentFields(HOPCBackendC* c, int32_t nodeId);

int EmitVarSizeStructDecl(HOPCBackendC* c, int32_t nodeId, uint32_t depth);

int EmitStructOrUnionDecl(HOPCBackendC* c, int32_t nodeId, uint32_t depth, int isUnion);

int EmitForwardTypeDecls(HOPCBackendC* c);

int EmitForwardAnonTypeDecls(HOPCBackendC* c);

int EmitAnonTypeDecls(HOPCBackendC* c);

int EmitHeaderTypeAliasDecls(HOPCBackendC* c);

int EmitFnTypeAliasDecls(HOPCBackendC* c);

int FnNodeHasBody(const HOPCBackendC* c, int32_t nodeId);

int HasFunctionBodyForName(const HOPCBackendC* c, int32_t nodeId);

int EmitFnDeclOrDef(
    HOPCBackendC* c,
    int32_t       nodeId,
    uint32_t      depth,
    int           emitBody,
    int           isPrivate,
    const HOPFnSig* _Nullable forcedSig);

int EmitConstDecl(
    HOPCBackendC* c, int32_t nodeId, uint32_t depth, int declarationOnly, int isPrivate);

int EmitVarDecl(
    HOPCBackendC* c, int32_t nodeId, uint32_t depth, int declarationOnly, int isPrivate);

int EmitTypeAliasDecl(
    HOPCBackendC* c, int32_t nodeId, uint32_t depth, int declarationOnly, int isPrivate);

int EmitDeclNode(
    HOPCBackendC* c,
    int32_t       nodeId,
    uint32_t      depth,
    int           declarationOnly,
    int           isPrivate,
    int           emitBody);

int EmitPrelude(HOPCBackendC* c);

char* _Nullable BuildDefaultMacro(HOPCBackendC* c, const char* pkgName, const char* suffix);

int EmitHeader(HOPCBackendC* c);

int ShouldEmitDeclNode(const HOPCBackendC* c, int32_t nodeId);

int InitAst(HOPCBackendC* c);

char* _Nullable AllocOutputCopy(HOPCBackendC* c);

void FreeContext(HOPCBackendC* c);

int EmitCBackend(
    const HOPCodegenBackend* backend,
    const HOPCodegenUnit*    unit,
    const HOPCodegenOptions* _Nullable options,
    HOPCodegenArtifact* _Nonnull outArtifact,
    HOPDiag* _Nullable diag);

HOP_API_END
