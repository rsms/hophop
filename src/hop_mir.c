#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
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
#include "typecheck/internal.h"

HOP_API_BEGIN

static int EnsureMirFunctionRefTypeRef(
    HOPArena* arena, HOPMirProgram* program, uint32_t functionIndex, uint32_t* _Nonnull outTypeRef);

static int FindFunctionBodyNode(const HOPParsedFile* file, int32_t fnNode) {
    int32_t child = ASTFirstChild(&file->ast, fnNode);
    while (child >= 0) {
        if (file->ast.nodes[child].kind == HOPAst_BLOCK) {
            return child;
        }
        child = ASTNextSibling(&file->ast, child);
    }
    return -1;
}

static int ErrorMirUnsupported(
    const HOPParsedFile* file,
    const HOPAstNode*    node,
    const char*          kind,
    const HOPDiag* _Nullable diag) {
    uint32_t    start = node != NULL ? node->start : 0u;
    uint32_t    end = node != NULL ? node->end : 0u;
    const char* detail = diag != NULL && diag->detail != NULL ? diag->detail : "not supported";
    if (diag != NULL && diag->end > diag->start) {
        start = diag->start;
        end = diag->end;
    }
    return Errorf(
        file->path,
        file->source,
        start,
        end,
        "MIR lowering does not yet support %s: %s",
        kind,
        detail);
}

typedef enum {
    HOPMirDeclKind_NONE = 0,
    HOPMirDeclKind_FN,
    HOPMirDeclKind_CONST,
    HOPMirDeclKind_VAR,
} HOPMirDeclKind;

typedef struct {
    const HOPPackage* pkg;
    const char*       src;
    uint32_t          nameStart;
    uint32_t          nameEnd;
    uint32_t          functionIndex;
    uint8_t           kind;
} HOPMirResolvedDecl;

typedef struct {
    HOPMirResolvedDecl* _Nullable v;
    uint32_t len;
    uint32_t cap;
} HOPMirResolvedDeclMap;

typedef struct {
    const HOPPackage*    pkg;
    const HOPParsedFile* file;
    uint32_t             tcFnIndex;
    uint32_t             mirFnIndex;
} HOPMirTcFunctionDecl;

typedef struct {
    HOPMirTcFunctionDecl* _Nullable v;
    uint32_t len;
    uint32_t cap;
} HOPMirTcFunctionMap;

static const HOPSymbolDecl* _Nullable FindPackageTypeDeclBySlice(
    const HOPPackage* pkg, const char* src, uint32_t start, uint32_t end);
static int PatchMirFunctionTypeRefsFromTC(
    HOPArena*               arena,
    const HOPPackageLoader* loader,
    HOPMirProgramBuilder*   builder,
    const HOPPackage*       pkg,
    const HOPParsedFile*    file,
    const HOPTypeCheckCtx*  tc,
    uint32_t                tcFnIndex,
    uint32_t                mirFnIndex);

static int AddMirResolvedDecl(
    HOPMirResolvedDeclMap* map,
    const HOPPackage*      pkg,
    const char*            src,
    uint32_t               nameStart,
    uint32_t               nameEnd,
    uint32_t               functionIndex,
    HOPMirDeclKind         kind) {
    if (map == NULL || pkg == NULL || src == NULL || nameEnd <= nameStart
        || kind == HOPMirDeclKind_NONE)
    {
        return -1;
    }
    if (EnsureCap((void**)&map->v, &map->cap, map->len + 1u, sizeof(HOPMirResolvedDecl)) != 0) {
        return -1;
    }
    map->v[map->len++] = (HOPMirResolvedDecl){
        .pkg = pkg,
        .src = src,
        .nameStart = nameStart,
        .nameEnd = nameEnd,
        .functionIndex = functionIndex,
        .kind = (uint8_t)kind,
    };
    return 0;
}

static int AddMirTcFunctionDecl(
    HOPMirTcFunctionMap* map,
    const HOPPackage*    pkg,
    const HOPParsedFile* file,
    uint32_t             tcFnIndex,
    uint32_t             mirFnIndex) {
    uint32_t i;
    if (map == NULL || pkg == NULL || file == NULL || tcFnIndex == UINT32_MAX
        || mirFnIndex == UINT32_MAX)
    {
        return -1;
    }
    for (i = 0; i < map->len; i++) {
        if (map->v[i].pkg == pkg && map->v[i].file == file && map->v[i].tcFnIndex == tcFnIndex) {
            map->v[i].mirFnIndex = mirFnIndex;
            return 0;
        }
    }
    if (EnsureCap((void**)&map->v, &map->cap, map->len + 1u, sizeof(HOPMirTcFunctionDecl)) != 0) {
        return -1;
    }
    map->v[map->len++] = (HOPMirTcFunctionDecl){
        .pkg = pkg,
        .file = file,
        .tcFnIndex = tcFnIndex,
        .mirFnIndex = mirFnIndex,
    };
    return 0;
}

static int FindMirTcFunctionDecl(
    const HOPMirTcFunctionMap* map,
    const HOPPackage*          pkg,
    const HOPParsedFile*       file,
    uint32_t                   tcFnIndex,
    uint32_t* _Nonnull outMirFnIndex) {
    uint32_t i;
    if (outMirFnIndex != NULL) {
        *outMirFnIndex = UINT32_MAX;
    }
    if (map == NULL || pkg == NULL || file == NULL || outMirFnIndex == NULL) {
        return 0;
    }
    for (i = 0; i < map->len; i++) {
        if (map->v[i].pkg == pkg && map->v[i].file == file && map->v[i].tcFnIndex == tcFnIndex) {
            *outMirFnIndex = map->v[i].mirFnIndex;
            return 1;
        }
    }
    return 0;
}

static int FindMirTcFunctionByMirIndex(
    const HOPMirTcFunctionMap* map,
    const HOPPackage*          pkg,
    const HOPParsedFile*       file,
    uint32_t                   mirFnIndex,
    uint32_t* _Nonnull outTcFnIndex) {
    uint32_t i;
    if (outTcFnIndex != NULL) {
        *outTcFnIndex = UINT32_MAX;
    }
    if (map == NULL || pkg == NULL || file == NULL || outTcFnIndex == NULL) {
        return 0;
    }
    for (i = 0; i < map->len; i++) {
        if (map->v[i].pkg == pkg && map->v[i].file == file && map->v[i].mirFnIndex == mirFnIndex) {
            *outTcFnIndex = map->v[i].tcFnIndex;
            return 1;
        }
    }
    return 0;
}

static int32_t FindTypecheckFunctionByDeclNode(
    const HOPTypeCheckCtx* tc, int32_t declNode, int wantTemplateInstance) {
    uint32_t i;
    if (tc == NULL || declNode < 0) {
        return -1;
    }
    for (i = 0; i < tc->funcLen; i++) {
        const HOPTCFunction* fn = &tc->funcs[i];
        int isInstance = (fn->flags & HOPTCFunctionFlag_TEMPLATE_INSTANCE) != 0 ? 1 : 0;
        if (fn->declNode == declNode && isInstance == wantTemplateInstance) {
            return (int32_t)i;
        }
    }
    return -1;
}

static int MirNameIsBuiltinTypeValue(const char* src, uint32_t start, uint32_t end) {
    return SliceEqCStr(src, start, end, "bool") || SliceEqCStr(src, start, end, "str")
        || SliceEqCStr(src, start, end, "type") || SliceEqCStr(src, start, end, "u8")
        || SliceEqCStr(src, start, end, "u16") || SliceEqCStr(src, start, end, "u32")
        || SliceEqCStr(src, start, end, "u64") || SliceEqCStr(src, start, end, "i8")
        || SliceEqCStr(src, start, end, "i16") || SliceEqCStr(src, start, end, "i32")
        || SliceEqCStr(src, start, end, "i64") || SliceEqCStr(src, start, end, "usize")
        || SliceEqCStr(src, start, end, "isize") || SliceEqCStr(src, start, end, "rawptr")
        || SliceEqCStr(src, start, end, "f32") || SliceEqCStr(src, start, end, "f64")
        || SliceEqCStr(src, start, end, "int") || SliceEqCStr(src, start, end, "uint");
}

static int MirTypecheckFunctionHasTypeParamName(
    const HOPTypeCheckCtx* tc, uint32_t tcFnIndex, const char* src, uint32_t start, uint32_t end) {
    const HOPTCFunction* fn;
    int32_t              declNode;
    int32_t              child;
    if (tc == NULL || src == NULL || tcFnIndex >= tc->funcLen) {
        return 0;
    }
    fn = &tc->funcs[tcFnIndex];
    declNode = fn->declNode;
    if (declNode < 0 || (uint32_t)declNode >= tc->ast->len) {
        return 0;
    }
    child = tc->ast->nodes[declNode].firstChild;
    while (child >= 0) {
        const HOPAstNode* n = &tc->ast->nodes[child];
        if (n->kind == HOPAst_TYPE_PARAM
            && HOPNameEqSlice(tc->src, n->dataStart, n->dataEnd, start, end))
        {
            return 1;
        }
        child = n->nextSibling;
    }
    return 0;
}

static int MirNameIsTypeValue(
    const HOPPackage*      pkg,
    const HOPTypeCheckCtx* tc,
    uint32_t               tcFnIndex,
    const char*            src,
    uint32_t               start,
    uint32_t               end) {
    if (src == NULL || end <= start) {
        return 0;
    }
    if (MirNameIsBuiltinTypeValue(src, start, end)) {
        return 1;
    }
    if (pkg != NULL && FindPackageTypeDeclBySlice(pkg, src, start, end) != NULL) {
        return 1;
    }
    return MirTypecheckFunctionHasTypeParamName(tc, tcFnIndex, src, start, end);
}

static int MirBuiltinTypeRefFromTC(
    const HOPTypeCheckCtx* tc, int32_t typeId, HOPMirTypeRef* outTypeRef) {
    const HOPTCType* t;
    if (tc == NULL || outTypeRef == NULL || typeId < 0 || (uint32_t)typeId >= tc->typeLen) {
        return 0;
    }
    t = &tc->types[typeId];
    if (t->kind != HOPTCType_BUILTIN) {
        return 0;
    }
    *outTypeRef = (HOPMirTypeRef){ .astNode = UINT32_MAX, .sourceRef = UINT32_MAX };
    switch (t->builtin) {
        case HOPBuiltin_BOOL:
            outTypeRef->flags = HOPMirTypeScalar_I32;
            outTypeRef->aux = HOPMirTypeAuxMakeScalarInt(HOPMirIntKind_BOOL);
            return 1;
        case HOPBuiltin_U8:
            outTypeRef->flags = HOPMirTypeScalar_I32;
            outTypeRef->aux = HOPMirTypeAuxMakeScalarInt(HOPMirIntKind_U8);
            return 1;
        case HOPBuiltin_I8:
            outTypeRef->flags = HOPMirTypeScalar_I32;
            outTypeRef->aux = HOPMirTypeAuxMakeScalarInt(HOPMirIntKind_I8);
            return 1;
        case HOPBuiltin_U16:
            outTypeRef->flags = HOPMirTypeScalar_I32;
            outTypeRef->aux = HOPMirTypeAuxMakeScalarInt(HOPMirIntKind_U16);
            return 1;
        case HOPBuiltin_I16:
            outTypeRef->flags = HOPMirTypeScalar_I32;
            outTypeRef->aux = HOPMirTypeAuxMakeScalarInt(HOPMirIntKind_I16);
            return 1;
        case HOPBuiltin_U32:
            outTypeRef->flags = HOPMirTypeScalar_I32;
            outTypeRef->aux = HOPMirTypeAuxMakeScalarInt(HOPMirIntKind_U32);
            return 1;
        case HOPBuiltin_I32:
        case HOPBuiltin_USIZE:
        case HOPBuiltin_ISIZE:
            outTypeRef->flags = HOPMirTypeScalar_I32;
            outTypeRef->aux = HOPMirTypeAuxMakeScalarInt(HOPMirIntKind_I32);
            return 1;
        case HOPBuiltin_U64:
        case HOPBuiltin_I64:    outTypeRef->flags = HOPMirTypeScalar_I64; return 1;
        case HOPBuiltin_F32:    outTypeRef->flags = HOPMirTypeScalar_F32; return 1;
        case HOPBuiltin_F64:    outTypeRef->flags = HOPMirTypeScalar_F64; return 1;
        case HOPBuiltin_TYPE:
        case HOPBuiltin_RAWPTR: outTypeRef->flags = HOPMirTypeFlag_OPAQUE_PTR; return 1;
        default:                return 0;
    }
}

static int MirAstNodeIsTypeNodeForSearch(HOPAstKind kind) {
    return kind == HOPAst_TYPE_NAME || kind == HOPAst_TYPE_PTR || kind == HOPAst_TYPE_REF
        || kind == HOPAst_TYPE_MUTREF || kind == HOPAst_TYPE_ARRAY || kind == HOPAst_TYPE_VARRAY
        || kind == HOPAst_TYPE_SLICE || kind == HOPAst_TYPE_MUTSLICE || kind == HOPAst_TYPE_OPTIONAL
        || kind == HOPAst_TYPE_FN || kind == HOPAst_TYPE_TUPLE || kind == HOPAst_TYPE_ANON_STRUCT
        || kind == HOPAst_TYPE_ANON_UNION;
}

static int32_t MirFunctionTypeParamIndexByTypeNode(
    const HOPAst* ast, HOPStrView src, int32_t fnNode, int32_t typeNode) {
    int32_t child;
    int32_t index = 0;
    if (ast == NULL || fnNode < 0 || typeNode < 0 || (uint32_t)fnNode >= ast->len
        || (uint32_t)typeNode >= ast->len || ast->nodes[typeNode].kind != HOPAst_TYPE_NAME
        || ast->nodes[typeNode].firstChild >= 0)
    {
        return -1;
    }
    child = ast->nodes[fnNode].firstChild;
    while (child >= 0) {
        if (ast->nodes[child].kind == HOPAst_TYPE_PARAM) {
            if (HOPNameEqSlice(
                    src,
                    ast->nodes[child].dataStart,
                    ast->nodes[child].dataEnd,
                    ast->nodes[typeNode].dataStart,
                    ast->nodes[typeNode].dataEnd))
            {
                return index;
            }
            index++;
        }
        child = ast->nodes[child].nextSibling;
    }
    return -1;
}

static int EnsureMirBuilderTypeRefFromTC(
    HOPMirProgramBuilder*  builder,
    const HOPTypeCheckCtx* tc,
    uint32_t               sourceRef,
    int32_t                typeId,
    uint32_t*              outTypeRef) {
    uint32_t      i;
    HOPMirTypeRef typeRef = { 0 };
    if (outTypeRef != NULL) {
        *outTypeRef = UINT32_MAX;
    }
    if (builder == NULL || tc == NULL || outTypeRef == NULL || typeId < 0) {
        return 0;
    }
    typeId = HOPTCResolveAliasBaseType((HOPTypeCheckCtx*)tc, typeId);
    if (typeId < 0 || (uint32_t)typeId >= tc->typeLen) {
        return 0;
    }
    if (MirBuiltinTypeRefFromTC(tc, typeId, &typeRef)) {
        return HOPMirProgramBuilderAddType(builder, &typeRef, outTypeRef) == 0 ? 1 : -1;
    }
    for (i = 0; i < tc->ast->len; i++) {
        int32_t resolved = -1;
        if (!MirAstNodeIsTypeNodeForSearch(tc->ast->nodes[i].kind)) {
            continue;
        }
        if (HOPTCResolveTypeNode((HOPTypeCheckCtx*)tc, (int32_t)i, &resolved) == 0
            && resolved == typeId)
        {
            typeRef = (HOPMirTypeRef){ .astNode = i, .sourceRef = sourceRef };
            return HOPMirProgramBuilderAddType(builder, &typeRef, outTypeRef) == 0 ? 1 : -1;
        }
    }
    return 0;
}

static int EnsureMirBuilderBuiltinTypeRefFromTC(
    HOPMirProgramBuilder*  builder,
    const HOPTypeCheckCtx* tc,
    int32_t                typeId,
    uint32_t*              outTypeRef) {
    HOPMirTypeRef typeRef = { 0 };
    if (outTypeRef != NULL) {
        *outTypeRef = UINT32_MAX;
    }
    if (builder == NULL || tc == NULL || outTypeRef == NULL || typeId < 0) {
        return 0;
    }
    typeId = HOPTCResolveAliasBaseType((HOPTypeCheckCtx*)tc, typeId);
    if (typeId < 0 || (uint32_t)typeId >= tc->typeLen) {
        return 0;
    }
    if (!MirBuiltinTypeRefFromTC(tc, typeId, &typeRef)) {
        return 0;
    }
    return HOPMirProgramBuilderAddType(builder, &typeRef, outTypeRef) == 0 ? 1 : -1;
}

static int32_t MirFindTCNamedTypeIndex(const HOPTypeCheckCtx* tc, int32_t typeId) {
    uint32_t i;
    if (tc == NULL || typeId < 0) {
        return -1;
    }
    for (i = 0; i < tc->namedTypeLen; i++) {
        if (tc->namedTypes[i].typeId == typeId) {
            return (int32_t)i;
        }
    }
    return -1;
}

static int MirBuiltinTypeNodeMatchesTC(const HOPTypeCheckCtx* tc, int32_t typeId, int32_t nodeId) {
    const HOPAstNode* n;
    if (tc == NULL || typeId < 0 || (uint32_t)typeId >= tc->typeLen || nodeId < 0
        || (uint32_t)nodeId >= tc->ast->len || tc->types[typeId].kind != HOPTCType_BUILTIN)
    {
        return 0;
    }
    n = &tc->ast->nodes[nodeId];
    if (n->kind != HOPAst_TYPE_NAME || n->firstChild >= 0) {
        return 0;
    }
    switch (tc->types[typeId].builtin) {
        case HOPBuiltin_BOOL: return SliceEqCStr(tc->src.ptr, n->dataStart, n->dataEnd, "bool");
        case HOPBuiltin_U8:   return SliceEqCStr(tc->src.ptr, n->dataStart, n->dataEnd, "u8");
        case HOPBuiltin_U16:  return SliceEqCStr(tc->src.ptr, n->dataStart, n->dataEnd, "u16");
        case HOPBuiltin_U32:  return SliceEqCStr(tc->src.ptr, n->dataStart, n->dataEnd, "u32");
        case HOPBuiltin_U64:  return SliceEqCStr(tc->src.ptr, n->dataStart, n->dataEnd, "u64");
        case HOPBuiltin_I8:   return SliceEqCStr(tc->src.ptr, n->dataStart, n->dataEnd, "i8");
        case HOPBuiltin_I16:  return SliceEqCStr(tc->src.ptr, n->dataStart, n->dataEnd, "i16");
        case HOPBuiltin_I32:
            return SliceEqCStr(tc->src.ptr, n->dataStart, n->dataEnd, "i32")
                || SliceEqCStr(tc->src.ptr, n->dataStart, n->dataEnd, "int");
        case HOPBuiltin_I64:    return SliceEqCStr(tc->src.ptr, n->dataStart, n->dataEnd, "i64");
        case HOPBuiltin_USIZE:  return SliceEqCStr(tc->src.ptr, n->dataStart, n->dataEnd, "usize");
        case HOPBuiltin_ISIZE:  return SliceEqCStr(tc->src.ptr, n->dataStart, n->dataEnd, "isize");
        case HOPBuiltin_TYPE:   return SliceEqCStr(tc->src.ptr, n->dataStart, n->dataEnd, "type");
        case HOPBuiltin_RAWPTR: return SliceEqCStr(tc->src.ptr, n->dataStart, n->dataEnd, "rawptr");
        case HOPBuiltin_F32:    return SliceEqCStr(tc->src.ptr, n->dataStart, n->dataEnd, "f32");
        case HOPBuiltin_F64:    return SliceEqCStr(tc->src.ptr, n->dataStart, n->dataEnd, "f64");
        default:                return 0;
    }
}

static int MirTypeNodeMatchesTCType(const HOPTypeCheckCtx* tc, int32_t typeId, int32_t nodeId) {
    int32_t  namedIndex;
    int32_t  child;
    uint16_t argIndex;
    if (tc == NULL || typeId < 0 || (uint32_t)typeId >= tc->typeLen || nodeId < 0
        || (uint32_t)nodeId >= tc->ast->len)
    {
        return 0;
    }
    if (tc->types[typeId].kind == HOPTCType_BUILTIN) {
        return MirBuiltinTypeNodeMatchesTC(tc, typeId, nodeId);
    }
    if (tc->types[typeId].kind != HOPTCType_NAMED) {
        return 0;
    }
    if (tc->ast->nodes[nodeId].kind != HOPAst_TYPE_NAME) {
        return 0;
    }
    if (!HOPNameEqSlice(
            tc->src,
            tc->types[typeId].nameStart,
            tc->types[typeId].nameEnd,
            tc->ast->nodes[nodeId].dataStart,
            tc->ast->nodes[nodeId].dataEnd))
    {
        return 0;
    }
    namedIndex = MirFindTCNamedTypeIndex(tc, typeId);
    if (namedIndex < 0) {
        return tc->ast->nodes[nodeId].firstChild < 0;
    }
    child = tc->ast->nodes[nodeId].firstChild;
    for (argIndex = 0; argIndex < tc->namedTypes[(uint32_t)namedIndex].templateArgCount; argIndex++)
    {
        if (child < 0
            || !MirTypeNodeMatchesTCType(
                tc,
                tc->genericArgTypes
                    [tc->namedTypes[(uint32_t)namedIndex].templateArgStart + argIndex],
                child))
        {
            return 0;
        }
        child = tc->ast->nodes[child].nextSibling;
    }
    return child < 0;
}

static int EnsureMirBuilderNamedTypeRefFromTC(
    HOPMirProgramBuilder*  builder,
    const HOPTypeCheckCtx* tc,
    uint32_t               sourceRef,
    int32_t                typeId,
    uint32_t*              outTypeRef) {
    uint32_t      i;
    HOPMirTypeRef typeRef = { 0 };
    if (outTypeRef != NULL) {
        *outTypeRef = UINT32_MAX;
    }
    if (builder == NULL || tc == NULL || outTypeRef == NULL || typeId < 0
        || (uint32_t)typeId >= tc->typeLen || tc->types[typeId].kind != HOPTCType_NAMED)
    {
        return 0;
    }
    for (i = 0; i < tc->ast->len; i++) {
        if (!MirAstNodeIsTypeNodeForSearch(tc->ast->nodes[i].kind)) {
            continue;
        }
        if (MirTypeNodeMatchesTCType(tc, typeId, (int32_t)i)) {
            typeRef = (HOPMirTypeRef){ .astNode = i, .sourceRef = sourceRef };
            return HOPMirProgramBuilderAddType(builder, &typeRef, outTypeRef) == 0 ? 1 : -1;
        }
    }
    return 0;
}

static int PatchMirFunctionTypeRefsFromTC(
    HOPArena*               arena,
    const HOPPackageLoader* loader,
    HOPMirProgramBuilder*   builder,
    const HOPPackage*       pkg,
    const HOPParsedFile*    file,
    const HOPTypeCheckCtx*  tc,
    uint32_t                tcFnIndex,
    uint32_t                mirFnIndex) {
    uint32_t sourceRef;
    uint32_t localStart;
    uint32_t paramCount;
    uint32_t tcParamCount;
    uint32_t tcParamTypeStart;
    uint32_t tcTemplateArgStart;
    uint16_t tcTemplateArgCount;
    int16_t  tcTemplateRootFuncIndex;
    uint32_t tcNameStart;
    uint32_t tcNameEnd;
    int32_t  tcReturnType;
    uint32_t typeRef = UINT32_MAX;
    uint32_t i;
    (void)arena;
    (void)loader;
    (void)pkg;
    (void)file;
    if (builder == NULL || tc == NULL || tcFnIndex >= tc->funcLen || mirFnIndex >= builder->funcLen)
    {
        return 0;
    }
    tcReturnType = tc->funcs[tcFnIndex].returnType;
    tcParamCount = tc->funcs[tcFnIndex].paramCount;
    tcParamTypeStart = tc->funcs[tcFnIndex].paramTypeStart;
    tcTemplateArgStart = tc->funcs[tcFnIndex].templateArgStart;
    tcTemplateArgCount = tc->funcs[tcFnIndex].templateArgCount;
    tcTemplateRootFuncIndex = tc->funcs[tcFnIndex].templateRootFuncIndex;
    tcNameStart = tc->funcs[tcFnIndex].nameStart;
    tcNameEnd = tc->funcs[tcFnIndex].nameEnd;
    sourceRef = builder->funcs[mirFnIndex].sourceRef;
    localStart = builder->funcs[mirFnIndex].localStart;
    paramCount = builder->funcs[mirFnIndex].paramCount;
    if (tcTemplateArgCount > 0 && builder->funcs[mirFnIndex].typeRef < builder->typeLen) {
        const HOPMirTypeRef* retTypeRef = &builder->types[builder->funcs[mirFnIndex].typeRef];
        if (retTypeRef->astNode < tc->ast->len) {
            int32_t paramIndex = MirFunctionTypeParamIndexByTypeNode(
                tc->ast, tc->src, tc->funcs[tcFnIndex].declNode, (int32_t)retTypeRef->astNode);
            if (paramIndex >= 0 && (uint32_t)paramIndex < tcTemplateArgCount) {
                tcReturnType = tc->genericArgTypes[tcTemplateArgStart + (uint32_t)paramIndex];
            }
        }
    }
    if (EnsureMirBuilderBuiltinTypeRefFromTC(builder, tc, tcReturnType, &typeRef) < 0) {
        return -1;
    }
    if (typeRef == UINT32_MAX
        && EnsureMirBuilderNamedTypeRefFromTC(builder, tc, sourceRef, tcReturnType, &typeRef) < 0)
    {
        return -1;
    }
    if (typeRef != UINT32_MAX) {
        builder->funcs[mirFnIndex].typeRef = typeRef;
    }
    for (i = 0; i < tcParamCount && i < paramCount; i++) {
        uint32_t localIndex = localStart + i;
        if (localIndex >= builder->localLen) {
            return -1;
        }
        typeRef = UINT32_MAX;
        {
            int32_t tcParamType = tc->funcParamTypes[tcParamTypeStart + i];
            if (tcTemplateArgCount > 0 && builder->locals[localIndex].typeRef < builder->typeLen) {
                const HOPMirTypeRef* localTypeRef =
                    &builder->types[builder->locals[localIndex].typeRef];
                if (localTypeRef->astNode < tc->ast->len) {
                    int32_t paramIndex = MirFunctionTypeParamIndexByTypeNode(
                        tc->ast,
                        tc->src,
                        tc->funcs[tcFnIndex].declNode,
                        (int32_t)localTypeRef->astNode);
                    if (paramIndex >= 0 && (uint32_t)paramIndex < tcTemplateArgCount) {
                        tcParamType =
                            tc->genericArgTypes[tcTemplateArgStart + (uint32_t)paramIndex];
                    }
                }
            }
            if (EnsureMirBuilderBuiltinTypeRefFromTC(builder, tc, tcParamType, &typeRef) < 0) {
                return -1;
            }
            if (typeRef == UINT32_MAX
                && EnsureMirBuilderNamedTypeRefFromTC(builder, tc, sourceRef, tcParamType, &typeRef)
                       < 0)
            {
                return -1;
            }
        }
        if (typeRef == UINT32_MAX) {
            continue;
        }
        builder->locals[localIndex].typeRef = typeRef;
    }
    return 0;
}

static int32_t FindVarLikeTypeNode(const HOPAst* ast, int32_t nodeId) {
    int32_t child;
    if (ast == NULL || nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    child = ast->nodes[nodeId].firstChild;
    if (child >= 0 && ast->nodes[child].kind == HOPAst_NAME_LIST) {
        child = ast->nodes[child].nextSibling;
    }
    while (child >= 0) {
        if (ast->nodes[child].kind == HOPAst_TYPE_NAME || ast->nodes[child].kind == HOPAst_TYPE_PTR
            || ast->nodes[child].kind == HOPAst_TYPE_REF
            || ast->nodes[child].kind == HOPAst_TYPE_MUTREF
            || ast->nodes[child].kind == HOPAst_TYPE_ARRAY
            || ast->nodes[child].kind == HOPAst_TYPE_VARRAY
            || ast->nodes[child].kind == HOPAst_TYPE_SLICE
            || ast->nodes[child].kind == HOPAst_TYPE_MUTSLICE
            || ast->nodes[child].kind == HOPAst_TYPE_OPTIONAL
            || ast->nodes[child].kind == HOPAst_TYPE_FN
            || ast->nodes[child].kind == HOPAst_TYPE_ANON_STRUCT
            || ast->nodes[child].kind == HOPAst_TYPE_ANON_UNION
            || ast->nodes[child].kind == HOPAst_TYPE_TUPLE)
        {
            return child;
        }
        child = ast->nodes[child].nextSibling;
    }
    return -1;
}

static int32_t FindFnReturnTypeNode(const HOPAst* ast, int32_t fnNode) {
    int32_t child;
    if (ast == NULL || fnNode < 0 || (uint32_t)fnNode >= ast->len) {
        return -1;
    }
    child = ast->nodes[fnNode].firstChild;
    while (child >= 0) {
        if (ast->nodes[child].kind == HOPAst_TYPE_NAME || ast->nodes[child].kind == HOPAst_TYPE_PTR
            || ast->nodes[child].kind == HOPAst_TYPE_REF
            || ast->nodes[child].kind == HOPAst_TYPE_MUTREF
            || ast->nodes[child].kind == HOPAst_TYPE_ARRAY
            || ast->nodes[child].kind == HOPAst_TYPE_VARRAY
            || ast->nodes[child].kind == HOPAst_TYPE_SLICE
            || ast->nodes[child].kind == HOPAst_TYPE_MUTSLICE
            || ast->nodes[child].kind == HOPAst_TYPE_OPTIONAL
            || ast->nodes[child].kind == HOPAst_TYPE_FN
            || ast->nodes[child].kind == HOPAst_TYPE_TUPLE
            || ast->nodes[child].kind == HOPAst_TYPE_ANON_STRUCT
            || ast->nodes[child].kind == HOPAst_TYPE_ANON_UNION)
        {
            return child;
        }
        if (ast->nodes[child].kind == HOPAst_BLOCK) {
            break;
        }
        child = ast->nodes[child].nextSibling;
    }
    return -1;
}

static int DeclHasDirective(
    const HOPParsedFile* file,
    int32_t              nodeId,
    const char*          name,
    int32_t* _Nullable outDirectiveNode) {
    int32_t firstDirective = -1;
    int32_t lastDirective = -1;
    int32_t child;
    if (outDirectiveNode != NULL) {
        *outDirectiveNode = -1;
    }
    if (file == NULL || name == NULL
        || FindAttachedDirectiveRun(&file->ast, nodeId, &firstDirective, &lastDirective) != 0
        || firstDirective < 0)
    {
        return 0;
    }
    child = firstDirective;
    while (child >= 0) {
        if (DirectiveNameEq(file, child, name)) {
            if (outDirectiveNode != NULL) {
                *outDirectiveNode = child;
            }
            return 1;
        }
        if (child == lastDirective) {
            break;
        }
        child = ASTNextSibling(&file->ast, child);
    }
    return 0;
}

static int AppendMirForeignImportDecl(
    HOPMirProgramBuilder* builder,
    HOPArena*             arena,
    const HOPParsedFile*  file,
    int32_t               nodeId,
    uint32_t*             outFunctionIndex) {
    const HOPAst*     ast = &file->ast;
    const HOPAstNode* n = &ast->nodes[nodeId];
    HOPMirSourceRef   sourceRef = { .src = { file->source, file->sourceLen } };
    HOPMirFunction    fn = { 0 };
    uint32_t          sourceIndex = 0;
    int32_t           typeNode = -1;
    int32_t           child;
    if (builder == NULL || arena == NULL || file == NULL || outFunctionIndex == NULL) {
        return -1;
    }
    *outFunctionIndex = UINT32_MAX;
    if (HOPMirProgramBuilderAddSource(builder, &sourceRef, &sourceIndex) != 0) {
        return -1;
    }
    fn.nameStart = n->dataStart;
    fn.nameEnd = n->dataEnd;
    fn.sourceRef = sourceIndex;
    fn.typeRef = UINT32_MAX;
    typeNode =
        n->kind == HOPAst_FN ? FindFnReturnTypeNode(ast, nodeId) : FindVarLikeTypeNode(ast, nodeId);
    if (typeNode >= 0) {
        HOPMirTypeRef typeRef = {
            .astNode = (uint32_t)typeNode,
            .sourceRef = sourceIndex,
            .flags = 0,
            .aux = 0,
        };
        if (HOPMirProgramBuilderAddType(builder, &typeRef, &fn.typeRef) != 0) {
            return -1;
        }
    }
    if (HOPMirProgramBuilderBeginFunction(builder, &fn, outFunctionIndex) != 0) {
        return -1;
    }
    if (n->kind == HOPAst_FN) {
        child = n->firstChild;
        while (child >= 0) {
            if (ast->nodes[child].kind == HOPAst_PARAM) {
                HOPMirLocal   local = { 0 };
                HOPMirTypeRef typeRef = { 0 };
                int32_t       paramTypeNode = ast->nodes[child].firstChild;
                local.typeRef = UINT32_MAX;
                local.flags = HOPMirLocalFlag_PARAM | HOPMirLocalFlag_MUTABLE;
                local.nameStart = ast->nodes[child].dataStart;
                local.nameEnd = ast->nodes[child].dataEnd;
                if (paramTypeNode >= 0) {
                    typeRef.astNode = (uint32_t)paramTypeNode;
                    typeRef.sourceRef = sourceIndex;
                    typeRef.flags = 0;
                    typeRef.aux = 0;
                    if (HOPMirProgramBuilderAddType(builder, &typeRef, &local.typeRef) != 0) {
                        return -1;
                    }
                }
                if (HOPMirProgramBuilderAddLocal(builder, &local, NULL) != 0) {
                    return -1;
                }
                builder->funcs[*outFunctionIndex].paramCount++;
                if ((ast->nodes[child].flags & HOPAstFlag_PARAM_VARIADIC) != 0u) {
                    builder->funcs[*outFunctionIndex].flags |= HOPMirFunctionFlag_VARIADIC;
                }
            }
            child = ast->nodes[child].nextSibling;
        }
    }
    return HOPMirProgramBuilderEndFunction(builder);
}

static int AppendMirPlaybitEntryHookWrapper(
    HOPMirProgramBuilder* builder,
    HOPArena*             arena,
    const HOPParsedFile*  file,
    int32_t               nodeId,
    uint32_t              entryMainFunctionIndex,
    uint32_t*             outFunctionIndex) {
    const HOPAst*     ast;
    const HOPAstNode* n;
    HOPMirFunction    fn = { 0 };
    HOPMirSourceRef   sourceRef = { 0 };
    uint32_t          sourceIndex = 0;
    if (builder == NULL || arena == NULL || file == NULL || outFunctionIndex == NULL || nodeId < 0
        || (uint32_t)nodeId >= file->ast.len)
    {
        return -1;
    }
    *outFunctionIndex = UINT32_MAX;
    ast = &file->ast;
    n = &ast->nodes[nodeId];
    sourceRef.src = (HOPStrView){ file->source, file->sourceLen };
    if (HOPMirProgramBuilderAddSource(builder, &sourceRef, &sourceIndex) != 0) {
        return -1;
    }
    fn.nameStart = n->dataStart;
    fn.nameEnd = n->dataEnd;
    fn.sourceRef = sourceIndex;
    fn.typeRef = UINT32_MAX;
    if (HOPMirProgramBuilderBeginFunction(builder, &fn, outFunctionIndex) != 0) {
        return -1;
    }
    if (HOPMirProgramBuilderAppendInst(
            builder,
            &(HOPMirInst){
                .op = HOPMirOp_CALL_FN,
                .tok = 0u,
                ._reserved = 0u,
                .aux = entryMainFunctionIndex,
                .start = n->start,
                .end = n->end,
            })
            != 0
        || HOPMirProgramBuilderAppendInst(
               builder,
               &(HOPMirInst){
                   .op = HOPMirOp_DROP,
                   .tok = 0u,
                   ._reserved = 0u,
                   .aux = 0u,
                   .start = n->start,
                   .end = n->end,
               })
               != 0
        || HOPMirProgramBuilderAppendInst(
               builder,
               &(HOPMirInst){
                   .op = HOPMirOp_RETURN_VOID,
                   .tok = 0u,
                   ._reserved = 0u,
                   .aux = 0u,
                   .start = n->start,
                   .end = n->end,
               })
               != 0)
    {
        return -1;
    }
    return HOPMirProgramBuilderEndFunction(builder);
}

static const HOPMirResolvedDecl* _Nullable FindMirResolvedDeclBySlice(
    const HOPMirResolvedDeclMap* map,
    const HOPPackage*            pkg,
    const char*                  src,
    uint32_t                     nameStart,
    uint32_t                     nameEnd,
    HOPMirDeclKind               kind) {
    uint32_t i;
    if (map == NULL || pkg == NULL || src == NULL || nameEnd <= nameStart) {
        return NULL;
    }
    for (i = 0; i < map->len; i++) {
        const HOPMirResolvedDecl* entry = &map->v[i];
        if (entry->pkg != pkg || entry->kind != (uint8_t)kind) {
            continue;
        }
        if (SliceEqSlice(entry->src, entry->nameStart, entry->nameEnd, src, nameStart, nameEnd)) {
            return entry;
        }
    }
    return NULL;
}

static const HOPMirResolvedDecl* _Nullable FindMirResolvedDeclByCStr(
    const HOPMirResolvedDeclMap* map,
    const HOPPackage*            pkg,
    const char*                  name,
    HOPMirDeclKind               kind) {
    uint32_t i;
    size_t   nameLen;
    if (map == NULL || pkg == NULL || name == NULL) {
        return NULL;
    }
    nameLen = strlen(name);
    for (i = 0; i < map->len; i++) {
        const HOPMirResolvedDecl* entry = &map->v[i];
        if (entry->pkg != pkg || entry->kind != (uint8_t)kind) {
            continue;
        }
        if ((size_t)(entry->nameEnd - entry->nameStart) == nameLen
            && memcmp(entry->src + entry->nameStart, name, nameLen) == 0)
        {
            return entry;
        }
    }
    return NULL;
}

static const HOPMirResolvedDecl* _Nullable FindMirResolvedValueBySlice(
    const HOPMirResolvedDeclMap* map,
    const HOPPackage*            pkg,
    const char*                  src,
    uint32_t                     nameStart,
    uint32_t                     nameEnd) {
    const HOPMirResolvedDecl* entry = FindMirResolvedDeclBySlice(
        map, pkg, src, nameStart, nameEnd, HOPMirDeclKind_CONST);
    if (entry != NULL) {
        return entry;
    }
    return FindMirResolvedDeclBySlice(map, pkg, src, nameStart, nameEnd, HOPMirDeclKind_VAR);
}

static const HOPMirResolvedDecl* _Nullable FindMirResolvedValueByCStr(
    const HOPMirResolvedDeclMap* map, const HOPPackage* pkg, const char* name) {
    const HOPMirResolvedDecl* entry = FindMirResolvedDeclByCStr(
        map, pkg, name, HOPMirDeclKind_CONST);
    if (entry != NULL) {
        return entry;
    }
    return FindMirResolvedDeclByCStr(map, pkg, name, HOPMirDeclKind_VAR);
}

static int MirAstDeclHasTypeParams(const HOPAst* ast, int32_t nodeId) {
    int32_t child;
    if (ast == NULL || nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return 0;
    }
    child = ast->nodes[nodeId].firstChild;
    while (child >= 0) {
        if (ast->nodes[child].kind == HOPAst_TYPE_PARAM) {
            return 1;
        }
        child = ast->nodes[child].nextSibling;
    }
    return 0;
}

static int AppendMirDeclsFromFile(
    const HOPPackageLoader* loader,
    const HOPPackage*       entryPkg,
    HOPMirProgramBuilder*   builder,
    HOPArena*               arena,
    const HOPPackage*       pkg,
    const HOPParsedFile*    file,
    HOPMirResolvedDeclMap* _Nullable declMap,
    HOPMirTcFunctionMap* _Nullable tcFnMap) {
    int32_t          child = ASTFirstChild(&file->ast, file->ast.root);
    HOPTypeCheckCtx* tc = file->hasTypecheckCtx ? (HOPTypeCheckCtx*)file->typecheckCtx : NULL;
    while (child >= 0) {
        const HOPAstNode* n = &file->ast.nodes[child];
        if (n->kind == HOPAst_FN) {
            uint32_t      outFunctionIndex = UINT32_MAX;
            int32_t       bodyNode;
            int32_t       wasmImportNode = -1;
            HOPDiag       diag = { 0 };
            int           supported = 0;
            HOPStrView    src = { file->source, file->sourceLen };
            const HOPAst* ast = &file->ast;
            if (MirAstDeclHasTypeParams(ast, child)) {
                child = ASTNextSibling(ast, child);
                continue;
            }
            bodyNode = FindFunctionBodyNode(file, child);
            if (bodyNode >= 0) {
                if (HOPMirLowerAppendSimpleFunction(
                        builder,
                        arena,
                        ast,
                        src,
                        child,
                        bodyNode,
                        &outFunctionIndex,
                        &supported,
                        &diag)
                    != 0)
                {
                    return PrintHOPDiagLineCol(file->path, file->source, &diag, 0);
                }
                if (!supported) {
                    return ErrorMirUnsupported(file, &ast->nodes[child], "function body", &diag);
                }
                if (declMap != NULL
                    && AddMirResolvedDecl(
                           declMap,
                           pkg,
                           file->source,
                           n->dataStart,
                           n->dataEnd,
                           outFunctionIndex,
                           HOPMirDeclKind_FN)
                           != 0)
                {
                    return ErrorSimple("out of memory");
                }
                if (tcFnMap != NULL && tc != NULL) {
                    int32_t tcFnIndex = FindTypecheckFunctionByDeclNode(tc, child, 0);
                    if (tcFnIndex >= 0
                        && AddMirTcFunctionDecl(
                               tcFnMap, pkg, file, (uint32_t)tcFnIndex, outFunctionIndex)
                               != 0)
                    {
                        return ErrorSimple("out of memory");
                    }
                }
            } else if (
                loader != NULL && entryPkg != NULL && loader->selectedPlatformPkg == pkg
                && loader->platformTarget != NULL
                && StrEq(loader->platformTarget, HOP_PLAYBIT_PLATFORM_TARGET)
                && SliceEqCStr(file->source, n->dataStart, n->dataEnd, HOP_PLAYBIT_ENTRY_HOOK_NAME))
            {
                const HOPMirResolvedDecl* entryMain =
                    declMap != NULL
                        ? FindMirResolvedDeclByCStr(declMap, entryPkg, "main", HOPMirDeclKind_FN)
                        : NULL;
                uint32_t wrapperFunctionIndex = UINT32_MAX;
                if (entryMain == NULL) {
                    return ErrorSimple("internal error: missing entry main MIR function");
                }
                if (AppendMirPlaybitEntryHookWrapper(
                        builder,
                        arena,
                        file,
                        child,
                        entryMain->functionIndex,
                        &wrapperFunctionIndex)
                    != 0)
                {
                    return ErrorSimple("out of memory");
                }
                if (declMap != NULL
                    && AddMirResolvedDecl(
                           declMap,
                           pkg,
                           file->source,
                           n->dataStart,
                           n->dataEnd,
                           wrapperFunctionIndex,
                           HOPMirDeclKind_FN)
                           != 0)
                {
                    return ErrorSimple("out of memory");
                }
            } else if (DeclHasDirective(file, child, "wasm_import", &wasmImportNode)) {
                if (AppendMirForeignImportDecl(builder, arena, file, child, &outFunctionIndex) != 0)
                {
                    return ErrorSimple("out of memory");
                }
                if (declMap != NULL
                    && AddMirResolvedDecl(
                           declMap,
                           pkg,
                           file->source,
                           n->dataStart,
                           n->dataEnd,
                           outFunctionIndex,
                           HOPMirDeclKind_FN)
                           != 0)
                {
                    return ErrorSimple("out of memory");
                }
            }
        } else if (n->kind == HOPAst_VAR || n->kind == HOPAst_CONST) {
            const HOPAst* ast = &file->ast;
            const char*   kindName = n->kind == HOPAst_CONST ? "top-level const" : "top-level var";
            int32_t       firstChild = ASTFirstChild(ast, child);
            HOPStrView    src = { file->source, file->sourceLen };
            HOPDiag       diag = { 0 };
            int           supported = 0;
            if (firstChild >= 0 && ast->nodes[firstChild].kind == HOPAst_NAME_LIST) {
                uint32_t i;
                uint32_t nameCount = AstListCount(ast, firstChild);
                for (i = 0; i < nameCount; i++) {
                    uint32_t          outFunctionIndex = UINT32_MAX;
                    int32_t           nameNode = AstListItemAt(ast, firstChild, i);
                    const HOPAstNode* nameAst =
                        (nameNode >= 0 && (uint32_t)nameNode < ast->len)
                            ? &ast->nodes[nameNode]
                            : NULL;
                    if (nameAst == NULL) {
                        return ErrorMirUnsupported(file, n, kindName, NULL);
                    }
                    diag = (HOPDiag){ 0 };
                    supported = 0;
                    if (HOPMirLowerAppendNamedVarLikeTopInitFunctionBySlice(
                            builder,
                            arena,
                            ast,
                            src,
                            child,
                            nameAst->dataStart,
                            nameAst->dataEnd,
                            &outFunctionIndex,
                            &supported,
                            &diag)
                        != 0)
                    {
                        return PrintHOPDiagLineCol(file->path, file->source, &diag, 0);
                    }
                    if (!supported) {
                        return ErrorMirUnsupported(file, nameAst, kindName, &diag);
                    }
                    if (declMap != NULL
                        && AddMirResolvedDecl(
                               declMap,
                               pkg,
                               file->source,
                               nameAst->dataStart,
                               nameAst->dataEnd,
                               outFunctionIndex,
                               n->kind == HOPAst_CONST ? HOPMirDeclKind_CONST : HOPMirDeclKind_VAR)
                               != 0)
                    {
                        return ErrorSimple("out of memory");
                    }
                }
            } else {
                uint32_t outFunctionIndex = UINT32_MAX;
                int32_t  wasmImportNode = -1;
                if (DeclHasDirective(file, child, "wasm_import", &wasmImportNode)) {
                    if (AppendMirForeignImportDecl(builder, arena, file, child, &outFunctionIndex)
                        != 0)
                    {
                        return ErrorSimple("out of memory");
                    }
                } else {
                    if (HOPMirLowerAppendNamedVarLikeTopInitFunctionBySlice(
                            builder,
                            arena,
                            ast,
                            src,
                            child,
                            n->dataStart,
                            n->dataEnd,
                            &outFunctionIndex,
                            &supported,
                            &diag)
                        != 0)
                    {
                        return PrintHOPDiagLineCol(file->path, file->source, &diag, 0);
                    }
                    if (!supported) {
                        return ErrorMirUnsupported(file, n, kindName, &diag);
                    }
                }
                if (declMap != NULL
                    && AddMirResolvedDecl(
                           declMap,
                           pkg,
                           file->source,
                           n->dataStart,
                           n->dataEnd,
                           outFunctionIndex,
                           n->kind == HOPAst_CONST ? HOPMirDeclKind_CONST : HOPMirDeclKind_VAR)
                           != 0)
                {
                    return ErrorSimple("out of memory");
                }
            }
        }
        child = ASTNextSibling(&file->ast, child);
    }
    return 0;
}

static int AppendMirTemplateInstancesFromFile(
    const HOPPackageLoader* loader,
    HOPMirProgramBuilder*   builder,
    HOPArena*               arena,
    const HOPPackage*       pkg,
    const HOPParsedFile*    file,
    HOPMirTcFunctionMap*    tcFnMap) {
    HOPTypeCheckCtx* tc =
        file != NULL && file->hasTypecheckCtx ? (HOPTypeCheckCtx*)file->typecheckCtx : NULL;
    uint32_t i;
    if (loader == NULL || builder == NULL || arena == NULL || pkg == NULL || file == NULL
        || tcFnMap == NULL || tc == NULL)
    {
        return 0;
    }
    for (i = 0; i < tc->funcLen; i++) {
        const HOPTCFunction* fn = &tc->funcs[i];
        uint32_t             outFunctionIndex = UINT32_MAX;
        int32_t              bodyNode;
        HOPDiag              diag = { 0 };
        int                  supported = 0;
        if ((fn->flags & HOPTCFunctionFlag_TEMPLATE_INSTANCE) == 0 || fn->defNode < 0) {
            continue;
        }
        if (i >= tc->funcUsedCap || tc->funcUsed[i] == 0u) {
            continue;
        }
        if (FindMirTcFunctionDecl(tcFnMap, pkg, file, i, &outFunctionIndex)) {
            continue;
        }
        bodyNode = FindFunctionBodyNode(file, fn->defNode);
        if (bodyNode < 0) {
            continue;
        }
        if (HOPMirLowerAppendSimpleFunction(
                builder,
                arena,
                &file->ast,
                (HOPStrView){ file->source, file->sourceLen },
                fn->declNode,
                bodyNode,
                &outFunctionIndex,
                &supported,
                &diag)
            != 0)
        {
            return PrintHOPDiagLineCol(file->path, file->source, &diag, 0);
        }
        if (!supported) {
            return ErrorMirUnsupported(
                file, &file->ast.nodes[fn->declNode], "function body", &diag);
        }
        if (PatchMirFunctionTypeRefsFromTC(
                arena, loader, builder, pkg, file, tc, i, outFunctionIndex)
            != 0)
        {
            return ErrorSimple("out of memory");
        }
        if (AddMirTcFunctionDecl(tcFnMap, pkg, file, i, outFunctionIndex) != 0) {
            return ErrorSimple("out of memory");
        }
    }
    return 0;
}

static int AppendMirSelectedPlatformNamedImportDeclsFromFile(
    HOPMirProgramBuilder* builder,
    HOPArena*             arena,
    const HOPPackage*     pkg,
    const HOPParsedFile*  file,
    const char*           name,
    HOPMirResolvedDeclMap* _Nullable declMap) {
    int32_t child = ASTFirstChild(&file->ast, file->ast.root);
    while (child >= 0) {
        const HOPAstNode* n = &file->ast.nodes[child];
        int32_t           wasmImportNode = -1;
        if (n->kind == HOPAst_FN && name != NULL
            && SliceEqCStr(file->source, n->dataStart, n->dataEnd, name)
            && DeclHasDirective(file, child, "wasm_import", &wasmImportNode))
        {
            uint32_t outFunctionIndex = UINT32_MAX;
            if (AppendMirForeignImportDecl(builder, arena, file, child, &outFunctionIndex) != 0) {
                return ErrorSimple("out of memory");
            }
            if (declMap != NULL
                && AddMirResolvedDecl(
                       declMap,
                       pkg,
                       file->source,
                       n->dataStart,
                       n->dataEnd,
                       outFunctionIndex,
                       HOPMirDeclKind_FN)
                       != 0)
            {
                return ErrorSimple("out of memory");
            }
        }
        child = ASTNextSibling(&file->ast, child);
    }
    return 0;
}

static int AppendMirSelectedPlatformPanicDeclsFromFile(
    HOPMirProgramBuilder* builder,
    HOPArena*             arena,
    const HOPPackage*     pkg,
    const HOPParsedFile*  file,
    HOPMirResolvedDeclMap* _Nullable declMap) {
    return AppendMirSelectedPlatformNamedImportDeclsFromFile(
        builder, arena, pkg, file, "panic", declMap);
}

static int AppendMirSelectedPlatformConsoleLogDeclsFromFile(
    HOPMirProgramBuilder* builder,
    HOPArena*             arena,
    const HOPPackage*     pkg,
    const HOPParsedFile*  file,
    HOPMirResolvedDeclMap* _Nullable declMap) {
    return AppendMirSelectedPlatformNamedImportDeclsFromFile(
        builder, arena, pkg, file, "console_log", declMap);
}

static int IsPlatformImportPath(const char* _Nullable path) {
    return path != NULL && (StrEq(path, "platform") || strncmp(path, "platform/", 9u) == 0);
}

static int IsSelectedPlatformImportPath(
    const HOPPackageLoader* loader, const char* _Nullable path) {
    size_t prefixLen = 9u;
    if (loader == NULL || loader->platformTarget == NULL || path == NULL) {
        return 0;
    }
    if (StrEq(path, "platform")) {
        return 1;
    }
    return strncmp(path, "platform/", prefixLen) == 0
        && StrEq(path + prefixLen, loader->platformTarget);
}

int PackageHasPlatformImport(const HOPPackage* _Nullable pkg) {
    uint32_t importIndex;
    if (pkg == NULL) {
        return 0;
    }
    for (importIndex = 0; importIndex < pkg->importLen; importIndex++) {
        if (IsPlatformImportPath(pkg->imports[importIndex].path)) {
            return 1;
        }
    }
    return 0;
}

static int ShouldSkipPackageMirImportPath(const char* _Nullable path) {
    return IsPlatformImportPath(path)
        || (path != NULL
            && (StrEq(path, "builtin") || StrEq(path, "reflect")
                || strncmp(path, "builtin/", 8u) == 0 || strncmp(path, "reflect/", 8u) == 0));
}

static const HOPPackage* _Nullable EffectiveMirImportTargetPackage(
    const HOPPackageLoader* loader, const HOPImportRef* imp) {
    if (imp == NULL) {
        return NULL;
    }
    if (loader != NULL && loader->selectedPlatformPkg != NULL
        && IsSelectedPlatformImportPath(loader, imp->path))
    {
        return loader->selectedPlatformPkg;
    }
    return imp->target;
}

static int BuildEntryPackageMirOrderVisit(
    const HOPPackageLoader* loader,
    uint32_t                pkgIndex,
    uint8_t*                state,
    uint32_t*               order,
    uint32_t*               orderLen) {
    const HOPPackage* pkg = &loader->packages[pkgIndex];
    uint32_t          i;
    if (state[pkgIndex] == 2u) {
        return 0;
    }
    if (state[pkgIndex] == 1u) {
        return ErrorSimple("import cycle detected");
    }
    state[pkgIndex] = 1u;
    for (i = 0; i < pkg->importLen; i++) {
        int depIndex;
        if (ShouldSkipPackageMirImportPath(pkg->imports[i].path)) {
            continue;
        }
        depIndex = FindPackageIndex(loader, pkg->imports[i].target);
        if (depIndex < 0) {
            return ErrorSimple("internal error: unresolved import");
        }
        if (BuildEntryPackageMirOrderVisit(loader, (uint32_t)depIndex, state, order, orderLen) != 0)
        {
            return -1;
        }
    }
    state[pkgIndex] = 2u;
    order[(*orderLen)++] = pkgIndex;
    return 0;
}

int PackageUsesPlatformImport(const HOPPackageLoader* loader);

static int BuildEntryPackageMirOrder(
    const HOPPackageLoader* loader,
    const HOPPackage*       entryPkg,
    int                     includeSelectedPlatform,
    uint32_t*               outOrder,
    uint32_t                outOrderCap,
    uint32_t*               outOrderLen) {
    uint8_t* state = NULL;
    int      entryPkgIndex;
    int      rc = -1;
    *outOrderLen = 0;
    if (loader == NULL || entryPkg == NULL || outOrder == NULL || outOrderLen == NULL
        || outOrderCap < loader->packageLen)
    {
        return -1;
    }
    entryPkgIndex = FindPackageIndex(loader, entryPkg);
    if (entryPkgIndex < 0) {
        return ErrorSimple("internal error: entry package missing from loader");
    }
    state = (uint8_t*)calloc(loader->packageLen, sizeof(uint8_t));
    if (state == NULL) {
        return ErrorSimple("out of memory");
    }
    rc = BuildEntryPackageMirOrderVisit(
        loader, (uint32_t)entryPkgIndex, state, outOrder, outOrderLen);
    if (rc == 0 && loader->selectedPlatformPkg != NULL
        && (includeSelectedPlatform || PackageUsesPlatformImport(loader)))
    {
        int platformPkgIndex = FindPackageIndex(loader, loader->selectedPlatformPkg);
        if (platformPkgIndex < 0) {
            rc = ErrorSimple("internal error: selected platform package missing from loader");
        } else {
            rc = BuildEntryPackageMirOrderVisit(
                loader, (uint32_t)platformPkgIndex, state, outOrder, outOrderLen);
        }
    }
    free(state);
    return rc;
}

void FreeForeignLinkageInfo(HOPForeignLinkageInfo* info) {
    uint32_t                i;
    HOPForeignLinkageEntry* entries;
    if (info == NULL || info->entries == NULL) {
        if (info != NULL) {
            *info = (HOPForeignLinkageInfo){ 0 };
        }
        return;
    }
    entries = (HOPForeignLinkageEntry*)(uintptr_t)info->entries;
    for (i = 0; i < info->len; i++) {
        free(entries[i].arg0.bytes);
        free(entries[i].arg1.bytes);
    }
    free(entries);
    *info = (HOPForeignLinkageInfo){ 0 };
}

static int ForeignLinkageBuilderAppend(
    HOPForeignLinkageBuilder* b, const HOPForeignLinkageEntry* entry) {
    HOPForeignLinkageEntry* newEntries;
    uint32_t                newCap;
    if (b == NULL || entry == NULL) {
        return -1;
    }
    if (b->len >= b->cap) {
        newCap = b->cap >= 8u ? b->cap * 2u : 8u;
        newEntries = (HOPForeignLinkageEntry*)realloc(
            b->entries, sizeof(HOPForeignLinkageEntry) * newCap);
        if (newEntries == NULL) {
            return -1;
        }
        b->entries = newEntries;
        b->cap = newCap;
    }
    b->entries[b->len++] = *entry;
    return 0;
}

static void ForeignLinkageBuilderFree(HOPForeignLinkageBuilder* b) {
    if (b == NULL) {
        return;
    }
    FreeForeignLinkageInfo((HOPForeignLinkageInfo*)b);
    b->cap = 0;
}

static int BuildMirForeignLinkageInfo(
    const HOPPackageLoader*      loader,
    const HOPMirResolvedDeclMap* declMap,
    HOPForeignLinkageInfo*       outInfo,
    HOPDiag* _Nullable diag) {
    HOPForeignLinkageBuilder b = { 0 };
    uint32_t                 pkgIndex;
    if (outInfo == NULL) {
        return -1;
    }
    *outInfo = (HOPForeignLinkageInfo){ 0 };
    if (loader == NULL || declMap == NULL) {
        return 0;
    }
    for (pkgIndex = 0; pkgIndex < loader->packageLen; pkgIndex++) {
        const HOPPackage* pkg = &loader->packages[pkgIndex];
        uint32_t          fileIndex;
        for (fileIndex = 0; fileIndex < pkg->fileLen; fileIndex++) {
            const HOPParsedFile* file = &pkg->files[fileIndex];
            int32_t              child = ASTFirstChild(&file->ast, file->ast.root);
            while (child >= 0) {
                const HOPAstNode* decl = &file->ast.nodes[child];
                int32_t           cImportNode = -1;
                int32_t           wasmImportNode = -1;
                int32_t           exportNode = -1;
                if (DeclHasDirective(file, child, "c_import", &cImportNode)) {
                    if (diag != NULL) {
                        *diag = (HOPDiag){
                            .code = HOPDiag_WASM_BACKEND_UNSUPPORTED_MIR,
                            .type = HOPDiagTypeOfCode(HOPDiag_WASM_BACKEND_UNSUPPORTED_MIR),
                            .start = file->ast.nodes[cImportNode].start,
                            .end = file->ast.nodes[cImportNode].end,
                            .detail = "@c_import is not supported by the direct Wasm backend",
                        };
                    }
                    ForeignLinkageBuilderFree(&b);
                    return -1;
                }
                if (DeclHasDirective(file, child, "wasm_import", &wasmImportNode)) {
                    int32_t                   arg0 = DirectiveArgAt(&file->ast, wasmImportNode, 0u);
                    int32_t                   arg1 = DirectiveArgAt(&file->ast, wasmImportNode, 1u);
                    const HOPMirResolvedDecl* resolved = NULL;
                    HOPForeignLinkageEntry    entry = { 0 };
                    HOPStringLitErr           litErr = { 0 };
                    if (arg0 < 0 || arg1 < 0) {
                        ForeignLinkageBuilderFree(&b);
                        return -1;
                    }
                    if (decl->kind == HOPAst_FN) {
                        resolved = FindMirResolvedDeclBySlice(
                            declMap,
                            pkg,
                            file->source,
                            decl->dataStart,
                            decl->dataEnd,
                            HOPMirDeclKind_FN);
                        entry.kind = HOPForeignLinkage_WASM_IMPORT_FN;
                    } else if (decl->kind == HOPAst_CONST) {
                        resolved = FindMirResolvedDeclBySlice(
                            declMap,
                            pkg,
                            file->source,
                            decl->dataStart,
                            decl->dataEnd,
                            HOPMirDeclKind_CONST);
                        entry.kind = HOPForeignLinkage_WASM_IMPORT_CONST;
                    } else if (decl->kind == HOPAst_VAR) {
                        resolved = FindMirResolvedDeclBySlice(
                            declMap,
                            pkg,
                            file->source,
                            decl->dataStart,
                            decl->dataEnd,
                            HOPMirDeclKind_VAR);
                        entry.kind = HOPForeignLinkage_WASM_IMPORT_VAR;
                    }
                    if (resolved == NULL) {
                        child = ASTNextSibling(&file->ast, child);
                        continue;
                    }
                    entry.functionIndex = resolved->functionIndex;
                    entry.start = file->ast.nodes[wasmImportNode].start;
                    entry.end = file->ast.nodes[wasmImportNode].end;
                    if (loader->selectedPlatformPkg == pkg && decl->kind == HOPAst_FN
                        && SliceEqCStr(file->source, decl->dataStart, decl->dataEnd, "panic"))
                    {
                        entry.flags = HOPForeignLinkageFlag_PLATFORM_PANIC;
                    }
                    if (HOPDecodeStringLiteralMalloc(
                            file->source,
                            file->ast.nodes[arg0].start,
                            file->ast.nodes[arg0].end,
                            &entry.arg0.bytes,
                            &entry.arg0.len,
                            &litErr)
                            != 0
                        || HOPDecodeStringLiteralMalloc(
                               file->source,
                               file->ast.nodes[arg1].start,
                               file->ast.nodes[arg1].end,
                               &entry.arg1.bytes,
                               &entry.arg1.len,
                               &litErr)
                               != 0
                        || ForeignLinkageBuilderAppend(&b, &entry) != 0)
                    {
                        free(entry.arg0.bytes);
                        free(entry.arg1.bytes);
                        ForeignLinkageBuilderFree(&b);
                        return -1;
                    }
                } else if (
                    DeclHasDirective(file, child, "export", &exportNode) && decl->kind == HOPAst_FN)
                {
                    int32_t                   arg0 = DirectiveArgAt(&file->ast, exportNode, 0u);
                    const HOPMirResolvedDecl* resolved = FindMirResolvedDeclBySlice(
                        declMap,
                        pkg,
                        file->source,
                        decl->dataStart,
                        decl->dataEnd,
                        HOPMirDeclKind_FN);
                    HOPForeignLinkageEntry entry = { 0 };
                    HOPStringLitErr        litErr = { 0 };
                    if (arg0 >= 0 && resolved != NULL) {
                        entry.kind = HOPForeignLinkage_EXPORT_FN;
                        entry.functionIndex = resolved->functionIndex;
                        entry.start = file->ast.nodes[exportNode].start;
                        entry.end = file->ast.nodes[exportNode].end;
                        if (HOPDecodeStringLiteralMalloc(
                                file->source,
                                file->ast.nodes[arg0].start,
                                file->ast.nodes[arg0].end,
                                &entry.arg0.bytes,
                                &entry.arg0.len,
                                &litErr)
                                != 0
                            || ForeignLinkageBuilderAppend(&b, &entry) != 0)
                        {
                            free(entry.arg0.bytes);
                            ForeignLinkageBuilderFree(&b);
                            return -1;
                        }
                    }
                }
                child = ASTNextSibling(&file->ast, child);
            }
        }
    }
    outInfo->entries = b.entries;
    outInfo->len = b.len;
    return 0;
}

static const HOPParsedFile* _Nullable FindLoaderFileByMirSource(
    const HOPPackageLoader* loader,
    const HOPMirProgram*    program,
    uint32_t                sourceRef,
    const HOPPackage** _Nullable outPkg) {
    uint32_t pkgIndex;
    if (outPkg != NULL) {
        *outPkg = NULL;
    }
    if (loader == NULL || program == NULL || sourceRef >= program->sourceLen) {
        return NULL;
    }
    for (pkgIndex = 0; pkgIndex < loader->packageLen; pkgIndex++) {
        const HOPPackage* pkg = &loader->packages[pkgIndex];
        uint32_t          fileIndex;
        for (fileIndex = 0; fileIndex < pkg->fileLen; fileIndex++) {
            if (pkg->files[fileIndex].source == program->sources[sourceRef].src.ptr
                && pkg->files[fileIndex].sourceLen == program->sources[sourceRef].src.len)
            {
                if (outPkg != NULL) {
                    *outPkg = pkg;
                }
                return &pkg->files[fileIndex];
            }
        }
    }
    return NULL;
}

static int DecodeNewExprNodes(
    const HOPParsedFile* file,
    int32_t              nodeId,
    int32_t*             outTypeNode,
    int32_t*             outCountNode,
    int32_t*             outInitNode,
    int32_t*             outAllocNode) {
    const HOPAstNode* n;
    int32_t           nextNode;
    int               hasCount;
    int               hasInit;
    int               hasAlloc;
    if (outTypeNode != NULL) {
        *outTypeNode = -1;
    }
    if (outCountNode != NULL) {
        *outCountNode = -1;
    }
    if (outInitNode != NULL) {
        *outInitNode = -1;
    }
    if (outAllocNode != NULL) {
        *outAllocNode = -1;
    }
    if (file == NULL || nodeId < 0 || (uint32_t)nodeId >= file->ast.len) {
        return 0;
    }
    n = &file->ast.nodes[nodeId];
    if (n->kind != HOPAst_NEW) {
        return 0;
    }
    hasCount = (n->flags & HOPAstFlag_NEW_HAS_COUNT) != 0;
    hasInit = (n->flags & HOPAstFlag_NEW_HAS_INIT) != 0;
    hasAlloc = (n->flags & HOPAstFlag_NEW_HAS_ALLOC) != 0;
    if (outTypeNode == NULL || outCountNode == NULL || outInitNode == NULL || outAllocNode == NULL)
    {
        return 0;
    }
    *outTypeNode = ASTFirstChild(&file->ast, nodeId);
    if (*outTypeNode < 0) {
        return 0;
    }
    nextNode = ASTNextSibling(&file->ast, *outTypeNode);
    if (hasCount) {
        *outCountNode = nextNode;
        if (*outCountNode < 0) {
            return 0;
        }
        nextNode = ASTNextSibling(&file->ast, *outCountNode);
    }
    if (hasInit) {
        *outInitNode = nextNode;
        if (*outInitNode < 0) {
            return 0;
        }
        nextNode = ASTNextSibling(&file->ast, *outInitNode);
    }
    if (hasAlloc) {
        *outAllocNode = nextNode;
        if (*outAllocNode < 0) {
            return 0;
        }
        nextNode = ASTNextSibling(&file->ast, *outAllocNode);
    }
    return nextNode < 0;
}

static int ParseMirIntLiteral(const char* src, uint32_t start, uint32_t end, int64_t* out) {
    uint64_t value = 0;
    uint32_t i;
    uint32_t base = 10u;
    if (src == NULL || out == NULL || end <= start) {
        return 0;
    }
    if (end - start >= 3u && src[start] == '0'
        && (src[start + 1u] == 'x' || src[start + 1u] == 'X'))
    {
        base = 16u;
        start += 2u;
        if (end <= start) {
            return 0;
        }
    }
    for (i = start; i < end; i++) {
        unsigned char ch = (unsigned char)src[i];
        uint32_t      digit;
        if (ch >= (unsigned char)'0' && ch <= (unsigned char)'9') {
            digit = (uint32_t)(ch - (unsigned char)'0');
        } else if (base == 16u && ch >= (unsigned char)'a' && ch <= (unsigned char)'f') {
            digit = 10u + (uint32_t)(ch - (unsigned char)'a');
        } else if (base == 16u && ch >= (unsigned char)'A' && ch <= (unsigned char)'F') {
            digit = 10u + (uint32_t)(ch - (unsigned char)'A');
        } else {
            return 0;
        }
        if (digit >= base) {
            return 0;
        }
        if (value > (uint64_t)INT64_MAX / (uint64_t)base
            || (value == (uint64_t)INT64_MAX / (uint64_t)base
                && (uint64_t)digit > (uint64_t)INT64_MAX % (uint64_t)base))
        {
            return 0;
        }
        value = value * (uint64_t)base + (uint64_t)digit;
    }
    *out = (int64_t)value;
    return 1;
}

static const HOPSymbolDecl* _Nullable FindPackageTypeDeclBySlice(
    const HOPPackage* pkg, const char* src, uint32_t start, uint32_t end);

static int ResolvePackageEnumVariantConstValue(
    const HOPPackage* pkg,
    const char*       typeSrc,
    uint32_t          typeStart,
    uint32_t          typeEnd,
    const char*       memberSrc,
    uint32_t          memberStart,
    uint32_t          memberEnd,
    int64_t*          outValue) {
    const HOPSymbolDecl* decl;
    const HOPParsedFile* file;
    const HOPAst*        ast;
    int32_t              variantNode;
    int64_t              nextValue = 0;
    if (outValue != NULL) {
        *outValue = 0;
    }
    if (pkg == NULL || typeSrc == NULL || memberSrc == NULL || outValue == NULL) {
        return 0;
    }
    decl = FindPackageTypeDeclBySlice(pkg, typeSrc, typeStart, typeEnd);
    if (decl == NULL || decl->kind != HOPAst_ENUM || decl->fileIndex >= pkg->fileLen
        || decl->nodeId < 0)
    {
        return 0;
    }
    file = &pkg->files[decl->fileIndex];
    ast = &file->ast;
    if ((uint32_t)decl->nodeId >= ast->len) {
        return 0;
    }
    variantNode = ast->nodes[decl->nodeId].firstChild;
    if (variantNode >= 0
        && (ast->nodes[variantNode].kind == HOPAst_TYPE_NAME
            || ast->nodes[variantNode].kind == HOPAst_TYPE_PTR
            || ast->nodes[variantNode].kind == HOPAst_TYPE_REF
            || ast->nodes[variantNode].kind == HOPAst_TYPE_MUTREF
            || ast->nodes[variantNode].kind == HOPAst_TYPE_ARRAY
            || ast->nodes[variantNode].kind == HOPAst_TYPE_VARRAY
            || ast->nodes[variantNode].kind == HOPAst_TYPE_SLICE
            || ast->nodes[variantNode].kind == HOPAst_TYPE_MUTSLICE
            || ast->nodes[variantNode].kind == HOPAst_TYPE_OPTIONAL
            || ast->nodes[variantNode].kind == HOPAst_TYPE_FN
            || ast->nodes[variantNode].kind == HOPAst_TYPE_TUPLE
            || ast->nodes[variantNode].kind == HOPAst_TYPE_ANON_STRUCT
            || ast->nodes[variantNode].kind == HOPAst_TYPE_ANON_UNION))
    {
        variantNode = ast->nodes[variantNode].nextSibling;
    }
    while (variantNode >= 0) {
        const HOPAstNode* variant = &ast->nodes[variantNode];
        int64_t           value = nextValue;
        int32_t           child = variant->firstChild;
        if (variant->kind == HOPAst_FIELD) {
            while (child >= 0 && ast->nodes[child].kind == HOPAst_FIELD) {
                child = ast->nodes[child].nextSibling;
            }
            if (child >= 0
                && !ParseMirIntLiteral(
                    file->source, ast->nodes[child].dataStart, ast->nodes[child].dataEnd, &value))
            {
                return 0;
            }
            if (SliceEqSlice(
                    file->source,
                    variant->dataStart,
                    variant->dataEnd,
                    memberSrc,
                    memberStart,
                    memberEnd))
            {
                *outValue = value;
                return 1;
            }
            nextValue = value + 1;
        }
        variantNode = ast->nodes[variantNode].nextSibling;
    }
    return 0;
}

static uint32_t FindMirSourceRefByText(
    const HOPMirProgram* program, const char* src, uint32_t srcLen);
static int FindMirTypeRefByAstNode(
    const HOPMirProgram* program, uint32_t sourceRef, int32_t astNode, uint32_t* outTypeRef);
static int AppendMirInst(
    HOPMirInst* outInsts, uint32_t outCap, uint32_t* outLen, const HOPMirInst* inst);
static int MirTypeNodeKind(HOPAstKind kind);
static int ResolveMirAggregateTypeRefForTypeNode(
    const HOPPackageLoader* loader,
    const HOPMirProgram*    program,
    const HOPParsedFile*    file,
    int32_t                 typeNode,
    uint32_t*               outTypeRef);
static int EnsureMirAstTypeRef(
    HOPArena*               arena,
    const HOPPackageLoader* loader,
    HOPMirProgram*          program,
    uint32_t                astNode,
    uint32_t                sourceRef,
    uint32_t* _Nonnull outTypeRef);
static int EnsureMirScalarTypeRef(
    HOPArena*        arena,
    HOPMirProgram*   program,
    HOPMirTypeScalar scalar,
    uint32_t* _Nonnull outTypeRef);

static uint32_t MirIntKindByteWidth(HOPMirIntKind intKind) {
    switch (intKind) {
        case HOPMirIntKind_U8:
        case HOPMirIntKind_I8:
        case HOPMirIntKind_BOOL: return 1u;
        case HOPMirIntKind_U16:
        case HOPMirIntKind_I16:  return 2u;
        case HOPMirIntKind_U32:
        case HOPMirIntKind_I32:  return 4u;
        default:                 return 0u;
    }
}

static int ResolveMirAllocNewPointeeTypeRef(
    const HOPPackageLoader* loader,
    HOPArena*               arena,
    HOPMirProgram*          program,
    const HOPParsedFile*    file,
    const HOPMirInst*       allocInst,
    uint32_t*               outTypeRef) {
    int32_t  typeNode = -1;
    int32_t  countNode = -1;
    int32_t  initNode = -1;
    int32_t  allocNode = -1;
    uint32_t sourceRef;
    if (outTypeRef != NULL) {
        *outTypeRef = UINT32_MAX;
    }
    if (loader == NULL || arena == NULL || program == NULL || file == NULL || allocInst == NULL
        || outTypeRef == NULL
        || !DecodeNewExprNodes(
            file, (int32_t)allocInst->aux, &typeNode, &countNode, &initNode, &allocNode)
        || typeNode < 0)
    {
        return 0;
    }
    sourceRef = FindMirSourceRefByText(program, file->source, file->sourceLen);
    if (sourceRef == UINT32_MAX) {
        return 0;
    }
    if (ResolveMirAggregateTypeRefForTypeNode(loader, program, file, typeNode, outTypeRef)) {
        return 1;
    }
    if (FindMirTypeRefByAstNode(program, sourceRef, typeNode, outTypeRef)) {
        return 1;
    }
    return EnsureMirAstTypeRef(arena, loader, program, (uint32_t)typeNode, sourceRef, outTypeRef)
        == 0;
}

static int EnsureMirIntConst(
    HOPArena* arena, HOPMirProgram* program, int64_t value, uint32_t* _Nonnull outIndex) {
    HOPMirConst* newConsts;
    uint32_t     i;
    if (arena == NULL || program == NULL || outIndex == NULL) {
        return -1;
    }
    for (i = 0; i < program->constLen; i++) {
        if (program->consts[i].kind == HOPMirConst_INT && (int64_t)program->consts[i].bits == value)
        {
            *outIndex = i;
            return 0;
        }
    }
    newConsts = (HOPMirConst*)HOPArenaAlloc(
        arena, sizeof(HOPMirConst) * (program->constLen + 1u), (uint32_t)_Alignof(HOPMirConst));
    if (newConsts == NULL) {
        return -1;
    }
    if (program->constLen != 0u) {
        memcpy(newConsts, program->consts, sizeof(HOPMirConst) * program->constLen);
    }
    newConsts[program->constLen] = (HOPMirConst){
        .kind = HOPMirConst_INT,
        .aux = 0u,
        .bits = (uint64_t)value,
        .bytes = { 0 },
    };
    program->consts = newConsts;
    *outIndex = program->constLen++;
    return 0;
}

static int EnsureMirNullConst(
    HOPArena* arena, HOPMirProgram* program, uint32_t* _Nonnull outIndex) {
    HOPMirConst* newConsts;
    uint32_t     i;
    if (arena == NULL || program == NULL || outIndex == NULL) {
        return -1;
    }
    for (i = 0; i < program->constLen; i++) {
        if (program->consts[i].kind == HOPMirConst_NULL) {
            *outIndex = i;
            return 0;
        }
    }
    newConsts = (HOPMirConst*)HOPArenaAlloc(
        arena, sizeof(HOPMirConst) * (program->constLen + 1u), (uint32_t)_Alignof(HOPMirConst));
    if (newConsts == NULL) {
        return -1;
    }
    if (program->constLen != 0u) {
        memcpy(newConsts, program->consts, sizeof(HOPMirConst) * program->constLen);
    }
    newConsts[program->constLen] =
        (HOPMirConst){ .kind = HOPMirConst_NULL, .aux = 0u, .bits = 0u, .bytes = { 0 } };
    program->consts = newConsts;
    *outIndex = program->constLen++;
    return 0;
}

static int EnsureMirBoolConst(
    HOPArena* arena, HOPMirProgram* program, bool value, uint32_t* _Nonnull outIndex) {
    HOPMirConst* newConsts;
    uint32_t     i;
    if (arena == NULL || program == NULL || outIndex == NULL) {
        return -1;
    }
    for (i = 0; i < program->constLen; i++) {
        if (program->consts[i].kind == HOPMirConst_BOOL
            && ((program->consts[i].bits != 0u) == value))
        {
            *outIndex = i;
            return 0;
        }
    }
    newConsts = (HOPMirConst*)HOPArenaAlloc(
        arena, sizeof(HOPMirConst) * (program->constLen + 1u), (uint32_t)_Alignof(HOPMirConst));
    if (newConsts == NULL) {
        return -1;
    }
    if (program->constLen != 0u) {
        memcpy(newConsts, program->consts, sizeof(HOPMirConst) * program->constLen);
    }
    newConsts[program->constLen] = (HOPMirConst){
        .kind = HOPMirConst_BOOL,
        .aux = 0u,
        .bits = value ? 1u : 0u,
        .bytes = { 0 },
    };
    program->consts = newConsts;
    *outIndex = program->constLen++;
    return 0;
}

static int EnsureMirStringConst(
    HOPArena* arena, HOPMirProgram* program, HOPStrView value, uint32_t* _Nonnull outIndex) {
    HOPMirConst* newConsts;
    uint32_t     i;
    if (arena == NULL || program == NULL || outIndex == NULL) {
        return -1;
    }
    for (i = 0; i < program->constLen; i++) {
        if (program->consts[i].kind == HOPMirConst_STRING
            && program->consts[i].bytes.len == value.len
            && (value.len == 0u || memcmp(program->consts[i].bytes.ptr, value.ptr, value.len) == 0))
        {
            *outIndex = i;
            return 0;
        }
    }
    newConsts = (HOPMirConst*)HOPArenaAlloc(
        arena, sizeof(HOPMirConst) * (program->constLen + 1u), (uint32_t)_Alignof(HOPMirConst));
    if (newConsts == NULL) {
        return -1;
    }
    if (program->constLen != 0u) {
        memcpy(newConsts, program->consts, sizeof(HOPMirConst) * program->constLen);
    }
    newConsts[program->constLen] =
        (HOPMirConst){ .kind = HOPMirConst_STRING, .aux = 0u, .bits = 0u, .bytes = value };
    program->consts = newConsts;
    *outIndex = program->constLen++;
    return 0;
}

static int EnsureMirFunctionConst(
    HOPArena* arena, HOPMirProgram* program, uint32_t functionIndex, uint32_t* _Nonnull outIndex) {
    HOPMirConst* newConsts;
    uint32_t     i;
    if (arena == NULL || program == NULL || outIndex == NULL || functionIndex >= program->funcLen) {
        return -1;
    }
    for (i = 0; i < program->constLen; i++) {
        if (program->consts[i].kind == HOPMirConst_FUNCTION
            && program->consts[i].aux == functionIndex)
        {
            *outIndex = i;
            return 0;
        }
    }
    newConsts = (HOPMirConst*)HOPArenaAlloc(
        arena, sizeof(HOPMirConst) * (program->constLen + 1u), (uint32_t)_Alignof(HOPMirConst));
    if (newConsts == NULL) {
        return -1;
    }
    if (program->constLen != 0u) {
        memcpy(newConsts, program->consts, sizeof(HOPMirConst) * program->constLen);
    }
    newConsts[program->constLen] = (HOPMirConst){
        .kind = HOPMirConst_FUNCTION,
        .aux = functionIndex,
        .bits = functionIndex,
        .bytes = { 0 },
    };
    program->consts = newConsts;
    *outIndex = program->constLen++;
    return 0;
}

static bool MirSourceSliceEq(
    const HOPMirProgram* program,
    uint32_t             sourceRefA,
    uint32_t             startA,
    uint32_t             endA,
    uint32_t             sourceRefB,
    uint32_t             startB,
    uint32_t             endB) {
    uint32_t len;
    if (program == NULL || sourceRefA >= program->sourceLen || sourceRefB >= program->sourceLen
        || endA < startA || endB < startB)
    {
        return false;
    }
    len = endA - startA;
    if (len != endB - startB || program->sources[sourceRefA].src.ptr == NULL
        || program->sources[sourceRefB].src.ptr == NULL)
    {
        return false;
    }
    return len == 0u
        || memcmp(
               program->sources[sourceRefA].src.ptr + startA,
               program->sources[sourceRefB].src.ptr + startB,
               len)
               == 0;
}

static uint32_t FindMirSourceRefByText(
    const HOPMirProgram* program, const char* src, uint32_t srcLen) {
    uint32_t i;
    if (program == NULL || src == NULL) {
        return UINT32_MAX;
    }
    for (i = 0; i < program->sourceLen; i++) {
        if (program->sources[i].src.len == srcLen
            && memcmp(program->sources[i].src.ptr, src, srcLen) == 0)
        {
            return i;
        }
    }
    return UINT32_MAX;
}

static int FindMirTypeRefByAstNode(
    const HOPMirProgram* program, uint32_t sourceRef, int32_t astNode, uint32_t* outTypeRef) {
    uint32_t i;
    if (outTypeRef != NULL) {
        *outTypeRef = UINT32_MAX;
    }
    if (program == NULL || outTypeRef == NULL || astNode < 0) {
        return 0;
    }
    for (i = 0; i < program->typeLen; i++) {
        if (program->types[i].sourceRef == sourceRef
            && program->types[i].astNode == (uint32_t)astNode)
        {
            *outTypeRef = i;
            return 1;
        }
    }
    return 0;
}

static int FindMirFieldByOwnerAndSlice(
    const HOPMirProgram* program,
    uint32_t             ownerTypeRef,
    uint32_t             sourceRef,
    uint32_t             nameStart,
    uint32_t             nameEnd,
    uint32_t*            outFieldIndex) {
    uint32_t i;
    if (outFieldIndex != NULL) {
        *outFieldIndex = UINT32_MAX;
    }
    if (program == NULL || outFieldIndex == NULL || nameEnd < nameStart) {
        return 0;
    }
    for (i = 0; i < program->fieldLen; i++) {
        if (program->fields[i].ownerTypeRef != ownerTypeRef) {
            continue;
        }
        if (MirSourceSliceEq(
                program,
                program->fields[i].sourceRef,
                program->fields[i].nameStart,
                program->fields[i].nameEnd,
                sourceRef,
                nameStart,
                nameEnd))
        {
            *outFieldIndex = i;
            return 1;
        }
    }
    return 0;
}

static int FindMirPseudoFieldByName(
    const HOPMirProgram* program, const char* name, uint32_t* outFieldIndex) {
    uint32_t i;
    size_t   nameLen = 0u;
    if (outFieldIndex != NULL) {
        *outFieldIndex = UINT32_MAX;
    }
    if (program == NULL || name == NULL || outFieldIndex == NULL) {
        return 0;
    }
    while (name[nameLen] != '\0') {
        nameLen++;
    }
    for (i = 0; i < program->fieldLen; i++) {
        if (program->fields[i].ownerTypeRef != UINT32_MAX
            || program->fields[i].sourceRef >= program->sourceLen
            || program->fields[i].nameEnd < program->fields[i].nameStart
            || (size_t)(program->fields[i].nameEnd - program->fields[i].nameStart) != nameLen)
        {
            continue;
        }
        if (memcmp(
                program->sources[program->fields[i].sourceRef].src.ptr
                    + program->fields[i].nameStart,
                name,
                nameLen)
            == 0)
        {
            *outFieldIndex = i;
            return 1;
        }
    }
    return 0;
}

static int FindMirFunctionLocalBySlice(
    const HOPMirProgram*  program,
    const HOPMirFunction* fn,
    const char*           src,
    uint32_t              nameStart,
    uint32_t              nameEnd,
    uint32_t*             outLocalIndex) {
    uint32_t i;
    uint32_t nameLen;
    if (outLocalIndex != NULL) {
        *outLocalIndex = UINT32_MAX;
    }
    if (program == NULL || fn == NULL || src == NULL || outLocalIndex == NULL
        || nameEnd < nameStart)
    {
        return 0;
    }
    nameLen = nameEnd - nameStart;
    for (i = 0; i < fn->localCount; i++) {
        const HOPMirLocal* local = &program->locals[fn->localStart + i];
        if (local->nameEnd >= local->nameStart && local->nameEnd - local->nameStart == nameLen
            && memcmp(src + local->nameStart, src + nameStart, nameLen) == 0)
        {
            *outLocalIndex = i;
            return 1;
        }
    }
    return 0;
}

static int CountMirAllocNewCountExprInsts(
    const HOPParsedFile* file, int32_t exprNode, uint32_t* _Nonnull outCount) {
    const HOPAstNode* n;
    uint32_t          leftCount = 0u;
    uint32_t          rightCount = 0u;
    if (outCount != NULL) {
        *outCount = 0u;
    }
    if (file == NULL || outCount == NULL || exprNode < 0 || (uint32_t)exprNode >= file->ast.len) {
        return 0;
    }
    n = &file->ast.nodes[exprNode];
    switch (n->kind) {
        case HOPAst_INT:
        case HOPAst_IDENT: *outCount = 1u; return 1;
        case HOPAst_UNARY:
            if (!CountMirAllocNewCountExprInsts(file, n->firstChild, &leftCount)) {
                return 0;
            }
            *outCount = leftCount + 1u;
            return 1;
        case HOPAst_BINARY: {
            int32_t rhsNode = ASTNextSibling(&file->ast, n->firstChild);
            if (!CountMirAllocNewCountExprInsts(file, n->firstChild, &leftCount) || rhsNode < 0
                || !CountMirAllocNewCountExprInsts(file, rhsNode, &rightCount))
            {
                return 0;
            }
            *outCount = leftCount + rightCount + 1u;
            return 1;
        }
        default: return 0;
    }
}

static int CountMirAllocNewAllocExprInsts(
    const HOPParsedFile* file, int32_t exprNode, uint32_t* _Nonnull outCount) {
    const HOPAstNode* n;
    if (outCount != NULL) {
        *outCount = 0u;
    }
    if (file == NULL || outCount == NULL || exprNode < 0 || (uint32_t)exprNode >= file->ast.len) {
        return 0;
    }
    n = &file->ast.nodes[exprNode];
    switch (n->kind) {
        case HOPAst_IDENT:
        case HOPAst_NULL:  *outCount = 1u; return 1;
        case HOPAst_CAST:  {
            int32_t lhsNode = n->firstChild;
            if (lhsNode < 0 || (uint32_t)lhsNode >= file->ast.len) {
                return 0;
            }
            return CountMirAllocNewAllocExprInsts(file, lhsNode, outCount);
        }
        case HOPAst_FIELD_EXPR: {
            int32_t baseNode = n->firstChild;
            if (baseNode < 0 || (uint32_t)baseNode >= file->ast.len) {
                return 0;
            }
            if (file->ast.nodes[baseNode].kind == HOPAst_IDENT
                && SliceEqCStr(
                    file->source,
                    file->ast.nodes[baseNode].dataStart,
                    file->ast.nodes[baseNode].dataEnd,
                    "context")
                && (SliceEqCStr(file->source, n->dataStart, n->dataEnd, "allocator")
                    || SliceEqCStr(file->source, n->dataStart, n->dataEnd, "temp_allocator")))
            {
                *outCount = 1u;
                return 1;
            }
            return 0;
        }
        default: return 0;
    }
}

static int LowerMirAllocNewCountExpr(
    const HOPMirProgram*  program,
    HOPMirProgram*        mutableProgram,
    const HOPMirFunction* fn,
    const HOPParsedFile*  file,
    int32_t               exprNode,
    HOPArena*             arena,
    HOPMirInst*           outInsts,
    uint32_t              outCap,
    uint32_t*             outLen) {
    const HOPAstNode* n;
    if (outLen != NULL) {
        *outLen = 0u;
    }
    if (program == NULL || mutableProgram == NULL || fn == NULL || file == NULL || arena == NULL
        || outInsts == NULL || outLen == NULL || exprNode < 0
        || (uint32_t)exprNode >= file->ast.len)
    {
        return 0;
    }
    n = &file->ast.nodes[exprNode];
    switch (n->kind) {
        case HOPAst_INT: {
            int64_t  value = 0;
            uint32_t constIndex = UINT32_MAX;
            if (outCap < 1u || !ParseMirIntLiteral(file->source, n->dataStart, n->dataEnd, &value)
                || EnsureMirIntConst(arena, mutableProgram, value, &constIndex) != 0)
            {
                return 0;
            }
            outInsts[0] = (HOPMirInst){
                .op = HOPMirOp_PUSH_CONST,
                .tok = 0u,
                ._reserved = 0u,
                .aux = constIndex,
                .start = n->start,
                .end = n->end,
            };
            *outLen = 1u;
            return 1;
        }
        case HOPAst_IDENT: {
            uint32_t localIndex = UINT32_MAX;
            if (outCap < 1u
                || !FindMirFunctionLocalBySlice(
                    program, fn, file->source, n->dataStart, n->dataEnd, &localIndex))
            {
                return 0;
            }
            outInsts[0] = (HOPMirInst){
                .op = HOPMirOp_LOCAL_LOAD,
                .tok = 0u,
                ._reserved = 0u,
                .aux = localIndex,
                .start = n->start,
                .end = n->end,
            };
            *outLen = 1u;
            return 1;
        }
        case HOPAst_UNARY: {
            uint32_t innerLen = 0u;
            if (!LowerMirAllocNewCountExpr(
                    program,
                    mutableProgram,
                    fn,
                    file,
                    n->firstChild,
                    arena,
                    outInsts,
                    outCap > 0u ? outCap - 1u : 0u,
                    &innerLen)
                || innerLen + 1u > outCap)
            {
                return 0;
            }
            outInsts[innerLen] = (HOPMirInst){
                .op = HOPMirOp_UNARY,
                .tok = (uint16_t)n->op,
                ._reserved = 0u,
                .aux = 0u,
                .start = n->start,
                .end = n->end,
            };
            *outLen = innerLen + 1u;
            return 1;
        }
        case HOPAst_BINARY: {
            uint32_t leftLen = 0u;
            uint32_t rightLen = 0u;
            int32_t  rhsNode = ASTNextSibling(&file->ast, n->firstChild);
            if (rhsNode < 0
                || !LowerMirAllocNewCountExpr(
                    program,
                    mutableProgram,
                    fn,
                    file,
                    n->firstChild,
                    arena,
                    outInsts,
                    outCap,
                    &leftLen)
                || !LowerMirAllocNewCountExpr(
                    program,
                    mutableProgram,
                    fn,
                    file,
                    rhsNode,
                    arena,
                    outInsts + leftLen,
                    outCap >= leftLen ? outCap - leftLen : 0u,
                    &rightLen)
                || leftLen + rightLen + 1u > outCap)
            {
                return 0;
            }
            outInsts[leftLen + rightLen] = (HOPMirInst){
                .op = HOPMirOp_BINARY,
                .tok = (uint16_t)n->op,
                ._reserved = 0u,
                .aux = 0u,
                .start = n->start,
                .end = n->end,
            };
            *outLen = leftLen + rightLen + 1u;
            return 1;
        }
        default: return 0;
    }
}

static int FindCompoundFieldValueNodeBySlice(
    const HOPParsedFile* file,
    int32_t              initNode,
    uint32_t             nameStart,
    uint32_t             nameEnd,
    int32_t*             outValueNode) {
    int32_t child;
    if (outValueNode != NULL) {
        *outValueNode = -1;
    }
    if (file == NULL || outValueNode == NULL || initNode < 0 || (uint32_t)initNode >= file->ast.len
        || nameEnd < nameStart)
    {
        return 0;
    }
    child = file->ast.nodes[initNode].firstChild;
    if (child >= 0 && (uint32_t)child < file->ast.len
        && MirTypeNodeKind(file->ast.nodes[child].kind))
    {
        child = file->ast.nodes[child].nextSibling;
    }
    while (child >= 0) {
        const HOPAstNode* field = &file->ast.nodes[child];
        if (field->kind == HOPAst_COMPOUND_FIELD && field->dataEnd >= field->dataStart
            && field->dataEnd - field->dataStart == nameEnd - nameStart
            && memcmp(
                   file->source + field->dataStart, file->source + nameStart, nameEnd - nameStart)
                   == 0)
        {
            *outValueNode = field->firstChild;
            return *outValueNode >= 0 || (field->flags & HOPAstFlag_COMPOUND_FIELD_SHORTHAND) != 0u;
        }
        child = field->nextSibling;
    }
    return 0;
}

static int FindCompoundFieldValueNodeByText(
    const HOPParsedFile* file, int32_t initNode, const char* name, int32_t* outValueNode) {
    int32_t child;
    size_t  nameLen = 0u;
    if (outValueNode != NULL) {
        *outValueNode = -1;
    }
    if (file == NULL || name == NULL || outValueNode == NULL || initNode < 0
        || (uint32_t)initNode >= file->ast.len)
    {
        return 0;
    }
    while (name[nameLen] != '\0') {
        nameLen++;
    }
    child = file->ast.nodes[initNode].firstChild;
    if (child >= 0 && (uint32_t)child < file->ast.len
        && MirTypeNodeKind(file->ast.nodes[child].kind))
    {
        child = file->ast.nodes[child].nextSibling;
    }
    while (child >= 0) {
        const HOPAstNode* field = &file->ast.nodes[child];
        if (field->kind == HOPAst_COMPOUND_FIELD && field->dataEnd >= field->dataStart
            && (size_t)(field->dataEnd - field->dataStart) == nameLen
            && memcmp(file->source + field->dataStart, name, nameLen) == 0)
        {
            *outValueNode = field->firstChild;
            return *outValueNode >= 0 || (field->flags & HOPAstFlag_COMPOUND_FIELD_SHORTHAND) != 0u;
        }
        child = field->nextSibling;
    }
    return 0;
}

static int AppendMirBinaryInst(
    HOPMirInst* outInsts, uint32_t outCap, uint32_t* outLen, HOPTokenKind tok) {
    return AppendMirInst(
        outInsts,
        outCap,
        outLen,
        &(HOPMirInst){
            .op = HOPMirOp_BINARY,
            .tok = (uint16_t)tok,
            ._reserved = 0u,
            .aux = 0u,
            .start = 0u,
            .end = 0u,
        });
}

static int AppendMirIntConstInst(
    HOPArena*      arena,
    HOPMirProgram* program,
    HOPMirInst*    outInsts,
    uint32_t       outCap,
    uint32_t*      outLen,
    int64_t        value) {
    uint32_t constIndex = UINT32_MAX;
    return EnsureMirIntConst(arena, program, value, &constIndex) == 0
        && AppendMirInst(
               outInsts,
               outCap,
               outLen,
               &(HOPMirInst){
                   .op = HOPMirOp_PUSH_CONST,
                   .tok = 0u,
                   ._reserved = 0u,
                   .aux = constIndex,
                   .start = 0u,
                   .end = 0u,
               });
}

static int MirStaticTypeByteSize(const HOPMirProgram* program, uint32_t typeRefIndex) {
    const HOPMirTypeRef* typeRef;
    uint32_t             i;
    uint32_t             offset = 0u;
    uint32_t             maxAlign = 1u;
    if (program == NULL || typeRefIndex == UINT32_MAX || typeRefIndex >= program->typeLen) {
        return -1;
    }
    typeRef = &program->types[typeRefIndex];
    if (HOPMirTypeRefIsAggregate(typeRef)) {
        for (i = 0; i < program->fieldLen; i++) {
            int fieldSize;
            int fieldAlign;
            if (program->fields[i].ownerTypeRef != typeRefIndex) {
                continue;
            }
            if (program->fields[i].typeRef >= program->typeLen) {
                return -1;
            }
            if (HOPMirTypeRefIsVArrayView(&program->types[program->fields[i].typeRef])) {
                fieldSize = 0;
                fieldAlign = 1;
            } else if (HOPMirTypeRefIsStrObj(&program->types[program->fields[i].typeRef])) {
                fieldSize = 8;
                fieldAlign = 4;
            } else {
                fieldSize = MirStaticTypeByteSize(program, program->fields[i].typeRef);
                fieldAlign = fieldSize >= 4 ? 4 : fieldSize;
            }
            if (fieldSize < 0 || fieldAlign <= 0) {
                return -1;
            }
            if ((uint32_t)fieldAlign > maxAlign) {
                maxAlign = (uint32_t)fieldAlign;
            }
            offset = (offset + ((uint32_t)fieldAlign - 1u)) & ~((uint32_t)fieldAlign - 1u);
            offset += (uint32_t)fieldSize;
        }
        return (int)(maxAlign > 1u ? ((offset + (maxAlign - 1u)) & ~(maxAlign - 1u)) : offset);
    }
    if (HOPMirTypeRefIsStrObj(typeRef) || HOPMirTypeRefIsStrRef(typeRef)
        || HOPMirTypeRefIsSliceView(typeRef) || HOPMirTypeRefIsAggSliceView(typeRef))
    {
        return 8;
    }
    if (HOPMirTypeRefIsFixedArray(typeRef)) {
        return (int)(MirIntKindByteWidth(HOPMirTypeRefIntKind(typeRef))
                     * HOPMirTypeRefFixedArrayCount(typeRef));
    }
    if (HOPMirTypeRefIsFixedArrayView(typeRef) || HOPMirTypeRefIsStrPtr(typeRef)
        || HOPMirTypeRefIsOpaquePtr(typeRef) || HOPMirTypeRefIsU8Ptr(typeRef)
        || HOPMirTypeRefIsI8Ptr(typeRef) || HOPMirTypeRefIsU16Ptr(typeRef)
        || HOPMirTypeRefIsI16Ptr(typeRef) || HOPMirTypeRefIsU32Ptr(typeRef)
        || HOPMirTypeRefIsI32Ptr(typeRef) || HOPMirTypeRefIsFuncRef(typeRef))
    {
        return 4;
    }
    if (HOPMirTypeRefScalarKind(typeRef) == HOPMirTypeScalar_I32) {
        return (int)MirIntKindByteWidth(HOPMirTypeRefIntKind(typeRef));
    }
    return -1;
}

static int LowerMirVarSizeAllocNewSizeExpr(
    const HOPMirProgram*  program,
    HOPMirProgram*        mutableProgram,
    const HOPMirFunction* fn,
    const HOPParsedFile*  file,
    const HOPMirInst*     allocInst,
    uint32_t              pointeeTypeRef,
    HOPArena*             arena,
    HOPMirInst*           outInsts,
    uint32_t              outCap,
    uint32_t*             outLen) {
    int32_t              typeNode = -1;
    int32_t              countNode = -1;
    int32_t              initNode = -1;
    int32_t              allocNode = -1;
    uint32_t             len = 0u;
    uint32_t             i;
    const HOPMirTypeRef* pointee;
    int                  hasDynamic = 0;
    if (outLen != NULL) {
        *outLen = 0u;
    }
    if (program == NULL || mutableProgram == NULL || fn == NULL || file == NULL || allocInst == NULL
        || arena == NULL || outInsts == NULL || outLen == NULL || pointeeTypeRef >= program->typeLen
        || !DecodeNewExprNodes(
            file, (int32_t)allocInst->aux, &typeNode, &countNode, &initNode, &allocNode))
    {
        return 0;
    }
    pointee = &program->types[pointeeTypeRef];
    if (HOPMirTypeRefIsStrObj(pointee)) {
        int32_t lenNode = -1;
        if (initNode < 0 || !FindCompoundFieldValueNodeByText(file, initNode, "len", &lenNode)) {
            /* caller handles unsupported shape */
            return 0;
        }
        if (!LowerMirAllocNewCountExpr(
                program, mutableProgram, fn, file, lenNode, arena, outInsts, outCap, &len)
            || !AppendMirIntConstInst(arena, mutableProgram, outInsts, outCap, &len, 9)
            || !AppendMirBinaryInst(outInsts, outCap, &len, HOPTok_ADD))
        {
            return 0;
        }
        *outLen = len;
        return 1;
    }
    if (!HOPMirTypeRefIsAggregate(pointee)) {
        return 0;
    }
    if (!AppendMirIntConstInst(
            arena,
            mutableProgram,
            outInsts,
            outCap,
            &len,
            MirStaticTypeByteSize(program, pointeeTypeRef)))
    {
        return 0;
    }
    for (i = 0; i < program->fieldLen; i++) {
        const HOPMirField*   fieldRef;
        const HOPMirTypeRef* fieldType;
        int32_t              valueNode = -1;
        uint32_t             elemSize;
        uint32_t             align;
        if (program->fields[i].ownerTypeRef != pointeeTypeRef) {
            continue;
        }
        fieldRef = &program->fields[i];
        if (fieldRef->typeRef >= program->typeLen) {
            return 0;
        }
        fieldType = &program->types[fieldRef->typeRef];
        if (HOPMirTypeRefIsStrObj(fieldType)) {
            char     pathBuf[128];
            uint32_t baseLen;
            int32_t  valueNode = -1;
            if (fieldRef->sourceRef >= program->sourceLen || fieldRef->nameEnd < fieldRef->nameStart
                || initNode < 0)
            {
                return 0;
            }
            baseLen = fieldRef->nameEnd - fieldRef->nameStart;
            if (baseLen + 4u >= sizeof(pathBuf)) {
                return 0;
            }
            memcpy(
                pathBuf,
                program->sources[fieldRef->sourceRef].src.ptr + fieldRef->nameStart,
                (size_t)baseLen);
            memcpy(pathBuf + baseLen, ".len", 4u);
            pathBuf[baseLen + 4u] = '\0';
            hasDynamic = 1;
            (void)FindCompoundFieldValueNodeByText(file, initNode, pathBuf, &valueNode);
            if (valueNode >= 0) {
                uint32_t valueLen = 0u;
                if (!LowerMirAllocNewCountExpr(
                        program,
                        mutableProgram,
                        fn,
                        file,
                        valueNode,
                        arena,
                        outInsts + len,
                        outCap >= len ? outCap - len : 0u,
                        &valueLen))
                {
                    return 0;
                }
                len += valueLen;
            } else if (!AppendMirIntConstInst(arena, mutableProgram, outInsts, outCap, &len, 0)) {
                return 0;
            }
            if (!AppendMirIntConstInst(arena, mutableProgram, outInsts, outCap, &len, 1)
                || !AppendMirBinaryInst(outInsts, outCap, &len, HOPTok_ADD)
                || !AppendMirBinaryInst(outInsts, outCap, &len, HOPTok_ADD))
            {
                return 0;
            }
            continue;
        }
        if (!HOPMirTypeRefIsVArrayView(fieldType)) {
            continue;
        }
        hasDynamic = 1;
        elemSize = MirIntKindByteWidth(HOPMirTypeRefIntKind(fieldType));
        align = elemSize >= 4u ? 4u : elemSize;
        if (align > 1u
            && (!AppendMirIntConstInst(
                    arena, mutableProgram, outInsts, outCap, &len, (int64_t)(align - 1u))
                || !AppendMirBinaryInst(outInsts, outCap, &len, HOPTok_ADD)
                || !AppendMirIntConstInst(
                    arena, mutableProgram, outInsts, outCap, &len, -(int64_t)align)
                || !AppendMirBinaryInst(outInsts, outCap, &len, HOPTok_AND)))
        {
            return 0;
        }
        if (HOPMirTypeRefVArrayCountField(fieldType) == UINT32_MAX
            || HOPMirTypeRefVArrayCountField(fieldType) >= program->fieldLen)
        {
            return 0;
        }
        {
            const HOPMirField* countField =
                &program->fields[HOPMirTypeRefVArrayCountField(fieldType)];
            int32_t  countValueNode = -1;
            uint32_t countExprLen = 0u;
            if (!FindCompoundFieldValueNodeBySlice(
                    file, initNode, countField->nameStart, countField->nameEnd, &countValueNode)
                || !LowerMirAllocNewCountExpr(
                    program,
                    mutableProgram,
                    fn,
                    file,
                    countValueNode,
                    arena,
                    outInsts + len,
                    outCap >= len ? outCap - len : 0u,
                    &countExprLen))
            {
                return 0;
            }
            len += countExprLen;
        }
        if (elemSize > 1u
            && (!AppendMirIntConstInst(
                    arena, mutableProgram, outInsts, outCap, &len, (int64_t)elemSize)
                || !AppendMirBinaryInst(outInsts, outCap, &len, HOPTok_MUL)))
        {
            return 0;
        }
        if (!AppendMirBinaryInst(outInsts, outCap, &len, HOPTok_ADD)) {
            return 0;
        }
    }
    if (!hasDynamic) {
        return 0;
    }
    *outLen = len;
    return 1;
}

static int CountMirVarSizeAllocNewSizeExpr(
    const HOPMirProgram* program,
    const HOPParsedFile* file,
    const HOPMirInst*    allocInst,
    uint32_t             pointeeTypeRef,
    uint32_t*            outCount) {
    int32_t              typeNode = -1;
    int32_t              countNode = -1;
    int32_t              initNode = -1;
    int32_t              allocNode = -1;
    uint32_t             count = 0u;
    uint32_t             i;
    int                  hasDynamic = 0;
    const HOPMirTypeRef* pointee;
    if (outCount != NULL) {
        *outCount = 0u;
    }
    if (program == NULL || file == NULL || allocInst == NULL || outCount == NULL
        || pointeeTypeRef >= program->typeLen
        || !DecodeNewExprNodes(
            file, (int32_t)allocInst->aux, &typeNode, &countNode, &initNode, &allocNode))
    {
        return 0;
    }
    pointee = &program->types[pointeeTypeRef];
    if (HOPMirTypeRefIsStrObj(pointee)) {
        int32_t  lenNode = -1;
        uint32_t exprCount = 0u;
        if (initNode < 0 || !FindCompoundFieldValueNodeByText(file, initNode, "len", &lenNode)
            || !CountMirAllocNewCountExprInsts(file, lenNode, &exprCount))
        {
            return 0;
        }
        *outCount = exprCount + 2u;
        return 1;
    }
    if (!HOPMirTypeRefIsAggregate(pointee)) {
        return 0;
    }
    count = 1u;
    for (i = 0; i < program->fieldLen; i++) {
        const HOPMirField*   fieldRef;
        const HOPMirTypeRef* fieldType;
        if (program->fields[i].ownerTypeRef != pointeeTypeRef) {
            continue;
        }
        fieldRef = &program->fields[i];
        if (fieldRef->typeRef >= program->typeLen) {
            return 0;
        }
        fieldType = &program->types[fieldRef->typeRef];
        if (HOPMirTypeRefIsStrObj(fieldType)) {
            char     pathBuf[128];
            uint32_t baseLen;
            int32_t  valueNode = -1;
            uint32_t exprCount = 0u;
            if (fieldRef->sourceRef >= program->sourceLen || fieldRef->nameEnd < fieldRef->nameStart
                || initNode < 0)
            {
                return 0;
            }
            baseLen = fieldRef->nameEnd - fieldRef->nameStart;
            if (baseLen + 4u >= sizeof(pathBuf)) {
                return 0;
            }
            memcpy(
                pathBuf,
                program->sources[fieldRef->sourceRef].src.ptr + fieldRef->nameStart,
                (size_t)baseLen);
            memcpy(pathBuf + baseLen, ".len", 4u);
            pathBuf[baseLen + 4u] = '\0';
            hasDynamic = 1;
            if (FindCompoundFieldValueNodeByText(file, initNode, pathBuf, &valueNode)) {
                if (valueNode < 0 || !CountMirAllocNewCountExprInsts(file, valueNode, &exprCount)) {
                    return 0;
                }
                count += exprCount;
            } else {
                count += 1u;
            }
            count += 3u;
            continue;
        }
        if (HOPMirTypeRefIsVArrayView(fieldType)) {
            const HOPMirField* countField;
            int32_t            countValueNode = -1;
            uint32_t           exprCount = 0u;
            uint32_t           elemSize = MirIntKindByteWidth(HOPMirTypeRefIntKind(fieldType));
            uint32_t           countFieldRef = HOPMirTypeRefVArrayCountField(fieldType);
            hasDynamic = 1;
            if (countFieldRef == UINT32_MAX || countFieldRef >= program->fieldLen || initNode < 0) {
                return 0;
            }
            countField = &program->fields[countFieldRef];
            if (!FindCompoundFieldValueNodeBySlice(
                    file, initNode, countField->nameStart, countField->nameEnd, &countValueNode)
                || countValueNode < 0
                || !CountMirAllocNewCountExprInsts(file, countValueNode, &exprCount))
            {
                return 0;
            }
            if (elemSize >= 4u) {
                count += 4u;
            } else if (elemSize == 2u) {
                count += 4u;
            }
            count += exprCount;
            if (elemSize > 1u) {
                count += 2u;
            }
            count += 1u;
        }
    }
    if (!hasDynamic) {
        return 0;
    }
    *outCount = count;
    return 1;
}

static int LowerMirAllocNewAllocExpr(
    const HOPMirProgram*  program,
    HOPMirProgram*        mutableProgram,
    const HOPMirFunction* fn,
    const HOPParsedFile*  file,
    int32_t               exprNode,
    HOPArena*             arena,
    HOPMirInst*           outInsts,
    uint32_t              outCap,
    uint32_t*             outLen) {
    const HOPAstNode* n;
    if (outLen != NULL) {
        *outLen = 0u;
    }
    if (program == NULL || mutableProgram == NULL || fn == NULL || file == NULL || arena == NULL
        || outInsts == NULL || outLen == NULL || exprNode < 0 || (uint32_t)exprNode >= file->ast.len
        || outCap == 0u)
    {
        return 0;
    }
    n = &file->ast.nodes[exprNode];
    switch (n->kind) {
        case HOPAst_IDENT: {
            uint32_t localIndex = UINT32_MAX;
            if (!FindMirFunctionLocalBySlice(
                    program, fn, file->source, n->dataStart, n->dataEnd, &localIndex))
            {
                return 0;
            }
            outInsts[0] = (HOPMirInst){
                .op = HOPMirOp_LOCAL_LOAD,
                .tok = 0u,
                ._reserved = 0u,
                .aux = localIndex,
                .start = n->start,
                .end = n->end,
            };
            *outLen = 1u;
            return 1;
        }
        case HOPAst_NULL: {
            uint32_t constIndex = UINT32_MAX;
            if (EnsureMirNullConst(arena, mutableProgram, &constIndex) != 0) {
                return 0;
            }
            outInsts[0] = (HOPMirInst){
                .op = HOPMirOp_PUSH_CONST,
                .tok = 0u,
                ._reserved = 0u,
                .aux = constIndex,
                .start = n->start,
                .end = n->end,
            };
            *outLen = 1u;
            return 1;
        }
        case HOPAst_CAST:
            return LowerMirAllocNewAllocExpr(
                program, mutableProgram, fn, file, n->firstChild, arena, outInsts, outCap, outLen);
        case HOPAst_FIELD_EXPR: {
            int32_t  baseNode = n->firstChild;
            uint32_t field = HOPMirContextField_INVALID;
            if (baseNode < 0 || (uint32_t)baseNode >= file->ast.len
                || file->ast.nodes[baseNode].kind != HOPAst_IDENT
                || !SliceEqCStr(
                    file->source,
                    file->ast.nodes[baseNode].dataStart,
                    file->ast.nodes[baseNode].dataEnd,
                    "context"))
            {
                return 0;
            }
            if (SliceEqCStr(file->source, n->dataStart, n->dataEnd, "allocator")) {
                field = HOPMirContextField_ALLOCATOR;
            } else if (SliceEqCStr(file->source, n->dataStart, n->dataEnd, "temp_allocator")) {
                field = HOPMirContextField_TEMP_ALLOCATOR;
            } else {
                return 0;
            }
            outInsts[0] = (HOPMirInst){
                .op = HOPMirOp_CTX_GET,
                .tok = 0u,
                ._reserved = 0u,
                .aux = field,
                .start = n->start,
                .end = n->end,
            };
            *outLen = 1u;
            return 1;
        }
        default: return 0;
    }
}

static int AppendMirInst(
    HOPMirInst* outInsts, uint32_t outCap, uint32_t* _Nonnull ioLen, const HOPMirInst* inst) {
    if (outInsts == NULL || ioLen == NULL || inst == NULL || *ioLen >= outCap) {
        return 0;
    }
    outInsts[*ioLen] = *inst;
    (*ioLen)++;
    return 1;
}

static uint32_t MirInitOwnerTypeRefForType(const HOPMirProgram* program, uint32_t typeRef) {
    if (program == NULL || typeRef >= program->typeLen) {
        return UINT32_MAX;
    }
    if (HOPMirTypeRefIsAggregate(&program->types[typeRef])) {
        return typeRef;
    }
    if (HOPMirTypeRefIsOpaquePtr(&program->types[typeRef])) {
        return HOPMirTypeRefOpaquePointeeTypeRef(&program->types[typeRef]);
    }
    return UINT32_MAX;
}

static int MirTypeNodeKind(HOPAstKind kind) {
    return IsFnReturnTypeNodeKind(kind) || kind == HOPAst_TYPE_ANON_STRUCT
        || kind == HOPAst_TYPE_ANON_UNION;
}

static int LowerMirHeapInitValueExpr(
    const HOPPackageLoader* loader,
    const HOPMirProgram*    program,
    HOPMirProgram*          mutableProgram,
    const HOPMirFunction*   fn,
    const HOPParsedFile*    fnFile,
    const HOPParsedFile*    exprFile,
    int32_t                 exprNode,
    HOPArena*               arena,
    uint32_t                currentLocalIndex,
    uint32_t                currentOwnerTypeRef,
    uint32_t                expectedTypeRef,
    HOPMirInst*             outInsts,
    uint32_t                outCap,
    uint32_t*               outLen,
    uint32_t*               outTypeRef);

static const HOPAstNode* _Nullable ResolveMirAggregateDeclNode(
    const HOPPackageLoader* loader,
    const HOPMirProgram*    program,
    const HOPMirTypeRef*    typeRef,
    const HOPParsedFile** _Nullable outFile,
    uint32_t* _Nullable outSourceRef);

static int EnsureMirAggregateFieldRef(
    HOPArena*      arena,
    HOPMirProgram* program,
    uint32_t       nameStart,
    uint32_t       nameEnd,
    uint32_t       sourceRef,
    uint32_t       ownerTypeRef,
    uint32_t       typeRef,
    uint32_t* _Nonnull outFieldRef);

static const HOPSymbolDecl* _Nullable FindPackageTypeDeclBySlice(
    const HOPPackage* pkg, const char* src, uint32_t start, uint32_t end);

static int ResolveMirAggregateTypeRefForTypeNode(
    const HOPPackageLoader* loader,
    const HOPMirProgram*    program,
    const HOPParsedFile*    file,
    int32_t                 typeNode,
    uint32_t*               outTypeRef) {
    const HOPPackage*    pkg = NULL;
    const HOPAstNode*    node;
    const HOPSymbolDecl* decl;
    const HOPParsedFile* declFile;
    uint32_t             sourceRef;
    uint32_t             declSourceRef;
    if (outTypeRef != NULL) {
        *outTypeRef = UINT32_MAX;
    }
    if (loader == NULL || program == NULL || file == NULL || outTypeRef == NULL || typeNode < 0
        || (uint32_t)typeNode >= file->ast.len)
    {
        return 0;
    }
    sourceRef = FindMirSourceRefByText(program, file->source, file->sourceLen);
    if (sourceRef == UINT32_MAX) {
        return 0;
    }
    if (FindMirTypeRefByAstNode(program, sourceRef, typeNode, outTypeRef)
        && *outTypeRef < program->typeLen && HOPMirTypeRefIsAggregate(&program->types[*outTypeRef]))
    {
        return 1;
    }
    node = &file->ast.nodes[typeNode];
    if (node->kind != HOPAst_TYPE_NAME) {
        return 0;
    }
    if (FindLoaderFileByMirSource(loader, program, sourceRef, &pkg) == NULL || pkg == NULL) {
        return 0;
    }
    decl = FindPackageTypeDeclBySlice(pkg, file->source, node->dataStart, node->dataEnd);
    if (decl == NULL || decl->nodeId < 0 || (uint32_t)decl->fileIndex >= pkg->fileLen) {
        return 0;
    }
    declFile = &pkg->files[decl->fileIndex];
    declSourceRef = FindMirSourceRefByText(program, declFile->source, declFile->sourceLen);
    if (declSourceRef == UINT32_MAX) {
        return 0;
    }
    if (!FindMirTypeRefByAstNode(program, declSourceRef, decl->nodeId, outTypeRef)
        || *outTypeRef >= program->typeLen
        || !HOPMirTypeRefIsAggregate(&program->types[*outTypeRef]))
    {
        return 0;
    }
    return 1;
}

static int FindMirFieldByOwnerAndSlicePromotedDepth(
    const HOPPackageLoader* loader,
    const HOPMirProgram*    program,
    uint32_t                ownerTypeRef,
    uint32_t                sourceRef,
    uint32_t                nameStart,
    uint32_t                nameEnd,
    uint32_t                depth,
    uint32_t*               outFieldIndex) {
    const HOPParsedFile* typeFile = NULL;
    const HOPAstNode*    structNode;
    uint32_t             typeSourceRef = UINT32_MAX;
    int32_t              fieldNode;
    if (FindMirFieldByOwnerAndSlice(
            program, ownerTypeRef, sourceRef, nameStart, nameEnd, outFieldIndex))
    {
        return 1;
    }
    if (depth > 16u || ownerTypeRef >= program->typeLen) {
        return 0;
    }
    structNode = ResolveMirAggregateDeclNode(
        loader, program, &program->types[ownerTypeRef], &typeFile, &typeSourceRef);
    if (structNode == NULL || typeFile == NULL || structNode->kind != HOPAst_STRUCT) {
        return 0;
    }
    fieldNode = structNode->firstChild;
    while (fieldNode >= 0) {
        const HOPAstNode* fieldDecl = &typeFile->ast.nodes[fieldNode];
        uint32_t          embeddedFieldIndex = UINT32_MAX;
        uint32_t          embeddedTypeRef;
        if (fieldDecl->kind != HOPAst_FIELD) {
            fieldNode = fieldDecl->nextSibling;
            continue;
        }
        if ((fieldDecl->flags & HOPAstFlag_FIELD_EMBEDDED) == 0u
            || !FindMirFieldByOwnerAndSlice(
                program,
                ownerTypeRef,
                typeSourceRef,
                fieldDecl->dataStart,
                fieldDecl->dataEnd,
                &embeddedFieldIndex))
        {
            fieldNode = fieldDecl->nextSibling;
            continue;
        }
        embeddedTypeRef = program->fields[embeddedFieldIndex].typeRef;
        if (FindMirFieldByOwnerAndSlicePromotedDepth(
                loader,
                program,
                embeddedTypeRef,
                sourceRef,
                nameStart,
                nameEnd,
                depth + 1u,
                outFieldIndex))
        {
            return 1;
        }
        fieldNode = fieldDecl->nextSibling;
    }
    return 0;
}

static int FindMirFieldByOwnerAndSlicePromoted(
    const HOPPackageLoader* loader,
    const HOPMirProgram*    program,
    uint32_t                ownerTypeRef,
    uint32_t                sourceRef,
    uint32_t                nameStart,
    uint32_t                nameEnd,
    uint32_t*               outFieldIndex) {
    if (outFieldIndex != NULL) {
        *outFieldIndex = UINT32_MAX;
    }
    if (loader == NULL || program == NULL || outFieldIndex == NULL) {
        return 0;
    }
    return FindMirFieldByOwnerAndSlicePromotedDepth(
        loader, program, ownerTypeRef, sourceRef, nameStart, nameEnd, 0u, outFieldIndex);
}

static int LowerMirHeapInitValueBySlice(
    const HOPPackageLoader* loader,
    const HOPMirProgram*    program,
    const HOPMirFunction*   fn,
    const HOPParsedFile*    fnFile,
    const HOPParsedFile*    exprFile,
    uint32_t                nameStart,
    uint32_t                nameEnd,
    uint32_t                start,
    uint32_t                end,
    uint32_t                currentLocalIndex,
    uint32_t                currentOwnerTypeRef,
    HOPMirInst*             outInsts,
    uint32_t                outCap,
    uint32_t*               outLen,
    uint32_t*               outTypeRef) {
    uint32_t localIndex = UINT32_MAX;
    uint32_t fieldIndex = UINT32_MAX;
    if (outLen == NULL || outTypeRef == NULL || loader == NULL || program == NULL || fn == NULL
        || exprFile == NULL || nameEnd < nameStart)
    {
        return 0;
    }
    *outTypeRef = UINT32_MAX;
    if (fnFile != NULL && fnFile == exprFile
        && FindMirFunctionLocalBySlice(
            program, fn, fnFile->source, nameStart, nameEnd, &localIndex))
    {
        if (!AppendMirInst(
                outInsts,
                outCap,
                outLen,
                &(HOPMirInst){
                    .op = HOPMirOp_LOCAL_LOAD,
                    .tok = 0u,
                    ._reserved = 0u,
                    .aux = localIndex,
                    .start = start,
                    .end = end,
                }))
        {
            return 0;
        }
        *outTypeRef = program->locals[fn->localStart + localIndex].typeRef;
        return 1;
    }
    if (currentOwnerTypeRef != UINT32_MAX
        && FindMirFieldByOwnerAndSlicePromoted(
            loader,
            program,
            currentOwnerTypeRef,
            FindMirSourceRefByText(program, exprFile->source, exprFile->sourceLen),
            nameStart,
            nameEnd,
            &fieldIndex))
    {
        if (!AppendMirInst(
                outInsts,
                outCap,
                outLen,
                &(HOPMirInst){
                    .op = HOPMirOp_LOCAL_LOAD,
                    .tok = 0u,
                    ._reserved = 0u,
                    .aux = currentLocalIndex,
                    .start = start,
                    .end = end,
                })
            || !AppendMirInst(
                outInsts,
                outCap,
                outLen,
                &(HOPMirInst){
                    .op = HOPMirOp_AGG_GET,
                    .tok = 0u,
                    ._reserved = 0u,
                    .aux = fieldIndex,
                    .start = start,
                    .end = end,
                }))
        {
            return 0;
        }
        *outTypeRef = program->fields[fieldIndex].typeRef;
        return 1;
    }
    return 0;
}

static int LowerMirHeapInitCompoundLiteral(
    const HOPPackageLoader* loader,
    const HOPMirProgram*    program,
    HOPMirProgram*          mutableProgram,
    const HOPMirFunction*   fn,
    const HOPParsedFile*    fnFile,
    const HOPParsedFile*    exprFile,
    int32_t                 exprNode,
    HOPArena*               arena,
    uint32_t                currentLocalIndex,
    uint32_t                currentOwnerTypeRef,
    uint32_t                expectedTypeRef,
    HOPMirInst*             outInsts,
    uint32_t                outCap,
    uint32_t*               outLen,
    uint32_t*               outTypeRef) {
    const HOPAstNode* lit;
    int32_t           child;
    int32_t           typeNode = -1;
    uint32_t          ownerTypeRef = expectedTypeRef;
    uint32_t          exprSourceRef;
    if (outLen == NULL || outTypeRef == NULL || program == NULL || exprFile == NULL || exprNode < 0
        || (uint32_t)exprNode >= exprFile->ast.len)
    {
        return 0;
    }
    lit = &exprFile->ast.nodes[exprNode];
    if (lit->kind != HOPAst_COMPOUND_LIT) {
        return 0;
    }
    child = lit->firstChild;
    exprSourceRef = FindMirSourceRefByText(program, exprFile->source, exprFile->sourceLen);
    if (child >= 0 && (uint32_t)child < exprFile->ast.len
        && MirTypeNodeKind(exprFile->ast.nodes[child].kind))
    {
        typeNode = child;
        child = exprFile->ast.nodes[child].nextSibling;
        if (!ResolveMirAggregateTypeRefForTypeNode(
                loader, program, exprFile, typeNode, &ownerTypeRef))
        {
            return 0;
        }
    }
    if (ownerTypeRef >= program->typeLen
        || !HOPMirTypeRefIsAggregate(&program->types[ownerTypeRef]))
    {
        return 0;
    }
    if (!AppendMirInst(
            outInsts,
            outCap,
            outLen,
            &(HOPMirInst){
                .op = HOPMirOp_AGG_ZERO,
                .tok = 0u,
                ._reserved = 0u,
                .aux = ownerTypeRef,
                .start = lit->start,
                .end = lit->end,
            }))
    {
        return 0;
    }
    while (child >= 0) {
        const HOPAstNode* field = &exprFile->ast.nodes[child];
        int32_t           valueNode = field->firstChild;
        uint32_t          fieldIndex = UINT32_MAX;
        uint32_t          fieldTypeRef = UINT32_MAX;
        uint32_t          valueTypeRef = UINT32_MAX;
        uint32_t          beforeLen;
        if (field->kind != HOPAst_COMPOUND_FIELD || field->dataEnd < field->dataStart
            || field->nextSibling == child)
        {
            return 0;
        }
        if (memchr(exprFile->source + field->dataStart, '.', field->dataEnd - field->dataStart)
                != NULL
            || !FindMirFieldByOwnerAndSlicePromoted(
                loader,
                program,
                ownerTypeRef,
                exprSourceRef,
                field->dataStart,
                field->dataEnd,
                &fieldIndex))
        {
            return 0;
        }
        fieldTypeRef = program->fields[fieldIndex].typeRef;
        beforeLen = *outLen;
        if (valueNode >= 0) {
            if (!LowerMirHeapInitValueExpr(
                    loader,
                    program,
                    mutableProgram,
                    fn,
                    fnFile,
                    exprFile,
                    valueNode,
                    arena,
                    currentLocalIndex,
                    currentOwnerTypeRef,
                    fieldTypeRef,
                    outInsts,
                    outCap,
                    outLen,
                    &valueTypeRef))
            {
                return 0;
            }
        } else if ((field->flags & HOPAstFlag_COMPOUND_FIELD_SHORTHAND) != 0u) {
            if (!LowerMirHeapInitValueBySlice(
                    loader,
                    program,
                    fn,
                    fnFile,
                    exprFile,
                    field->dataStart,
                    field->dataEnd,
                    field->start,
                    field->end,
                    currentLocalIndex,
                    currentOwnerTypeRef,
                    outInsts,
                    outCap,
                    outLen,
                    &valueTypeRef))
            {
                return 0;
            }
        } else {
            return 0;
        }
        if (!AppendMirInst(
                outInsts,
                outCap,
                outLen,
                &(HOPMirInst){
                    .op = HOPMirOp_AGG_SET,
                    .tok = 0u,
                    ._reserved = 0u,
                    .aux = fieldIndex,
                    .start = field->start,
                    .end = field->end,
                }))
        {
            *outLen = beforeLen;
            return 0;
        }
        child = field->nextSibling;
    }
    *outTypeRef = ownerTypeRef;
    return 1;
}

static int LowerMirHeapInitValueExpr(
    const HOPPackageLoader* loader,
    const HOPMirProgram*    program,
    HOPMirProgram*          mutableProgram,
    const HOPMirFunction*   fn,
    const HOPParsedFile*    fnFile,
    const HOPParsedFile*    exprFile,
    int32_t                 exprNode,
    HOPArena*               arena,
    uint32_t                currentLocalIndex,
    uint32_t                currentOwnerTypeRef,
    uint32_t                expectedTypeRef,
    HOPMirInst*             outInsts,
    uint32_t                outCap,
    uint32_t*               outLen,
    uint32_t*               outTypeRef) {
    const HOPAstNode* n;
    if (outTypeRef != NULL) {
        *outTypeRef = UINT32_MAX;
    }
    if (loader == NULL || program == NULL || mutableProgram == NULL || fn == NULL
        || exprFile == NULL || arena == NULL || outInsts == NULL || outLen == NULL
        || outTypeRef == NULL || exprNode < 0 || (uint32_t)exprNode >= exprFile->ast.len)
    {
        return 0;
    }
    n = &exprFile->ast.nodes[exprNode];
    switch (n->kind) {
        case HOPAst_INT: {
            int64_t  value = 0;
            uint32_t constIndex = UINT32_MAX;
            if (!ParseMirIntLiteral(exprFile->source, n->dataStart, n->dataEnd, &value)
                || EnsureMirIntConst(arena, mutableProgram, value, &constIndex) != 0
                || !AppendMirInst(
                    outInsts,
                    outCap,
                    outLen,
                    &(HOPMirInst){
                        .op = HOPMirOp_PUSH_CONST,
                        .tok = 0u,
                        ._reserved = 0u,
                        .aux = constIndex,
                        .start = n->start,
                        .end = n->end,
                    }))
            {
                return 0;
            }
            return 1;
        }
        case HOPAst_BOOL: {
            uint32_t constIndex = UINT32_MAX;
            bool     value = SliceEqCStr(exprFile->source, n->dataStart, n->dataEnd, "true");
            if ((!value && !SliceEqCStr(exprFile->source, n->dataStart, n->dataEnd, "false"))
                || EnsureMirBoolConst(arena, mutableProgram, value, &constIndex) != 0
                || !AppendMirInst(
                    outInsts,
                    outCap,
                    outLen,
                    &(HOPMirInst){
                        .op = HOPMirOp_PUSH_CONST,
                        .tok = 0u,
                        ._reserved = 0u,
                        .aux = constIndex,
                        .start = n->start,
                        .end = n->end,
                    }))
            {
                return 0;
            }
            return 1;
        }
        case HOPAst_STRING: {
            uint8_t*        bytes = NULL;
            uint32_t        len = 0u;
            HOPStringLitErr litErr = { 0 };
            uint32_t        constIndex = UINT32_MAX;
            if (HOPDecodeStringLiteralArena(
                    arena, exprFile->source, n->dataStart, n->dataEnd, &bytes, &len, &litErr)
                    != 0
                || EnsureMirStringConst(
                       arena,
                       mutableProgram,
                       (HOPStrView){ .ptr = (const char*)bytes, .len = len },
                       &constIndex)
                       != 0
                || !AppendMirInst(
                    outInsts,
                    outCap,
                    outLen,
                    &(HOPMirInst){
                        .op = HOPMirOp_PUSH_CONST,
                        .tok = 0u,
                        ._reserved = 0u,
                        .aux = constIndex,
                        .start = n->start,
                        .end = n->end,
                    }))
            {
                return 0;
            }
            return 1;
        }
        case HOPAst_NULL: {
            uint32_t constIndex = UINT32_MAX;
            if (EnsureMirNullConst(arena, mutableProgram, &constIndex) != 0
                || !AppendMirInst(
                    outInsts,
                    outCap,
                    outLen,
                    &(HOPMirInst){
                        .op = HOPMirOp_PUSH_CONST,
                        .tok = 0u,
                        ._reserved = 0u,
                        .aux = constIndex,
                        .start = n->start,
                        .end = n->end,
                    }))
            {
                return 0;
            }
            return 1;
        }
        case HOPAst_IDENT:
            return LowerMirHeapInitValueBySlice(
                loader,
                program,
                fn,
                fnFile,
                exprFile,
                n->dataStart,
                n->dataEnd,
                n->start,
                n->end,
                currentLocalIndex,
                currentOwnerTypeRef,
                outInsts,
                outCap,
                outLen,
                outTypeRef);
        case HOPAst_UNARY: {
            if (!LowerMirHeapInitValueExpr(
                    loader,
                    program,
                    mutableProgram,
                    fn,
                    fnFile,
                    exprFile,
                    n->firstChild,
                    arena,
                    currentLocalIndex,
                    currentOwnerTypeRef,
                    expectedTypeRef,
                    outInsts,
                    outCap,
                    outLen,
                    outTypeRef)
                || !AppendMirInst(
                    outInsts,
                    outCap,
                    outLen,
                    &(HOPMirInst){
                        .op = HOPMirOp_UNARY,
                        .tok = (uint16_t)n->op,
                        ._reserved = 0u,
                        .aux = 0u,
                        .start = n->start,
                        .end = n->end,
                    }))
            {
                return 0;
            }
            *outTypeRef = UINT32_MAX;
            return 1;
        }
        case HOPAst_BINARY: {
            int32_t  rhsNode = ASTNextSibling(&exprFile->ast, n->firstChild);
            uint32_t rhsTypeRef = UINT32_MAX;
            if (rhsNode < 0
                || !LowerMirHeapInitValueExpr(
                    loader,
                    program,
                    mutableProgram,
                    fn,
                    fnFile,
                    exprFile,
                    n->firstChild,
                    arena,
                    currentLocalIndex,
                    currentOwnerTypeRef,
                    expectedTypeRef,
                    outInsts,
                    outCap,
                    outLen,
                    outTypeRef)
                || !LowerMirHeapInitValueExpr(
                    loader,
                    program,
                    mutableProgram,
                    fn,
                    fnFile,
                    exprFile,
                    rhsNode,
                    arena,
                    currentLocalIndex,
                    currentOwnerTypeRef,
                    expectedTypeRef,
                    outInsts,
                    outCap,
                    outLen,
                    &rhsTypeRef)
                || !AppendMirInst(
                    outInsts,
                    outCap,
                    outLen,
                    &(HOPMirInst){
                        .op = HOPMirOp_BINARY,
                        .tok = (uint16_t)n->op,
                        ._reserved = 0u,
                        .aux = 0u,
                        .start = n->start,
                        .end = n->end,
                    }))
            {
                return 0;
            }
            *outTypeRef = UINT32_MAX;
            return 1;
        }
        case HOPAst_FIELD_EXPR: {
            uint32_t baseTypeRef = UINT32_MAX;
            uint32_t ownerTypeRef = UINT32_MAX;
            uint32_t fieldIndex = UINT32_MAX;
            int32_t  baseNode = n->firstChild;
            if (baseNode < 0
                || !LowerMirHeapInitValueExpr(
                    loader,
                    program,
                    mutableProgram,
                    fn,
                    fnFile,
                    exprFile,
                    baseNode,
                    arena,
                    currentLocalIndex,
                    currentOwnerTypeRef,
                    expectedTypeRef,
                    outInsts,
                    outCap,
                    outLen,
                    &baseTypeRef))
            {
                return 0;
            }
            ownerTypeRef = MirInitOwnerTypeRefForType(program, baseTypeRef);
            if (ownerTypeRef == UINT32_MAX
                || !FindMirFieldByOwnerAndSlicePromoted(
                    loader,
                    program,
                    ownerTypeRef,
                    FindMirSourceRefByText(program, exprFile->source, exprFile->sourceLen),
                    n->dataStart,
                    n->dataEnd,
                    &fieldIndex)
                || !AppendMirInst(
                    outInsts,
                    outCap,
                    outLen,
                    &(HOPMirInst){
                        .op = HOPMirOp_AGG_GET,
                        .tok = 0u,
                        ._reserved = 0u,
                        .aux = fieldIndex,
                        .start = n->start,
                        .end = n->end,
                    }))
            {
                return 0;
            }
            *outTypeRef = program->fields[fieldIndex].typeRef;
            return 1;
        }
        case HOPAst_COMPOUND_LIT:
            return LowerMirHeapInitCompoundLiteral(
                loader,
                program,
                mutableProgram,
                fn,
                fnFile,
                exprFile,
                exprNode,
                arena,
                currentLocalIndex,
                currentOwnerTypeRef,
                expectedTypeRef,
                outInsts,
                outCap,
                outLen,
                outTypeRef);
        default: return 0;
    }
}

static int LowerMirAllocNewPostInitInsts(
    const HOPPackageLoader* loader,
    const HOPMirProgram*    program,
    HOPMirProgram*          mutableProgram,
    const HOPMirFunction*   fn,
    const HOPParsedFile*    fnFile,
    const HOPParsedFile*    typeFile,
    const HOPParsedFile*    newFile,
    const HOPMirInst*       allocInst,
    const HOPMirInst*       storeInst,
    uint32_t                pointeeTypeRef,
    HOPArena*               arena,
    HOPMirInst*             outInsts,
    uint32_t                outCap,
    uint32_t*               outLen,
    bool*                   outClearedInitFlag) {
    int32_t           typeNode = -1;
    int32_t           countNode = -1;
    int32_t           initNode = -1;
    int32_t           allocNode = -1;
    const HOPAstNode* structNode;
    const HOPAstNode* astNode;
    uint32_t          fieldSourceRef;
    uint32_t          typeSourceRef = UINT32_MAX;
    uint32_t          emittedLen = 0u;
    uint32_t          localIndex = storeInst->aux;
    bool              clearedInitFlag = false;
    bool              explicitDirect[256] = { 0 };
    uint32_t          directFieldIndices[256];
    uint32_t          directFieldCount = 0u;
    uint32_t          i;
    if (outLen != NULL) {
        *outLen = 0u;
    }
    if (outClearedInitFlag != NULL) {
        *outClearedInitFlag = false;
    }
    if (loader == NULL || program == NULL || mutableProgram == NULL || fn == NULL || newFile == NULL
        || typeFile == NULL || allocInst == NULL || storeInst == NULL || outInsts == NULL
        || outLen == NULL || outClearedInitFlag == NULL || pointeeTypeRef >= program->typeLen)
    {
        return 0;
    }
    if (!DecodeNewExprNodes(
            newFile, (int32_t)allocInst->aux, &typeNode, &countNode, &initNode, &allocNode))
    {
        return 0;
    }
    astNode = &newFile->ast.nodes[allocInst->aux];
    fieldSourceRef = FindMirSourceRefByText(program, newFile->source, newFile->sourceLen);
    if (astNode->kind != HOPAst_NEW || fieldSourceRef == UINT32_MAX) {
        return 0;
    }
    if ((astNode->flags & HOPAstFlag_NEW_HAS_INIT) != 0u) {
        const HOPAstNode* initLit;
        int32_t           child;
        if (initNode < 0 || (uint32_t)initNode >= newFile->ast.len) {
            return 0;
        }
        initLit = &newFile->ast.nodes[initNode];
        if (initLit->kind != HOPAst_COMPOUND_LIT) {
            return 0;
        }
        child = initLit->firstChild;
        if (child >= 0 && (uint32_t)child < newFile->ast.len
            && MirTypeNodeKind(newFile->ast.nodes[child].kind))
        {
            child = newFile->ast.nodes[child].nextSibling;
        }
        while (child >= 0) {
            const HOPAstNode* field = &newFile->ast.nodes[child];
            int32_t           valueNode = field->firstChild;
            uint32_t          fieldIndex = UINT32_MAX;
            uint32_t          fieldTypeRef = UINT32_MAX;
            uint32_t          valueTypeRef = UINT32_MAX;
            const char*       dot;
            if (field->kind != HOPAst_COMPOUND_FIELD || field->dataEnd < field->dataStart) {
                return 0;
            }
            dot = memchr(
                newFile->source + field->dataStart, '.', field->dataEnd - field->dataStart);
            if (HOPMirTypeRefIsStrObj(&program->types[pointeeTypeRef])) {
                uint32_t pseudoFieldRef = UINT32_MAX;
                uint32_t expectedTypeRef = UINT32_MAX;
                if (dot != NULL) {
                    return 0;
                }
                if (SliceEqCStr(newFile->source, field->dataStart, field->dataEnd, "len")) {
                    if (EnsureMirScalarTypeRef(
                            arena, mutableProgram, HOPMirTypeScalar_I32, &expectedTypeRef)
                        != 0)
                    {
                        return 0;
                    }
                    if (!FindMirPseudoFieldByName(program, "len", &pseudoFieldRef)
                        && EnsureMirAggregateFieldRef(
                               arena,
                               mutableProgram,
                               field->dataStart,
                               field->dataEnd,
                               fieldSourceRef,
                               UINT32_MAX,
                               UINT32_MAX,
                               &pseudoFieldRef)
                               != 0)
                    {
                        return 0;
                    }
                } else {
                    return 0;
                }
                if (!AppendMirInst(
                        outInsts,
                        outCap,
                        &emittedLen,
                        &(HOPMirInst){
                            .op = HOPMirOp_LOCAL_LOAD,
                            .tok = 0u,
                            ._reserved = 0u,
                            .aux = localIndex,
                            .start = field->start,
                            .end = field->end,
                        })
                    || !LowerMirHeapInitValueExpr(
                        loader,
                        program,
                        mutableProgram,
                        fn,
                        fnFile,
                        newFile,
                        valueNode,
                        arena,
                        localIndex,
                        pointeeTypeRef,
                        expectedTypeRef,
                        outInsts,
                        outCap,
                        &emittedLen,
                        &valueTypeRef)
                    || !AppendMirInst(
                        outInsts,
                        outCap,
                        &emittedLen,
                        &(HOPMirInst){
                            .op = HOPMirOp_AGG_SET,
                            .tok = 0u,
                            ._reserved = 0u,
                            .aux = pseudoFieldRef,
                            .start = field->start,
                            .end = field->end,
                        })
                    || !AppendMirInst(
                        outInsts,
                        outCap,
                        &emittedLen,
                        &(HOPMirInst){
                            .op = HOPMirOp_DROP,
                            .tok = 0u,
                            ._reserved = 0u,
                            .aux = 0u,
                            .start = field->start,
                            .end = field->end,
                        }))
                {
                    return 0;
                }
                child = field->nextSibling;
                clearedInitFlag = true;
                continue;
            }
            if (dot != NULL) {
                uint32_t baseFieldRef = UINT32_MAX;
                uint32_t pseudoFieldRef = UINT32_MAX;
                uint32_t expectedTypeRef = UINT32_MAX;
                if (!FindMirFieldByOwnerAndSlice(
                        program,
                        pointeeTypeRef,
                        fieldSourceRef,
                        field->dataStart,
                        (uint32_t)(dot - newFile->source),
                        &baseFieldRef))
                {
                    return 0;
                }
                if (directFieldCount
                    >= (uint32_t)(sizeof(directFieldIndices) / sizeof(directFieldIndices[0])))
                {
                    return 0;
                }
                directFieldIndices[directFieldCount] = baseFieldRef;
                explicitDirect[directFieldCount] = true;
                directFieldCount++;
                if (program->fields[baseFieldRef].typeRef >= program->typeLen
                    || !HOPMirTypeRefIsStrObj(
                        &program->types[program->fields[baseFieldRef].typeRef]))
                {
                    return 0;
                }
                if (SliceEqCStr(
                        newFile->source,
                        (uint32_t)(dot - newFile->source) + 1u,
                        field->dataEnd,
                        "len"))
                {
                    if (EnsureMirScalarTypeRef(
                            arena, mutableProgram, HOPMirTypeScalar_I32, &expectedTypeRef)
                        != 0)
                    {
                        return 0;
                    }
                    if (!FindMirPseudoFieldByName(program, "len", &pseudoFieldRef)
                        && EnsureMirAggregateFieldRef(
                               arena,
                               mutableProgram,
                               (uint32_t)(dot - newFile->source) + 1u,
                               field->dataEnd,
                               fieldSourceRef,
                               UINT32_MAX,
                               UINT32_MAX,
                               &pseudoFieldRef)
                               != 0)
                    {
                        return 0;
                    }
                } else {
                    return 0;
                }
                if (!AppendMirInst(
                        outInsts,
                        outCap,
                        &emittedLen,
                        &(HOPMirInst){
                            .op = HOPMirOp_LOCAL_LOAD,
                            .tok = 0u,
                            ._reserved = 0u,
                            .aux = localIndex,
                            .start = field->start,
                            .end = field->end,
                        })
                    || !AppendMirInst(
                        outInsts,
                        outCap,
                        &emittedLen,
                        &(HOPMirInst){
                            .op = HOPMirOp_AGG_GET,
                            .tok = 0u,
                            ._reserved = 0u,
                            .aux = baseFieldRef,
                            .start = field->start,
                            .end = field->end,
                        })
                    || !LowerMirHeapInitValueExpr(
                        loader,
                        program,
                        mutableProgram,
                        fn,
                        fnFile,
                        newFile,
                        valueNode,
                        arena,
                        localIndex,
                        pointeeTypeRef,
                        expectedTypeRef,
                        outInsts,
                        outCap,
                        &emittedLen,
                        &valueTypeRef)
                    || !AppendMirInst(
                        outInsts,
                        outCap,
                        &emittedLen,
                        &(HOPMirInst){
                            .op = HOPMirOp_AGG_SET,
                            .tok = 0u,
                            ._reserved = 0u,
                            .aux = pseudoFieldRef,
                            .start = field->start,
                            .end = field->end,
                        })
                    || !AppendMirInst(
                        outInsts,
                        outCap,
                        &emittedLen,
                        &(HOPMirInst){
                            .op = HOPMirOp_DROP,
                            .tok = 0u,
                            ._reserved = 0u,
                            .aux = 0u,
                            .start = field->start,
                            .end = field->end,
                        }))
                {
                    return 0;
                }
                child = field->nextSibling;
                clearedInitFlag = true;
                continue;
            }
            if (!FindMirFieldByOwnerAndSlice(
                    program,
                    pointeeTypeRef,
                    fieldSourceRef,
                    field->dataStart,
                    field->dataEnd,
                    &fieldIndex))
            {
                return 0;
            }
            if (directFieldCount
                >= (uint32_t)(sizeof(directFieldIndices) / sizeof(directFieldIndices[0])))
            {
                return 0;
            }
            directFieldIndices[directFieldCount] = fieldIndex;
            explicitDirect[directFieldCount] = true;
            directFieldCount++;
            fieldTypeRef = program->fields[fieldIndex].typeRef;
            if (!AppendMirInst(
                    outInsts,
                    outCap,
                    &emittedLen,
                    &(HOPMirInst){
                        .op = HOPMirOp_LOCAL_LOAD,
                        .tok = 0u,
                        ._reserved = 0u,
                        .aux = localIndex,
                        .start = field->start,
                        .end = field->end,
                    }))
            {
                return 0;
            }
            if (valueNode >= 0) {
                if (!LowerMirHeapInitValueExpr(
                        loader,
                        program,
                        mutableProgram,
                        fn,
                        fnFile,
                        newFile,
                        valueNode,
                        arena,
                        localIndex,
                        pointeeTypeRef,
                        fieldTypeRef,
                        outInsts,
                        outCap,
                        &emittedLen,
                        &valueTypeRef))
                {
                    return 0;
                }
            } else if ((field->flags & HOPAstFlag_COMPOUND_FIELD_SHORTHAND) != 0u) {
                if (!LowerMirHeapInitValueBySlice(
                        loader,
                        program,
                        fn,
                        fnFile,
                        newFile,
                        field->dataStart,
                        field->dataEnd,
                        field->start,
                        field->end,
                        localIndex,
                        pointeeTypeRef,
                        outInsts,
                        outCap,
                        &emittedLen,
                        &valueTypeRef))
                {
                    return 0;
                }
            } else {
                return 0;
            }
            if (!AppendMirInst(
                    outInsts,
                    outCap,
                    &emittedLen,
                    &(HOPMirInst){
                        .op = HOPMirOp_AGG_SET,
                        .tok = 0u,
                        ._reserved = 0u,
                        .aux = fieldIndex,
                        .start = field->start,
                        .end = field->end,
                    })
                || !AppendMirInst(
                    outInsts,
                    outCap,
                    &emittedLen,
                    &(HOPMirInst){
                        .op = HOPMirOp_DROP,
                        .tok = 0u,
                        ._reserved = 0u,
                        .aux = 0u,
                        .start = field->start,
                        .end = field->end,
                    }))
            {
                return 0;
            }
            child = field->nextSibling;
        }
        clearedInitFlag = true;
    }
    if (!HOPMirTypeRefIsAggregate(&program->types[pointeeTypeRef])) {
        *outLen = emittedLen;
        *outClearedInitFlag = clearedInitFlag;
        return emittedLen != 0u || clearedInitFlag;
    }
    structNode = ResolveMirAggregateDeclNode(
        loader, program, &program->types[pointeeTypeRef], &typeFile, &typeSourceRef);
    if (structNode == NULL || structNode->kind != HOPAst_STRUCT) {
        *outLen = emittedLen;
        *outClearedInitFlag = clearedInitFlag;
        return emittedLen != 0u || clearedInitFlag;
    }
    for (i = 0; i < program->fieldLen; i++) {
        if (program->fields[i].ownerTypeRef == pointeeTypeRef) {
            directFieldIndices[directFieldCount++] = i;
            if (directFieldCount
                >= (uint32_t)(sizeof(directFieldIndices) / sizeof(directFieldIndices[0])))
            {
                break;
            }
        }
    }
    {
        int32_t fieldNode = structNode->firstChild;
        while (fieldNode >= 0) {
            const HOPAstNode* fieldDecl = &typeFile->ast.nodes[fieldNode];
            int32_t           typeChild;
            int32_t           defaultExprNode;
            uint32_t          fieldIndex = UINT32_MAX;
            uint32_t          fieldTypeRef = UINT32_MAX;
            uint32_t          valueTypeRef = UINT32_MAX;
            bool              alreadyExplicit = false;
            uint32_t          directIndex;
            if (fieldDecl->kind != HOPAst_FIELD) {
                fieldNode = fieldDecl->nextSibling;
                continue;
            }
            typeChild = fieldDecl->firstChild;
            if (typeChild < 0 || (uint32_t)typeChild >= typeFile->ast.len) {
                return 0;
            }
            defaultExprNode = typeFile->ast.nodes[typeChild].nextSibling;
            if (defaultExprNode < 0) {
                fieldNode = fieldDecl->nextSibling;
                continue;
            }
            if (!FindMirFieldByOwnerAndSlice(
                    program,
                    pointeeTypeRef,
                    typeSourceRef,
                    fieldDecl->dataStart,
                    fieldDecl->dataEnd,
                    &fieldIndex))
            {
                return 0;
            }
            for (directIndex = 0; directIndex < directFieldCount; directIndex++) {
                if (directFieldIndices[directIndex] == fieldIndex && explicitDirect[directIndex]) {
                    alreadyExplicit = true;
                    break;
                }
            }
            if (alreadyExplicit) {
                fieldNode = fieldDecl->nextSibling;
                continue;
            }
            fieldTypeRef = program->fields[fieldIndex].typeRef;
            if (!AppendMirInst(
                    outInsts,
                    outCap,
                    &emittedLen,
                    &(HOPMirInst){
                        .op = HOPMirOp_LOCAL_LOAD,
                        .tok = 0u,
                        ._reserved = 0u,
                        .aux = localIndex,
                        .start = fieldDecl->start,
                        .end = fieldDecl->end,
                    })
                || !LowerMirHeapInitValueExpr(
                    loader,
                    program,
                    mutableProgram,
                    fn,
                    fnFile,
                    typeFile,
                    defaultExprNode,
                    arena,
                    localIndex,
                    pointeeTypeRef,
                    fieldTypeRef,
                    outInsts,
                    outCap,
                    &emittedLen,
                    &valueTypeRef)
                || !AppendMirInst(
                    outInsts,
                    outCap,
                    &emittedLen,
                    &(HOPMirInst){
                        .op = HOPMirOp_AGG_SET,
                        .tok = 0u,
                        ._reserved = 0u,
                        .aux = fieldIndex,
                        .start = fieldDecl->start,
                        .end = fieldDecl->end,
                    })
                || !AppendMirInst(
                    outInsts,
                    outCap,
                    &emittedLen,
                    &(HOPMirInst){
                        .op = HOPMirOp_DROP,
                        .tok = 0u,
                        ._reserved = 0u,
                        .aux = 0u,
                        .start = fieldDecl->start,
                        .end = fieldDecl->end,
                    }))
            {
                return 0;
            }
            fieldNode = fieldDecl->nextSibling;
        }
    }
    *outLen = emittedLen;
    *outClearedInitFlag = clearedInitFlag;
    return emittedLen != 0u || clearedInitFlag;
}

static int RewriteMirAllocNewInitExprs(
    const HOPPackageLoader* loader, HOPArena* arena, HOPMirProgram* program) {
    HOPMirInst*     insts = NULL;
    HOPMirFunction* funcs = NULL;
    uint32_t        instOutLen = 0u;
    uint32_t        totalExtraLen = 0u;
    uint32_t        funcIndex;
    if (loader == NULL || arena == NULL || program == NULL) {
        return -1;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const HOPMirFunction* fn = &program->funcs[funcIndex];
        const HOPParsedFile*  fnFile = FindLoaderFileByMirSource(
            loader, program, fn->sourceRef, NULL);
        uint32_t pc;
        if (fnFile == NULL) {
            continue;
        }
        for (pc = 0u; pc + 1u < fn->instLen; pc++) {
            const HOPMirInst*    allocInst = &program->insts[fn->instStart + pc];
            const HOPMirInst*    storeInst = &program->insts[fn->instStart + pc + 1u];
            const HOPMirLocal*   local;
            uint32_t             pointeeTypeRef;
            const HOPParsedFile* typeFile;
            HOPMirInst*          tempInsts;
            uint32_t             tempLen = 0u;
            bool                 clearedInit = false;
            uint32_t             tempCap;
            if (allocInst->op != HOPMirOp_ALLOC_NEW || storeInst->op != HOPMirOp_LOCAL_STORE
                || storeInst->aux >= fn->localCount)
            {
                continue;
            }
            local = &program->locals[fn->localStart + storeInst->aux];
            pointeeTypeRef = MirInitOwnerTypeRefForType(program, local->typeRef);
            if ((pointeeTypeRef == UINT32_MAX || pointeeTypeRef >= program->typeLen)
                && !ResolveMirAllocNewPointeeTypeRef(
                    loader, arena, program, fnFile, allocInst, &pointeeTypeRef))
            {
                continue;
            }
            if (pointeeTypeRef == UINT32_MAX || pointeeTypeRef >= program->typeLen) {
                continue;
            }
            typeFile = FindLoaderFileByMirSource(
                loader, program, program->types[pointeeTypeRef].sourceRef, NULL);
            if (typeFile == NULL) {
                continue;
            }
            tempCap = fnFile->ast.len * 8u + typeFile->ast.len * 4u + 64u;
            tempInsts = tempCap != 0u ? (HOPMirInst*)malloc(sizeof(HOPMirInst) * tempCap) : NULL;
            if (tempInsts == NULL) {
                return -1;
            }
            if (!LowerMirAllocNewPostInitInsts(
                    loader,
                    program,
                    program,
                    fn,
                    fnFile,
                    typeFile,
                    fnFile,
                    allocInst,
                    storeInst,
                    pointeeTypeRef,
                    arena,
                    tempInsts,
                    tempCap,
                    &tempLen,
                    &clearedInit))
            {
                free(tempInsts);
                continue;
            }
            free(tempInsts);
            totalExtraLen += tempLen;
        }
    }
    if (totalExtraLen == 0u) {
        return 0;
    }
    funcs = (HOPMirFunction*)HOPArenaAlloc(
        arena, sizeof(HOPMirFunction) * program->funcLen, (uint32_t)_Alignof(HOPMirFunction));
    insts = (HOPMirInst*)HOPArenaAlloc(
        arena,
        sizeof(HOPMirInst) * (program->instLen + totalExtraLen),
        (uint32_t)_Alignof(HOPMirInst));
    if ((funcs == NULL && program->funcLen != 0u)
        || (insts == NULL && program->instLen + totalExtraLen != 0u))
    {
        return -1;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const HOPMirFunction* fn = &program->funcs[funcIndex];
        const HOPParsedFile*  fnFile = FindLoaderFileByMirSource(
            loader, program, fn->sourceRef, NULL);
        uint32_t* insertCounts = NULL;
        uint32_t* pcMap = NULL;
        uint8_t*  clearInit = NULL;
        uint32_t  extraLen = 0u;
        uint32_t  delta = 0u;
        uint32_t  pc;
        funcs[funcIndex] = *fn;
        funcs[funcIndex].instStart = instOutLen;
        if (fn->instLen != 0u) {
            insertCounts = (uint32_t*)calloc(fn->instLen, sizeof(uint32_t));
            pcMap = (uint32_t*)calloc(fn->instLen, sizeof(uint32_t));
            clearInit = (uint8_t*)calloc(fn->instLen, sizeof(uint8_t));
            if (insertCounts == NULL || pcMap == NULL || clearInit == NULL) {
                free(insertCounts);
                free(pcMap);
                free(clearInit);
                return -1;
            }
        }
        if (fnFile != NULL) {
            for (pc = 0u; pc + 1u < fn->instLen; pc++) {
                const HOPMirInst*    allocInst = &program->insts[fn->instStart + pc];
                const HOPMirInst*    storeInst = &program->insts[fn->instStart + pc + 1u];
                const HOPMirLocal*   local;
                uint32_t             pointeeTypeRef;
                const HOPParsedFile* typeFile;
                HOPMirInst*          tempInsts;
                uint32_t             tempLen = 0u;
                bool                 cleared = false;
                uint32_t             tempCap;
                if (allocInst->op != HOPMirOp_ALLOC_NEW || storeInst->op != HOPMirOp_LOCAL_STORE
                    || storeInst->aux >= fn->localCount)
                {
                    continue;
                }
                local = &program->locals[fn->localStart + storeInst->aux];
                pointeeTypeRef = MirInitOwnerTypeRefForType(program, local->typeRef);
                if ((pointeeTypeRef == UINT32_MAX || pointeeTypeRef >= program->typeLen)
                    && !ResolveMirAllocNewPointeeTypeRef(
                        loader, arena, program, fnFile, allocInst, &pointeeTypeRef))
                {
                    continue;
                }
                if (pointeeTypeRef == UINT32_MAX || pointeeTypeRef >= program->typeLen) {
                    continue;
                }
                typeFile = FindLoaderFileByMirSource(
                    loader, program, program->types[pointeeTypeRef].sourceRef, NULL);
                if (typeFile == NULL) {
                    continue;
                }
                tempCap = fnFile->ast.len * 8u + typeFile->ast.len * 4u + 64u;
                tempInsts =
                    tempCap != 0u ? (HOPMirInst*)malloc(sizeof(HOPMirInst) * tempCap) : NULL;
                if (tempInsts == NULL) {
                    free(insertCounts);
                    free(pcMap);
                    free(clearInit);
                    return -1;
                }
                if (!LowerMirAllocNewPostInitInsts(
                        loader,
                        program,
                        program,
                        fn,
                        fnFile,
                        typeFile,
                        fnFile,
                        allocInst,
                        storeInst,
                        pointeeTypeRef,
                        arena,
                        tempInsts,
                        tempCap,
                        &tempLen,
                        &cleared))
                {
                    free(tempInsts);
                    continue;
                }
                free(tempInsts);
                insertCounts[pc + 1u] = tempLen;
                clearInit[pc] = cleared ? 1u : 0u;
                extraLen += tempLen;
            }
        }
        funcs[funcIndex].instLen = fn->instLen + extraLen;
        for (pc = 0u; pc < fn->instLen; pc++) {
            pcMap[pc] = pc + delta;
            delta += insertCounts[pc];
        }
        for (pc = 0u; pc < fn->instLen; pc++) {
            const HOPMirInst* srcInst = &program->insts[fn->instStart + pc];
            insts[instOutLen] = *srcInst;
            if (clearInit != NULL && clearInit[pc] && insts[instOutLen].op == HOPMirOp_ALLOC_NEW) {
                insts[instOutLen].tok &= (uint16_t)~HOPAstFlag_NEW_HAS_INIT;
            }
            if ((srcInst->op == HOPMirOp_JUMP || srcInst->op == HOPMirOp_JUMP_IF_FALSE)
                && srcInst->aux < fn->instLen)
            {
                insts[instOutLen].aux = pcMap[srcInst->aux];
            }
            instOutLen++;
            if (insertCounts != NULL && insertCounts[pc] != 0u) {
                const HOPMirInst*    allocInst = &program->insts[fn->instStart + pc - 1u];
                const HOPMirInst*    storeInst = &program->insts[fn->instStart + pc];
                const HOPMirLocal*   local;
                uint32_t             pointeeTypeRef;
                const HOPParsedFile* typeFile;
                uint32_t             emittedLen = 0u;
                HOPMirInst*          tempInsts = insts + instOutLen;
                local = &program->locals[fn->localStart + storeInst->aux];
                pointeeTypeRef = MirInitOwnerTypeRefForType(program, local->typeRef);
                if ((pointeeTypeRef == UINT32_MAX || pointeeTypeRef >= program->typeLen)
                    && !ResolveMirAllocNewPointeeTypeRef(
                        loader, arena, program, fnFile, allocInst, &pointeeTypeRef))
                {
                    free(insertCounts);
                    free(pcMap);
                    free(clearInit);
                    return -1;
                }
                typeFile = FindLoaderFileByMirSource(
                    loader, program, program->types[pointeeTypeRef].sourceRef, NULL);
                if (typeFile == NULL
                    || !LowerMirAllocNewPostInitInsts(
                        loader,
                        program,
                        program,
                        fn,
                        fnFile,
                        typeFile,
                        fnFile,
                        allocInst,
                        storeInst,
                        pointeeTypeRef,
                        arena,
                        tempInsts,
                        insertCounts[pc],
                        &emittedLen,
                        &(bool){ false })
                    || emittedLen != insertCounts[pc])
                {
                    free(insertCounts);
                    free(pcMap);
                    free(clearInit);
                    return -1;
                }
                instOutLen += emittedLen;
            }
        }
        free(insertCounts);
        free(pcMap);
        free(clearInit);
    }
    program->insts = insts;
    program->instLen = instOutLen;
    program->funcs = funcs;
    return 0;
}

static int RewriteMirDynamicSliceAllocCounts(
    const HOPPackageLoader* loader, HOPArena* arena, HOPMirProgram* program) {
    HOPMirInst*     insts = NULL;
    HOPMirFunction* funcs = NULL;
    uint32_t        instOutLen = 0u;
    uint32_t        totalExtraLen = 0u;
    uint32_t        funcIndex;
    if (loader == NULL || arena == NULL || program == NULL) {
        return -1;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const HOPMirFunction* fn = &program->funcs[funcIndex];
        const HOPParsedFile*  ownerFile = FindLoaderFileByMirSource(
            loader, program, fn->sourceRef, NULL);
        uint32_t pc;
        if (ownerFile != NULL) {
            for (pc = 0u; pc < fn->instLen; pc++) {
                const HOPMirInst* inst = &program->insts[fn->instStart + pc];
                const HOPMirInst* nextInst;
                uint32_t          localTypeRef;
                uint32_t          countInstLen = 0u;
                int32_t           typeNode = -1;
                int32_t           countNode = -1;
                int32_t           initNode = -1;
                int32_t           allocNode = -1;
                if (inst->op != HOPMirOp_ALLOC_NEW || (inst->tok & HOPAstFlag_NEW_HAS_COUNT) == 0u
                    || pc + 1u >= fn->instLen || ownerFile->source == NULL)
                {
                    continue;
                }
                nextInst = &program->insts[fn->instStart + pc + 1u];
                if (nextInst->op != HOPMirOp_LOCAL_STORE || nextInst->aux >= fn->localCount) {
                    continue;
                }
                localTypeRef = program->locals[fn->localStart + nextInst->aux].typeRef;
                if (localTypeRef >= program->typeLen
                    || (!HOPMirTypeRefIsSliceView(&program->types[localTypeRef])
                        && !HOPMirTypeRefIsAggSliceView(&program->types[localTypeRef])))
                {
                    continue;
                }
                if (!DecodeNewExprNodes(
                        ownerFile, (int32_t)inst->aux, &typeNode, &countNode, &initNode, &allocNode)
                    || countNode < 0)
                {
                    continue;
                }
                if (!CountMirAllocNewCountExprInsts(ownerFile, countNode, &countInstLen)) {
                    continue;
                }
                totalExtraLen += countInstLen;
            }
        }
    }
    if (totalExtraLen == 0u) {
        return 0;
    }
    if (program->instLen + totalExtraLen < program->instLen) {
        return -1;
    }
    funcs = (HOPMirFunction*)HOPArenaAlloc(
        arena, sizeof(HOPMirFunction) * program->funcLen, (uint32_t)_Alignof(HOPMirFunction));
    insts = (HOPMirInst*)HOPArenaAlloc(
        arena,
        sizeof(HOPMirInst) * (program->instLen + totalExtraLen),
        (uint32_t)_Alignof(HOPMirInst));
    if ((funcs == NULL && program->funcLen != 0u)
        || (insts == NULL && program->instLen + totalExtraLen != 0u))
    {
        return -1;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const HOPMirFunction* fn = &program->funcs[funcIndex];
        const HOPParsedFile*  ownerFile = FindLoaderFileByMirSource(
            loader, program, fn->sourceRef, NULL);
        uint32_t* insertCounts = NULL;
        uint32_t* pcMap = NULL;
        uint32_t  extraLen = 0u;
        uint32_t  delta = 0u;
        uint32_t  pc;
        funcs[funcIndex] = *fn;
        funcs[funcIndex].instStart = instOutLen;
        if (fn->instLen != 0u) {
            insertCounts = (uint32_t*)calloc(fn->instLen, sizeof(uint32_t));
            pcMap = (uint32_t*)calloc(fn->instLen, sizeof(uint32_t));
            if (insertCounts == NULL || pcMap == NULL) {
                free(insertCounts);
                free(pcMap);
                return -1;
            }
        }
        if (ownerFile != NULL) {
            for (pc = 0u; pc < fn->instLen; pc++) {
                const HOPMirInst* inst = &program->insts[fn->instStart + pc];
                const HOPMirInst* nextInst;
                uint32_t          localTypeRef;
                int32_t           typeNode = -1;
                int32_t           countNode = -1;
                int32_t           initNode = -1;
                int32_t           allocNode = -1;
                if (inst->op != HOPMirOp_ALLOC_NEW || (inst->tok & HOPAstFlag_NEW_HAS_COUNT) == 0u
                    || pc + 1u >= fn->instLen || ownerFile->source == NULL)
                {
                    continue;
                }
                nextInst = &program->insts[fn->instStart + pc + 1u];
                if (nextInst->op != HOPMirOp_LOCAL_STORE || nextInst->aux >= fn->localCount) {
                    continue;
                }
                localTypeRef = program->locals[fn->localStart + nextInst->aux].typeRef;
                if (localTypeRef >= program->typeLen
                    || (!HOPMirTypeRefIsSliceView(&program->types[localTypeRef])
                        && !HOPMirTypeRefIsAggSliceView(&program->types[localTypeRef])))
                {
                    continue;
                }
                if (!DecodeNewExprNodes(
                        ownerFile, (int32_t)inst->aux, &typeNode, &countNode, &initNode, &allocNode)
                    || countNode < 0
                    || !CountMirAllocNewCountExprInsts(ownerFile, countNode, &insertCounts[pc]))
                {
                    insertCounts[pc] = 0u;
                    continue;
                }
                extraLen += insertCounts[pc];
            }
        }
        funcs[funcIndex].instLen = fn->instLen + extraLen;
        for (pc = 0u; pc < fn->instLen; pc++) {
            pcMap[pc] = pc + delta;
            delta += insertCounts[pc];
        }
        for (pc = 0u; pc < fn->instLen; pc++) {
            const HOPMirInst* srcInst = &program->insts[fn->instStart + pc];
            if (insertCounts != NULL && insertCounts[pc] != 0u) {
                int32_t  typeNode = -1;
                int32_t  countNode = -1;
                int32_t  initNode = -1;
                int32_t  allocNode = -1;
                uint32_t emittedLen = 0u;
                if (!DecodeNewExprNodes(
                        ownerFile,
                        (int32_t)srcInst->aux,
                        &typeNode,
                        &countNode,
                        &initNode,
                        &allocNode)
                    || countNode < 0
                    || !LowerMirAllocNewCountExpr(
                        program,
                        program,
                        fn,
                        ownerFile,
                        countNode,
                        arena,
                        &insts[instOutLen],
                        insertCounts[pc],
                        &emittedLen)
                    || emittedLen != insertCounts[pc])
                {
                    free(insertCounts);
                    free(pcMap);
                    return -1;
                }
                instOutLen += emittedLen;
            }
            insts[instOutLen] = *srcInst;
            if ((srcInst->op == HOPMirOp_JUMP || srcInst->op == HOPMirOp_JUMP_IF_FALSE)
                && srcInst->aux < fn->instLen)
            {
                insts[instOutLen].aux = pcMap[srcInst->aux];
            }
            instOutLen++;
        }
        free(insertCounts);
        free(pcMap);
    }
    program->insts = insts;
    program->instLen = instOutLen;
    program->funcs = funcs;
    return 0;
}

static int RewriteMirVarSizeAllocCounts(
    const HOPPackageLoader* loader, HOPArena* arena, HOPMirProgram* program) {
    HOPMirInst*     insts = NULL;
    HOPMirFunction* funcs = NULL;
    uint32_t        instOutLen = 0u;
    uint32_t        totalExtraLen = 0u;
    uint32_t        funcIndex;
    if (loader == NULL || arena == NULL || program == NULL) {
        return -1;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const HOPMirFunction* fn = &program->funcs[funcIndex];
        const HOPParsedFile*  ownerFile = FindLoaderFileByMirSource(
            loader, program, fn->sourceRef, NULL);
        uint32_t pc;
        if (ownerFile == NULL || ownerFile->source == NULL) {
            continue;
        }
        for (pc = 0u; pc + 1u < fn->instLen; pc++) {
            const HOPMirInst* inst = &program->insts[fn->instStart + pc];
            const HOPMirInst* nextInst = &program->insts[fn->instStart + pc + 1u];
            uint32_t          localTypeRef;
            uint32_t          pointeeTypeRef = UINT32_MAX;
            uint32_t          countInstLen = 0u;
            uint32_t          tempCap;
            HOPMirInst*       tempInsts = NULL;
            if (inst->op != HOPMirOp_ALLOC_NEW || (inst->tok & HOPAstFlag_NEW_HAS_COUNT) != 0u
                || nextInst->op != HOPMirOp_LOCAL_STORE || nextInst->aux >= fn->localCount)
            {
                continue;
            }
            localTypeRef = program->locals[fn->localStart + nextInst->aux].typeRef;
            pointeeTypeRef = MirInitOwnerTypeRefForType(program, localTypeRef);
            if ((pointeeTypeRef == UINT32_MAX || pointeeTypeRef >= program->typeLen)
                && !ResolveMirAllocNewPointeeTypeRef(
                    loader, arena, program, ownerFile, inst, &pointeeTypeRef))
            {
                continue;
            }
            tempCap = ownerFile->ast.len * 8u + 64u;
            tempInsts = tempCap != 0u ? (HOPMirInst*)malloc(sizeof(HOPMirInst) * tempCap) : NULL;
            if (tempInsts == NULL) {
                return -1;
            }
            if (!LowerMirVarSizeAllocNewSizeExpr(
                    program,
                    program,
                    fn,
                    ownerFile,
                    inst,
                    pointeeTypeRef,
                    arena,
                    tempInsts,
                    tempCap,
                    &countInstLen))
            {
                free(tempInsts);
                continue;
            }
            free(tempInsts);
            totalExtraLen += countInstLen;
        }
    }
    if (totalExtraLen == 0u) {
        return 0;
    }
    if (program->instLen + totalExtraLen < program->instLen) {
        return -1;
    }
    funcs = (HOPMirFunction*)HOPArenaAlloc(
        arena, sizeof(HOPMirFunction) * program->funcLen, (uint32_t)_Alignof(HOPMirFunction));
    insts = (HOPMirInst*)HOPArenaAlloc(
        arena,
        sizeof(HOPMirInst) * (program->instLen + totalExtraLen),
        (uint32_t)_Alignof(HOPMirInst));
    if ((funcs == NULL && program->funcLen != 0u)
        || (insts == NULL && program->instLen + totalExtraLen != 0u))
    {
        return -1;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const HOPMirFunction* fn = &program->funcs[funcIndex];
        const HOPParsedFile*  ownerFile = FindLoaderFileByMirSource(
            loader, program, fn->sourceRef, NULL);
        uint32_t* insertCounts = NULL;
        uint32_t* pcMap = NULL;
        uint8_t*  setCount = NULL;
        uint32_t  extraLen = 0u;
        uint32_t  delta = 0u;
        uint32_t  pc;
        funcs[funcIndex] = *fn;
        funcs[funcIndex].instStart = instOutLen;
        if (fn->instLen != 0u) {
            insertCounts = (uint32_t*)calloc(fn->instLen, sizeof(uint32_t));
            pcMap = (uint32_t*)calloc(fn->instLen, sizeof(uint32_t));
            setCount = (uint8_t*)calloc(fn->instLen, sizeof(uint8_t));
            if (insertCounts == NULL || pcMap == NULL || setCount == NULL) {
                free(insertCounts);
                free(pcMap);
                free(setCount);
                return -1;
            }
        }
        if (ownerFile != NULL && ownerFile->source != NULL) {
            for (pc = 0u; pc + 1u < fn->instLen; pc++) {
                const HOPMirInst* inst = &program->insts[fn->instStart + pc];
                const HOPMirInst* nextInst = &program->insts[fn->instStart + pc + 1u];
                uint32_t          localTypeRef;
                uint32_t          pointeeTypeRef = UINT32_MAX;
                uint32_t          tempCap;
                HOPMirInst*       tempInsts = NULL;
                if (inst->op != HOPMirOp_ALLOC_NEW || (inst->tok & HOPAstFlag_NEW_HAS_COUNT) != 0u
                    || nextInst->op != HOPMirOp_LOCAL_STORE || nextInst->aux >= fn->localCount)
                {
                    continue;
                }
                localTypeRef = program->locals[fn->localStart + nextInst->aux].typeRef;
                pointeeTypeRef = MirInitOwnerTypeRefForType(program, localTypeRef);
                if ((pointeeTypeRef == UINT32_MAX || pointeeTypeRef >= program->typeLen)
                    && !ResolveMirAllocNewPointeeTypeRef(
                        loader, arena, program, ownerFile, inst, &pointeeTypeRef))
                {
                    continue;
                }
                tempCap = ownerFile->ast.len * 8u + 64u;
                tempInsts =
                    tempCap != 0u ? (HOPMirInst*)malloc(sizeof(HOPMirInst) * tempCap) : NULL;
                if (tempInsts == NULL) {
                    free(insertCounts);
                    free(pcMap);
                    free(setCount);
                    return -1;
                }
                if (!LowerMirVarSizeAllocNewSizeExpr(
                        program,
                        program,
                        fn,
                        ownerFile,
                        inst,
                        pointeeTypeRef,
                        arena,
                        tempInsts,
                        tempCap,
                        &insertCounts[pc]))
                {
                    free(tempInsts);
                    insertCounts[pc] = 0u;
                    continue;
                }
                free(tempInsts);
                setCount[pc] = 1u;
                extraLen += insertCounts[pc];
            }
        }
        funcs[funcIndex].instLen = fn->instLen + extraLen;
        for (pc = 0u; pc < fn->instLen; pc++) {
            pcMap[pc] = pc + delta;
            delta += insertCounts[pc];
        }
        for (pc = 0u; pc < fn->instLen; pc++) {
            const HOPMirInst* srcInst = &program->insts[fn->instStart + pc];
            if (insertCounts != NULL && insertCounts[pc] != 0u) {
                const HOPMirInst* nextInst =
                    pc + 1u < fn->instLen ? &program->insts[fn->instStart + pc + 1u] : NULL;
                uint32_t pointeeTypeRef = UINT32_MAX;
                uint32_t emittedLen = 0u;
                if (nextInst == NULL || nextInst->op != HOPMirOp_LOCAL_STORE
                    || nextInst->aux >= fn->localCount)
                {
                    free(insertCounts);
                    free(pcMap);
                    free(setCount);
                    return -1;
                }
                pointeeTypeRef = MirInitOwnerTypeRefForType(
                    program, program->locals[fn->localStart + nextInst->aux].typeRef);
                if ((pointeeTypeRef == UINT32_MAX || pointeeTypeRef >= program->typeLen)
                    && !ResolveMirAllocNewPointeeTypeRef(
                        loader, arena, program, ownerFile, srcInst, &pointeeTypeRef))
                {
                    free(insertCounts);
                    free(pcMap);
                    free(setCount);
                    return -1;
                }
                if (!LowerMirVarSizeAllocNewSizeExpr(
                        program,
                        program,
                        fn,
                        ownerFile,
                        srcInst,
                        pointeeTypeRef,
                        arena,
                        &insts[instOutLen],
                        insertCounts[pc],
                        &emittedLen)
                    || emittedLen != insertCounts[pc])
                {
                    free(insertCounts);
                    free(pcMap);
                    free(setCount);
                    return -1;
                }
                instOutLen += emittedLen;
            }
            insts[instOutLen] = *srcInst;
            if (setCount != NULL && setCount[pc] != 0u) {
                insts[instOutLen].tok |= HOPAstFlag_NEW_HAS_COUNT;
            }
            if ((srcInst->op == HOPMirOp_JUMP || srcInst->op == HOPMirOp_JUMP_IF_FALSE)
                && srcInst->aux < fn->instLen)
            {
                insts[instOutLen].aux = pcMap[srcInst->aux];
            }
            instOutLen++;
        }
        free(insertCounts);
        free(pcMap);
        free(setCount);
    }
    program->insts = insts;
    program->instLen = instOutLen;
    program->funcs = funcs;
    return 0;
}

static int RewriteMirAllocNewAllocExprs(
    const HOPPackageLoader* loader, HOPArena* arena, HOPMirProgram* program) {
    HOPMirInst*     insts = NULL;
    HOPMirFunction* funcs = NULL;
    uint32_t        instOutLen = 0u;
    uint32_t        totalExtraLen = 0u;
    uint32_t        funcIndex;
    if (loader == NULL || arena == NULL || program == NULL) {
        return -1;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const HOPMirFunction* fn = &program->funcs[funcIndex];
        const HOPParsedFile*  ownerFile = FindLoaderFileByMirSource(
            loader, program, fn->sourceRef, NULL);
        uint32_t pc;
        if (ownerFile == NULL || ownerFile->source == NULL) {
            continue;
        }
        for (pc = 0u; pc < fn->instLen; pc++) {
            const HOPMirInst* inst = &program->insts[fn->instStart + pc];
            uint32_t          allocInstLen = 0u;
            int32_t           typeNode = -1;
            int32_t           countNode = -1;
            int32_t           initNode = -1;
            int32_t           allocNode = -1;
            if (inst->op != HOPMirOp_ALLOC_NEW || (inst->tok & HOPAstFlag_NEW_HAS_ALLOC) == 0u) {
                continue;
            }
            if (!DecodeNewExprNodes(
                    ownerFile, (int32_t)inst->aux, &typeNode, &countNode, &initNode, &allocNode)
                || allocNode < 0
                || !CountMirAllocNewAllocExprInsts(ownerFile, allocNode, &allocInstLen))
            {
                continue;
            }
            totalExtraLen += allocInstLen;
        }
    }
    if (totalExtraLen == 0u) {
        return 0;
    }
    if (program->instLen + totalExtraLen < program->instLen) {
        return -1;
    }
    funcs = (HOPMirFunction*)HOPArenaAlloc(
        arena, sizeof(HOPMirFunction) * program->funcLen, (uint32_t)_Alignof(HOPMirFunction));
    insts = (HOPMirInst*)HOPArenaAlloc(
        arena,
        sizeof(HOPMirInst) * (program->instLen + totalExtraLen),
        (uint32_t)_Alignof(HOPMirInst));
    if ((funcs == NULL && program->funcLen != 0u)
        || (insts == NULL && program->instLen + totalExtraLen != 0u))
    {
        return -1;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const HOPMirFunction* fn = &program->funcs[funcIndex];
        const HOPParsedFile*  ownerFile = FindLoaderFileByMirSource(
            loader, program, fn->sourceRef, NULL);
        uint32_t* insertCounts = NULL;
        uint32_t* pcMap = NULL;
        uint32_t  extraLen = 0u;
        uint32_t  delta = 0u;
        uint32_t  pc;
        funcs[funcIndex] = *fn;
        funcs[funcIndex].instStart = instOutLen;
        if (fn->instLen != 0u) {
            insertCounts = (uint32_t*)calloc(fn->instLen, sizeof(uint32_t));
            pcMap = (uint32_t*)calloc(fn->instLen, sizeof(uint32_t));
            if (insertCounts == NULL || pcMap == NULL) {
                free(insertCounts);
                free(pcMap);
                return -1;
            }
        }
        if (ownerFile != NULL && ownerFile->source != NULL) {
            for (pc = 0u; pc < fn->instLen; pc++) {
                const HOPMirInst* inst = &program->insts[fn->instStart + pc];
                int32_t           typeNode = -1;
                int32_t           countNode = -1;
                int32_t           initNode = -1;
                int32_t           allocNode = -1;
                if (inst->op != HOPMirOp_ALLOC_NEW || (inst->tok & HOPAstFlag_NEW_HAS_ALLOC) == 0u)
                {
                    continue;
                }
                if (!DecodeNewExprNodes(
                        ownerFile, (int32_t)inst->aux, &typeNode, &countNode, &initNode, &allocNode)
                    || allocNode < 0
                    || !CountMirAllocNewAllocExprInsts(ownerFile, allocNode, &insertCounts[pc]))
                {
                    insertCounts[pc] = 0u;
                    continue;
                }
                extraLen += insertCounts[pc];
            }
        }
        funcs[funcIndex].instLen = fn->instLen + extraLen;
        for (pc = 0u; pc < fn->instLen; pc++) {
            pcMap[pc] = pc + delta;
            delta += insertCounts[pc];
        }
        for (pc = 0u; pc < fn->instLen; pc++) {
            const HOPMirInst* srcInst = &program->insts[fn->instStart + pc];
            if (insertCounts != NULL && insertCounts[pc] != 0u) {
                int32_t  typeNode = -1;
                int32_t  countNode = -1;
                int32_t  initNode = -1;
                int32_t  allocNode = -1;
                uint32_t emittedLen = 0u;
                if (!DecodeNewExprNodes(
                        ownerFile,
                        (int32_t)srcInst->aux,
                        &typeNode,
                        &countNode,
                        &initNode,
                        &allocNode)
                    || allocNode < 0
                    || !LowerMirAllocNewAllocExpr(
                        program,
                        program,
                        fn,
                        ownerFile,
                        allocNode,
                        arena,
                        &insts[instOutLen],
                        insertCounts[pc],
                        &emittedLen)
                    || emittedLen != insertCounts[pc])
                {
                    free(insertCounts);
                    free(pcMap);
                    return -1;
                }
                instOutLen += emittedLen;
            }
            insts[instOutLen] = *srcInst;
            if ((srcInst->op == HOPMirOp_JUMP || srcInst->op == HOPMirOp_JUMP_IF_FALSE)
                && srcInst->aux < fn->instLen)
            {
                insts[instOutLen].aux = pcMap[srcInst->aux];
            }
            instOutLen++;
        }
        free(insertCounts);
        free(pcMap);
    }
    program->insts = insts;
    program->instLen = instOutLen;
    program->funcs = funcs;
    return 0;
}

static const HOPImportSymbolRef* _Nullable FindImportValueSymbolBySlice(
    const HOPPackage* pkg, const char* src, uint32_t start, uint32_t end) {
    uint32_t i;
    if (pkg == NULL || src == NULL || end <= start) {
        return NULL;
    }
    for (i = 0; i < pkg->importSymbolLen; i++) {
        const HOPImportSymbolRef* sym = &pkg->importSymbols[i];
        size_t                    nameLen;
        if (sym->isType || sym->isFunction) {
            continue;
        }
        nameLen = strlen(sym->localName);
        if (nameLen == (size_t)(end - start) && memcmp(sym->localName, src + start, nameLen) == 0) {
            return sym;
        }
    }
    return NULL;
}

static const HOPImportSymbolRef* _Nullable FindImportFunctionSymbolBySlice(
    const HOPPackage* pkg, const char* src, uint32_t start, uint32_t end) {
    uint32_t i;
    if (pkg == NULL || src == NULL || end <= start) {
        return NULL;
    }
    for (i = 0; i < pkg->importSymbolLen; i++) {
        const HOPImportSymbolRef* sym = &pkg->importSymbols[i];
        size_t                    nameLen;
        if (!sym->isFunction || sym->useWrapper) {
            continue;
        }
        nameLen = strlen(sym->localName);
        if (nameLen == (size_t)(end - start) && memcmp(sym->localName, src + start, nameLen) == 0) {
            return sym;
        }
    }
    return NULL;
}

static const HOPMirResolvedDecl* _Nullable FindResolvedImportValueBySlice(
    const HOPPackageLoader*      loader,
    const HOPMirResolvedDeclMap* map,
    const HOPPackage*            pkg,
    const char*                  src,
    uint32_t                     start,
    uint32_t                     end) {
    const HOPImportSymbolRef* sym = FindImportValueSymbolBySlice(pkg, src, start, end);
    const HOPPackage*         depPkg;
    if (sym == NULL || sym->importIndex >= pkg->importLen) {
        return NULL;
    }
    depPkg = EffectiveMirImportTargetPackage(loader, &pkg->imports[sym->importIndex]);
    return depPkg != NULL ? FindMirResolvedValueByCStr(map, depPkg, sym->sourceName) : NULL;
}

static const HOPMirResolvedDecl* _Nullable FindResolvedImportFunctionBySlice(
    const HOPPackageLoader*      loader,
    const HOPMirResolvedDeclMap* map,
    const HOPPackage*            pkg,
    const char*                  src,
    uint32_t                     start,
    uint32_t                     end) {
    const HOPImportSymbolRef* sym = FindImportFunctionSymbolBySlice(pkg, src, start, end);
    const HOPPackage*         depPkg;
    if (sym == NULL || sym->importIndex >= pkg->importLen) {
        return NULL;
    }
    depPkg = EffectiveMirImportTargetPackage(loader, &pkg->imports[sym->importIndex]);
    return depPkg != NULL
             ? FindMirResolvedDeclByCStr(map, depPkg, sym->sourceName, HOPMirDeclKind_FN)
             : NULL;
}

static int MirExprInstStackDelta(const HOPMirInst* inst, int32_t* outDelta) {
    uint32_t elemCount = 0;
    if (inst == NULL || outDelta == NULL) {
        return 0;
    }
    switch (inst->op) {
        case HOPMirOp_PUSH_CONST:
        case HOPMirOp_PUSH_INT:
        case HOPMirOp_PUSH_FLOAT:
        case HOPMirOp_PUSH_BOOL:
        case HOPMirOp_PUSH_STRING:
        case HOPMirOp_PUSH_NULL:
        case HOPMirOp_LOAD_IDENT:
        case HOPMirOp_LOCAL_LOAD:
        case HOPMirOp_LOCAL_ADDR:
        case HOPMirOp_ADDR_OF:
        case HOPMirOp_AGG_ZERO:
        case HOPMirOp_ARRAY_ZERO:
        case HOPMirOp_CTX_GET:
        case HOPMirOp_CTX_ADDR:        *outDelta = 1; return 1;
        case HOPMirOp_UNARY:
        case HOPMirOp_CAST:
        case HOPMirOp_COERCE:
        case HOPMirOp_SEQ_LEN:
        case HOPMirOp_STR_CSTR:
        case HOPMirOp_OPTIONAL_WRAP:
        case HOPMirOp_OPTIONAL_UNWRAP:
        case HOPMirOp_DEREF_LOAD:
        case HOPMirOp_AGG_GET:
        case HOPMirOp_AGG_ADDR:
        case HOPMirOp_ARRAY_GET:
        case HOPMirOp_ARRAY_ADDR:
        case HOPMirOp_TAGGED_TAG:
        case HOPMirOp_TAGGED_PAYLOAD:  *outDelta = 0; return 1;
        case HOPMirOp_BINARY:
        case HOPMirOp_INDEX:
        case HOPMirOp_LOCAL_STORE:
        case HOPMirOp_STORE_IDENT:
        case HOPMirOp_DROP:
        case HOPMirOp_ASSERT:
        case HOPMirOp_CTX_SET:
        case HOPMirOp_DEREF_STORE:
        case HOPMirOp_ARRAY_SET:
        case HOPMirOp_AGG_SET:         *outDelta = -1; return 1;
        case HOPMirOp_CALL:
        case HOPMirOp_CALL_FN:
        case HOPMirOp_CALL_HOST:
        case HOPMirOp_CALL_INDIRECT:
            *outDelta = 1 - (int32_t)HOPMirCallArgCountFromTok(inst->tok);
            return 1;
        case HOPMirOp_TUPLE_MAKE:
        case HOPMirOp_AGG_MAKE:
            elemCount = (uint32_t)inst->tok;
            *outDelta = 1 - (int32_t)elemCount;
            return 1;
        case HOPMirOp_SLICE_MAKE:
            *outDelta = 0 - (((inst->tok & HOPAstFlag_INDEX_HAS_START) != 0u) ? 1 : 0)
                      - (((inst->tok & HOPAstFlag_INDEX_HAS_END) != 0u) ? 1 : 0);
            return 1;
        case HOPMirOp_TAGGED_MAKE:   *outDelta = 0; return 1;
        case HOPMirOp_RETURN:
        case HOPMirOp_JUMP_IF_FALSE: *outDelta = -1; return 1;
        case HOPMirOp_RETURN_VOID:
        case HOPMirOp_LOCAL_ZERO:
        case HOPMirOp_JUMP:          return ((*outDelta = 0), 1);
        default:                     return 0;
    }
}

static int FindCallArgStartInFunction(
    const HOPMirProgram*  program,
    const HOPMirFunction* fn,
    uint32_t              callIndex,
    uint32_t              argCount,
    uint32_t*             outArgStart) {
    int32_t  need = 0;
    uint32_t i;
    if (outArgStart != NULL) {
        *outArgStart = UINT32_MAX;
    }
    if (program == NULL || fn == NULL || outArgStart == NULL || argCount == 0u
        || callIndex < fn->instStart || callIndex >= fn->instStart + fn->instLen)
    {
        return 0;
    }
    need = (int32_t)argCount;
    i = callIndex;
    while (i > fn->instStart) {
        int32_t delta = 0;
        i--;
        if (!MirExprInstStackDelta(&program->insts[i], &delta)) {
            return 0;
        }
        need -= delta;
        if (need == 0) {
            *outArgStart = i;
            return 1;
        }
    }
    return 0;
}

static int FindFirstCallArgEndInFunction(
    const HOPMirProgram*  program,
    const HOPMirFunction* fn,
    uint32_t              callIndex,
    uint32_t              argCount,
    uint32_t*             outArgEnd) {
    uint32_t argStart = UINT32_MAX;
    uint32_t i;
    int32_t  depth = 0;
    if (outArgEnd != NULL) {
        *outArgEnd = UINT32_MAX;
    }
    if (program == NULL || fn == NULL || outArgEnd == NULL || argCount == 0u
        || !FindCallArgStartInFunction(program, fn, callIndex, argCount, &argStart)
        || argStart < fn->instStart || argStart >= callIndex)
    {
        return 0;
    }
    for (i = argStart; i < callIndex; i++) {
        int32_t delta = 0;
        if (!MirExprInstStackDelta(&program->insts[i], &delta)) {
            return 0;
        }
        depth += delta;
        if (depth == 1) {
            *outArgEnd = i + 1u;
            return 1;
        }
    }
    return 0;
}

static uint32_t MirInstResultTypeRef(
    const HOPMirProgram* program, const HOPMirFunction* fn, const HOPMirInst* inst) {
    if (program == NULL || fn == NULL || inst == NULL) {
        return UINT32_MAX;
    }
    switch (inst->op) {
        case HOPMirOp_LOCAL_LOAD:
            return inst->aux < fn->localCount
                     ? program->locals[fn->localStart + inst->aux].typeRef
                     : UINT32_MAX;
        case HOPMirOp_AGG_GET:
        case HOPMirOp_AGG_ADDR:
            return inst->aux < program->fieldLen ? program->fields[inst->aux].typeRef : UINT32_MAX;
        default: return UINT32_MAX;
    }
}

static uint32_t MirAggregateOwnerTypeRef(const HOPMirProgram* program, uint32_t typeRef) {
    if (program == NULL || typeRef >= program->typeLen) {
        return UINT32_MAX;
    }
    if (HOPMirTypeRefIsAggregate(&program->types[typeRef])) {
        return typeRef;
    }
    if (HOPMirTypeRefIsOpaquePtr(&program->types[typeRef])) {
        return HOPMirTypeRefOpaquePointeeTypeRef(&program->types[typeRef]);
    }
    return UINT32_MAX;
}

static uint32_t FindMirFieldNamed(
    const HOPMirProgram* program,
    uint32_t             ownerTypeRef,
    const char*          src,
    uint32_t             start,
    uint32_t             end) {
    uint32_t i;
    uint32_t nameLen;
    if (program == NULL || src == NULL || ownerTypeRef >= program->typeLen || end < start) {
        return UINT32_MAX;
    }
    nameLen = end - start;
    for (i = 0; i < program->fieldLen; i++) {
        const HOPMirField* field = &program->fields[i];
        if (field->ownerTypeRef != ownerTypeRef || field->sourceRef >= program->sourceLen) {
            continue;
        }
        if (field->nameEnd - field->nameStart == nameLen
            && memcmp(
                   program->sources[field->sourceRef].src.ptr + field->nameStart,
                   src + start,
                   nameLen)
                   == 0)
        {
            return i;
        }
    }
    return UINT32_MAX;
}

static uint32_t FindMirFieldNamedAny(
    const HOPMirProgram* program, const char* src, uint32_t start, uint32_t end) {
    uint32_t i;
    uint32_t nameLen;
    if (program == NULL || src == NULL || end < start) {
        return UINT32_MAX;
    }
    nameLen = end - start;
    for (i = 0; i < program->fieldLen; i++) {
        const HOPMirField* field = &program->fields[i];
        if (field->sourceRef >= program->sourceLen) {
            continue;
        }
        if (field->nameEnd - field->nameStart == nameLen
            && memcmp(
                   program->sources[field->sourceRef].src.ptr + field->nameStart,
                   src + start,
                   nameLen)
                   == 0)
        {
            return i;
        }
    }
    return UINT32_MAX;
}

static int EnsureMirHostRef(
    HOPArena*      arena,
    HOPMirProgram* program,
    HOPMirHostKind kind,
    uint32_t       flags,
    uint32_t       target,
    uint32_t       nameStart,
    uint32_t       nameEnd,
    uint32_t* _Nonnull outIndex) {
    uint32_t       i;
    HOPMirHostRef* newHosts;
    if (arena == NULL || program == NULL || outIndex == NULL) {
        return -1;
    }
    for (i = 0; i < program->hostLen; i++) {
        if (program->hosts[i].kind == kind && program->hosts[i].flags == flags
            && program->hosts[i].target == target)
        {
            *outIndex = i;
            return 0;
        }
    }
    newHosts = (HOPMirHostRef*)HOPArenaAlloc(
        arena, sizeof(HOPMirHostRef) * (program->hostLen + 1u), (uint32_t)_Alignof(HOPMirHostRef));
    if (newHosts == NULL) {
        return -1;
    }
    if (program->hostLen != 0u) {
        memcpy(newHosts, program->hosts, sizeof(HOPMirHostRef) * program->hostLen);
    }
    newHosts[program->hostLen] = (HOPMirHostRef){
        .nameStart = nameStart,
        .nameEnd = nameEnd,
        .kind = kind,
        .flags = flags,
        .target = target,
    };
    program->hosts = newHosts;
    *outIndex = program->hostLen++;
    return 0;
}

static int FindMirStaticCallTarget(
    const HOPMirTcFunctionMap* tcFnMap,
    const HOPPackage*          ownerPkg,
    const HOPParsedFile*       ownerFile,
    HOPTypeCheckCtx*           ownerTc,
    uint32_t                   ownerTcFnIndex,
    const HOPMirSymbolRef*     sym,
    uint32_t* _Nonnull outTargetMirFn);

static int ClassifyMirFuncFieldCall(
    const HOPPackageLoader* loader,
    const HOPMirProgram*    program,
    const HOPMirFunction*   fn,
    uint32_t                localIndex,
    uint32_t* _Nullable outInsertAfterPc,
    uint32_t* _Nullable outImplFieldRef);

static int ResolvePackageMirProgram(
    const HOPPackageLoader*      loader,
    const HOPMirResolvedDeclMap* declMap,
    const HOPMirTcFunctionMap*   tcFnMap,
    HOPArena*                    arena,
    const HOPMirProgram*         program,
    HOPMirProgram*               outProgram) {
    HOPMirInst*     insts = NULL;
    HOPMirFunction* funcs = NULL;
    uint32_t        instOutLen = 0;
    uint32_t        funcIndex;
    if (loader == NULL || declMap == NULL || arena == NULL || program == NULL || outProgram == NULL)
    {
        return -1;
    }
    *outProgram = *program;
    insts = (HOPMirInst*)HOPArenaAlloc(
        arena, sizeof(HOPMirInst) * program->instLen, (uint32_t)_Alignof(HOPMirInst));
    funcs = (HOPMirFunction*)HOPArenaAlloc(
        arena, sizeof(HOPMirFunction) * program->funcLen, (uint32_t)_Alignof(HOPMirFunction));
    if ((program->instLen != 0u && insts == NULL) || (program->funcLen != 0u && funcs == NULL)) {
        return -1;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const HOPMirFunction* fn = &program->funcs[funcIndex];
        const HOPPackage*     ownerPkg = NULL;
        const HOPParsedFile*  ownerFile = FindLoaderFileByMirSource(
            loader, program, fn->sourceRef, &ownerPkg);
        HOPTypeCheckCtx* ownerTc =
            ownerFile != NULL && ownerFile->hasTypecheckCtx
                ? (HOPTypeCheckCtx*)ownerFile->typecheckCtx
                : NULL;
        uint32_t ownerTcFnIndex = UINT32_MAX;
        uint8_t* omit = NULL;
        uint32_t localIndex;
        funcs[funcIndex] = *fn;
        funcs[funcIndex].instStart = instOutLen;
        if (ownerPkg != NULL && ownerFile != NULL && tcFnMap != NULL) {
            (void)FindMirTcFunctionByMirIndex(
                tcFnMap, ownerPkg, ownerFile, funcIndex, &ownerTcFnIndex);
        }
        if (fn->instLen != 0u) {
            omit = (uint8_t*)calloc(fn->instLen, sizeof(uint8_t));
            if (omit == NULL) {
                return -1;
            }
        }
        if (ownerPkg != NULL && ownerFile != NULL) {
            for (localIndex = 0; localIndex < fn->instLen; localIndex++) {
                uint32_t          instIndex = fn->instStart + localIndex;
                const HOPMirInst* inst = &program->insts[instIndex];
                if (inst->op == HOPMirOp_CALL && inst->aux < program->symbolLen) {
                    const HOPMirSymbolRef*    sym = &program->symbols[inst->aux];
                    const HOPMirResolvedDecl* target = NULL;
                    if (sym->kind != HOPMirSymbol_CALL) {
                        continue;
                    }
                    if ((sym->flags & HOPMirSymbolFlag_CALL_RECEIVER_ARG0) != 0u) {
                        uint32_t argc = HOPMirCallArgCountFromTok(inst->tok);
                        uint32_t argStart = UINT32_MAX;
                        if (argc == 0u
                            || !FindCallArgStartInFunction(program, fn, instIndex, argc, &argStart)
                            || argStart < fn->instStart || argStart >= fn->instStart + fn->instLen
                            || program->insts[argStart].op != HOPMirOp_LOAD_IDENT)
                        {
                            continue;
                        }
                        {
                            const HOPMirInst*   recvInst = &program->insts[argStart];
                            const HOPImportRef* imp = FindImportByAliasSlice(
                                ownerPkg, ownerFile->source, recvInst->start, recvInst->end);
                            if (imp == NULL || EffectiveMirImportTargetPackage(loader, imp) == NULL)
                            {
                                continue;
                            }
                            target = FindMirResolvedDeclBySlice(
                                declMap,
                                EffectiveMirImportTargetPackage(loader, imp),
                                ownerFile->source,
                                inst->start,
                                inst->end,
                                HOPMirDeclKind_FN);
                            if (target == NULL) {
                                continue;
                            }
                            omit[argStart - fn->instStart] = 1u;
                        }
                    } else {
                        target = FindMirResolvedDeclBySlice(
                            declMap,
                            ownerPkg,
                            ownerFile->source,
                            inst->start,
                            inst->end,
                            HOPMirDeclKind_FN);
                        if (target == NULL) {
                            target = FindResolvedImportFunctionBySlice(
                                loader,
                                declMap,
                                ownerPkg,
                                ownerFile->source,
                                inst->start,
                                inst->end);
                        }
                        if (target == NULL) {
                            continue;
                        }
                    }
                } else if (inst->op == HOPMirOp_AGG_GET && localIndex > 0u) {
                    const HOPMirInst*         recvInst = &program->insts[instIndex - 1u];
                    const HOPImportRef*       imp;
                    const HOPMirResolvedDecl* target;
                    int64_t                   enumValue = 0;
                    if (recvInst->op != HOPMirOp_LOAD_IDENT) {
                        continue;
                    }
                    if (ResolvePackageEnumVariantConstValue(
                            ownerPkg,
                            ownerFile->source,
                            recvInst->start,
                            recvInst->end,
                            ownerFile->source,
                            inst->start,
                            inst->end,
                            &enumValue))
                    {
                        omit[localIndex - 1u] = 1u;
                        continue;
                    }
                    imp = FindImportByAliasSlice(
                        ownerPkg, ownerFile->source, recvInst->start, recvInst->end);
                    if (imp == NULL || EffectiveMirImportTargetPackage(loader, imp) == NULL) {
                        continue;
                    }
                    target = FindMirResolvedValueBySlice(
                        declMap,
                        EffectiveMirImportTargetPackage(loader, imp),
                        ownerFile->source,
                        inst->start,
                        inst->end);
                    if (target == NULL) {
                        continue;
                    }
                    omit[localIndex - 1u] = 1u;
                }
            }
        }
        for (localIndex = 0; localIndex < fn->instLen; localIndex++) {
            uint32_t   instIndex = fn->instStart + localIndex;
            HOPMirInst inst = program->insts[instIndex];
            if (omit != NULL && omit[localIndex] != 0u) {
                continue;
            }
            if (ownerPkg != NULL && ownerFile != NULL) {
                if (inst.op == HOPMirOp_LOAD_IDENT) {
                    const HOPMirResolvedDecl* target = FindMirResolvedValueBySlice(
                        declMap, ownerPkg, ownerFile->source, inst.start, inst.end);
                    if (target == NULL) {
                        target = FindResolvedImportValueBySlice(
                            loader, declMap, ownerPkg, ownerFile->source, inst.start, inst.end);
                    }
                    if (target != NULL) {
                        inst.op = HOPMirOp_CALL_FN;
                        inst.tok = 0;
                        inst.aux = target->functionIndex;
                    } else {
                        target = FindMirResolvedDeclBySlice(
                            declMap,
                            ownerPkg,
                            ownerFile->source,
                            inst.start,
                            inst.end,
                            HOPMirDeclKind_FN);
                        if (target == NULL) {
                            target = FindResolvedImportFunctionBySlice(
                                loader, declMap, ownerPkg, ownerFile->source, inst.start, inst.end);
                        }
                        if (target != NULL) {
                            uint32_t constIndex = UINT32_MAX;
                            if (EnsureMirFunctionConst(
                                    arena, outProgram, target->functionIndex, &constIndex)
                                != 0)
                            {
                                free(omit);
                                return -1;
                            }
                            inst.op = HOPMirOp_PUSH_CONST;
                            inst.tok = 0u;
                            inst.aux = constIndex;
                        } else if (
                            MirNameIsTypeValue(
                                ownerPkg,
                                ownerTc,
                                ownerTcFnIndex,
                                ownerFile->source,
                                inst.start,
                                inst.end))
                        {
                            uint32_t constIndex = UINT32_MAX;
                            if (EnsureMirIntConst(arena, outProgram, 0, &constIndex) != 0) {
                                free(omit);
                                return -1;
                            }
                            inst.op = HOPMirOp_PUSH_CONST;
                            inst.tok = 0u;
                            inst.aux = constIndex;
                        }
                    }
                } else if (inst.op == HOPMirOp_CALL && inst.aux < program->symbolLen) {
                    const HOPMirSymbolRef*    sym = &program->symbols[inst.aux];
                    const HOPMirResolvedDecl* target = NULL;
                    if (sym->kind == HOPMirSymbol_CALL) {
                        uint32_t targetMirFn = UINT32_MAX;
                        if (SliceEqCStr(ownerFile->source, inst.start, inst.end, "typeof")) {
                            uint32_t constIndex = UINT32_MAX;
                            if (EnsureMirIntConst(arena, outProgram, 0, &constIndex) != 0) {
                                free(omit);
                                return -1;
                            }
                            inst.op = HOPMirOp_PUSH_CONST;
                            inst.tok = 0u;
                            inst.aux = constIndex;
                        } else if ((sym->flags & HOPMirSymbolFlag_CALL_RECEIVER_ARG0) != 0u) {
                            uint32_t argc = HOPMirCallArgCountFromTok(inst.tok);
                            uint32_t argStart = UINT32_MAX;
                            if (argc != 0u
                                && FindCallArgStartInFunction(
                                    program, fn, instIndex, argc, &argStart)
                                && argStart < instIndex
                                && program->insts[argStart].op == HOPMirOp_LOAD_IDENT)
                            {
                                const HOPMirInst*   recvInst = &program->insts[argStart];
                                const HOPImportRef* imp = FindImportByAliasSlice(
                                    ownerPkg, ownerFile->source, recvInst->start, recvInst->end);
                                if (imp != NULL
                                    && EffectiveMirImportTargetPackage(loader, imp) != NULL)
                                {
                                    target = FindMirResolvedDeclBySlice(
                                        declMap,
                                        EffectiveMirImportTargetPackage(loader, imp),
                                        ownerFile->source,
                                        inst.start,
                                        inst.end,
                                        HOPMirDeclKind_FN);
                                    if (target != NULL) {
                                        inst.op = HOPMirOp_CALL_FN;
                                        inst.tok =
                                            (uint16_t)((HOPMirCallArgCountFromTok(inst.tok) - 1u)
                                                       | (inst.tok
                                                          & HOPMirCallArgFlag_SPREAD_LAST));
                                        inst.aux = target->functionIndex;
                                    }
                                }
                            }
                            if (target == NULL
                                && !ClassifyMirFuncFieldCall(
                                    loader, program, fn, localIndex, NULL, NULL)
                                && FindMirStaticCallTarget(
                                    tcFnMap,
                                    ownerPkg,
                                    ownerFile,
                                    ownerTc,
                                    ownerTcFnIndex,
                                    sym,
                                    &targetMirFn))
                            {
                                inst.op = HOPMirOp_CALL_FN;
                                inst.tok = (uint16_t)(HOPMirCallArgCountFromTok(inst.tok)
                                                      | (inst.tok & HOPMirCallArgFlag_SPREAD_LAST));
                                inst.aux = targetMirFn;
                            }
                        } else {
                            if (FindMirStaticCallTarget(
                                    tcFnMap,
                                    ownerPkg,
                                    ownerFile,
                                    ownerTc,
                                    ownerTcFnIndex,
                                    sym,
                                    &targetMirFn))
                            {
                                inst.op = HOPMirOp_CALL_FN;
                                inst.tok = (uint16_t)(HOPMirCallArgCountFromTok(inst.tok)
                                                      | (inst.tok & HOPMirCallArgFlag_SPREAD_LAST));
                                inst.aux = targetMirFn;
                            } else {
                                target = FindMirResolvedDeclBySlice(
                                    declMap,
                                    ownerPkg,
                                    ownerFile->source,
                                    inst.start,
                                    inst.end,
                                    HOPMirDeclKind_FN);
                                if (target == NULL) {
                                    target = FindResolvedImportFunctionBySlice(
                                        loader,
                                        declMap,
                                        ownerPkg,
                                        ownerFile->source,
                                        inst.start,
                                        inst.end);
                                }
                                if (target != NULL) {
                                    inst.op = HOPMirOp_CALL_FN;
                                    inst.aux = target->functionIndex;
                                }
                            }
                        }
                    }
                } else if (inst.op == HOPMirOp_AGG_GET && localIndex > 0u) {
                    const HOPMirInst*         recvInst = &program->insts[instIndex - 1u];
                    const HOPImportRef*       imp;
                    const HOPMirResolvedDecl* target;
                    if (recvInst->op == HOPMirOp_LOAD_IDENT) {
                        int64_t  enumValue = 0;
                        uint32_t constIndex = UINT32_MAX;
                        if (ResolvePackageEnumVariantConstValue(
                                ownerPkg,
                                ownerFile->source,
                                recvInst->start,
                                recvInst->end,
                                ownerFile->source,
                                inst.start,
                                inst.end,
                                &enumValue))
                        {
                            if (EnsureMirIntConst(arena, outProgram, enumValue, &constIndex) != 0) {
                                free(omit);
                                return -1;
                            }
                            inst.op = HOPMirOp_PUSH_CONST;
                            inst.tok = 0u;
                            inst.aux = constIndex;
                        } else {
                            imp = FindImportByAliasSlice(
                                ownerPkg, ownerFile->source, recvInst->start, recvInst->end);
                            if (imp != NULL && EffectiveMirImportTargetPackage(loader, imp) != NULL)
                            {
                                target = FindMirResolvedValueBySlice(
                                    declMap,
                                    EffectiveMirImportTargetPackage(loader, imp),
                                    ownerFile->source,
                                    inst.start,
                                    inst.end);
                                if (target != NULL) {
                                    inst.op = HOPMirOp_CALL_FN;
                                    inst.tok = 0;
                                    inst.aux = target->functionIndex;
                                }
                            }
                        }
                    }
                }
            }
            insts[instOutLen++] = inst;
        }
        funcs[funcIndex].instLen = instOutLen - funcs[funcIndex].instStart;
        free(omit);
    }
    outProgram->insts = insts;
    outProgram->instLen = instOutLen;
    outProgram->funcs = funcs;
    return 0;
}

static int FindMirStaticCallTarget(
    const HOPMirTcFunctionMap* tcFnMap,
    const HOPPackage*          ownerPkg,
    const HOPParsedFile*       ownerFile,
    HOPTypeCheckCtx*           ownerTc,
    uint32_t                   ownerTcFnIndex,
    const HOPMirSymbolRef*     sym,
    uint32_t* _Nonnull outTargetMirFn) {
    int32_t targetTcFn = -1;
    if (outTargetMirFn != NULL) {
        *outTargetMirFn = UINT32_MAX;
    }
    if (tcFnMap == NULL || ownerPkg == NULL || ownerFile == NULL || ownerTc == NULL
        || ownerTcFnIndex == UINT32_MAX || sym == NULL || outTargetMirFn == NULL
        || sym->target >= ownerTc->ast->len)
    {
        return 0;
    }
    if (!HOPTCFindCallTarget(ownerTc, (int32_t)ownerTcFnIndex, (int32_t)sym->target, &targetTcFn)
        || targetTcFn < 0 || (uint32_t)targetTcFn >= ownerTc->funcLen)
    {
        return 0;
    }
    return FindMirTcFunctionDecl(
        tcFnMap, ownerPkg, ownerFile, (uint32_t)targetTcFn, outTargetMirFn);
}

static uint32_t FindMirFuncRefFieldByName(
    const HOPMirProgram* program, const char* src, uint32_t start, uint32_t end) {
    uint32_t i;
    if (program == NULL || src == NULL) {
        return UINT32_MAX;
    }
    for (i = 0; i < program->fieldLen; i++) {
        const HOPMirField* field = &program->fields[i];
        if (field->typeRef < program->typeLen
            && HOPMirTypeRefIsFuncRef(&program->types[field->typeRef])
            && SliceEqSlice(src, field->nameStart, field->nameEnd, src, start, end))
        {
            return i;
        }
    }
    return UINT32_MAX;
}

static int ClassifyMirFuncFieldCall(
    const HOPPackageLoader* loader,
    const HOPMirProgram*    program,
    const HOPMirFunction*   fn,
    uint32_t                localIndex,
    uint32_t* _Nullable outInsertAfterPc,
    uint32_t* _Nullable outImplFieldRef) {
    const HOPParsedFile*   ownerFile;
    const HOPMirInst*      inst;
    const HOPMirSymbolRef* sym;
    uint32_t               argc;
    uint32_t               argStartPc = UINT32_MAX;
    uint32_t               recvEndPc = UINT32_MAX;
    uint32_t               fieldRef = UINT32_MAX;
    if (outInsertAfterPc != NULL) {
        *outInsertAfterPc = UINT32_MAX;
    }
    if (outImplFieldRef != NULL) {
        *outImplFieldRef = UINT32_MAX;
    }
    if (loader == NULL || program == NULL || fn == NULL || localIndex >= fn->instLen) {
        return 0;
    }
    ownerFile = FindLoaderFileByMirSource(loader, program, fn->sourceRef, NULL);
    inst = &program->insts[fn->instStart + localIndex];
    if (ownerFile == NULL || ownerFile->source == NULL || inst->op != HOPMirOp_CALL
        || inst->aux >= program->symbolLen)
    {
        return 0;
    }
    sym = &program->symbols[inst->aux];
    argc = HOPMirCallArgCountFromTok(inst->tok);
    if (sym->kind != HOPMirSymbol_CALL || argc < 2u
        || (sym->flags & HOPMirSymbolFlag_CALL_RECEIVER_ARG0) == 0u
        || !FindCallArgStartInFunction(program, fn, fn->instStart + localIndex, argc, &argStartPc)
        || !FindCallArgStartInFunction(
            program, fn, fn->instStart + localIndex, argc - 1u, &recvEndPc)
        || argStartPc < fn->instStart || argStartPc >= recvEndPc
        || recvEndPc > fn->instStart + localIndex)
    {
        return 0;
    }
    fieldRef = FindMirFuncRefFieldByName(program, ownerFile->source, inst->start, inst->end);
    if (fieldRef >= program->fieldLen) {
        fieldRef = FindMirFieldNamedAny(program, ownerFile->source, inst->start, inst->end);
    }
    if (fieldRef >= program->fieldLen) {
        return 0;
    }
    if (outInsertAfterPc != NULL) {
        *outInsertAfterPc = recvEndPc - fn->instStart - 1u;
    }
    if (outImplFieldRef != NULL) {
        *outImplFieldRef = fieldRef;
    }
    return 1;
}

static int LoaderTargetNeedsSelectedPlatformMir(const HOPPackageLoader* _Nullable loader) {
    return loader != NULL && loader->platformTarget != NULL
        && (StrEq(loader->platformTarget, HOP_WASM_MIN_PLATFORM_TARGET)
            || StrEq(loader->platformTarget, HOP_PLAYBIT_PLATFORM_TARGET));
}

static int BuilderHasHostPrintCall(const HOPMirProgramBuilder* builder) {
    uint32_t i;
    if (builder == NULL) {
        return 0;
    }
    for (i = 0; i < builder->instLen; i++) {
        const HOPMirInst* inst = &builder->insts[i];
        if (inst->op == HOPMirOp_CALL_HOST && inst->aux < builder->hostLen
            && builder->hosts[inst->aux].target == HOPMirHostTarget_PRINT)
        {
            return 1;
        }
    }
    return 0;
}

static int FindMirIntConstIndex(const HOPMirProgram* program, uint64_t bits, uint32_t* outIndex) {
    uint32_t i;
    if (outIndex != NULL) {
        *outIndex = UINT32_MAX;
    }
    if (program == NULL || outIndex == NULL) {
        return 0;
    }
    for (i = 0; i < program->constLen; i++) {
        if (program->consts[i].kind == HOPMirConst_INT && program->consts[i].bits == bits) {
            *outIndex = i;
            return 1;
        }
    }
    return 0;
}

static int EnsureMirIntConstIndex(
    HOPArena* arena, HOPMirProgram* program, uint64_t bits, uint32_t* outIndex) {
    HOPMirConst* consts;
    uint32_t     i;
    if (outIndex != NULL) {
        *outIndex = UINT32_MAX;
    }
    if (arena == NULL || program == NULL || outIndex == NULL) {
        return 0;
    }
    for (i = 0; i < program->constLen; i++) {
        if (program->consts[i].kind == HOPMirConst_INT && program->consts[i].bits == bits) {
            *outIndex = i;
            return 1;
        }
    }
    consts = (HOPMirConst*)HOPArenaAlloc(
        arena, sizeof(HOPMirConst) * (program->constLen + 1u), (uint32_t)_Alignof(HOPMirConst));
    if (consts == NULL) {
        return 0;
    }
    for (i = 0; i < program->constLen; i++) {
        consts[i] = program->consts[i];
    }
    consts[program->constLen] = (HOPMirConst){
        .kind = HOPMirConst_INT,
        .bits = bits,
    };
    *outIndex = program->constLen;
    program->consts = consts;
    program->constLen++;
    return 1;
}

static int RewriteMirWasmPrintHostcalls(
    const HOPPackageLoader*      loader,
    const HOPMirResolvedDeclMap* declMap,
    HOPArena*                    arena,
    HOPMirProgram*               program) {
    const HOPMirResolvedDecl* consoleLogDecl;
    const HOPMirFunction*     consoleLogFn;
    HOPMirFunction*           funcs;
    HOPMirInst*               insts;
    uint32_t                  zeroConstIndex = UINT32_MAX;
    uint32_t                  flagsTypeRef;
    uint32_t                  totalExtraLen = 0u;
    uint32_t                  funcIndex;
    uint32_t                  instOutLen = 0u;
    if (!LoaderTargetNeedsSelectedPlatformMir(loader)) {
        return 0;
    }
    if (loader == NULL || loader->selectedPlatformPkg == NULL || declMap == NULL || arena == NULL
        || program == NULL)
    {
        return -1;
    }
    consoleLogDecl = FindMirResolvedDeclByCStr(
        declMap, loader->selectedPlatformPkg, "console_log", HOPMirDeclKind_FN);
    if (consoleLogDecl == NULL || consoleLogDecl->functionIndex >= program->funcLen) {
        return 0;
    }
    consoleLogFn = &program->funcs[consoleLogDecl->functionIndex];
    if (consoleLogFn->paramCount < 2u || consoleLogFn->localStart + 1u >= program->localLen) {
        return -1;
    }
    flagsTypeRef = program->locals[consoleLogFn->localStart + 1u].typeRef;
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const HOPMirFunction* fn = &program->funcs[funcIndex];
        uint32_t              pc;
        for (pc = 0; pc < fn->instLen; pc++) {
            const HOPMirInst* inst = &program->insts[fn->instStart + pc];
            if (inst->op == HOPMirOp_CALL_HOST && inst->aux < program->hostLen
                && program->hosts[inst->aux].target == HOPMirHostTarget_PRINT)
            {
                totalExtraLen += 2u;
            }
        }
    }
    if (totalExtraLen == 0u) {
        return 0;
    }
    if (!EnsureMirIntConstIndex(arena, program, 0u, &zeroConstIndex)) {
        return -1;
    }
    if (program->instLen + totalExtraLen < program->instLen) {
        return -1;
    }
    funcs = (HOPMirFunction*)HOPArenaAlloc(
        arena, sizeof(HOPMirFunction) * program->funcLen, (uint32_t)_Alignof(HOPMirFunction));
    insts = (HOPMirInst*)HOPArenaAlloc(
        arena,
        sizeof(HOPMirInst) * (program->instLen + totalExtraLen),
        (uint32_t)_Alignof(HOPMirInst));
    if ((funcs == NULL && program->funcLen != 0u)
        || (insts == NULL && program->instLen + totalExtraLen != 0u))
    {
        return -1;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const HOPMirFunction* fn = &program->funcs[funcIndex];
        uint32_t*             insertCounts = NULL;
        uint32_t*             pcMap = NULL;
        uint32_t              extraLen = 0u;
        uint32_t              delta = 0u;
        uint32_t              pc;
        funcs[funcIndex] = *fn;
        funcs[funcIndex].instStart = instOutLen;
        if (fn->instLen != 0u) {
            insertCounts = (uint32_t*)calloc(fn->instLen, sizeof(uint32_t));
            pcMap = (uint32_t*)calloc(fn->instLen, sizeof(uint32_t));
            if (insertCounts == NULL || pcMap == NULL) {
                free(insertCounts);
                free(pcMap);
                return -1;
            }
        }
        for (pc = 0; pc < fn->instLen; pc++) {
            const HOPMirInst* inst = &program->insts[fn->instStart + pc];
            if (inst->op == HOPMirOp_CALL_HOST && inst->aux < program->hostLen
                && program->hosts[inst->aux].target == HOPMirHostTarget_PRINT)
            {
                insertCounts[pc] = 2u;
                extraLen += 2u;
            }
        }
        funcs[funcIndex].instLen = fn->instLen + extraLen;
        for (pc = 0; pc < fn->instLen; pc++) {
            pcMap[pc] = pc + delta;
            delta += insertCounts[pc];
        }
        for (pc = 0; pc < fn->instLen; pc++) {
            const HOPMirInst* srcInst = &program->insts[fn->instStart + pc];
            if (insertCounts[pc] != 0u) {
                insts[instOutLen++] = (HOPMirInst){
                    .op = HOPMirOp_PUSH_CONST,
                    .aux = zeroConstIndex,
                    .start = srcInst->start,
                    .end = srcInst->end,
                };
                insts[instOutLen++] = (HOPMirInst){
                    .op = HOPMirOp_CAST,
                    .tok = HOPMirCastTarget_INT,
                    .aux = flagsTypeRef,
                    .start = srcInst->start,
                    .end = srcInst->end,
                };
            }
            insts[instOutLen] = *srcInst;
            if ((srcInst->op == HOPMirOp_JUMP || srcInst->op == HOPMirOp_JUMP_IF_FALSE)
                && srcInst->aux < fn->instLen)
            {
                insts[instOutLen].aux = pcMap[srcInst->aux];
            } else if (
                srcInst->op == HOPMirOp_CALL_HOST && srcInst->aux < program->hostLen
                && program->hosts[srcInst->aux].target == HOPMirHostTarget_PRINT)
            {
                insts[instOutLen].op = HOPMirOp_CALL_FN;
                insts[instOutLen].aux = consoleLogDecl->functionIndex;
                insts[instOutLen].tok = (uint16_t)((srcInst->tok & HOPMirCallArgFlag_MASK) | 2u);
            }
            instOutLen++;
        }
        free(insertCounts);
        free(pcMap);
    }
    program->funcs = funcs;
    program->insts = insts;
    program->instLen = instOutLen;
    return 0;
}

static int RewriteMirFuncFieldCalls(
    const HOPPackageLoader* loader, HOPArena* arena, HOPMirProgram* program) {
    HOPMirInst*     insts = NULL;
    HOPMirFunction* funcs = NULL;
    uint32_t        instOutLen = 0u;
    uint32_t        totalExtraLen = 0u;
    uint32_t        funcIndex;
    if (loader == NULL || arena == NULL || program == NULL) {
        return -1;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const HOPMirFunction* fn = &program->funcs[funcIndex];
        uint32_t              pc;
        for (pc = 0u; pc < fn->instLen; pc++) {
            if (ClassifyMirFuncFieldCall(loader, program, fn, pc, NULL, NULL)) {
                totalExtraLen++;
            }
        }
    }
    if (totalExtraLen == 0u) {
        return 0;
    }
    funcs = (HOPMirFunction*)HOPArenaAlloc(
        arena, sizeof(HOPMirFunction) * program->funcLen, (uint32_t)_Alignof(HOPMirFunction));
    insts = (HOPMirInst*)HOPArenaAlloc(
        arena,
        sizeof(HOPMirInst) * (program->instLen + totalExtraLen),
        (uint32_t)_Alignof(HOPMirInst));
    if ((funcs == NULL && program->funcLen != 0u)
        || (insts == NULL && program->instLen + totalExtraLen != 0u))
    {
        return -1;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const HOPMirFunction* fn = &program->funcs[funcIndex];
        uint32_t*             insertCounts = NULL;
        uint32_t*             insertFieldRefs = NULL;
        uint32_t*             pcMap = NULL;
        uint32_t              extraLen = 0u;
        uint32_t              delta = 0u;
        uint32_t              pc;
        funcs[funcIndex] = *fn;
        funcs[funcIndex].instStart = instOutLen;
        if (fn->instLen != 0u) {
            insertCounts = (uint32_t*)calloc(fn->instLen, sizeof(uint32_t));
            insertFieldRefs = (uint32_t*)calloc(fn->instLen, sizeof(uint32_t));
            pcMap = (uint32_t*)calloc(fn->instLen, sizeof(uint32_t));
            if (insertCounts == NULL || insertFieldRefs == NULL || pcMap == NULL) {
                free(insertCounts);
                free(insertFieldRefs);
                free(pcMap);
                return -1;
            }
            for (pc = 0u; pc < fn->instLen; pc++) {
                insertFieldRefs[pc] = UINT32_MAX;
            }
        }
        for (pc = 0u; pc < fn->instLen; pc++) {
            uint32_t insertAfterPc = UINT32_MAX;
            uint32_t implFieldRef = UINT32_MAX;
            if (!ClassifyMirFuncFieldCall(loader, program, fn, pc, &insertAfterPc, &implFieldRef)) {
                continue;
            }
            insertCounts[insertAfterPc] = 1u;
            insertFieldRefs[insertAfterPc] = implFieldRef;
            extraLen++;
        }
        funcs[funcIndex].instLen = fn->instLen + extraLen;
        for (pc = 0u; pc < fn->instLen; pc++) {
            pcMap[pc] = pc + delta;
            delta += insertCounts[pc];
        }
        for (pc = 0u; pc < fn->instLen; pc++) {
            HOPMirInst inst = program->insts[fn->instStart + pc];
            if ((inst.op == HOPMirOp_JUMP || inst.op == HOPMirOp_JUMP_IF_FALSE)
                && inst.aux < fn->instLen)
            {
                inst.aux = pcMap[inst.aux];
            } else if (inst.op == HOPMirOp_CALL && inst.aux < program->symbolLen) {
                const HOPMirSymbolRef* sym = &program->symbols[inst.aux];
                if (sym->kind == HOPMirSymbol_CALL
                    && (sym->flags & HOPMirSymbolFlag_CALL_RECEIVER_ARG0) != 0u
                    && HOPMirCallArgCountFromTok(inst.tok) > 0u)
                {
                    inst.op = HOPMirOp_CALL_INDIRECT;
                    inst.tok = (uint16_t)((HOPMirCallArgCountFromTok(inst.tok) - 1u)
                                          | (inst.tok & HOPMirCallArgFlag_SPREAD_LAST));
                    inst.aux = 0u;
                }
            }
            insts[instOutLen++] = inst;
            if (insertCounts != NULL && insertCounts[pc] != 0u) {
                insts[instOutLen++] = (HOPMirInst){
                    .op = HOPMirOp_AGG_GET,
                    .tok = 0u,
                    ._reserved = 0u,
                    .aux = insertFieldRefs[pc],
                    .start = inst.start,
                    .end = inst.end,
                };
            }
        }
        free(insertCounts);
        free(insertFieldRefs);
        free(pcMap);
    }
    program->insts = insts;
    program->instLen = instOutLen;
    program->funcs = funcs;
    return 0;
}

static void SpecializeMirDirectFunctionFieldStores(HOPArena* arena, HOPMirProgram* program) {
    uint32_t funcIndex;
    if (arena == NULL || program == NULL) {
        return;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const HOPMirFunction* fn = &program->funcs[funcIndex];
        uint32_t              pc;
        for (pc = 3u; pc < fn->instLen; pc++) {
            const HOPMirInst*  storeInst = &program->insts[fn->instStart + pc];
            const HOPMirInst*  addrInst = &program->insts[fn->instStart + pc - 1u];
            const HOPMirInst*  valueInst = &program->insts[fn->instStart + pc - 3u];
            const HOPMirField* fieldRef;
            HOPMirField*       field = NULL;
            uint32_t           typeRef = UINT32_MAX;
            if (storeInst->op != HOPMirOp_DEREF_STORE || addrInst->op != HOPMirOp_AGG_ADDR
                || addrInst->aux >= program->fieldLen || valueInst->op != HOPMirOp_PUSH_CONST
                || valueInst->aux >= program->constLen
                || program->consts[valueInst->aux].kind != HOPMirConst_FUNCTION)
            {
                continue;
            }
            fieldRef = &program->fields[addrInst->aux];
            if (fieldRef->sourceRef >= program->sourceLen) {
                continue;
            }
            {
                uint32_t fieldIndex = FindMirFuncRefFieldByName(
                    program,
                    program->sources[fieldRef->sourceRef].src.ptr,
                    fieldRef->nameStart,
                    fieldRef->nameEnd);
                if (fieldIndex >= program->fieldLen) {
                    continue;
                }
                field = (HOPMirField*)&program->fields[fieldIndex];
            }
            if (field->typeRef >= program->typeLen
                || !HOPMirTypeRefIsFuncRef(&program->types[field->typeRef])
                || HOPMirTypeRefFuncRefFunctionIndex(&program->types[field->typeRef]) != UINT32_MAX)
            {
                continue;
            }
            if (EnsureMirFunctionRefTypeRef(
                    arena, program, program->consts[valueInst->aux].aux, &typeRef)
                != 0)
            {
                continue;
            }
            field->typeRef = typeRef;
        }
    }
}

static bool MirTypeNodesEquivalent(
    const HOPParsedFile* fileA, int32_t nodeA, const HOPParsedFile* fileB, int32_t nodeB) {
    const HOPAstNode* astNodeA;
    const HOPAstNode* astNodeB;
    int32_t           childA;
    int32_t           childB;
    if (fileA == NULL || fileB == NULL || nodeA < 0 || nodeB < 0
        || (uint32_t)nodeA >= fileA->ast.len || (uint32_t)nodeB >= fileB->ast.len)
    {
        return false;
    }
    astNodeA = &fileA->ast.nodes[nodeA];
    astNodeB = &fileB->ast.nodes[nodeB];
    if (astNodeA->kind != astNodeB->kind || astNodeA->flags != astNodeB->flags
        || !SliceEqSlice(
            fileA->source,
            astNodeA->dataStart,
            astNodeA->dataEnd,
            fileB->source,
            astNodeB->dataStart,
            astNodeB->dataEnd))
    {
        return false;
    }
    childA = ASTFirstChild(&fileA->ast, nodeA);
    childB = ASTFirstChild(&fileB->ast, nodeB);
    while (childA >= 0 && childB >= 0) {
        if (!MirTypeNodesEquivalent(fileA, childA, fileB, childB)) {
            return false;
        }
        childA = ASTNextSibling(&fileA->ast, childA);
        childB = ASTNextSibling(&fileB->ast, childB);
    }
    return childA < 0 && childB < 0;
}

static int32_t FindMirFunctionDeclNode(const HOPParsedFile* file, const HOPMirFunction* fn) {
    int32_t child;
    if (file == NULL || fn == NULL) {
        return -1;
    }
    child = ASTFirstChild(&file->ast, file->ast.root);
    while (child >= 0) {
        const HOPAstNode* n = &file->ast.nodes[child];
        if (n->kind == HOPAst_FN
            && SliceEqSlice(
                file->source, n->dataStart, n->dataEnd, file->source, fn->nameStart, fn->nameEnd))
        {
            return child;
        }
        child = ASTNextSibling(&file->ast, child);
    }
    return -1;
}

static bool MirFunctionTypeMatchesDecl(
    const HOPParsedFile* typeFile, int32_t typeNode, const HOPParsedFile* fnFile, int32_t fnNode) {
    int32_t typeChild;
    int32_t fnChild;
    int32_t typeReturnNode = -1;
    int32_t fnReturnNode = -1;
    if (typeFile == NULL || fnFile == NULL || typeNode < 0 || fnNode < 0
        || (uint32_t)typeNode >= typeFile->ast.len || (uint32_t)fnNode >= fnFile->ast.len
        || typeFile->ast.nodes[typeNode].kind != HOPAst_TYPE_FN
        || fnFile->ast.nodes[fnNode].kind != HOPAst_FN)
    {
        return false;
    }
    typeChild = ASTFirstChild(&typeFile->ast, typeNode);
    fnChild = ASTFirstChild(&fnFile->ast, fnNode);
    while (fnChild >= 0 && fnFile->ast.nodes[fnChild].kind == HOPAst_PARAM) {
        int32_t fnParamType = ASTFirstChild(&fnFile->ast, fnChild);
        if (typeChild < 0 || fnParamType < 0
            || !MirTypeNodesEquivalent(typeFile, typeChild, fnFile, fnParamType))
        {
            return false;
        }
        typeChild = ASTNextSibling(&typeFile->ast, typeChild);
        fnChild = ASTNextSibling(&fnFile->ast, fnChild);
    }
    if (typeChild >= 0 && IsFnReturnTypeNodeKind(typeFile->ast.nodes[typeChild].kind)
        && typeFile->ast.nodes[typeChild].flags == 1u)
    {
        typeReturnNode = typeChild;
        typeChild = ASTNextSibling(&typeFile->ast, typeChild);
    }
    if (fnChild >= 0 && IsFnReturnTypeNodeKind(fnFile->ast.nodes[fnChild].kind)
        && fnFile->ast.nodes[fnChild].flags == 1u)
    {
        fnReturnNode = fnChild;
    }
    if (typeChild >= 0) {
        return false;
    }
    if (typeReturnNode < 0 || fnReturnNode < 0) {
        return typeReturnNode < 0 && fnReturnNode < 0;
    }
    return MirTypeNodesEquivalent(typeFile, typeReturnNode, fnFile, fnReturnNode);
}

static uint32_t FindMirRepresentativeFunctionForFuncType(
    const HOPPackageLoader* loader, const HOPMirProgram* program, const HOPMirTypeRef* typeRef) {
    const HOPParsedFile* typeFile;
    uint32_t             functionIndex;
    if (loader == NULL || program == NULL || typeRef == NULL || !HOPMirTypeRefIsFuncRef(typeRef)
        || typeRef->astNode == UINT32_MAX)
    {
        return UINT32_MAX;
    }
    typeFile = FindLoaderFileByMirSource(loader, program, typeRef->sourceRef, NULL);
    if (typeFile == NULL) {
        return UINT32_MAX;
    }
    for (functionIndex = 0; functionIndex < program->funcLen; functionIndex++) {
        const HOPMirFunction* fn = &program->funcs[functionIndex];
        const HOPParsedFile*  fnFile = FindLoaderFileByMirSource(
            loader, program, fn->sourceRef, NULL);
        int32_t fnNode;
        if (fnFile == NULL) {
            continue;
        }
        fnNode = FindMirFunctionDeclNode(fnFile, fn);
        if (fnNode >= 0
            && MirFunctionTypeMatchesDecl(typeFile, (int32_t)typeRef->astNode, fnFile, fnNode))
        {
            return functionIndex;
        }
    }
    return UINT32_MAX;
}

static void EnrichMirFunctionRefRepresentatives(
    const HOPPackageLoader* loader, HOPMirProgram* program) {
    uint32_t i;
    if (loader == NULL || program == NULL) {
        return;
    }
    for (i = 0; i < program->typeLen; i++) {
        HOPMirTypeRef* typeRef = (HOPMirTypeRef*)&program->types[i];
        uint32_t       functionIndex;
        if (!HOPMirTypeRefIsFuncRef(typeRef) || typeRef->aux != 0u
            || typeRef->astNode == UINT32_MAX)
        {
            continue;
        }
        functionIndex = FindMirRepresentativeFunctionForFuncType(loader, program, typeRef);
        if (functionIndex < program->funcLen) {
            typeRef->aux = functionIndex + 1u;
        }
    }
}

int PackageUsesPlatformImport(const HOPPackageLoader* loader) {
    uint32_t pkgIndex;
    if (loader == NULL) {
        return 0;
    }
    for (pkgIndex = 0; pkgIndex < loader->packageLen; pkgIndex++) {
        const HOPPackage* pkg = &loader->packages[pkgIndex];
        uint32_t          importIndex;
        for (importIndex = 0; importIndex < pkg->importLen; importIndex++) {
            const char* path = pkg->imports[importIndex].path;
            if (path == NULL) {
                continue;
            }
            if (StrEq(path, "platform") || strncmp(path, "platform/", 9u) == 0) {
                return 1;
            }
        }
    }
    return 0;
}

static HOPMirTypeScalar ClassifyMirScalarType(
    const HOPParsedFile* file, const HOPMirTypeRef* typeRef) {
    const HOPAstNode* node;
    const HOPAstNode* child;
    if (file == NULL || typeRef == NULL || typeRef->astNode >= file->ast.len) {
        return HOPMirTypeScalar_NONE;
    }
    node = &file->ast.nodes[typeRef->astNode];
    switch (node->kind) {
        case HOPAst_TYPE_PTR:
        case HOPAst_TYPE_MUTREF: return HOPMirTypeScalar_I32;
        case HOPAst_TYPE_REF:
            if (node->firstChild >= 0 && (uint32_t)node->firstChild < file->ast.len) {
                child = &file->ast.nodes[node->firstChild];
                if (child->kind == HOPAst_TYPE_NAME
                    && SliceEqCStr(file->source, child->dataStart, child->dataEnd, "str"))
                {
                    return HOPMirTypeScalar_NONE;
                }
            }
            return HOPMirTypeScalar_I32;
        case HOPAst_TYPE_NAME:
            if (SliceEqCStr(file->source, node->dataStart, node->dataEnd, "rawptr")) {
                return HOPMirTypeScalar_NONE;
            }
            if (SliceEqCStr(file->source, node->dataStart, node->dataEnd, "bool")
                || SliceEqCStr(file->source, node->dataStart, node->dataEnd, "u8")
                || SliceEqCStr(file->source, node->dataStart, node->dataEnd, "u16")
                || SliceEqCStr(file->source, node->dataStart, node->dataEnd, "u32")
                || SliceEqCStr(file->source, node->dataStart, node->dataEnd, "i8")
                || SliceEqCStr(file->source, node->dataStart, node->dataEnd, "i16")
                || SliceEqCStr(file->source, node->dataStart, node->dataEnd, "i32")
                || SliceEqCStr(file->source, node->dataStart, node->dataEnd, "uint")
                || SliceEqCStr(file->source, node->dataStart, node->dataEnd, "int"))
            {
                return HOPMirTypeScalar_I32;
            }
            if (SliceEqCStr(file->source, node->dataStart, node->dataEnd, "u64")
                || SliceEqCStr(file->source, node->dataStart, node->dataEnd, "i64"))
            {
                return HOPMirTypeScalar_I64;
            }
            if (SliceEqCStr(file->source, node->dataStart, node->dataEnd, "f32")) {
                return HOPMirTypeScalar_F32;
            }
            if (SliceEqCStr(file->source, node->dataStart, node->dataEnd, "f64")) {
                return HOPMirTypeScalar_F64;
            }
            return HOPMirTypeScalar_NONE;
        default: return HOPMirTypeScalar_NONE;
    }
}

static HOPMirIntKind ClassifyMirIntKindFromTypeNode(
    const HOPParsedFile* file, const HOPAstNode* node) {
    if (file == NULL || node == NULL) {
        return HOPMirIntKind_NONE;
    }
    if (node->kind != HOPAst_TYPE_NAME) {
        return HOPMirIntKind_NONE;
    }
    if (SliceEqCStr(file->source, node->dataStart, node->dataEnd, "bool")) {
        return HOPMirIntKind_BOOL;
    }
    if (SliceEqCStr(file->source, node->dataStart, node->dataEnd, "u8")) {
        return HOPMirIntKind_U8;
    }
    if (SliceEqCStr(file->source, node->dataStart, node->dataEnd, "i8")) {
        return HOPMirIntKind_I8;
    }
    if (SliceEqCStr(file->source, node->dataStart, node->dataEnd, "u16")) {
        return HOPMirIntKind_U16;
    }
    if (SliceEqCStr(file->source, node->dataStart, node->dataEnd, "i16")) {
        return HOPMirIntKind_I16;
    }
    if (SliceEqCStr(file->source, node->dataStart, node->dataEnd, "u32")) {
        return HOPMirIntKind_U32;
    }
    if (SliceEqCStr(file->source, node->dataStart, node->dataEnd, "i32")
        || SliceEqCStr(file->source, node->dataStart, node->dataEnd, "uint")
        || SliceEqCStr(file->source, node->dataStart, node->dataEnd, "int"))
    {
        return HOPMirIntKind_I32;
    }
    return HOPMirIntKind_NONE;
}

static uint32_t ParseMirArrayLen(const HOPParsedFile* file, const HOPAstNode* node) {
    uint32_t value = 0;
    uint32_t i;
    if (file == NULL || node == NULL || node->dataEnd <= node->dataStart) {
        return 0;
    }
    for (i = node->dataStart; i < node->dataEnd; i++) {
        char ch = file->source[i];
        if (ch < '0' || ch > '9') {
            return 0;
        }
        value = value * 10u + (uint32_t)(ch - '0');
    }
    return value;
}

static uint32_t FindMirSourceRefByFile(
    const HOPMirProgram* program, const HOPParsedFile* file, uint32_t defaultSourceRef) {
    uint32_t i;
    if (program == NULL || file == NULL) {
        return defaultSourceRef;
    }
    for (i = 0; i < program->sourceLen; i++) {
        if (program->sources[i].src.ptr == file->source
            && program->sources[i].src.len == file->sourceLen)
        {
            return i;
        }
    }
    return defaultSourceRef;
}

static const HOPSymbolDecl* _Nullable FindPackageTypeDeclBySlice(
    const HOPPackage* pkg, const char* src, uint32_t start, uint32_t end) {
    uint32_t i;
    size_t   nameLen;
    if (pkg == NULL || src == NULL || end < start) {
        return NULL;
    }
    nameLen = (size_t)(end - start);
    for (i = 0; i < pkg->declLen; i++) {
        if (IsTypeDeclKind(pkg->decls[i].kind) && strlen(pkg->decls[i].name) == nameLen
            && memcmp(pkg->decls[i].name, src + start, nameLen) == 0)
        {
            return &pkg->decls[i];
        }
    }
    for (i = 0; i < pkg->pubDeclLen; i++) {
        if (IsTypeDeclKind(pkg->pubDecls[i].kind) && strlen(pkg->pubDecls[i].name) == nameLen
            && memcmp(pkg->pubDecls[i].name, src + start, nameLen) == 0)
        {
            return &pkg->pubDecls[i];
        }
    }
    return NULL;
}

static const HOPImportSymbolRef* _Nullable FindImportTypeSymbolBySlice(
    const HOPPackage* pkg, const char* src, uint32_t start, uint32_t end) {
    uint32_t i;
    if (pkg == NULL || src == NULL || end <= start) {
        return NULL;
    }
    for (i = 0; i < pkg->importSymbolLen; i++) {
        const HOPImportSymbolRef* sym = &pkg->importSymbols[i];
        size_t                    nameLen;
        if (!sym->isType) {
            continue;
        }
        nameLen = strlen(sym->localName);
        if (nameLen == (size_t)(end - start) && memcmp(sym->localName, src + start, nameLen) == 0) {
            return sym;
        }
    }
    return NULL;
}

static const HOPAstNode* _Nullable ResolveMirTypeAliasTargetNode(
    const HOPPackageLoader* loader,
    const HOPMirProgram*    program,
    const HOPMirTypeRef*    typeRef,
    const HOPParsedFile** _Nullable outFile,
    uint32_t* _Nullable outSourceRef) {
    const HOPPackage*    pkg = NULL;
    const HOPParsedFile* file;
    const HOPAstNode*    node;
    const HOPSymbolDecl* decl;
    const HOPParsedFile* declFile;
    uint32_t             sourceRef;
    int32_t              targetNode;
    if (outFile != NULL) {
        *outFile = NULL;
    }
    if (outSourceRef != NULL) {
        *outSourceRef = UINT32_MAX;
    }
    if (loader == NULL || program == NULL || typeRef == NULL) {
        return NULL;
    }
    file = FindLoaderFileByMirSource(loader, program, typeRef->sourceRef, &pkg);
    if (file == NULL || pkg == NULL || typeRef->astNode >= file->ast.len) {
        return NULL;
    }
    node = &file->ast.nodes[typeRef->astNode];
    if (node->kind != HOPAst_TYPE_NAME) {
        return NULL;
    }
    decl = FindPackageTypeDeclBySlice(pkg, file->source, node->dataStart, node->dataEnd);
    if (decl == NULL || decl->kind != HOPAst_TYPE_ALIAS || decl->nodeId < 0
        || (uint32_t)decl->fileIndex >= pkg->fileLen)
    {
        return NULL;
    }
    declFile = &pkg->files[decl->fileIndex];
    if ((uint32_t)decl->nodeId >= declFile->ast.len) {
        return NULL;
    }
    targetNode = declFile->ast.nodes[decl->nodeId].firstChild;
    if (targetNode < 0 || (uint32_t)targetNode >= declFile->ast.len) {
        return NULL;
    }
    sourceRef = FindMirSourceRefByFile(program, declFile, typeRef->sourceRef);
    if (outFile != NULL) {
        *outFile = declFile;
    }
    if (outSourceRef != NULL) {
        *outSourceRef = sourceRef;
    }
    return &declFile->ast.nodes[targetNode];
}

static const HOPAstNode* _Nullable ResolveMirEnumUnderlyingTypeNode(
    const HOPPackageLoader* loader,
    const HOPMirProgram*    program,
    const HOPMirTypeRef*    typeRef,
    const HOPParsedFile** _Nullable outFile,
    uint32_t* _Nullable outSourceRef) {
    const HOPPackage*    pkg = NULL;
    const HOPParsedFile* file;
    const HOPAstNode*    node;
    const HOPSymbolDecl* decl;
    const HOPParsedFile* declFile;
    uint32_t             sourceRef;
    int32_t              underTypeNode;
    if (outFile != NULL) {
        *outFile = NULL;
    }
    if (outSourceRef != NULL) {
        *outSourceRef = UINT32_MAX;
    }
    if (loader == NULL || program == NULL || typeRef == NULL) {
        return NULL;
    }
    file = FindLoaderFileByMirSource(loader, program, typeRef->sourceRef, &pkg);
    if (file == NULL || pkg == NULL || typeRef->astNode >= file->ast.len) {
        return NULL;
    }
    node = &file->ast.nodes[typeRef->astNode];
    if (node->kind != HOPAst_TYPE_NAME) {
        return NULL;
    }
    decl = FindPackageTypeDeclBySlice(pkg, file->source, node->dataStart, node->dataEnd);
    if (decl == NULL || decl->kind != HOPAst_ENUM || decl->nodeId < 0
        || (uint32_t)decl->fileIndex >= pkg->fileLen)
    {
        return NULL;
    }
    declFile = &pkg->files[decl->fileIndex];
    if ((uint32_t)decl->nodeId >= declFile->ast.len) {
        return NULL;
    }
    underTypeNode = declFile->ast.nodes[decl->nodeId].firstChild;
    if (underTypeNode < 0 || (uint32_t)underTypeNode >= declFile->ast.len
        || !(
            declFile->ast.nodes[underTypeNode].kind == HOPAst_TYPE_NAME
            || declFile->ast.nodes[underTypeNode].kind == HOPAst_TYPE_PTR
            || declFile->ast.nodes[underTypeNode].kind == HOPAst_TYPE_REF
            || declFile->ast.nodes[underTypeNode].kind == HOPAst_TYPE_MUTREF
            || declFile->ast.nodes[underTypeNode].kind == HOPAst_TYPE_ARRAY
            || declFile->ast.nodes[underTypeNode].kind == HOPAst_TYPE_VARRAY
            || declFile->ast.nodes[underTypeNode].kind == HOPAst_TYPE_SLICE
            || declFile->ast.nodes[underTypeNode].kind == HOPAst_TYPE_MUTSLICE
            || declFile->ast.nodes[underTypeNode].kind == HOPAst_TYPE_OPTIONAL
            || declFile->ast.nodes[underTypeNode].kind == HOPAst_TYPE_FN
            || declFile->ast.nodes[underTypeNode].kind == HOPAst_TYPE_ANON_STRUCT
            || declFile->ast.nodes[underTypeNode].kind == HOPAst_TYPE_ANON_UNION
            || declFile->ast.nodes[underTypeNode].kind == HOPAst_TYPE_TUPLE))
    {
        return NULL;
    }
    sourceRef = FindMirSourceRefByFile(program, declFile, typeRef->sourceRef);
    if (outFile != NULL) {
        *outFile = declFile;
    }
    if (outSourceRef != NULL) {
        *outSourceRef = sourceRef;
    }
    return &declFile->ast.nodes[underTypeNode];
}

static const HOPAstNode* _Nullable ResolveMirAggregateDeclNode(
    const HOPPackageLoader* loader,
    const HOPMirProgram*    program,
    const HOPMirTypeRef*    typeRef,
    const HOPParsedFile** _Nullable outFile,
    uint32_t* _Nullable outSourceRef) {
    const HOPPackage*         pkg = NULL;
    const HOPParsedFile*      file;
    const HOPAstNode*         node;
    const HOPSymbolDecl*      decl;
    const HOPImportSymbolRef* importSym;
    const HOPParsedFile*      declFile;
    uint32_t                  sourceRef;
    if (outFile != NULL) {
        *outFile = NULL;
    }
    if (outSourceRef != NULL) {
        *outSourceRef = UINT32_MAX;
    }
    if (loader == NULL || program == NULL || typeRef == NULL) {
        return NULL;
    }
    file = FindLoaderFileByMirSource(loader, program, typeRef->sourceRef, &pkg);
    if (file == NULL || pkg == NULL || typeRef->astNode >= file->ast.len) {
        return NULL;
    }
    node = &file->ast.nodes[typeRef->astNode];
    if (node->kind == HOPAst_TYPE_ANON_STRUCT || node->kind == HOPAst_TYPE_ANON_UNION) {
        if (outFile != NULL) {
            *outFile = file;
        }
        if (outSourceRef != NULL) {
            *outSourceRef = typeRef->sourceRef;
        }
        return node;
    }
    if (node->kind != HOPAst_TYPE_NAME) {
        return NULL;
    }
    decl = FindPackageTypeDeclBySlice(pkg, file->source, node->dataStart, node->dataEnd);
    if (decl != NULL) {
        if (decl->nodeId < 0 || (uint32_t)decl->fileIndex >= pkg->fileLen) {
            return NULL;
        }
        declFile = &pkg->files[decl->fileIndex];
        if ((decl->kind != HOPAst_STRUCT && decl->kind != HOPAst_UNION)
            || (uint32_t)decl->nodeId >= declFile->ast.len)
        {
            return NULL;
        }
        sourceRef = FindMirSourceRefByFile(program, declFile, typeRef->sourceRef);
        if (outFile != NULL) {
            *outFile = declFile;
        }
        if (outSourceRef != NULL) {
            *outSourceRef = sourceRef;
        }
        return &declFile->ast.nodes[decl->nodeId];
    }
    importSym = FindImportTypeSymbolBySlice(pkg, file->source, node->dataStart, node->dataEnd);
    if (importSym == NULL || importSym->importIndex >= pkg->importLen) {
        return NULL;
    }
    {
        const HOPPackage* targetPkg = EffectiveMirImportTargetPackage(
            loader, &pkg->imports[importSym->importIndex]);
        if (targetPkg == NULL || importSym->exportNodeId < 0
            || importSym->exportFileIndex >= targetPkg->fileLen)
        {
            return NULL;
        }
        declFile = &targetPkg->files[importSym->exportFileIndex];
        if ((uint32_t)importSym->exportNodeId >= declFile->ast.len
            || (declFile->ast.nodes[importSym->exportNodeId].kind != HOPAst_STRUCT
                && declFile->ast.nodes[importSym->exportNodeId].kind != HOPAst_UNION))
        {
            return NULL;
        }
        sourceRef = FindMirSourceRefByFile(program, declFile, typeRef->sourceRef);
        if (outFile != NULL) {
            *outFile = declFile;
        }
        if (outSourceRef != NULL) {
            *outSourceRef = sourceRef;
        }
        return &declFile->ast.nodes[importSym->exportNodeId];
    }
}

static uint32_t ClassifyMirTypeFlags(
    const HOPPackageLoader* loader,
    const HOPMirProgram*    program,
    const HOPParsedFile*    file,
    const HOPMirTypeRef*    typeRef) {
    uint32_t          flags = ClassifyMirScalarType(file, typeRef);
    const HOPAstNode* node;
    const HOPAstNode* child;
    if (file == NULL || typeRef == NULL || typeRef->astNode >= file->ast.len) {
        return flags;
    }
    node = &file->ast.nodes[typeRef->astNode];
    if (node->kind == HOPAst_TYPE_OPTIONAL && node->firstChild >= 0
        && (uint32_t)node->firstChild < file->ast.len)
    {
        HOPMirTypeRef childTypeRef = {
            .astNode = (uint32_t)node->firstChild,
            .sourceRef = typeRef->sourceRef,
            .flags = 0u,
            .aux = 0u,
        };
        return HOPMirTypeFlag_OPTIONAL | ClassifyMirTypeFlags(loader, program, file, &childTypeRef);
    }
    if ((node->kind == HOPAst_TYPE_REF || node->kind == HOPAst_TYPE_PTR) && node->firstChild >= 0
        && (uint32_t)node->firstChild < file->ast.len)
    {
        HOPMirTypeRef childTypeRef = {
            .astNode = (uint32_t)node->firstChild,
            .sourceRef = typeRef->sourceRef,
            .flags = 0u,
            .aux = 0u,
        };
        child = &file->ast.nodes[node->firstChild];
        if (child->kind == HOPAst_TYPE_NAME) {
            if (SliceEqCStr(file->source, child->dataStart, child->dataEnd, "str")) {
                flags |=
                    node->kind == HOPAst_TYPE_REF ? HOPMirTypeFlag_STR_REF : HOPMirTypeFlag_STR_PTR;
            } else if (SliceEqCStr(file->source, child->dataStart, child->dataEnd, "u8")) {
                flags |= HOPMirTypeFlag_U8_PTR;
            } else if (SliceEqCStr(file->source, child->dataStart, child->dataEnd, "i8")) {
                flags |= HOPMirTypeFlag_I8_PTR;
            } else if (SliceEqCStr(file->source, child->dataStart, child->dataEnd, "u16")) {
                flags |= HOPMirTypeFlag_U16_PTR;
            } else if (SliceEqCStr(file->source, child->dataStart, child->dataEnd, "i16")) {
                flags |= HOPMirTypeFlag_I16_PTR;
            } else if (SliceEqCStr(file->source, child->dataStart, child->dataEnd, "u32")) {
                flags |= HOPMirTypeFlag_U32_PTR;
            } else if (
                SliceEqCStr(file->source, child->dataStart, child->dataEnd, "bool")
                || SliceEqCStr(file->source, child->dataStart, child->dataEnd, "i32")
                || SliceEqCStr(file->source, child->dataStart, child->dataEnd, "uint")
                || SliceEqCStr(file->source, child->dataStart, child->dataEnd, "int"))
            {
                flags |= HOPMirTypeFlag_I32_PTR;
            } else if (
                loader != NULL && program != NULL
                && ResolveMirAggregateDeclNode(loader, program, &childTypeRef, NULL, NULL) != NULL)
            {
                flags |= HOPMirTypeFlag_OPAQUE_PTR;
            }
        } else if (
            child->kind == HOPAst_TYPE_ARRAY
            && ClassifyMirIntKindFromTypeNode(
                   file,
                   child->firstChild >= 0 && (uint32_t)child->firstChild < file->ast.len
                       ? &file->ast.nodes[child->firstChild]
                       : NULL)
                   != HOPMirIntKind_NONE
            && ParseMirArrayLen(file, child) != 0u)
        {
            flags |= HOPMirTypeFlag_FIXED_ARRAY_VIEW;
        } else if (
            child->kind == HOPAst_TYPE_SLICE && child->firstChild >= 0
            && (uint32_t)child->firstChild < file->ast.len
            && (ClassifyMirIntKindFromTypeNode(file, &file->ast.nodes[child->firstChild])
                    != HOPMirIntKind_NONE
                || (loader != NULL && program != NULL
                    && ResolveMirAggregateDeclNode(
                           loader,
                           program,
                           &(HOPMirTypeRef){
                               .astNode = (uint32_t)child->firstChild,
                               .sourceRef = typeRef->sourceRef,
                               .flags = 0u,
                               .aux = 0u,
                           },
                           NULL,
                           NULL)
                           != NULL)))
        {
            flags |= ClassifyMirIntKindFromTypeNode(file, &file->ast.nodes[child->firstChild])
                          != HOPMirIntKind_NONE
                       ? HOPMirTypeFlag_SLICE_VIEW
                       : HOPMirTypeFlag_AGG_SLICE_VIEW;
        } else if (
            child->kind == HOPAst_TYPE_VARRAY && child->firstChild >= 0
            && (uint32_t)child->firstChild < file->ast.len
            && (ClassifyMirIntKindFromTypeNode(file, &file->ast.nodes[child->firstChild])
                    != HOPMirIntKind_NONE
                || (loader != NULL && program != NULL
                    && ResolveMirAggregateDeclNode(
                           loader,
                           program,
                           &(HOPMirTypeRef){
                               .astNode = (uint32_t)child->firstChild,
                               .sourceRef = typeRef->sourceRef,
                               .flags = 0u,
                               .aux = 0u,
                           },
                           NULL,
                           NULL)
                           != NULL)))
        {
            flags |= ClassifyMirIntKindFromTypeNode(file, &file->ast.nodes[child->firstChild])
                          != HOPMirIntKind_NONE
                       ? HOPMirTypeFlag_VARRAY_VIEW
                       : HOPMirTypeFlag_AGG_SLICE_VIEW;
        }
    } else if (
        node->kind == HOPAst_TYPE_ARRAY && node->firstChild >= 0
        && (uint32_t)node->firstChild < file->ast.len)
    {
        child = &file->ast.nodes[node->firstChild];
        if (ClassifyMirIntKindFromTypeNode(file, child) != HOPMirIntKind_NONE
            && ParseMirArrayLen(file, node) != 0u)
        {
            flags |= HOPMirTypeFlag_FIXED_ARRAY;
        }
    } else if (node->kind == HOPAst_TYPE_FN) {
        flags |= HOPMirTypeFlag_FUNC_REF;
    } else if (
        node->kind == HOPAst_TYPE_VARRAY && node->firstChild >= 0
        && (uint32_t)node->firstChild < file->ast.len
        && ClassifyMirIntKindFromTypeNode(file, &file->ast.nodes[node->firstChild])
               != HOPMirIntKind_NONE)
    {
        flags |= HOPMirTypeFlag_VARRAY_VIEW;
    } else if (
        node->kind == HOPAst_TYPE_NAME
        && SliceEqCStr(file->source, node->dataStart, node->dataEnd, "str"))
    {
        flags |= HOPMirTypeFlag_STR_OBJ;
    } else if (
        node->kind == HOPAst_TYPE_NAME
        && SliceEqCStr(file->source, node->dataStart, node->dataEnd, "rawptr"))
    {
        flags |= HOPMirTypeFlag_OPAQUE_PTR;
    } else if (node->kind == HOPAst_TYPE_NAME) {
        const HOPParsedFile* aliasFile = NULL;
        uint32_t             aliasSourceRef = UINT32_MAX;
        const HOPAstNode*    aliasTarget = ResolveMirTypeAliasTargetNode(
            loader, program, typeRef, &aliasFile, &aliasSourceRef);
        if (aliasTarget != NULL && aliasFile != NULL) {
            HOPMirTypeRef aliasTypeRef = {
                .astNode = (uint32_t)(aliasTarget - aliasFile->ast.nodes),
                .sourceRef = aliasSourceRef,
                .flags = 0u,
                .aux = 0u,
            };
            flags |= ClassifyMirTypeFlags(loader, program, aliasFile, &aliasTypeRef);
        }
        if (flags == HOPMirTypeScalar_NONE) {
            const HOPParsedFile* enumFile = NULL;
            uint32_t             enumSourceRef = UINT32_MAX;
            const HOPAstNode*    enumType = ResolveMirEnumUnderlyingTypeNode(
                loader, program, typeRef, &enumFile, &enumSourceRef);
            if (enumType != NULL && enumFile != NULL) {
                HOPMirTypeRef enumTypeRef = {
                    .astNode = (uint32_t)(enumType - enumFile->ast.nodes),
                    .sourceRef = enumSourceRef,
                    .flags = 0u,
                    .aux = 0u,
                };
                flags |= ClassifyMirTypeFlags(loader, program, enumFile, &enumTypeRef);
            }
        }
    } else if (
        loader != NULL && program != NULL
        && ResolveMirAggregateDeclNode(loader, program, typeRef, NULL, NULL) != NULL)
    {
        flags |= HOPMirTypeFlag_AGGREGATE;
    }
    return flags;
}

static uint32_t ClassifyMirTypeAux(
    const HOPPackageLoader* loader,
    const HOPMirProgram*    program,
    const HOPParsedFile*    file,
    const HOPMirTypeRef*    typeRef) {
    const HOPAstNode* node;
    const HOPAstNode* child;
    HOPMirIntKind     intKind;
    uint32_t          arrayCount;
    if (file == NULL || typeRef == NULL || typeRef->astNode >= file->ast.len) {
        return 0u;
    }
    node = &file->ast.nodes[typeRef->astNode];
    switch (node->kind) {
        case HOPAst_TYPE_OPTIONAL:
            if (node->firstChild < 0 || (uint32_t)node->firstChild >= file->ast.len) {
                return 0u;
            }
            return ClassifyMirTypeAux(
                loader,
                program,
                file,
                &(HOPMirTypeRef){
                    .astNode = (uint32_t)node->firstChild,
                    .sourceRef = typeRef->sourceRef,
                    .flags = 0u,
                    .aux = 0u,
                });
        case HOPAst_TYPE_NAME:
            intKind = ClassifyMirIntKindFromTypeNode(file, node);
            if (intKind != HOPMirIntKind_NONE) {
                return HOPMirTypeAuxMakeScalarInt(intKind);
            }
            if (loader != NULL && program != NULL) {
                const HOPParsedFile* aliasFile = NULL;
                uint32_t             aliasSourceRef = UINT32_MAX;
                const HOPAstNode*    aliasTarget = ResolveMirTypeAliasTargetNode(
                    loader, program, typeRef, &aliasFile, &aliasSourceRef);
                if (aliasTarget != NULL && aliasFile != NULL) {
                    return ClassifyMirTypeAux(
                        loader,
                        program,
                        aliasFile,
                        &(HOPMirTypeRef){
                            .astNode = (uint32_t)(aliasTarget - aliasFile->ast.nodes),
                            .sourceRef = aliasSourceRef,
                            .flags = 0u,
                            .aux = 0u,
                        });
                }
                {
                    const HOPParsedFile* enumFile = NULL;
                    uint32_t             enumSourceRef = UINT32_MAX;
                    const HOPAstNode*    enumType = ResolveMirEnumUnderlyingTypeNode(
                        loader, program, typeRef, &enumFile, &enumSourceRef);
                    if (enumType != NULL && enumFile != NULL) {
                        return ClassifyMirTypeAux(
                            loader,
                            program,
                            enumFile,
                            &(HOPMirTypeRef){
                                .astNode = (uint32_t)(enumType - enumFile->ast.nodes),
                                .sourceRef = enumSourceRef,
                                .flags = 0u,
                                .aux = 0u,
                            });
                    }
                }
            }
            return 0u;
        case HOPAst_TYPE_PTR:
        case HOPAst_TYPE_REF:
        case HOPAst_TYPE_MUTREF:
            if (node->firstChild < 0 || (uint32_t)node->firstChild >= file->ast.len) {
                return 0u;
            }
            child = &file->ast.nodes[node->firstChild];
            if (child->kind == HOPAst_TYPE_ARRAY && child->firstChild >= 0
                && (uint32_t)child->firstChild < file->ast.len)
            {
                intKind = ClassifyMirIntKindFromTypeNode(file, &file->ast.nodes[child->firstChild]);
                arrayCount = ParseMirArrayLen(file, child);
                if (intKind != HOPMirIntKind_NONE && arrayCount != 0u) {
                    return HOPMirTypeAuxMakeFixedArray(intKind, arrayCount);
                }
                return 0u;
            }
            if (child->kind == HOPAst_TYPE_SLICE && child->firstChild >= 0
                && (uint32_t)child->firstChild < file->ast.len)
            {
                intKind = ClassifyMirIntKindFromTypeNode(file, &file->ast.nodes[child->firstChild]);
                if (intKind != HOPMirIntKind_NONE) {
                    return HOPMirTypeAuxMakeScalarInt(intKind);
                }
                return HOPMirTypeAuxMakeAggSliceView(UINT32_MAX);
            }
            if (child->kind == HOPAst_TYPE_VARRAY && child->firstChild >= 0
                && (uint32_t)child->firstChild < file->ast.len)
            {
                intKind = ClassifyMirIntKindFromTypeNode(file, &file->ast.nodes[child->firstChild]);
                if (intKind != HOPMirIntKind_NONE) {
                    return HOPMirTypeAuxMakeScalarInt(intKind);
                }
                return HOPMirTypeAuxMakeAggSliceView(UINT32_MAX);
            }
            intKind = ClassifyMirIntKindFromTypeNode(file, child);
            return intKind != HOPMirIntKind_NONE ? HOPMirTypeAuxMakeScalarInt(intKind) : 0u;
        case HOPAst_TYPE_ARRAY:
            if (node->firstChild < 0 || (uint32_t)node->firstChild >= file->ast.len) {
                return 0u;
            }
            child = &file->ast.nodes[node->firstChild];
            intKind = ClassifyMirIntKindFromTypeNode(file, child);
            arrayCount = ParseMirArrayLen(file, node);
            if (intKind == HOPMirIntKind_NONE || arrayCount == 0u) {
                return 0u;
            }
            return HOPMirTypeAuxMakeFixedArray(intKind, arrayCount);
        case HOPAst_TYPE_VARRAY:
            if (node->firstChild < 0 || (uint32_t)node->firstChild >= file->ast.len) {
                return 0u;
            }
            intKind = ClassifyMirIntKindFromTypeNode(file, &file->ast.nodes[node->firstChild]);
            return intKind != HOPMirIntKind_NONE ? HOPMirTypeAuxMakeScalarInt(intKind) : 0u;
        default: return 0u;
    }
}

static void EnrichMirTypeFlags(const HOPPackageLoader* loader, HOPMirProgram* program) {
    uint32_t i;
    if (loader == NULL || program == NULL || program->types == NULL) {
        return;
    }
    for (i = 0; i < program->typeLen; i++) {
        HOPMirTypeRef*       typeRef = (HOPMirTypeRef*)&program->types[i];
        const HOPParsedFile* file = FindLoaderFileByMirSource(
            loader, program, typeRef->sourceRef, NULL);
        if (typeRef->astNode == UINT32_MAX && typeRef->sourceRef == UINT32_MAX) {
            continue;
        }
        typeRef->flags =
            (typeRef->flags
             & ~(HOPMirTypeFlag_SCALAR_MASK | HOPMirTypeFlag_STR_REF | HOPMirTypeFlag_STR_PTR
                 | HOPMirTypeFlag_STR_OBJ | HOPMirTypeFlag_U8_PTR | HOPMirTypeFlag_I32_PTR
                 | HOPMirTypeFlag_I8_PTR | HOPMirTypeFlag_U16_PTR | HOPMirTypeFlag_I16_PTR
                 | HOPMirTypeFlag_U32_PTR | HOPMirTypeFlag_FIXED_ARRAY
                 | HOPMirTypeFlag_FIXED_ARRAY_VIEW | HOPMirTypeFlag_SLICE_VIEW
                 | HOPMirTypeFlag_VARRAY_VIEW | HOPMirTypeFlag_AGG_SLICE_VIEW
                 | HOPMirTypeFlag_AGGREGATE | HOPMirTypeFlag_OPAQUE_PTR | HOPMirTypeFlag_OPTIONAL
                 | HOPMirTypeFlag_FUNC_REF))
            | ClassifyMirTypeFlags(loader, program, file, typeRef);
        typeRef->aux = ClassifyMirTypeAux(loader, program, file, typeRef);
    }
}

static int EnsureMirAstTypeRef(
    HOPArena*               arena,
    const HOPPackageLoader* loader,
    HOPMirProgram*          program,
    uint32_t                astNode,
    uint32_t                sourceRef,
    uint32_t* _Nonnull outTypeRef) {
    uint32_t             i;
    HOPMirTypeRef*       newTypes;
    const HOPParsedFile* file;
    if (arena == NULL || loader == NULL || program == NULL || outTypeRef == NULL) {
        return -1;
    }
    for (i = 0; i < program->typeLen; i++) {
        if (program->types[i].astNode == astNode && program->types[i].sourceRef == sourceRef) {
            *outTypeRef = i;
            return 0;
        }
    }
    newTypes = (HOPMirTypeRef*)HOPArenaAlloc(
        arena, sizeof(HOPMirTypeRef) * (program->typeLen + 1u), (uint32_t)_Alignof(HOPMirTypeRef));
    if (newTypes == NULL) {
        return -1;
    }
    if (program->typeLen != 0u) {
        memcpy(newTypes, program->types, sizeof(HOPMirTypeRef) * program->typeLen);
    }
    newTypes[program->typeLen] =
        (HOPMirTypeRef){ .astNode = astNode, .sourceRef = sourceRef, .flags = 0u, .aux = 0u };
    file = FindLoaderFileByMirSource(loader, program, sourceRef, NULL);
    newTypes[program->typeLen].flags = ClassifyMirTypeFlags(
        loader, program, file, &newTypes[program->typeLen]);
    newTypes[program->typeLen].aux = ClassifyMirTypeAux(
        loader, program, file, &newTypes[program->typeLen]);
    program->types = newTypes;
    *outTypeRef = program->typeLen++;
    return 0;
}

static int EnsureMirAggregateFieldRef(
    HOPArena*      arena,
    HOPMirProgram* program,
    uint32_t       nameStart,
    uint32_t       nameEnd,
    uint32_t       sourceRef,
    uint32_t       ownerTypeRef,
    uint32_t       typeRef,
    uint32_t* _Nonnull outFieldRef) {
    uint32_t     i;
    HOPMirField* newFields;
    if (arena == NULL || program == NULL || outFieldRef == NULL) {
        return -1;
    }
    for (i = 0; i < program->fieldLen; i++) {
        if (program->fields[i].nameStart == nameStart && program->fields[i].nameEnd == nameEnd
            && program->fields[i].sourceRef == sourceRef
            && program->fields[i].ownerTypeRef == ownerTypeRef
            && program->fields[i].typeRef == typeRef)
        {
            *outFieldRef = i;
            return 0;
        }
    }
    newFields = (HOPMirField*)HOPArenaAlloc(
        arena, sizeof(HOPMirField) * (program->fieldLen + 1u), (uint32_t)_Alignof(HOPMirField));
    if (newFields == NULL) {
        return -1;
    }
    if (program->fieldLen != 0u) {
        memcpy(newFields, program->fields, sizeof(HOPMirField) * program->fieldLen);
    }
    newFields[program->fieldLen] = (HOPMirField){
        .nameStart = nameStart,
        .nameEnd = nameEnd,
        .sourceRef = sourceRef,
        .ownerTypeRef = ownerTypeRef,
        .typeRef = typeRef,
    };
    program->fields = newFields;
    *outFieldRef = program->fieldLen++;
    return 0;
}

static int EnsureMirAggregateFieldsForType(
    const HOPPackageLoader* loader,
    HOPArena*               arena,
    HOPMirProgram*          program,
    uint32_t                ownerTypeRef) {
    const HOPParsedFile* file = NULL;
    const HOPParsedFile* ownerFile = NULL;
    const HOPAstNode*    declNode;
    const HOPAstNode*    ownerNode = NULL;
    uint32_t             ownerSourceRef = UINT32_MAX;
    int32_t              childNode;
    if (loader == NULL || arena == NULL || program == NULL || ownerTypeRef >= program->typeLen) {
        return -1;
    }
    if (program->types[ownerTypeRef].flags != 0u
        && !HOPMirTypeRefIsAggregate(&program->types[ownerTypeRef]))
    {
        return 0;
    }
    declNode = ResolveMirAggregateDeclNode(
        loader, program, &program->types[ownerTypeRef], &file, &ownerSourceRef);
    ownerFile = FindLoaderFileByMirSource(
        loader, program, program->types[ownerTypeRef].sourceRef, NULL);
    if (ownerFile != NULL && program->types[ownerTypeRef].astNode < ownerFile->ast.len) {
        ownerNode = &ownerFile->ast.nodes[program->types[ownerTypeRef].astNode];
    }
    if (declNode == NULL || file == NULL
        || (declNode->kind != HOPAst_STRUCT && declNode->kind != HOPAst_UNION
            && declNode->kind != HOPAst_TYPE_ANON_STRUCT
            && declNode->kind != HOPAst_TYPE_ANON_UNION))
    {
        return 0;
    }
    if (ownerNode == NULL
        || (ownerNode->kind != HOPAst_TYPE_NAME && ownerNode->kind != HOPAst_TYPE_ANON_STRUCT
            && ownerNode->kind != HOPAst_TYPE_ANON_UNION && ownerNode->kind != HOPAst_STRUCT
            && ownerNode->kind != HOPAst_UNION))
    {
        return 0;
    }
    ((HOPMirTypeRef*)&program->types[ownerTypeRef])->flags |= HOPMirTypeFlag_AGGREGATE;
    childNode = declNode->firstChild;
    while (childNode >= 0 && (uint32_t)childNode < file->ast.len) {
        const HOPAstNode* fieldNode = &file->ast.nodes[childNode];
        uint32_t          fieldTypeRef = UINT32_MAX;
        uint32_t          fieldRef = UINT32_MAX;
        int32_t           typeNode = fieldNode->firstChild;
        uint32_t          typeSourceRef = ownerSourceRef;
        if (fieldNode->kind != HOPAst_FIELD) {
            childNode = fieldNode->nextSibling;
            continue;
        }
        if (typeNode < 0 || (uint32_t)typeNode >= file->ast.len) {
            return -1;
        }
        if (ownerFile != NULL && ownerNode != NULL && ownerNode->kind == HOPAst_TYPE_NAME
            && file->ast.nodes[typeNode].kind == HOPAst_TYPE_NAME)
        {
            int32_t typeParamNode = declNode->firstChild;
            int32_t typeArgNode = ownerNode->firstChild;
            while (typeParamNode >= 0 && typeArgNode >= 0) {
                const HOPAstNode* param = &file->ast.nodes[typeParamNode];
                if (param->kind == HOPAst_TYPE_PARAM
                    && HOPNameEqSlice(
                        (HOPStrView){ file->source, file->sourceLen },
                        param->dataStart,
                        param->dataEnd,
                        file->ast.nodes[typeNode].dataStart,
                        file->ast.nodes[typeNode].dataEnd))
                {
                    typeNode = typeArgNode;
                    typeSourceRef = program->types[ownerTypeRef].sourceRef;
                    break;
                }
                typeParamNode = param->nextSibling;
                typeArgNode = ownerFile->ast.nodes[typeArgNode].nextSibling;
            }
        }
        if (EnsureMirAstTypeRef(
                arena, loader, program, (uint32_t)typeNode, typeSourceRef, &fieldTypeRef)
            != 0)
        {
            return -1;
        }
        if (EnsureMirAggregateFieldRef(
                arena,
                program,
                fieldNode->dataStart,
                fieldNode->dataEnd,
                ownerSourceRef,
                ownerTypeRef,
                fieldTypeRef,
                &fieldRef)
            != 0)
        {
            return -1;
        }
        (void)fieldRef;
        childNode = fieldNode->nextSibling;
    }
    return 0;
}

static int EnrichMirAggregateFields(
    const HOPPackageLoader* loader, HOPArena* arena, HOPMirProgram* program) {
    uint32_t i;
    if (loader == NULL || arena == NULL || program == NULL) {
        return -1;
    }
    for (i = 0; i < program->typeLen; i++) {
        if (EnsureMirAggregateFieldsForType(loader, arena, program, i) != 0) {
            return -1;
        }
    }
    return 0;
}

static int EnrichMirVArrayCountFields(const HOPPackageLoader* loader, HOPMirProgram* program) {
    uint32_t i;
    if (loader == NULL || program == NULL) {
        return -1;
    }
    for (i = 0; i < program->fieldLen; i++) {
        HOPMirTypeRef*       typeRef;
        const HOPParsedFile* file;
        const HOPAstNode*    typeNode;
        uint32_t             countFieldRef = UINT32_MAX;
        if (program->fields[i].typeRef >= program->typeLen) {
            continue;
        }
        typeRef = (HOPMirTypeRef*)&program->types[program->fields[i].typeRef];
        if (!HOPMirTypeRefIsVArrayView(typeRef)) {
            continue;
        }
        file = FindLoaderFileByMirSource(loader, program, typeRef->sourceRef, NULL);
        if (file == NULL || typeRef->astNode >= file->ast.len) {
            continue;
        }
        typeNode = &file->ast.nodes[typeRef->astNode];
        if (typeNode->kind != HOPAst_TYPE_VARRAY
            || !FindMirFieldByOwnerAndSlice(
                program,
                program->fields[i].ownerTypeRef,
                typeRef->sourceRef,
                typeNode->dataStart,
                typeNode->dataEnd,
                &countFieldRef))
        {
            return -1;
        }
        typeRef->aux = HOPMirTypeAuxMakeVArrayView(HOPMirTypeRefIntKind(typeRef), countFieldRef);
    }
    return 0;
}

static int EnrichMirAggSliceElemTypes(
    const HOPPackageLoader* loader, HOPArena* arena, HOPMirProgram* program) {
    uint32_t i = 0;
    if (loader == NULL || arena == NULL || program == NULL) {
        return -1;
    }
    while (i < program->typeLen) {
        HOPMirTypeRef*       typeRef = (HOPMirTypeRef*)&program->types[i];
        const HOPParsedFile* file;
        const HOPAstNode*    node;
        const HOPAstNode*    child;
        uint32_t             elemTypeRef = UINT32_MAX;
        uint32_t             sourceRef;
        if (!HOPMirTypeRefIsAggSliceView(typeRef)) {
            i++;
            continue;
        }
        sourceRef = typeRef->sourceRef;
        file = FindLoaderFileByMirSource(loader, program, sourceRef, NULL);
        if (file == NULL || typeRef->astNode >= file->ast.len) {
            i++;
            continue;
        }
        node = &file->ast.nodes[typeRef->astNode];
        if ((node->kind != HOPAst_TYPE_PTR && node->kind != HOPAst_TYPE_REF
             && node->kind != HOPAst_TYPE_MUTREF)
            || node->firstChild < 0 || (uint32_t)node->firstChild >= file->ast.len)
        {
            i++;
            continue;
        }
        child = &file->ast.nodes[node->firstChild];
        if ((child->kind != HOPAst_TYPE_SLICE && child->kind != HOPAst_TYPE_VARRAY)
            || child->firstChild < 0 || (uint32_t)child->firstChild >= file->ast.len)
        {
            i++;
            continue;
        }
        if (EnsureMirAstTypeRef(
                arena, loader, program, (uint32_t)child->firstChild, sourceRef, &elemTypeRef)
            != 0)
        {
            return -1;
        }
        typeRef = (HOPMirTypeRef*)&program->types[i];
        typeRef->aux = HOPMirTypeAuxMakeAggSliceView(elemTypeRef);
        if (EnsureMirAggregateFieldsForType(loader, arena, program, elemTypeRef) != 0) {
            return -1;
        }
        i++;
    }
    return 0;
}

static int EnrichMirOpaquePtrPointees(
    const HOPPackageLoader* loader, HOPArena* arena, HOPMirProgram* program) {
    uint32_t i = 0;
    if (loader == NULL || arena == NULL || program == NULL) {
        return -1;
    }
    while (i < program->typeLen) {
        HOPMirTypeRef*       typeRef = (HOPMirTypeRef*)&program->types[i];
        const HOPParsedFile* file;
        const HOPAstNode*    node;
        HOPMirTypeRef        childTypeRef;
        uint32_t             pointeeTypeRef = UINT32_MAX;
        uint32_t             sourceRef;
        if (!HOPMirTypeRefIsOpaquePtr(typeRef)) {
            i++;
            continue;
        }
        sourceRef = typeRef->sourceRef;
        file = FindLoaderFileByMirSource(loader, program, sourceRef, NULL);
        if (file == NULL || typeRef->astNode >= file->ast.len) {
            i++;
            continue;
        }
        node = &file->ast.nodes[typeRef->astNode];
        if (node->kind != HOPAst_TYPE_PTR || node->firstChild < 0
            || (uint32_t)node->firstChild >= file->ast.len)
        {
            i++;
            continue;
        }
        childTypeRef = (HOPMirTypeRef){
            .astNode = (uint32_t)node->firstChild,
            .sourceRef = sourceRef,
            .flags = 0u,
            .aux = 0u,
        };
        if (ResolveMirAggregateDeclNode(loader, program, &childTypeRef, NULL, NULL) == NULL) {
            i++;
            continue;
        }
        if (EnsureMirAstTypeRef(
                arena, loader, program, (uint32_t)node->firstChild, sourceRef, &pointeeTypeRef)
            != 0)
        {
            return -1;
        }
        typeRef = (HOPMirTypeRef*)&program->types[i];
        typeRef->aux = pointeeTypeRef;
        if (EnsureMirAggregateFieldsForType(loader, arena, program, pointeeTypeRef) != 0) {
            return -1;
        }
        i++;
    }
    return 0;
}

static int EnsureMirScalarTypeRef(
    HOPArena*        arena,
    HOPMirProgram*   program,
    HOPMirTypeScalar scalar,
    uint32_t* _Nonnull outTypeRef) {
    uint32_t       i;
    HOPMirTypeRef* newTypes;
    if (arena == NULL || program == NULL || outTypeRef == NULL || scalar == HOPMirTypeScalar_NONE) {
        return -1;
    }
    for (i = 0; i < program->typeLen; i++) {
        if (HOPMirTypeRefScalarKind(&program->types[i]) == scalar) {
            *outTypeRef = i;
            return 0;
        }
    }
    newTypes = (HOPMirTypeRef*)HOPArenaAlloc(
        arena, sizeof(HOPMirTypeRef) * (program->typeLen + 1u), (uint32_t)_Alignof(HOPMirTypeRef));
    if (newTypes == NULL) {
        return -1;
    }
    if (program->typeLen != 0u) {
        memcpy(newTypes, program->types, sizeof(HOPMirTypeRef) * program->typeLen);
    }
    newTypes[program->typeLen] = (HOPMirTypeRef){
        .astNode = UINT32_MAX,
        .sourceRef = 0,
        .flags = (uint32_t)scalar,
        .aux = 0,
    };
    program->types = newTypes;
    *outTypeRef = program->typeLen++;
    return 0;
}

typedef enum {
    MirInferredType_NONE = 0,
    MirInferredType_I32,
    MirInferredType_I64,
    MirInferredType_F32,
    MirInferredType_F64,
    MirInferredType_STR_REF,
    MirInferredType_STR_PTR,
    MirInferredType_U8_PTR,
    MirInferredType_I8_PTR,
    MirInferredType_U16_PTR,
    MirInferredType_I16_PTR,
    MirInferredType_U32_PTR,
    MirInferredType_I32_PTR,
    MirInferredType_OPAQUE_PTR,
    MirInferredType_ARRAY_U8,
    MirInferredType_ARRAY_I8,
    MirInferredType_ARRAY_U16,
    MirInferredType_ARRAY_I16,
    MirInferredType_ARRAY_U32,
    MirInferredType_ARRAY_I32,
    MirInferredType_SLICE_U8,
    MirInferredType_SLICE_I8,
    MirInferredType_SLICE_U16,
    MirInferredType_SLICE_I16,
    MirInferredType_SLICE_U32,
    MirInferredType_SLICE_I32,
    MirInferredType_SLICE_AGG,
    MirInferredType_AGG,
    MirInferredType_FUNC_REF,
} MirInferredType;

static MirInferredType MirInferredTypeFromTypeRef(const HOPMirTypeRef* typeRef) {
    uint32_t flags;
    if (typeRef == NULL) {
        return MirInferredType_NONE;
    }
    flags = typeRef->flags;
    if ((flags & HOPMirTypeFlag_STR_REF) != 0) {
        return MirInferredType_STR_REF;
    }
    if ((flags & HOPMirTypeFlag_STR_PTR) != 0) {
        return MirInferredType_STR_PTR;
    }
    if ((flags & HOPMirTypeFlag_STR_OBJ) != 0) {
        return MirInferredType_STR_PTR;
    }
    if ((flags & HOPMirTypeFlag_U8_PTR) != 0) {
        return MirInferredType_U8_PTR;
    }
    if ((flags & HOPMirTypeFlag_I8_PTR) != 0) {
        return MirInferredType_I8_PTR;
    }
    if ((flags & HOPMirTypeFlag_U16_PTR) != 0) {
        return MirInferredType_U16_PTR;
    }
    if ((flags & HOPMirTypeFlag_I16_PTR) != 0) {
        return MirInferredType_I16_PTR;
    }
    if ((flags & HOPMirTypeFlag_U32_PTR) != 0) {
        return MirInferredType_U32_PTR;
    }
    if ((flags & HOPMirTypeFlag_I32_PTR) != 0) {
        return MirInferredType_I32_PTR;
    }
    if ((flags & HOPMirTypeFlag_OPAQUE_PTR) != 0) {
        return MirInferredType_OPAQUE_PTR;
    }
    if ((flags & HOPMirTypeFlag_FUNC_REF) != 0) {
        return MirInferredType_FUNC_REF;
    }
    if ((flags & (HOPMirTypeFlag_FIXED_ARRAY | HOPMirTypeFlag_FIXED_ARRAY_VIEW)) != 0) {
        switch (HOPMirTypeRefIntKind(typeRef)) {
            case HOPMirIntKind_U8:   return MirInferredType_ARRAY_U8;
            case HOPMirIntKind_I8:   return MirInferredType_ARRAY_I8;
            case HOPMirIntKind_U16:  return MirInferredType_ARRAY_U16;
            case HOPMirIntKind_I16:  return MirInferredType_ARRAY_I16;
            case HOPMirIntKind_U32:  return MirInferredType_ARRAY_U32;
            case HOPMirIntKind_BOOL:
            case HOPMirIntKind_I32:  return MirInferredType_ARRAY_I32;
            default:                 return MirInferredType_NONE;
        }
    }
    if ((flags & (HOPMirTypeFlag_SLICE_VIEW | HOPMirTypeFlag_VARRAY_VIEW)) != 0) {
        switch (HOPMirTypeRefIntKind(typeRef)) {
            case HOPMirIntKind_U8:   return MirInferredType_SLICE_U8;
            case HOPMirIntKind_I8:   return MirInferredType_SLICE_I8;
            case HOPMirIntKind_U16:  return MirInferredType_SLICE_U16;
            case HOPMirIntKind_I16:  return MirInferredType_SLICE_I16;
            case HOPMirIntKind_U32:  return MirInferredType_SLICE_U32;
            case HOPMirIntKind_BOOL:
            case HOPMirIntKind_I32:  return MirInferredType_SLICE_I32;
            default:                 return MirInferredType_NONE;
        }
    }
    if ((flags & HOPMirTypeFlag_AGG_SLICE_VIEW) != 0) {
        return MirInferredType_SLICE_AGG;
    }
    if ((flags & HOPMirTypeFlag_AGGREGATE) != 0) {
        return MirInferredType_AGG;
    }
    switch ((HOPMirTypeScalar)(flags & HOPMirTypeFlag_SCALAR_MASK)) {
        case HOPMirTypeScalar_I32: return MirInferredType_I32;
        case HOPMirTypeScalar_I64: return MirInferredType_I64;
        case HOPMirTypeScalar_F32: return MirInferredType_F32;
        case HOPMirTypeScalar_F64: return MirInferredType_F64;
        default:                   return MirInferredType_NONE;
    }
}

static MirInferredType MirProgramTypeKind(const HOPMirProgram* program, uint32_t typeRefIndex) {
    if (program == NULL || typeRefIndex == UINT32_MAX || typeRefIndex >= program->typeLen) {
        return MirInferredType_NONE;
    }
    return MirInferredTypeFromTypeRef(&program->types[typeRefIndex]);
}

static MirInferredType MirConstTypeKind(const HOPMirConst* value) {
    if (value == NULL) {
        return MirInferredType_NONE;
    }
    switch (value->kind) {
        case HOPMirConst_INT:
        case HOPMirConst_BOOL:
        case HOPMirConst_NULL:     return MirInferredType_I32;
        case HOPMirConst_STRING:   return MirInferredType_STR_REF;
        case HOPMirConst_FUNCTION: return MirInferredType_FUNC_REF;
        default:                   return MirInferredType_NONE;
    }
}

static MirInferredType MirFunctionResultTypeKind(
    const HOPMirProgram* program, uint32_t functionIndex) {
    if (program == NULL || functionIndex >= program->funcLen) {
        return MirInferredType_NONE;
    }
    return MirProgramTypeKind(program, program->funcs[functionIndex].typeRef);
}

static int EnsureMirStrRefTypeRef(
    HOPArena* arena, HOPMirProgram* program, uint32_t* _Nonnull outTypeRef) {
    uint32_t       i;
    HOPMirTypeRef* newTypes;
    if (arena == NULL || program == NULL || outTypeRef == NULL) {
        return -1;
    }
    for (i = 0; i < program->typeLen; i++) {
        if (HOPMirTypeRefIsStrRef(&program->types[i])) {
            *outTypeRef = i;
            return 0;
        }
    }
    newTypes = (HOPMirTypeRef*)HOPArenaAlloc(
        arena, sizeof(HOPMirTypeRef) * (program->typeLen + 1u), (uint32_t)_Alignof(HOPMirTypeRef));
    if (newTypes == NULL) {
        return -1;
    }
    if (program->typeLen != 0u) {
        memcpy(newTypes, program->types, sizeof(HOPMirTypeRef) * program->typeLen);
    }
    newTypes[program->typeLen] = (HOPMirTypeRef){
        .astNode = UINT32_MAX,
        .sourceRef = 0,
        .flags = HOPMirTypeFlag_STR_REF,
        .aux = 0,
    };
    program->types = newTypes;
    *outTypeRef = program->typeLen++;
    return 0;
}

static int EnsureMirFlaggedTypeRef(
    HOPArena* arena, HOPMirProgram* program, uint32_t flags, uint32_t* _Nonnull outTypeRef) {
    uint32_t       i;
    HOPMirTypeRef* newTypes;
    if (arena == NULL || program == NULL || outTypeRef == NULL || flags == 0u) {
        return -1;
    }
    for (i = 0; i < program->typeLen; i++) {
        if (program->types[i].flags == flags) {
            *outTypeRef = i;
            return 0;
        }
    }
    newTypes = (HOPMirTypeRef*)HOPArenaAlloc(
        arena, sizeof(HOPMirTypeRef) * (program->typeLen + 1u), (uint32_t)_Alignof(HOPMirTypeRef));
    if (newTypes == NULL) {
        return -1;
    }
    if (program->typeLen != 0u) {
        memcpy(newTypes, program->types, sizeof(HOPMirTypeRef) * program->typeLen);
    }
    newTypes[program->typeLen] =
        (HOPMirTypeRef){ .astNode = UINT32_MAX, .sourceRef = 0, .flags = flags, .aux = 0 };
    program->types = newTypes;
    *outTypeRef = program->typeLen++;
    return 0;
}

static int EnsureMirFunctionRefTypeRef(
    HOPArena*      arena,
    HOPMirProgram* program,
    uint32_t       functionIndex,
    uint32_t* _Nonnull outTypeRef) {
    uint32_t       i;
    HOPMirTypeRef* newTypes;
    uint32_t       aux;
    if (arena == NULL || program == NULL || outTypeRef == NULL || functionIndex >= program->funcLen)
    {
        return -1;
    }
    aux = functionIndex + 1u;
    for (i = 0; i < program->typeLen; i++) {
        if ((program->types[i].flags & HOPMirTypeFlag_FUNC_REF) != 0u
            && program->types[i].aux == aux)
        {
            *outTypeRef = i;
            return 0;
        }
    }
    newTypes = (HOPMirTypeRef*)HOPArenaAlloc(
        arena, sizeof(HOPMirTypeRef) * (program->typeLen + 1u), (uint32_t)_Alignof(HOPMirTypeRef));
    if (newTypes == NULL) {
        return -1;
    }
    if (program->typeLen != 0u) {
        memcpy(newTypes, program->types, sizeof(HOPMirTypeRef) * program->typeLen);
    }
    newTypes[program->typeLen] = (HOPMirTypeRef){
        .astNode = UINT32_MAX,
        .sourceRef = 0,
        .flags = HOPMirTypeFlag_FUNC_REF,
        .aux = aux,
    };
    program->types = newTypes;
    *outTypeRef = program->typeLen++;
    return 0;
}

static int EnsureMirInferredTypeRef(
    HOPArena* arena, HOPMirProgram* program, MirInferredType type, uint32_t* _Nonnull outTypeRef) {
    switch (type) {
        case MirInferredType_I32:
            return EnsureMirScalarTypeRef(arena, program, HOPMirTypeScalar_I32, outTypeRef);
        case MirInferredType_I64:
            return EnsureMirScalarTypeRef(arena, program, HOPMirTypeScalar_I64, outTypeRef);
        case MirInferredType_F32:
            return EnsureMirScalarTypeRef(arena, program, HOPMirTypeScalar_F32, outTypeRef);
        case MirInferredType_F64:
            return EnsureMirScalarTypeRef(arena, program, HOPMirTypeScalar_F64, outTypeRef);
        case MirInferredType_STR_REF: return EnsureMirStrRefTypeRef(arena, program, outTypeRef);
        case MirInferredType_STR_PTR:
            return EnsureMirFlaggedTypeRef(arena, program, HOPMirTypeFlag_STR_PTR, outTypeRef);
        case MirInferredType_U8_PTR:
            return EnsureMirFlaggedTypeRef(arena, program, HOPMirTypeFlag_U8_PTR, outTypeRef);
        case MirInferredType_I8_PTR:
            return EnsureMirFlaggedTypeRef(arena, program, HOPMirTypeFlag_I8_PTR, outTypeRef);
        case MirInferredType_U16_PTR:
            return EnsureMirFlaggedTypeRef(arena, program, HOPMirTypeFlag_U16_PTR, outTypeRef);
        case MirInferredType_I16_PTR:
            return EnsureMirFlaggedTypeRef(arena, program, HOPMirTypeFlag_I16_PTR, outTypeRef);
        case MirInferredType_U32_PTR:
            return EnsureMirFlaggedTypeRef(arena, program, HOPMirTypeFlag_U32_PTR, outTypeRef);
        case MirInferredType_I32_PTR:
            return EnsureMirFlaggedTypeRef(arena, program, HOPMirTypeFlag_I32_PTR, outTypeRef);
        case MirInferredType_OPAQUE_PTR:
            return EnsureMirFlaggedTypeRef(arena, program, HOPMirTypeFlag_OPAQUE_PTR, outTypeRef);
        case MirInferredType_FUNC_REF:
            return EnsureMirFlaggedTypeRef(arena, program, HOPMirTypeFlag_FUNC_REF, outTypeRef);
        default: return -1;
    }
}

static bool MirCanStoreInferredType(MirInferredType dstType, MirInferredType srcType) {
    if (dstType == MirInferredType_NONE || dstType == srcType) {
        return true;
    }
    if (dstType == MirInferredType_STR_PTR && srcType == MirInferredType_STR_REF) {
        return true;
    }
    return false;
}

static void RewriteMirAggregateMake(HOPMirProgram* program) {
    uint32_t funcIndex;
    if (program == NULL) {
        return;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const HOPMirFunction* fn = &program->funcs[funcIndex];
        uint32_t              pc;
        for (pc = 0; pc < fn->instLen; pc++) {
            HOPMirInst* inst = (HOPMirInst*)&program->insts[fn->instStart + pc];
            uint32_t    typeRef = UINT32_MAX;
            uint32_t    scanPc;
            int32_t     depth = 1;
            if (inst->op == HOPMirOp_AGG_ZERO && fn->typeRef < program->typeLen
                && HOPMirTypeRefIsAggregate(&program->types[fn->typeRef]))
            {
                for (scanPc = pc + 1u; scanPc < fn->instLen; scanPc++) {
                    HOPMirInst* next = (HOPMirInst*)&program->insts[fn->instStart + scanPc];
                    if (next->op == HOPMirOp_LOCAL_STORE) {
                        break;
                    }
                    if (next->op == HOPMirOp_COERCE && next->aux == inst->aux) {
                        next->aux = fn->typeRef;
                        continue;
                    }
                    if (next->op == HOPMirOp_RETURN) {
                        inst->aux = fn->typeRef;
                        break;
                    }
                }
            }
            if (inst->op != HOPMirOp_AGG_MAKE) {
                continue;
            }
            for (scanPc = pc + 1u; scanPc < fn->instLen && depth > 0; scanPc++) {
                const HOPMirInst* next = &program->insts[fn->instStart + scanPc];
                int32_t           delta = 0;
                if (depth == 1) {
                    if (next->op == HOPMirOp_COERCE && next->aux < program->typeLen
                        && HOPMirTypeRefIsAggregate(&program->types[next->aux]))
                    {
                        typeRef = next->aux;
                        break;
                    }
                    if (next->op == HOPMirOp_RETURN && fn->typeRef < program->typeLen
                        && HOPMirTypeRefIsAggregate(&program->types[fn->typeRef]))
                    {
                        typeRef = fn->typeRef;
                        break;
                    }
                    if (next->op == HOPMirOp_LOCAL_STORE && next->aux < fn->localCount) {
                        uint32_t localTypeRef = program->locals[fn->localStart + next->aux].typeRef;
                        if (localTypeRef < program->typeLen
                            && HOPMirTypeRefIsAggregate(&program->types[localTypeRef]))
                        {
                            typeRef = localTypeRef;
                            break;
                        }
                    }
                }
                if (!MirExprInstStackDelta(next, &delta)) {
                    break;
                }
                if (delta < 0 && (uint32_t)(-delta) > (uint32_t)depth) {
                    break;
                }
                depth += delta;
            }
            if (typeRef != UINT32_MAX) {
                inst->op = HOPMirOp_AGG_ZERO;
                inst->tok = 0u;
                inst->aux = typeRef;
            }
        }
    }
}

static void InferMirStraightLineLocalTypes(HOPArena* arena, HOPMirProgram* program) {
    uint32_t funcIndex;
    if (arena == NULL || program == NULL) {
        return;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const HOPMirFunction* fn = &program->funcs[funcIndex];
        MirInferredType       localTypes[256] = { 0 };
        MirInferredType       stackTypes[512] = { 0 };
        uint32_t              localTypeRefs[256];
        uint32_t              stackTypeRefs[512];
        uint32_t              stackLen = 0;
        uint32_t              localIndex;
        uint32_t              pc;
        int                   supported = 1;
        if (fn->localCount > 256u) {
            continue;
        }
        for (localIndex = 0; localIndex < 256u; localIndex++) {
            localTypeRefs[localIndex] = UINT32_MAX;
        }
        for (localIndex = 0; localIndex < 512u; localIndex++) {
            stackTypeRefs[localIndex] = UINT32_MAX;
        }
        for (localIndex = 0; localIndex < fn->localCount; localIndex++) {
            const HOPMirLocal* local = &program->locals[fn->localStart + localIndex];
            localTypes[localIndex] = MirProgramTypeKind(program, local->typeRef);
            localTypeRefs[localIndex] = local->typeRef;
        }
        for (pc = 0; pc < fn->instLen && supported; pc++) {
            const HOPMirInst* inst = &program->insts[fn->instStart + pc];
            switch (inst->op) {
                case HOPMirOp_PUSH_CONST:
                    if (inst->aux >= program->constLen || stackLen >= 512u) {
                        supported = 0;
                        break;
                    }
                    stackTypes[stackLen] = MirConstTypeKind(&program->consts[inst->aux]);
                    stackTypeRefs[stackLen] = UINT32_MAX;
                    if (program->consts[inst->aux].kind == HOPMirConst_FUNCTION
                        && EnsureMirFunctionRefTypeRef(
                               arena,
                               program,
                               program->consts[inst->aux].aux,
                               &stackTypeRefs[stackLen])
                               != 0)
                    {
                        supported = 0;
                        break;
                    }
                    stackLen++;
                    break;
                case HOPMirOp_AGG_ZERO:
                    if (inst->aux >= program->typeLen || stackLen >= 512u) {
                        supported = 0;
                        break;
                    }
                    stackTypes[stackLen] = MirInferredType_AGG;
                    stackTypeRefs[stackLen++] = inst->aux;
                    break;
                case HOPMirOp_LOCAL_ZERO:
                    if (inst->aux >= fn->localCount) {
                        supported = 0;
                    }
                    break;
                case HOPMirOp_CTX_GET:
                case HOPMirOp_CTX_ADDR:
                    if (stackLen >= 512u) {
                        supported = 0;
                        break;
                    }
                    stackTypes[stackLen] =
                        (inst->aux == HOPMirContextField_ALLOCATOR
                         || inst->aux == HOPMirContextField_TEMP_ALLOCATOR)
                            ? MirInferredType_OPAQUE_PTR
                            : MirInferredType_NONE;
                    stackTypeRefs[stackLen++] = UINT32_MAX;
                    break;
                case HOPMirOp_LOCAL_LOAD:
                    if (inst->aux >= fn->localCount || stackLen >= 512u) {
                        supported = 0;
                        break;
                    }
                    stackTypes[stackLen] = localTypes[inst->aux];
                    stackTypeRefs[stackLen++] = localTypeRefs[inst->aux];
                    break;
                case HOPMirOp_LOCAL_ADDR:
                    if (inst->aux >= fn->localCount || stackLen >= 512u) {
                        supported = 0;
                        break;
                    }
                    stackTypes[stackLen] = localTypes[inst->aux];
                    stackTypeRefs[stackLen++] = localTypeRefs[inst->aux];
                    break;
                case HOPMirOp_LOCAL_STORE: {
                    HOPMirLocal*    local;
                    MirInferredType srcType;
                    uint32_t        typeRef = UINT32_MAX;
                    if (inst->aux >= fn->localCount || stackLen == 0u) {
                        supported = 0;
                        break;
                    }
                    srcType = stackTypes[stackLen - 1u];
                    typeRef = stackTypeRefs[stackLen - 1u];
                    stackLen--;
                    if (srcType == MirInferredType_NONE) {
                        break;
                    }
                    if (srcType == MirInferredType_AGG) {
                        if ((localTypes[inst->aux] != MirInferredType_NONE
                             && localTypes[inst->aux] != MirInferredType_AGG)
                            || (localTypeRefs[inst->aux] != UINT32_MAX && typeRef != UINT32_MAX
                                && localTypeRefs[inst->aux] != typeRef
                                && (localTypeRefs[inst->aux] >= program->typeLen
                                    || typeRef >= program->typeLen
                                    || !HOPMirTypeRefIsAggregate(
                                        &program->types[localTypeRefs[inst->aux]])
                                    || !HOPMirTypeRefIsAggregate(&program->types[typeRef]))))
                        {
                            supported = 0;
                            break;
                        }
                        localTypes[inst->aux] = MirInferredType_AGG;
                        if (typeRef != UINT32_MAX) {
                            localTypeRefs[inst->aux] = typeRef;
                        }
                        local = (HOPMirLocal*)&program->locals[fn->localStart + inst->aux];
                        if (typeRef != UINT32_MAX) {
                            local->typeRef = typeRef;
                        }
                        break;
                    }
                    if (!MirCanStoreInferredType(localTypes[inst->aux], srcType)) {
                        supported = 0;
                        break;
                    }
                    if (localTypes[inst->aux] == MirInferredType_NONE) {
                        localTypes[inst->aux] = srcType;
                    }
                    local = (HOPMirLocal*)&program->locals[fn->localStart + inst->aux];
                    if (srcType == MirInferredType_FUNC_REF && typeRef != UINT32_MAX
                        && local->typeRef < program->typeLen
                        && HOPMirTypeRefIsFuncRef(&program->types[local->typeRef])
                        && HOPMirTypeRefFuncRefFunctionIndex(&program->types[local->typeRef])
                               == UINT32_MAX)
                    {
                        local->typeRef = typeRef;
                        localTypeRefs[inst->aux] = typeRef;
                    }
                    if (local->typeRef == UINT32_MAX) {
                        if (typeRef != UINT32_MAX) {
                            local->typeRef = typeRef;
                            localTypeRefs[inst->aux] = typeRef;
                        } else if (EnsureMirInferredTypeRef(arena, program, srcType, &typeRef) == 0)
                        {
                            local->typeRef = typeRef;
                            localTypeRefs[inst->aux] = typeRef;
                        }
                    }
                    break;
                }
                case HOPMirOp_AGG_SET:
                    if (stackLen < 2u || stackTypes[stackLen - 2u] != MirInferredType_AGG) {
                        supported = 0;
                        break;
                    }
                    stackLen--;
                    break;
                case HOPMirOp_UNARY:
                    if (stackLen == 0u
                        || (stackTypes[stackLen - 1u] != MirInferredType_I32
                            && stackTypes[stackLen - 1u] != MirInferredType_I64
                            && stackTypes[stackLen - 1u] != MirInferredType_F32
                            && stackTypes[stackLen - 1u] != MirInferredType_F64))
                    {
                        supported = 0;
                        break;
                    }
                    break;
                case HOPMirOp_BINARY:
                    if (stackLen < 2u) {
                        supported = 0;
                        break;
                    }
                    {
                        MirInferredType rhsType = stackTypes[stackLen - 1u];
                        MirInferredType lhsType = stackTypes[stackLen - 2u];
                        if (lhsType != rhsType
                            || (lhsType != MirInferredType_I32 && lhsType != MirInferredType_I64
                                && lhsType != MirInferredType_F32 && lhsType != MirInferredType_F64
                                && lhsType != MirInferredType_OPAQUE_PTR))
                        {
                            supported = 0;
                            break;
                        }
                        stackLen--;
                        switch ((HOPTokenKind)inst->tok) {
                            case HOPTok_EQ:
                            case HOPTok_NEQ:
                            case HOPTok_LT:
                            case HOPTok_GT:
                            case HOPTok_LTE:
                            case HOPTok_GTE:
                            case HOPTok_LOGICAL_AND:
                            case HOPTok_LOGICAL_OR:
                                stackTypes[stackLen - 1u] = MirInferredType_I32;
                                stackTypeRefs[stackLen - 1u] = UINT32_MAX;
                                break;
                            default: stackTypes[stackLen - 1u] = lhsType; break;
                        }
                    }
                    break;
                case HOPMirOp_CAST:
                case HOPMirOp_COERCE:
                    if (stackLen == 0u) {
                        supported = 0;
                        break;
                    }
                    stackTypes[stackLen - 1u] = MirProgramTypeKind(program, inst->aux);
                    stackTypeRefs[stackLen - 1u] = inst->aux;
                    break;
                case HOPMirOp_SEQ_LEN:
                    if (stackLen == 0u) {
                        supported = 0;
                        break;
                    }
                    stackTypes[stackLen - 1u] = MirInferredType_I32;
                    stackTypeRefs[stackLen - 1u] = UINT32_MAX;
                    break;
                case HOPMirOp_STR_CSTR:
                    if (stackLen == 0u) {
                        supported = 0;
                        break;
                    }
                    stackTypes[stackLen - 1u] = MirInferredType_U8_PTR;
                    stackTypeRefs[stackLen - 1u] = UINT32_MAX;
                    break;
                case HOPMirOp_CALL_FN: {
                    uint32_t argc = HOPMirCallArgCountFromTok(inst->tok);
                    if (inst->aux >= program->funcLen || stackLen < argc) {
                        supported = 0;
                        break;
                    }
                    stackLen -= argc;
                    {
                        MirInferredType resultType = MirFunctionResultTypeKind(program, inst->aux);
                        if (resultType != MirInferredType_NONE) {
                            if (stackLen >= 512u) {
                                supported = 0;
                                break;
                            }
                            stackTypes[stackLen] = resultType;
                            stackTypeRefs[stackLen++] = program->funcs[inst->aux].typeRef;
                        }
                    }
                    break;
                }
                case HOPMirOp_ARRAY_ADDR:
                    if (stackLen < 2u || stackTypes[stackLen - 1u] != MirInferredType_I32) {
                        supported = 0;
                        break;
                    }
                    stackLen--;
                    switch (stackTypes[stackLen - 1u]) {
                        case MirInferredType_ARRAY_U8:
                            stackTypes[stackLen - 1u] = MirInferredType_U8_PTR;
                            stackTypeRefs[stackLen - 1u] = UINT32_MAX;
                            break;
                        case MirInferredType_ARRAY_I8:
                            stackTypes[stackLen - 1u] = MirInferredType_I8_PTR;
                            stackTypeRefs[stackLen - 1u] = UINT32_MAX;
                            break;
                        case MirInferredType_ARRAY_U16:
                            stackTypes[stackLen - 1u] = MirInferredType_U16_PTR;
                            stackTypeRefs[stackLen - 1u] = UINT32_MAX;
                            break;
                        case MirInferredType_ARRAY_I16:
                            stackTypes[stackLen - 1u] = MirInferredType_I16_PTR;
                            stackTypeRefs[stackLen - 1u] = UINT32_MAX;
                            break;
                        case MirInferredType_ARRAY_U32:
                            stackTypes[stackLen - 1u] = MirInferredType_U32_PTR;
                            stackTypeRefs[stackLen - 1u] = UINT32_MAX;
                            break;
                        case MirInferredType_ARRAY_I32:
                            stackTypes[stackLen - 1u] = MirInferredType_I32_PTR;
                            stackTypeRefs[stackLen - 1u] = UINT32_MAX;
                            break;
                        case MirInferredType_SLICE_AGG:
                            stackTypes[stackLen - 1u] = MirInferredType_OPAQUE_PTR;
                            stackTypeRefs[stackLen - 1u] =
                                stackTypeRefs[stackLen - 1u] < program->typeLen
                                    ? HOPMirTypeRefAggSliceElemTypeRef(
                                          &program->types[stackTypeRefs[stackLen - 1u]])
                                    : UINT32_MAX;
                            if (stackTypeRefs[stackLen - 1u] == UINT32_MAX) {
                                supported = 0;
                            }
                            break;
                        default: supported = 0; break;
                    }
                    break;
                case HOPMirOp_DEREF_LOAD:
                    if (stackLen == 0u) {
                        supported = 0;
                        break;
                    }
                    switch (stackTypes[stackLen - 1u]) {
                        case MirInferredType_STR_PTR:
                            stackTypes[stackLen - 1u] = MirInferredType_STR_REF;
                            stackTypeRefs[stackLen - 1u] = UINT32_MAX;
                            break;
                        case MirInferredType_U8_PTR:
                        case MirInferredType_I8_PTR:
                        case MirInferredType_U16_PTR:
                        case MirInferredType_I16_PTR:
                        case MirInferredType_U32_PTR:
                        case MirInferredType_I32_PTR:
                            stackTypes[stackLen - 1u] = MirInferredType_I32;
                            stackTypeRefs[stackLen - 1u] = UINT32_MAX;
                            break;
                        default: supported = 0; break;
                    }
                    break;
                case HOPMirOp_DEREF_STORE:
                    if (stackLen < 2u) {
                        supported = 0;
                        break;
                    }
                    stackLen -= 2u;
                    break;
                case HOPMirOp_SLICE_MAKE:
                    if (stackLen == 0u) {
                        supported = 0;
                        break;
                    }
                    if ((inst->tok & HOPAstFlag_INDEX_HAS_END) != 0u) {
                        if (stackLen == 0u || stackTypes[stackLen - 1u] != MirInferredType_I32) {
                            supported = 0;
                            break;
                        }
                        stackLen--;
                    }
                    if ((inst->tok & HOPAstFlag_INDEX_HAS_START) != 0u) {
                        if (stackLen == 0u || stackTypes[stackLen - 1u] != MirInferredType_I32) {
                            supported = 0;
                            break;
                        }
                        stackLen--;
                    }
                    if (stackLen == 0u) {
                        supported = 0;
                        break;
                    }
                    switch (stackTypes[stackLen - 1u]) {
                        case MirInferredType_ARRAY_U8:
                        case MirInferredType_SLICE_U8:
                            stackTypes[stackLen - 1u] = MirInferredType_SLICE_U8;
                            stackTypeRefs[stackLen - 1u] = UINT32_MAX;
                            break;
                        case MirInferredType_ARRAY_I8:
                        case MirInferredType_SLICE_I8:
                            stackTypes[stackLen - 1u] = MirInferredType_SLICE_I8;
                            stackTypeRefs[stackLen - 1u] = UINT32_MAX;
                            break;
                        case MirInferredType_ARRAY_U16:
                        case MirInferredType_SLICE_U16:
                            stackTypes[stackLen - 1u] = MirInferredType_SLICE_U16;
                            stackTypeRefs[stackLen - 1u] = UINT32_MAX;
                            break;
                        case MirInferredType_ARRAY_I16:
                        case MirInferredType_SLICE_I16:
                            stackTypes[stackLen - 1u] = MirInferredType_SLICE_I16;
                            stackTypeRefs[stackLen - 1u] = UINT32_MAX;
                            break;
                        case MirInferredType_ARRAY_U32:
                        case MirInferredType_SLICE_U32:
                            stackTypes[stackLen - 1u] = MirInferredType_SLICE_U32;
                            stackTypeRefs[stackLen - 1u] = UINT32_MAX;
                            break;
                        case MirInferredType_ARRAY_I32:
                        case MirInferredType_SLICE_I32:
                            stackTypes[stackLen - 1u] = MirInferredType_SLICE_I32;
                            stackTypeRefs[stackLen - 1u] = UINT32_MAX;
                            break;
                        default: supported = 0; break;
                    }
                    break;
                case HOPMirOp_INDEX:
                    if (stackLen < 2u) {
                        supported = 0;
                        break;
                    }
                    stackLen--;
                    if (stackTypes[stackLen - 1u] == MirInferredType_SLICE_AGG) {
                        stackTypes[stackLen - 1u] = MirInferredType_OPAQUE_PTR;
                        stackTypeRefs[stackLen - 1u] =
                            stackTypeRefs[stackLen - 1u] < program->typeLen
                                ? HOPMirTypeRefAggSliceElemTypeRef(
                                      &program->types[stackTypeRefs[stackLen - 1u]])
                                : UINT32_MAX;
                        if (stackTypeRefs[stackLen - 1u] == UINT32_MAX) {
                            supported = 0;
                        }
                    } else {
                        stackTypes[stackLen - 1u] = MirInferredType_I32;
                        stackTypeRefs[stackLen - 1u] = UINT32_MAX;
                    }
                    break;
                case HOPMirOp_DROP:
                case HOPMirOp_ASSERT:
                case HOPMirOp_RETURN:
                    if (stackLen == 0u) {
                        supported = 0;
                        break;
                    }
                    stackLen--;
                    break;
                case HOPMirOp_RETURN_VOID: break;
                default:                   supported = 0; break;
            }
        }
    }
}

int BuildPackageMirProgram(
    const HOPPackageLoader* loader,
    const HOPPackage*       entryPkg,
    int                     includeSelectedPlatform,
    HOPArena*               arena,
    HOPMirProgram*          outProgram,
    HOPForeignLinkageInfo* _Nullable outForeignLinkage,
    HOPDiag* _Nullable diag) {
    HOPMirProgramBuilder  builder;
    HOPMirResolvedDeclMap declMap = { 0 };
    HOPMirTcFunctionMap   tcFnMap = { 0 };
    uint32_t*             topoOrder = NULL;
    uint32_t              topoOrderLen = 0;
    uint32_t              orderIndex;
    int                   autoIncludeSelectedPlatformPanicOnly = 0;
    if (diag != NULL) {
        *diag = (HOPDiag){ 0 };
    }
    if (outForeignLinkage != NULL) {
        *outForeignLinkage = (HOPForeignLinkageInfo){ 0 };
    }
    if (loader == NULL || entryPkg == NULL || arena == NULL || outProgram == NULL) {
        return -1;
    }
    topoOrder = (uint32_t*)calloc(loader->packageLen, sizeof(uint32_t));
    if (topoOrder == NULL && loader->packageLen != 0u) {
        return -1;
    }
    if (BuildEntryPackageMirOrder(
            loader, entryPkg, includeSelectedPlatform, topoOrder, loader->packageLen, &topoOrderLen)
        != 0)
    {
        free(topoOrder);
        return -1;
    }
    autoIncludeSelectedPlatformPanicOnly =
        includeSelectedPlatform && !PackageUsesPlatformImport(loader)
        && loader->platformTarget != NULL
        && StrEq(loader->platformTarget, HOP_WASM_MIN_PLATFORM_TARGET);
    HOPMirProgramBuilderInit(&builder, arena);
    for (orderIndex = 0; orderIndex < topoOrderLen; orderIndex++) {
        const HOPPackage* pkg = &loader->packages[topoOrder[orderIndex]];
        uint32_t          fileIndex;
        for (fileIndex = 0; fileIndex < pkg->fileLen; fileIndex++) {
            int rc;
            if (autoIncludeSelectedPlatformPanicOnly && loader->selectedPlatformPkg == pkg) {
                rc = AppendMirSelectedPlatformPanicDeclsFromFile(
                    &builder, arena, pkg, &pkg->files[fileIndex], &declMap);
            } else {
                rc = AppendMirDeclsFromFile(
                    loader,
                    entryPkg,
                    &builder,
                    arena,
                    pkg,
                    &pkg->files[fileIndex],
                    &declMap,
                    &tcFnMap);
            }
            if (rc != 0) {
                free(tcFnMap.v);
                free(declMap.v);
                free(topoOrder);
                return -1;
            }
        }
    }
    for (orderIndex = 0; orderIndex < topoOrderLen; orderIndex++) {
        const HOPPackage* pkg = &loader->packages[topoOrder[orderIndex]];
        uint32_t          fileIndex;
        for (fileIndex = 0; fileIndex < pkg->fileLen; fileIndex++) {
            if (AppendMirTemplateInstancesFromFile(
                    loader, &builder, arena, pkg, &pkg->files[fileIndex], &tcFnMap)
                != 0)
            {
                free(tcFnMap.v);
                free(declMap.v);
                free(topoOrder);
                return -1;
            }
        }
    }
    if (loader != NULL && loader->selectedPlatformPkg != NULL && loader->platformTarget != NULL
        && StrEq(loader->platformTarget, HOP_WASM_MIN_PLATFORM_TARGET)
        && !PackageUsesPlatformImport(loader) && BuilderHasHostPrintCall(&builder))
    {
        uint32_t fileIndex;
        for (fileIndex = 0; fileIndex < loader->selectedPlatformPkg->fileLen; fileIndex++) {
            if (AppendMirSelectedPlatformConsoleLogDeclsFromFile(
                    &builder,
                    arena,
                    loader->selectedPlatformPkg,
                    &loader->selectedPlatformPkg->files[fileIndex],
                    &declMap)
                != 0)
            {
                free(tcFnMap.v);
                free(declMap.v);
                free(topoOrder);
                return -1;
            }
        }
    }
    HOPMirProgramBuilderFinish(&builder, outProgram);
    EnrichMirTypeFlags(loader, outProgram);
    if (EnrichMirOpaquePtrPointees(loader, arena, outProgram) != 0) {
        free(tcFnMap.v);
        free(declMap.v);
        free(topoOrder);
        return -1;
    }
    if (EnrichMirAggSliceElemTypes(loader, arena, outProgram) != 0) {
        free(tcFnMap.v);
        free(declMap.v);
        free(topoOrder);
        return -1;
    }
    if (EnrichMirAggregateFields(loader, arena, outProgram) != 0) {
        free(tcFnMap.v);
        free(declMap.v);
        free(topoOrder);
        return -1;
    }
    if (EnrichMirVArrayCountFields(loader, outProgram) != 0) {
        free(tcFnMap.v);
        free(declMap.v);
        free(topoOrder);
        return -1;
    }
    if (ResolvePackageMirProgram(loader, &declMap, &tcFnMap, arena, outProgram, outProgram) != 0) {
        free(tcFnMap.v);
        free(declMap.v);
        free(topoOrder);
        return -1;
    }
    if (RewriteMirWasmPrintHostcalls(loader, &declMap, arena, outProgram) != 0) {
        free(tcFnMap.v);
        free(declMap.v);
        free(topoOrder);
        return -1;
    }
    if (RewriteMirFuncFieldCalls(loader, arena, outProgram) != 0) {
        free(tcFnMap.v);
        free(declMap.v);
        free(topoOrder);
        return -1;
    }
    if (RewriteMirVarSizeAllocCounts(loader, arena, outProgram) != 0) {
        free(tcFnMap.v);
        free(declMap.v);
        free(topoOrder);
        return -1;
    }
    if (RewriteMirDynamicSliceAllocCounts(loader, arena, outProgram) != 0) {
        free(tcFnMap.v);
        free(declMap.v);
        free(topoOrder);
        return -1;
    }
    if (RewriteMirAllocNewAllocExprs(loader, arena, outProgram) != 0) {
        free(tcFnMap.v);
        free(declMap.v);
        free(topoOrder);
        return -1;
    }
    if (RewriteMirAllocNewInitExprs(loader, arena, outProgram) != 0) {
        free(tcFnMap.v);
        free(declMap.v);
        free(topoOrder);
        return -1;
    }
    RewriteMirAggregateMake(outProgram);
    InferMirStraightLineLocalTypes(arena, outProgram);
    SpecializeMirDirectFunctionFieldStores(arena, outProgram);
    EnrichMirFunctionRefRepresentatives(loader, outProgram);
    if (outForeignLinkage != NULL
        && BuildMirForeignLinkageInfo(loader, &declMap, outForeignLinkage, diag) != 0)
    {
        free(tcFnMap.v);
        free(declMap.v);
        free(topoOrder);
        if (diag != NULL && diag->code != HOPDiag_NONE) {
            return -1;
        }
        return ErrorSimple("out of memory");
    }
    free(tcFnMap.v);
    free(declMap.v);
    free(topoOrder);
    return HOPMirValidateProgram(outProgram, diag);
}

int DumpMIR(
    const char* entryPath,
    const char* _Nullable platformTarget,
    const char* _Nullable archTarget,
    int testingBuild) {
    uint8_t          arenaStorage[4096];
    HOPArena         arena;
    HOPPackageLoader loader = { 0 };
    HOPPackage*      entryPkg = NULL;
    HOPMirProgram    program = { 0 };
    HOPDiag          diag = { 0 };
    HOPWriter        writer;
    HOPStrView       fallbackSrc = { 0 };
    int              includeSelectedPlatform = 0;

    HOPArenaInit(&arena, arenaStorage, sizeof(arenaStorage));
    HOPArenaSetAllocator(&arena, NULL, CodegenArenaGrow, CodegenArenaFree);

    if (LoadAndCheckPackage(entryPath, platformTarget, archTarget, testingBuild, &loader, &entryPkg)
        != 0)
    {
        HOPArenaDispose(&arena);
        return -1;
    }
    includeSelectedPlatform =
        PackageUsesPlatformImport(&loader)
        || (platformTarget != NULL && StrEq(platformTarget, HOP_PLAYBIT_PLATFORM_TARGET));
    if (BuildPackageMirProgram(
            &loader, entryPkg, includeSelectedPlatform, &arena, &program, NULL, &diag)
        != 0)
    {
        if (diag.code != HOPDiag_NONE && entryPkg->fileLen == 1
            && entryPkg->files[0].source != NULL)
        {
            (void)PrintHOPDiagLineCol(entryPkg->files[0].path, entryPkg->files[0].source, &diag, 0);
        } else if (diag.code != HOPDiag_NONE) {
            (void)ErrorSimple("invalid MIR program");
        }
        FreeLoader(&loader);
        HOPArenaDispose(&arena);
        return -1;
    }

    if (entryPkg->fileLen == 1 && entryPkg->files[0].source != NULL) {
        fallbackSrc.ptr = entryPkg->files[0].source;
        fallbackSrc.len = entryPkg->files[0].sourceLen;
    }

    writer.ctx = NULL;
    writer.write = StdoutWrite;
    if (HOPMirDumpProgram(&program, fallbackSrc, &writer, &diag) != 0) {
        FreeLoader(&loader);
        HOPArenaDispose(&arena);
        return -1;
    }

    FreeLoader(&loader);
    HOPArenaDispose(&arena);
    return 0;
}

HOP_API_END
