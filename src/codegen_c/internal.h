#pragma once

#include "../codegen.h"
#include "../libhop-impl.h"

H2_API_BEGIN
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
} H2TypeRef;

typedef struct {
    H2Arena* _Nullable arena;
    char* _Nullable v;
    uint32_t len;
    uint32_t cap;
} H2Buf;

typedef struct {
    char*     name;
    char*     cName;
    H2AstKind kind;
    int       isExported;
} H2NameMap;

typedef struct {
    int32_t nodeId;
} H2NodeRef;

typedef struct {
    char*     hopName;
    char*     cName;
    int32_t   nodeId;
    uint32_t  tcFuncIndex;
    H2TypeRef returnType;
    H2TypeRef* _Nullable paramTypes;
    char** _Nullable paramNames;
    uint8_t* _Nullable paramFlags;
    uint32_t paramLen;
    uint32_t packArgStart;
    uint32_t packArgCount;
    char* _Nullable packParamName;
    uint16_t  flags;
    H2TypeRef contextType;
    int       hasContext;
    uint8_t   isVariadic;
    uint8_t   _reserved[1];
} H2FnSig;

enum {
    H2FnSigFlag_TEMPLATE_BASE = 1u << 0,
    H2FnSigFlag_TEMPLATE_INSTANCE = 1u << 1,
    H2FnSigFlag_EXPANDED_ANYPACK = 1u << 2,
};

typedef struct {
    char*     aliasName;
    H2TypeRef returnType;
    H2TypeRef* _Nullable paramTypes;
    uint32_t paramLen;
    uint8_t  isVariadic;
    uint8_t  _reserved[3];
} H2FnTypeAlias;

typedef struct {
    char*     aliasName;
    H2TypeRef targetType;
} H2TypeAliasInfo;

typedef struct {
    char* _Nullable ownerType;
    char* _Nullable fieldName;
    char* _Nullable lenFieldName;
    int       isDependent;
    int       isEmbedded;
    int32_t   defaultExprNode;
    H2TypeRef type;
} H2FieldInfo;

typedef struct {
    char* cName;
    int   isUnion;
    int   isVarSize;
} H2VarSizeType;

typedef struct {
    char*    key;
    char*    cName;
    int      isUnion;
    uint32_t fieldStart;
    uint16_t fieldCount;
    uint16_t flags;
} H2AnonTypeInfo;

enum {
    H2AnonTypeFlag_EMITTED_GLOBAL = 1u << 0,
};

typedef struct {
    char*     name;
    H2TypeRef type;
} H2Local;

typedef struct {
    int32_t  localIdx;
    char*    enumTypeName;
    uint32_t variantStart;
    uint32_t variantEnd;
} H2VariantNarrow;

typedef struct {
    int32_t nodeId;
    char*   cName;
} H2FnNodeName;

typedef struct {
    uint8_t* _Nullable bytes;
    uint32_t len;
} H2StringLiteral;

typedef struct {
    const H2CodegenUnit*    unit;
    const H2CodegenOptions* options;
    H2Diag* _Nullable diag;

    H2Arena arena;
    uint8_t arenaInlineStorage[16384];
    H2Ast   ast;

    H2Buf out;

    H2NameMap* names;
    uint32_t   nameLen;
    uint32_t   nameCap;

    H2NodeRef* pubDecls;
    uint32_t   pubDeclLen;
    uint32_t   pubDeclCap;

    H2NodeRef* topDecls;
    uint32_t   topDeclLen;
    uint32_t   topDeclCap;

    H2FnSig* fnSigs;
    uint32_t fnSigLen;
    uint32_t fnSigCap;

    H2FnTypeAlias* fnTypeAliases;
    uint32_t       fnTypeAliasLen;
    uint32_t       fnTypeAliasCap;

    H2TypeAliasInfo* typeAliases;
    uint32_t         typeAliasLen;
    uint32_t         typeAliasCap;

    H2FieldInfo* fieldInfos;
    uint32_t     fieldInfoLen;
    uint32_t     fieldInfoCap;

    H2VarSizeType* varSizeTypes;
    uint32_t       varSizeTypeLen;
    uint32_t       varSizeTypeCap;

    H2AnonTypeInfo* anonTypes;
    uint32_t        anonTypeLen;
    uint32_t        anonTypeCap;

    H2Local* locals;
    uint32_t localLen;
    uint32_t localCap;

    H2VariantNarrow* variantNarrows;
    uint32_t         variantNarrowLen;
    uint32_t         variantNarrowCap;
    int32_t          activeOptionalNarrowLocalIdx;
    uint8_t          hasActiveOptionalNarrow;
    uint8_t          _reserved_optional_narrow[3];
    H2TypeRef        activeOptionalNarrowStorageType;

    H2FnNodeName* fnNodeNames;
    uint32_t      fnNodeNameLen;
    uint32_t      fnNodeNameCap;

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

    H2StringLiteral* stringLits;
    uint32_t         stringLitLen;
    uint32_t         stringLitCap;

    int32_t* stringLitByNode;
    uint32_t stringLitByNodeLen;

    int       emitPrivateFnDeclStatic;
    H2TypeRef currentReturnType;
    int       hasCurrentReturnType;
    H2TypeRef currentContextType;
    int       hasCurrentContext;
    int       currentFunctionIsMain;
    int32_t   activeCallWithNode;
    const char* _Nullable activePackParamName;
    char** _Nullable activePackElemNames;
    H2TypeRef* _Nullable activePackElemTypes;
    uint32_t activePackElemCount;
    uint32_t fmtTempCounter;
    H2ConstEvalSession* _Nullable constEval;
    uint32_t activeTcFuncIndex;
    int32_t  activeTcNamedTypeIndex;
} H2CBackendC;

enum {
    H2TypeContainer_SCALAR = 0,
    H2TypeContainer_ARRAY = 1,
    H2TypeContainer_SLICE_RO = 2,
    H2TypeContainer_SLICE_MUT = 3,
};

typedef struct {
    int32_t  nameListNode;
    int32_t  typeNode;
    int32_t  initNode;
    uint32_t nameCount;
    uint8_t  grouped;
} H2CCGVarLikeParts;

#define H2CCG_MAX_CALL_ARGS       128u
#define H2CCG_MAX_CALL_CANDIDATES 256u

typedef struct {
    int32_t  argNode;
    int32_t  exprNode;
    uint32_t explicitNameStart;
    uint32_t explicitNameEnd;
    uint32_t implicitNameStart;
    uint32_t implicitNameEnd;
    uint8_t  spread;
    uint8_t  _reserved[3];
} H2CCallArgInfo;

typedef struct {
    int       isVariadic;
    int       activePackSpread;
    uint32_t  fixedCount;
    uint32_t  fixedInputCount;
    uint32_t  spreadArgIndex;
    uint32_t  activePackSpreadArgIndex;
    uint32_t  activePackSpreadParamStart;
    int32_t   fixedMappedArgNodes[H2CCG_MAX_CALL_ARGS];
    int32_t   explicitTailNodes[H2CCG_MAX_CALL_ARGS];
    int32_t   argParamIndices[H2CCG_MAX_CALL_ARGS];
    uint32_t  explicitTailCount;
    H2TypeRef argExpectedTypes[H2CCG_MAX_CALL_ARGS];
} H2CCallBinding;

enum {
    H2CCGParamFlag_CONST = 1u << 0,
    H2CCGParamFlag_ANYTYPE = 1u << 1,
    H2CCGParamFlag_ANYPACK = 1u << 2,
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

void SetDiag(H2Diag* _Nullable diag, H2DiagCode code, uint32_t start, uint32_t end);

int EnsureCapArena(
    H2Arena* arena, void** ptr, uint32_t* cap, uint32_t need, size_t elemSize, uint32_t align);

int BufReserve(H2Buf* b, uint32_t extra);

int BufAppend(H2Buf* b, const char* s, uint32_t len);

int BufAppendCStr(H2Buf* b, const char* s);

int BufAppendChar(H2Buf* b, char c);

int BufAppendU32(H2Buf* b, uint32_t value);

int BufAppendSlice(H2Buf* b, const char* src, uint32_t start, uint32_t end);

char* _Nullable BufFinish(H2Buf* b);

void EmitIndent(H2CBackendC* c, uint32_t depth);

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

void TypeRefSetInvalid(H2TypeRef* t);

void TypeRefSetScalar(H2TypeRef* t, const char* baseName);

void CanonicalizeTypeRefBaseName(const H2CBackendC* c, H2TypeRef* t);

int SliceStructPtrDepth(const H2TypeRef* t);

int ParseArrayLenLiteral(const char* src, uint32_t start, uint32_t end, uint32_t* outLen);

void SetDiagNode(H2CBackendC* c, int32_t nodeId, H2DiagCode code);

int BufAppendI64(H2Buf* b, int64_t value);

int EvalConstIntExpr(H2CBackendC* c, int32_t nodeId, int64_t* outValue, int* outIsConst);
int EvalConstFloatExpr(H2CBackendC* c, int32_t nodeId, double* outValue, int* outIsConst);

int ConstIntFitsIntegerType(const char* typeName, int64_t value);
int ConstIntFitsFloatType(const char* typeName, int64_t value);
int ConstFloatFitsFloatType(const char* typeName, double value);

int EmitConstEvaluatedScalar(
    H2CBackendC* c, const H2TypeRef* dstType, const H2CTFEValue* value, int* outEmitted);

char* _Nullable DupSlice(H2CBackendC* c, const char* src, uint32_t start, uint32_t end);

int SliceIsHoleName(const char* src, uint32_t start, uint32_t end);

char* _Nullable DupParamNameForEmit(
    H2CBackendC* c, const H2AstNode* paramNode, uint32_t paramIndex);

char* _Nullable DupAndReplaceDots(H2CBackendC* c, const char* src, uint32_t start, uint32_t end);

char* _Nullable DupCStr(H2CBackendC* c, const char* s);

int DecodeStringLiteralNode(
    H2CBackendC* c, const H2AstNode* n, uint8_t** outBytes, uint32_t* outLen);

int AppendDecodedStringExpr(
    H2CBackendC* c, int32_t nodeId, uint8_t** bytes, uint32_t* len, uint32_t* cap);

int DecodeStringExpr(
    H2CBackendC* c,
    int32_t      nodeId,
    uint8_t**    outBytes,
    uint32_t*    outLen,
    uint32_t*    outStart,
    uint32_t*    outEnd);

int GetOrAddStringLiteralExpr(H2CBackendC* c, int32_t nodeId, int32_t* outLiteralId);

int GetOrAddStringLiteralBytes(
    H2CBackendC* c, const uint8_t* bytes, uint32_t len, int32_t* outLiteralId);

int CollectStringLiterals(H2CBackendC* c);

int HasDoubleUnderscore(const char* s);

int IsTypeDeclKind(H2AstKind kind);

int IsDeclKind(H2AstKind kind);

int IsPubDeclNode(const H2AstNode* n);

int32_t AstFirstChild(const H2Ast* ast, int32_t nodeId);

int32_t AstNextSibling(const H2Ast* ast, int32_t nodeId);

const H2AstNode* _Nullable NodeAt(const H2CBackendC* c, int32_t nodeId);

int GetDeclNameSpan(const H2CBackendC* c, int32_t nodeId, uint32_t* outStart, uint32_t* outEnd);

int AddName(H2CBackendC* c, uint32_t nameStart, uint32_t nameEnd, H2AstKind kind, int isExported);
int BuildTypeDeclFlatName(H2CBackendC* c, int32_t nodeId, char** outName);

const H2NameMap* _Nullable FindNameBySlice(const H2CBackendC* c, uint32_t start, uint32_t end);
const H2NameMap* _Nullable FindTypeDeclMapByNode(H2CBackendC* c, int32_t nodeId);

const H2NameMap* _Nullable FindNameByCString(const H2CBackendC* c, const char* name);

int NameHasPrefixSuffix(const char* name, const char* prefix, const char* suffix);

int ResolveMainSemanticContextType(H2CBackendC* c, H2TypeRef* outType);

const char* ResolveRuneTypeBaseName(H2CBackendC* c);

const H2NameMap* _Nullable FindNameByCName(const H2CBackendC* c, const char* cName);

int ResolveTypeValueNameExprTypeRef(
    H2CBackendC* c, uint32_t start, uint32_t end, H2TypeRef* outTypeRef);

uint64_t TypeTagHashAddByte(uint64_t h, uint8_t b);

uint64_t TypeTagHashAddU32(uint64_t h, uint32_t v);

uint64_t TypeTagHashAddStr(uint64_t h, const char* s);

uint8_t TypeTagKindFromTypeRef(const H2CBackendC* c, const H2TypeRef* t);

uint64_t TypeTagFromTypeRef(const H2CBackendC* c, const H2TypeRef* t);

int EmitTypeTagLiteralFromTypeRef(H2CBackendC* c, const H2TypeRef* t);

const H2TypeAliasInfo* _Nullable FindTypeAliasInfoByAliasName(
    const H2CBackendC* c, const char* aliasName);

int AddTypeAliasInfo(H2CBackendC* c, const char* aliasName, H2TypeRef targetType);

const char* ResolveScalarAliasBaseName(const H2CBackendC* c, const char* typeName);

int ResolveReflectedTypeValueExprTypeRef(H2CBackendC* c, int32_t exprNode, H2TypeRef* outTypeRef);

int TypeRefIsTypeValue(const H2TypeRef* t);

const char* _Nullable FindReflectKindTypeName(const H2CBackendC* c);

const char* TypeRefDisplayBaseName(const H2CBackendC* c, const char* baseName);

int EmitTypeNameStringLiteralFromTypeRef(H2CBackendC* c, const H2TypeRef* _Nullable t);

int EmitTypeTagKindLiteralFromTypeRef(H2CBackendC* c, const H2TypeRef* t);

int EmitRuntimeTypeTagKindFromExpr(H2CBackendC* c, int32_t exprNode);

int EmitTypeTagIsAliasLiteralFromTypeRef(H2CBackendC* c, const H2TypeRef* t);

int EmitRuntimeTypeTagIsAliasFromExpr(H2CBackendC* c, int32_t exprNode);

int EmitRuntimeTypeTagCtorUnary(H2CBackendC* c, uint32_t kindTag, uint64_t salt, int32_t argNode);

int EmitRuntimeTypeTagCtorArray(H2CBackendC* c, int32_t elemTagNode, int32_t lenNode);

int EmitTypeTagBaseLiteralFromTypeRef(H2CBackendC* c, const H2TypeRef* t);

int TypeRefIsRuneLike(const H2CBackendC* c, const H2TypeRef* typeRef);

const char* _Nullable ResolveTypeName(H2CBackendC* c, uint32_t start, uint32_t end);

void NormalizeCoreRuntimeTypeName(H2TypeRef* outType);

const char* _Nullable ConstEvalBuiltinCName(H2ConstEvalBuiltinKind builtin);

int ParseTypeRefFromConstEvalTypeId(H2CBackendC* c, int32_t typeId, H2TypeRef* outType);

int ParseTypeRefFromConstEvalTypeTag(H2CBackendC* c, uint64_t typeTag, H2TypeRef* outType);

char* _Nullable BuildTemplateNamedTypeCName(
    H2CBackendC* c, const char* baseCName, uint32_t tcNamedIndex);

int CollectTemplateInstanceNamedTypes(H2CBackendC* c);

int CodegenCNodeHasTypeParams(const H2CBackendC* c, int32_t nodeId);

int CodegenCPushActiveFunctionTypeContext(H2CBackendC* c, uint32_t tcFuncIndex);

int CodegenCPushActiveNamedTypeContext(H2CBackendC* c, uint32_t tcNamedIndex);

void CodegenCPopActiveTypeContext(
    H2CBackendC* c,
    uint32_t     savedFuncIndex,
    int32_t      savedNamedTypeIndex,
    uint32_t     savedArgStart,
    uint16_t     savedArgCount,
    int32_t      savedDeclNode);

int AddNodeRef(H2CBackendC* c, H2NodeRef** arr, uint32_t* len, uint32_t* cap, int32_t nodeId);

int CollectDeclSets(H2CBackendC* c);

const H2FnTypeAlias* _Nullable FindFnTypeAliasByName(const H2CBackendC* c, const char* name);

int EnsureFnTypeAlias(
    H2CBackendC* c,
    H2TypeRef    returnType,
    H2TypeRef* _Nullable paramTypes,
    uint32_t     paramLen,
    int          isVariadic,
    const char** outAliasName);

const char* _Nullable TupleFieldName(H2CBackendC* c, uint32_t index);

int ParseTypeRef(H2CBackendC* c, int32_t nodeId, H2TypeRef* outType);

int AddFnSig(
    H2CBackendC* c,
    const char*  hopName,
    const char*  baseCName,
    int32_t      nodeId,
    H2TypeRef    returnType,
    H2TypeRef* _Nullable paramTypes,
    char** _Nullable paramNames,
    uint8_t* _Nullable paramFlags,
    uint32_t  paramLen,
    int       isVariadic,
    int       hasContext,
    H2TypeRef contextType,
    uint16_t  sigFlags,
    uint32_t  tcFuncIndex,
    uint32_t  packArgStart,
    uint32_t  packArgCount,
    char* _Nullable packParamName);

int AddFieldInfo(
    H2CBackendC* c,
    const char*  ownerType,
    const char*  fieldName,
    const char* _Nullable lenFieldName,
    int32_t   defaultExprNode,
    int       isDependent,
    int       isEmbedded,
    H2TypeRef type);

int AppendTypeRefKey(H2Buf* b, const H2TypeRef* t);

const H2AnonTypeInfo* _Nullable FindAnonTypeByKey(const H2CBackendC* c, const char* key);

const H2AnonTypeInfo* _Nullable FindAnonTypeByCName(const H2CBackendC* c, const char* cName);

int IsTupleFieldName(const char* name, uint32_t index);

int TypeRefTupleInfo(const H2CBackendC* c, const H2TypeRef* t, const H2AnonTypeInfo** outInfo);

int IsLocalAnonTypedefVisible(const H2CBackendC* c, const char* cName);

int MarkLocalAnonTypedefVisible(H2CBackendC* c, const char* cName);

int IsAnonTypeNameVisible(const H2CBackendC* c, const char* cName);

int EmitAnonTypeDeclAtDepth(H2CBackendC* c, const H2AnonTypeInfo* t, uint32_t depth);

int EnsureAnonTypeVisible(H2CBackendC* c, const H2TypeRef* type, uint32_t depth);

int EnsureAnonTypeByFields(
    H2CBackendC*     c,
    int              isUnion,
    const char**     fieldNames,
    const H2TypeRef* fieldTypes,
    uint32_t         fieldCount,
    const char**     outCName);

const H2FnSig* _Nullable FindFnSigBySlice(const H2CBackendC* c, uint32_t start, uint32_t end);

uint32_t FindFnSigCandidatesBySlice(
    const H2CBackendC* c, uint32_t start, uint32_t end, const H2FnSig** out, uint32_t cap);

uint32_t FindFnSigCandidatesByName(
    const H2CBackendC* c, const char* hopName, const H2FnSig** out, uint32_t cap);

const char* _Nullable FindFnCNameByNodeId(const H2CBackendC* c, int32_t nodeId);

const H2FnSig* _Nullable FindFnSigByNodeId(const H2CBackendC* c, int32_t nodeId);
uint32_t FindFnSigCandidatesByNodeId(
    const H2CBackendC* c, int32_t nodeId, const H2FnSig** out, uint32_t cap);

const H2FieldInfo* _Nullable FindFieldInfo(
    const H2CBackendC* c, const char* ownerType, uint32_t fieldStart, uint32_t fieldEnd);

const H2FieldInfo* _Nullable FindEmbeddedFieldInfo(const H2CBackendC* c, const char* ownerType);

const H2FieldInfo* _Nullable FindFieldInfoByName(
    const H2CBackendC* c, const char* ownerType, const char* fieldName);

const char* _Nullable CanonicalFieldOwnerType(
    const H2CBackendC* c, const char* _Nullable ownerType);

int ResolveCoreStrFieldBySlice(
    const H2CBackendC* c, uint32_t fieldStart, uint32_t fieldEnd, const H2FieldInfo** outField);

int ResolveFieldPathSingleSegment(
    const H2CBackendC*  c,
    const char*         ownerTypeIn,
    uint32_t            fieldStart,
    uint32_t            fieldEnd,
    const H2FieldInfo** outPath,
    uint32_t            cap,
    uint32_t*           outLen,
    const H2FieldInfo** _Nullable outField);

int FieldPathNextSegment(
    const char* src, uint32_t pathEnd, uint32_t* ioPos, uint32_t* outSegStart, uint32_t* outSegEnd);

int ResolveFieldPathBySlice(
    const H2CBackendC*  c,
    const char*         ownerType,
    uint32_t            fieldStart,
    uint32_t            fieldEnd,
    const H2FieldInfo** outPath,
    uint32_t            cap,
    uint32_t*           outLen,
    const H2FieldInfo** _Nullable outField);

int ResolveEmbeddedPathByNames(
    const H2CBackendC*  c,
    const char*         srcTypeName,
    const char*         dstTypeName,
    const H2FieldInfo** outPath,
    uint32_t            cap,
    uint32_t*           outLen);

int CollectFnAndFieldInfoFromNode(H2CBackendC* c, int32_t nodeId);

int CollectFnAndFieldInfo(H2CBackendC* c);

int CollectTypeAliasInfo(H2CBackendC* c);

int CollectFnTypeAliasesFromNode(H2CBackendC* c, int32_t nodeId);

int CollectFnTypeAliases(H2CBackendC* c);

int AddVarSizeType(H2CBackendC* c, const char* cName, int isUnion);

H2VarSizeType* _Nullable FindVarSizeType(H2CBackendC* c, const char* cName);

int CollectVarSizeTypesFromDeclSets(H2CBackendC* c);

int IsVarSizeTypeName(const H2CBackendC* c, const char* cName);

int PropagateVarSizeTypes(H2CBackendC* c);

int PushScope(H2CBackendC* c);

void PopScope(H2CBackendC* c);

int PushDeferScope(H2CBackendC* c);

void PopDeferScope(H2CBackendC* c);

int AddDeferredStmt(H2CBackendC* c, int32_t stmtNodeId);

int AddLocal(H2CBackendC* c, const char* name, H2TypeRef type);

void TrimVariantNarrowsToLocalLen(H2CBackendC* c);

int AddVariantNarrow(
    H2CBackendC* c,
    int32_t      localIdx,
    const char*  enumTypeName,
    uint32_t     variantStart,
    uint32_t     variantEnd);

int32_t FindLocalIndexBySlice(const H2CBackendC* c, uint32_t start, uint32_t end);

const H2VariantNarrow* _Nullable FindVariantNarrowByLocalIdx(
    const H2CBackendC* c, int32_t localIdx);

const H2Local* _Nullable FindLocalBySlice(const H2CBackendC* c, uint32_t start, uint32_t end);

int FindEnumDeclNodeBySlice(const H2CBackendC* c, uint32_t start, uint32_t end, int32_t* outNodeId);

int EnumDeclHasMemberBySlice(
    const H2CBackendC* c, int32_t enumNodeId, uint32_t memberStart, uint32_t memberEnd);

int ResolveEnumSelectorByFieldExpr(
    const H2CBackendC* c,
    int32_t            fieldExprNode,
    const H2NameMap** _Nullable outEnumMap,
    int32_t* _Nullable outEnumDeclNode,
    int* _Nullable outEnumHasPayload,
    uint32_t* _Nullable outVariantStart,
    uint32_t* _Nullable outVariantEnd);

int EnumDeclHasPayload(const H2CBackendC* c, int32_t enumNodeId);

int32_t EnumVariantTagExprNode(const H2CBackendC* c, int32_t variantNode);
int32_t EnumVariantPayloadTypeNode(const H2CBackendC* c, int32_t variantNode);

int FindEnumDeclNodeByCName(const H2CBackendC* c, const char* enumCName, int32_t* outNodeId);

int FindEnumVariantNodeBySlice(
    const H2CBackendC* c,
    int32_t            enumNodeId,
    uint32_t           variantStart,
    uint32_t           variantEnd,
    int32_t*           outVariantNode);

int ResolveEnumVariantPayloadFieldType(
    H2CBackendC* c,
    const char*  enumTypeName,
    uint32_t     variantStart,
    uint32_t     variantEnd,
    uint32_t     fieldStart,
    uint32_t     fieldEnd,
    H2TypeRef*   outType);

int ResolveEnumVariantPayloadType(
    H2CBackendC* c,
    const char*  enumTypeName,
    uint32_t     variantStart,
    uint32_t     variantEnd,
    H2TypeRef*   outType);

int ResolveEnumVariantTypeNameNode(
    const H2CBackendC* c,
    int32_t            typeNode,
    const char**       outEnumCName,
    uint32_t*          outVariantStart,
    uint32_t*          outVariantEnd);

int CasePatternParts(
    const H2CBackendC* c, int32_t caseLabelNode, int32_t* outExprNode, int32_t* outAliasNode);

int DecodeEnumVariantPatternExpr(
    const H2CBackendC* c,
    int32_t            exprNode,
    const H2NameMap** _Nullable outEnumMap,
    int32_t* _Nullable outEnumDeclNode,
    int* _Nullable outEnumHasPayload,
    uint32_t* _Nullable outVariantStart,
    uint32_t* _Nullable outVariantEnd);

int ResolvePayloadEnumType(const H2CBackendC* c, const H2TypeRef* t, const char** outEnumName);

int AppendMappedIdentifier(H2CBackendC* c, uint32_t start, uint32_t end);

int EmitTypeNameWithDepth(H2CBackendC* c, const H2TypeRef* type);

int EmitTypeWithName(H2CBackendC* c, int32_t typeNode, const char* name);

int EmitAnonInlineTypeWithName(
    H2CBackendC* c, const char* ownerType, int isUnion, const char* name);

int EmitTypeRefWithName(H2CBackendC* c, const H2TypeRef* t, const char* name);

int EmitTypeForCast(H2CBackendC* c, int32_t typeNode);

void SetPreferredAllocatorPtrType(H2TypeRef* outType);

int IsTypeNodeKind(H2AstKind kind);

int DecodeNewExprNodes(
    H2CBackendC* c,
    int32_t      nodeId,
    int32_t*     outTypeNode,
    int32_t*     outCountNode,
    int32_t*     outInitNode,
    int32_t*     outAllocNode);

uint32_t ListCount(const H2Ast* ast, int32_t listNode);

int32_t ListItemAt(const H2Ast* ast, int32_t listNode, uint32_t index);

int ResolveVarLikeParts(H2CBackendC* c, int32_t nodeId, H2CCGVarLikeParts* out);

int InferVarLikeDeclType(H2CBackendC* c, int32_t initNode, H2TypeRef* outType);

int FindTopLevelVarLikeNodeBySliceEx(
    const H2CBackendC* c,
    uint32_t           start,
    uint32_t           end,
    int32_t*           outNodeId,
    int32_t* _Nullable outNameIndex);

int FindTopLevelVarLikeNodeBySlice(
    const H2CBackendC* c, uint32_t start, uint32_t end, int32_t* outNodeId);

int InferTopLevelVarLikeType(
    H2CBackendC* c, int32_t nodeId, uint32_t nameStart, uint32_t nameEnd, H2TypeRef* outType);

int ResolveTopLevelConstTypeValueBySlice(
    H2CBackendC* c, uint32_t start, uint32_t end, H2TypeRef* outType);

int TypeRefEqual(const H2TypeRef* a, const H2TypeRef* b);

int ExpandAliasSourceType(const H2CBackendC* c, const H2TypeRef* src, H2TypeRef* outExpanded);

int TypeRefAssignableCost(
    H2CBackendC* c, const H2TypeRef* dst, const H2TypeRef* src, uint8_t* outCost);

int CostVecCmp(const uint8_t* a, const uint8_t* b, uint32_t len);

int ExprNeedsExpectedType(const H2CBackendC* c, int32_t exprNode);

int32_t UnwrapCallArgExprNode(const H2CBackendC* c, int32_t argNode);

int CollectCallArgInfo(
    H2CBackendC*    c,
    int32_t         callNode,
    int32_t         calleeNode,
    int             includeReceiver,
    int32_t         receiverNode,
    H2CCallArgInfo* outArgs,
    H2TypeRef*      outArgTypes,
    uint32_t*       outArgCount);

void GatherCallCandidatesBySlice(
    const H2CBackendC* c,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    const H2FnSig**    outCandidates,
    uint32_t*          outCandidateLen,
    int*               outNameFound);

void GatherCallCandidatesByPkgMethod(
    const H2CBackendC* c,
    uint32_t           pkgStart,
    uint32_t           pkgEnd,
    uint32_t           methodStart,
    uint32_t           methodEnd,
    const H2FnSig**    outCandidates,
    uint32_t*          outCandidateLen,
    int*               outNameFound);

int MapCallArgsToParams(
    const H2CBackendC*    c,
    const H2FnSig*        sig,
    const H2CCallArgInfo* callArgs,
    uint32_t              argCount,
    uint32_t              firstPositionalArgIndex,
    int32_t*              outMappedArgNodes,
    H2TypeRef*            outMappedArgTypes,
    const H2TypeRef*      argTypes);

int PrepareCallBinding(
    const H2CBackendC*    c,
    const H2FnSig*        sig,
    const H2CCallArgInfo* callArgs,
    const int32_t*        argNodes,
    const H2TypeRef*      argTypes,
    uint32_t              argCount,
    uint32_t              firstPositionalArgIndex,
    int                   allowNamedMapping,
    H2CCallBinding*       out);

int ResolveCallTargetFromCandidates(
    H2CBackendC*          c,
    const H2FnSig**       candidates,
    uint32_t              candidateLen,
    int                   nameFound,
    const H2CCallArgInfo* callArgs,
    const int32_t*        argNodes,
    const H2TypeRef*      argTypes,
    uint32_t              argCount,
    uint32_t              firstPositionalArgIndex,
    int                   autoRefFirstArg,
    H2CCallBinding* _Nullable outBinding,
    const H2FnSig** outSig,
    const char**    outCalleeName);

int ResolveCallTarget(
    H2CBackendC*          c,
    uint32_t              nameStart,
    uint32_t              nameEnd,
    const H2CCallArgInfo* callArgs,
    const int32_t*        argNodes,
    const H2TypeRef*      argTypes,
    uint32_t              argCount,
    uint32_t              firstPositionalArgIndex,
    int                   autoRefFirstArg,
    H2CCallBinding* _Nullable outBinding,
    const H2FnSig** outSig,
    const char**    outCalleeName);

int ResolveCallTargetByPkgMethod(
    H2CBackendC*          c,
    uint32_t              pkgStart,
    uint32_t              pkgEnd,
    uint32_t              methodStart,
    uint32_t              methodEnd,
    const H2CCallArgInfo* callArgs,
    const int32_t*        argNodes,
    const H2TypeRef*      argTypes,
    uint32_t              argCount,
    uint32_t              firstPositionalArgIndex,
    int                   autoRefFirstArg,
    H2CCallBinding* _Nullable outBinding,
    const H2FnSig** outSig,
    const char**    outCalleeName);

int InferCompoundLiteralType(
    H2CBackendC* c, int32_t nodeId, const H2TypeRef* _Nullable expectedType, H2TypeRef* outType);

int InferExprTypeExpected(
    H2CBackendC* c, int32_t nodeId, const H2TypeRef* _Nullable expectedType, H2TypeRef* outType);

int InferExprType_IDENT(H2CBackendC* c, int32_t nodeId, const H2AstNode* n, H2TypeRef* outType);

int InferExprType_COMPOUND_LIT(
    H2CBackendC* c, int32_t nodeId, const H2AstNode* n, H2TypeRef* outType);

int InferExprType_CALL_WITH_CONTEXT(
    H2CBackendC* c, int32_t nodeId, const H2AstNode* n, H2TypeRef* outType);

int InferExprType_CALL(H2CBackendC* c, int32_t nodeId, const H2AstNode* n, H2TypeRef* outType);

int InferExprType_NEW(H2CBackendC* c, int32_t nodeId, const H2AstNode* n, H2TypeRef* outType);

int InferExprType_UNARY(H2CBackendC* c, int32_t nodeId, const H2AstNode* n, H2TypeRef* outType);

int InferExprType_FIELD_EXPR(
    H2CBackendC* c, int32_t nodeId, const H2AstNode* n, H2TypeRef* outType);

int InferExprType_INDEX(H2CBackendC* c, int32_t nodeId, const H2AstNode* n, H2TypeRef* outType);

int InferExprType_CAST(H2CBackendC* c, int32_t nodeId, const H2AstNode* n, H2TypeRef* outType);

int InferExprType_SIZEOF(H2CBackendC* c, int32_t nodeId, const H2AstNode* n, H2TypeRef* outType);

int InferExprType_STRING(H2CBackendC* c, int32_t nodeId, const H2AstNode* n, H2TypeRef* outType);

int InferExprType_BOOL(H2CBackendC* c, int32_t nodeId, const H2AstNode* n, H2TypeRef* outType);

int InferExprType_INT(H2CBackendC* c, int32_t nodeId, const H2AstNode* n, H2TypeRef* outType);

int InferExprType_RUNE(H2CBackendC* c, int32_t nodeId, const H2AstNode* n, H2TypeRef* outType);

int InferExprType_FLOAT(H2CBackendC* c, int32_t nodeId, const H2AstNode* n, H2TypeRef* outType);

int InferExprType_NULL(H2CBackendC* c, int32_t nodeId, const H2AstNode* n, H2TypeRef* outType);

int InferExprType_UNWRAP(H2CBackendC* c, int32_t nodeId, const H2AstNode* n, H2TypeRef* outType);

int InferExprType_TUPLE_EXPR(
    H2CBackendC* c, int32_t nodeId, const H2AstNode* n, H2TypeRef* outType);

int InferExprType_CALL_ARG(H2CBackendC* c, int32_t nodeId, const H2AstNode* n, H2TypeRef* outType);

int InferExprType(H2CBackendC* c, int32_t nodeId, H2TypeRef* outType);

int InferNewExprType(H2CBackendC* c, int32_t nodeId, H2TypeRef* outType);

const char* UnaryOpString(H2TokenKind op);

const char* BinaryOpString(H2TokenKind op);

int EmitHexByte(H2Buf* b, uint8_t value);

int BufAppendHexU64Literal(H2Buf* b, uint64_t value);

int EmitStringLiteralValue(H2CBackendC* c, int32_t literalId, int writable);

int EmitStringLiteralPool(H2CBackendC* c);

int IsStrBaseName(const char* _Nullable s);

int TypeRefIsStr(const H2TypeRef* t);

int TypeRefContainerWritable(const H2TypeRef* t);

int EmitElementTypeName(H2CBackendC* c, const H2TypeRef* t, int asConst);

int EmitLenExprFromType(H2CBackendC* c, int32_t exprNode, const H2TypeRef* t);

int EmitElemPtrExpr(
    H2CBackendC* c, int32_t baseNode, const H2TypeRef* baseType, int wantWritableElem);

int EmitSliceExpr(H2CBackendC* c, int32_t nodeId);

int TypeRefIsPointerLike(const H2TypeRef* t);
int TypeRefIsPointerBackedOptional(const H2TypeRef* t);
int TypeRefIsTaggedOptional(const H2TypeRef* t);
int TypeRefLowerForStorage(H2CBackendC* c, const H2TypeRef* type, H2TypeRef* outType);

int TypeRefIsOwnedRuntimeArrayStruct(const H2TypeRef* t);

int TypeRefIsNamedDeclKind(const H2CBackendC* c, const H2TypeRef* t, H2AstKind wantKind);

int TypeRefDerefReadonlyRefLike(const H2TypeRef* in, H2TypeRef* outBase);

int ResolveComparisonHookArgCost(
    H2CBackendC*     c,
    const H2TypeRef* paramType,
    const H2TypeRef* argType,
    uint8_t*         outCost,
    int*             outAutoRef);

int ResolveComparisonHook(
    H2CBackendC*     c,
    const char*      hookName,
    const H2TypeRef* lhsType,
    const H2TypeRef* rhsType,
    const H2FnSig**  outSig,
    const char**     outCalleeName,
    int              outAutoRef[2]);

int EmitExprAutoRefCoerced(H2CBackendC* c, int32_t argNode, const H2TypeRef* paramType);

int EmitExprAsSliceRO(H2CBackendC* c, int32_t exprNode, const H2TypeRef* exprType);

int EmitComparisonHookCall(
    H2CBackendC*   c,
    const H2FnSig* sig,
    const char*    calleeName,
    int32_t        lhsNode,
    int32_t        rhsNode,
    const int      autoRef[2]);

int EmitPointerIdentityExpr(H2CBackendC* c, int32_t exprNode, const H2TypeRef* exprType);

int EmitNewAllocArgExpr(H2CBackendC* c, int32_t allocArg);

const char* _Nullable ResolveVarSizeValueBaseName(H2CBackendC* c, const H2TypeRef* valueType);

int EmitConcatCallExpr(H2CBackendC* c, int32_t calleeNode);

uint32_t FmtNextTempId(H2CBackendC* c);

int TypeRefIsSignedIntegerLike(const H2CBackendC* c, const H2TypeRef* t);

int TypeRefIsIntegerLike(const H2CBackendC* c, const H2TypeRef* t);

int TypeRefIsBoolLike(const H2CBackendC* c, const H2TypeRef* t);

int TypeRefIsNamedEnumLike(const H2CBackendC* c, const H2TypeRef* t);

int TypeRefIsFmtStringLike(const H2CBackendC* c, const H2TypeRef* t);

int TypeRefIsFmtValueType(const H2CBackendC* c, const H2TypeRef* t);

int EmitFmtAppendLiteralBytes(
    H2CBackendC* c, const char* builderName, const uint8_t* bytes, uint32_t len);

int EmitFmtAppendLiteralText(
    H2CBackendC* c, const char* builderName, const char* text, uint32_t len);

int EmitFmtBuilderInitStmt(H2CBackendC* c, const char* builderName, int32_t allocArgNode);

char* _Nullable FmtMakeExprField(H2CBackendC* c, const char* baseExpr, const char* fieldName);

char* _Nullable FmtMakeExprIndex(H2CBackendC* c, const char* baseExpr, const char* indexExpr);

int EmitFmtAppendReflectArray(
    H2CBackendC*     c,
    const char*      builderName,
    const char*      expr,
    const H2TypeRef* type,
    uint32_t         depth);

int EmitFmtAppendReflectSlice(
    H2CBackendC*     c,
    const char*      builderName,
    const char*      expr,
    const H2TypeRef* type,
    uint32_t         depth);

int EmitFmtAppendReflectStruct(
    H2CBackendC*     c,
    const char*      builderName,
    const char*      expr,
    const H2TypeRef* type,
    uint32_t         depth);

int EmitFmtAppendReflectExpr(
    H2CBackendC*     c,
    const char*      builderName,
    const char*      expr,
    const H2TypeRef* type,
    uint32_t         depth);

int EmitExprCoerceFmtValue(
    H2CBackendC* c, int32_t exprNode, const H2TypeRef* srcType, const H2TypeRef* dstType);

int EmitFreeCallExpr(H2CBackendC* c, int32_t allocArgNode, int32_t valueNode);

int EmitNewExpr(
    H2CBackendC* c, int32_t nodeId, const H2TypeRef* _Nullable dstType, int requireNonNull);

int EmitExprCoerced(H2CBackendC* c, int32_t exprNode, const H2TypeRef* _Nullable dstType);

int32_t ActiveCallOverlayNode(const H2CBackendC* c);

int32_t FindActiveOverlayBindByName(const H2CBackendC* c, const char* fieldName);

int EmitCurrentContextFieldRaw(H2CBackendC* c, const char* fieldName);

int EmitCurrentContextFieldValue(
    H2CBackendC* c, const char* fieldName, const H2TypeRef* requiredType);

int EmitEffectiveContextFieldValue(
    H2CBackendC* c, const char* fieldName, const H2TypeRef* requiredType);

int EmitContextArgForSig(H2CBackendC* c, const H2FnSig* sig);

int EmitResolvedCall(
    H2CBackendC*          c,
    int32_t               callNode,
    const char*           calleeName,
    const H2FnSig*        sig,
    const H2CCallBinding* binding,
    int                   autoRefFirstArg);

int EmitFieldPathLValue(
    H2CBackendC* c, const char* base, const H2FieldInfo* const* path, uint32_t pathLen);

int EmitCompoundFieldValueCoerced(
    H2CBackendC* c, const H2AstNode* field, int32_t exprNode, const H2TypeRef* _Nullable dstType);

int EmitEnumVariantCompoundLiteral(
    H2CBackendC*     c,
    int32_t          nodeId,
    int32_t          firstField,
    const char*      enumTypeName,
    uint32_t         variantStart,
    uint32_t         variantEnd,
    const H2TypeRef* valueType);

int EmitCompoundLiteralDesignated(
    H2CBackendC* c, int32_t firstField, const char* ownerType, const H2TypeRef* valueType);

int EmitCompoundLiteralOrderedStruct(
    H2CBackendC* c, int32_t firstField, const char* ownerType, const H2TypeRef* valueType);

int StructHasFieldDefaults(const H2CBackendC* c, const char* ownerType);

int EmitCompoundLiteral(H2CBackendC* c, int32_t nodeId, const H2TypeRef* _Nullable expectedType);

int EmitExpr_IDENT(H2CBackendC* c, int32_t nodeId, const H2AstNode* n);

int EmitExpr_INT(H2CBackendC* c, int32_t nodeId, const H2AstNode* n);

int EmitExpr_RUNE(H2CBackendC* c, int32_t nodeId, const H2AstNode* n);

int EmitExpr_FLOAT(H2CBackendC* c, int32_t nodeId, const H2AstNode* n);

int EmitExpr_BOOL(H2CBackendC* c, int32_t nodeId, const H2AstNode* n);

int EmitExpr_COMPOUND_LIT(H2CBackendC* c, int32_t nodeId, const H2AstNode* n);

int EmitExpr_STRING(H2CBackendC* c, int32_t nodeId, const H2AstNode* n);

int EmitExpr_UNARY(H2CBackendC* c, int32_t nodeId, const H2AstNode* n);

int EmitExpr_BINARY(H2CBackendC* c, int32_t nodeId, const H2AstNode* n);

int EmitExpr_CALL_WITH_CONTEXT(H2CBackendC* c, int32_t nodeId, const H2AstNode* n);

int EmitExpr_CALL(H2CBackendC* c, int32_t nodeId, const H2AstNode* n);

int EmitExpr_NEW(H2CBackendC* c, int32_t nodeId, const H2AstNode* n);

int EmitExpr_INDEX(H2CBackendC* c, int32_t nodeId, const H2AstNode* n);

int EmitExpr_FIELD_EXPR(H2CBackendC* c, int32_t nodeId, const H2AstNode* n);

int EmitExpr_CAST(H2CBackendC* c, int32_t nodeId, const H2AstNode* n);

int EmitExpr_SIZEOF(H2CBackendC* c, int32_t nodeId, const H2AstNode* n);

int EmitExpr_NULL(H2CBackendC* c, int32_t nodeId, const H2AstNode* n);

int EmitExpr_UNWRAP(H2CBackendC* c, int32_t nodeId, const H2AstNode* n);

int EmitExpr_CALL_ARG(H2CBackendC* c, int32_t nodeId, const H2AstNode* n);

int EmitExpr_TUPLE_EXPR(H2CBackendC* c, int32_t nodeId, const H2AstNode* n);

int EmitExpr(H2CBackendC* c, int32_t nodeId);

int EmitDeferredRange(H2CBackendC* c, uint32_t start, uint32_t depth);

int EmitBlockImpl(H2CBackendC* c, int32_t nodeId, uint32_t depth, int inlineOpen);

int EmitBlock(H2CBackendC* c, int32_t nodeId, uint32_t depth);

int EmitBlockInline(H2CBackendC* c, int32_t nodeId, uint32_t depth);

int EmitVarLikeStmt(H2CBackendC* c, int32_t nodeId, uint32_t depth, int isConst);

int EmitMultiAssignStmt(H2CBackendC* c, int32_t nodeId, uint32_t depth);

int EmitShortAssignStmt(H2CBackendC* c, int32_t nodeId, uint32_t depth);

int EmitForStmt(H2CBackendC* c, int32_t nodeId, uint32_t depth);

int EmitSwitchStmt(H2CBackendC* c, int32_t nodeId, uint32_t depth);

int EmitAssertFormatArg(H2CBackendC* c, int32_t nodeId);

int EmitStmt(H2CBackendC* c, int32_t nodeId, uint32_t depth);

int IsMainFunctionNode(const H2CBackendC* c, int32_t nodeId);

int IsExplicitlyExportedNode(const H2CBackendC* c, int32_t nodeId);

int IsExportedNode(const H2CBackendC* c, int32_t nodeId);

int IsExportedTypeNode(const H2CBackendC* c, int32_t nodeId);

int EmitEnumDecl(H2CBackendC* c, int32_t nodeId, uint32_t depth);

int NodeHasDirectDependentFields(H2CBackendC* c, int32_t nodeId);

int EmitVarSizeStructDecl(H2CBackendC* c, int32_t nodeId, uint32_t depth);

int EmitStructOrUnionDecl(H2CBackendC* c, int32_t nodeId, uint32_t depth, int isUnion);

int EmitForwardTypeDecls(H2CBackendC* c);

int EmitForwardAnonTypeDecls(H2CBackendC* c);

int EmitAnonTypeDecls(H2CBackendC* c);

int EmitHeaderTypeAliasDecls(H2CBackendC* c);

int EmitFnTypeAliasDecls(H2CBackendC* c);

int FnNodeHasBody(const H2CBackendC* c, int32_t nodeId);

int HasFunctionBodyForName(const H2CBackendC* c, int32_t nodeId);

int EmitFnDeclOrDef(
    H2CBackendC* c,
    int32_t      nodeId,
    uint32_t     depth,
    int          emitBody,
    int          isPrivate,
    const H2FnSig* _Nullable forcedSig);

int EmitConstDecl(
    H2CBackendC* c, int32_t nodeId, uint32_t depth, int declarationOnly, int isPrivate);

int EmitVarDecl(H2CBackendC* c, int32_t nodeId, uint32_t depth, int declarationOnly, int isPrivate);

int EmitTypeAliasDecl(
    H2CBackendC* c, int32_t nodeId, uint32_t depth, int declarationOnly, int isPrivate);

int EmitDeclNode(
    H2CBackendC* c,
    int32_t      nodeId,
    uint32_t     depth,
    int          declarationOnly,
    int          isPrivate,
    int          emitBody);

int EmitPrelude(H2CBackendC* c);

char* _Nullable BuildDefaultMacro(H2CBackendC* c, const char* pkgName, const char* suffix);

int EmitHeader(H2CBackendC* c);

int ShouldEmitDeclNode(const H2CBackendC* c, int32_t nodeId);

int InitAst(H2CBackendC* c);

char* _Nullable AllocOutputCopy(H2CBackendC* c);

void FreeContext(H2CBackendC* c);

int EmitCBackend(
    const H2CodegenBackend* backend,
    const H2CodegenUnit*    unit,
    const H2CodegenOptions* _Nullable options,
    H2CodegenArtifact* _Nonnull outArtifact,
    H2Diag* _Nullable diag);

H2_API_END
