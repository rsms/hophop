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
#include "libsl-impl.h"
#include "mir.h"
#include "mir_lower_pkg.h"
#include "mir_lower_stmt.h"
#include "slc_internal.h"
#include "typecheck/internal.h"

SL_API_BEGIN

static int EnsureMirFunctionRefTypeRef(
    SLArena* arena, SLMirProgram* program, uint32_t functionIndex, uint32_t* _Nonnull outTypeRef);

static int FindFunctionBodyNode(const SLParsedFile* file, int32_t fnNode) {
    int32_t child = ASTFirstChild(&file->ast, fnNode);
    while (child >= 0) {
        if (file->ast.nodes[child].kind == SLAst_BLOCK) {
            return child;
        }
        child = ASTNextSibling(&file->ast, child);
    }
    return -1;
}

static int ErrorMirUnsupported(
    const SLParsedFile* file,
    const SLAstNode*    node,
    const char*         kind,
    const SLDiag* _Nullable diag) {
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
    SLMirDeclKind_NONE = 0,
    SLMirDeclKind_FN,
    SLMirDeclKind_CONST,
    SLMirDeclKind_VAR,
} SLMirDeclKind;

typedef struct {
    const SLPackage* pkg;
    const char*      src;
    uint32_t         nameStart;
    uint32_t         nameEnd;
    uint32_t         functionIndex;
    uint8_t          kind;
} SLMirResolvedDecl;

typedef struct {
    SLMirResolvedDecl* _Nullable v;
    uint32_t len;
    uint32_t cap;
} SLMirResolvedDeclMap;

typedef struct {
    const SLPackage*    pkg;
    const SLParsedFile* file;
    uint32_t            tcFnIndex;
    uint32_t            mirFnIndex;
} SLMirTcFunctionDecl;

typedef struct {
    SLMirTcFunctionDecl* _Nullable v;
    uint32_t len;
    uint32_t cap;
} SLMirTcFunctionMap;

static const SLSymbolDecl* _Nullable FindPackageTypeDeclBySlice(
    const SLPackage* pkg, const char* src, uint32_t start, uint32_t end);
static int PatchMirFunctionTypeRefsFromTC(
    SLArena*               arena,
    const SLPackageLoader* loader,
    SLMirProgramBuilder*   builder,
    const SLPackage*       pkg,
    const SLParsedFile*    file,
    const SLTypeCheckCtx*  tc,
    uint32_t               tcFnIndex,
    uint32_t               mirFnIndex);

static int AddMirResolvedDecl(
    SLMirResolvedDeclMap* map,
    const SLPackage*      pkg,
    const char*           src,
    uint32_t              nameStart,
    uint32_t              nameEnd,
    uint32_t              functionIndex,
    SLMirDeclKind         kind) {
    if (map == NULL || pkg == NULL || src == NULL || nameEnd <= nameStart
        || kind == SLMirDeclKind_NONE)
    {
        return -1;
    }
    if (EnsureCap((void**)&map->v, &map->cap, map->len + 1u, sizeof(SLMirResolvedDecl)) != 0) {
        return -1;
    }
    map->v[map->len++] = (SLMirResolvedDecl){
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
    SLMirTcFunctionMap* map,
    const SLPackage*    pkg,
    const SLParsedFile* file,
    uint32_t            tcFnIndex,
    uint32_t            mirFnIndex) {
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
    if (EnsureCap((void**)&map->v, &map->cap, map->len + 1u, sizeof(SLMirTcFunctionDecl)) != 0) {
        return -1;
    }
    map->v[map->len++] = (SLMirTcFunctionDecl){
        .pkg = pkg,
        .file = file,
        .tcFnIndex = tcFnIndex,
        .mirFnIndex = mirFnIndex,
    };
    return 0;
}

static int FindMirTcFunctionDecl(
    const SLMirTcFunctionMap* map,
    const SLPackage*          pkg,
    const SLParsedFile*       file,
    uint32_t                  tcFnIndex,
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
    const SLMirTcFunctionMap* map,
    const SLPackage*          pkg,
    const SLParsedFile*       file,
    uint32_t                  mirFnIndex,
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
    const SLTypeCheckCtx* tc, int32_t declNode, int wantTemplateInstance) {
    uint32_t i;
    if (tc == NULL || declNode < 0) {
        return -1;
    }
    for (i = 0; i < tc->funcLen; i++) {
        const SLTCFunction* fn = &tc->funcs[i];
        int isInstance = (fn->flags & SLTCFunctionFlag_TEMPLATE_INSTANCE) != 0 ? 1 : 0;
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
    const SLTypeCheckCtx* tc, uint32_t tcFnIndex, const char* src, uint32_t start, uint32_t end) {
    const SLTCFunction* fn;
    int32_t             declNode;
    int32_t             child;
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
        const SLAstNode* n = &tc->ast->nodes[child];
        if (n->kind == SLAst_TYPE_PARAM
            && SLNameEqSlice(tc->src, n->dataStart, n->dataEnd, start, end))
        {
            return 1;
        }
        child = n->nextSibling;
    }
    return 0;
}

static int MirNameIsTypeValue(
    const SLPackage*      pkg,
    const SLTypeCheckCtx* tc,
    uint32_t              tcFnIndex,
    const char*           src,
    uint32_t              start,
    uint32_t              end) {
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
    const SLTypeCheckCtx* tc, int32_t typeId, SLMirTypeRef* outTypeRef) {
    const SLTCType* t;
    if (tc == NULL || outTypeRef == NULL || typeId < 0 || (uint32_t)typeId >= tc->typeLen) {
        return 0;
    }
    t = &tc->types[typeId];
    if (t->kind != SLTCType_BUILTIN) {
        return 0;
    }
    *outTypeRef = (SLMirTypeRef){ .astNode = UINT32_MAX, .sourceRef = UINT32_MAX };
    switch (t->builtin) {
        case SLBuiltin_BOOL:
            outTypeRef->flags = SLMirTypeScalar_I32;
            outTypeRef->aux = SLMirTypeAuxMakeScalarInt(SLMirIntKind_BOOL);
            return 1;
        case SLBuiltin_U8:
            outTypeRef->flags = SLMirTypeScalar_I32;
            outTypeRef->aux = SLMirTypeAuxMakeScalarInt(SLMirIntKind_U8);
            return 1;
        case SLBuiltin_I8:
            outTypeRef->flags = SLMirTypeScalar_I32;
            outTypeRef->aux = SLMirTypeAuxMakeScalarInt(SLMirIntKind_I8);
            return 1;
        case SLBuiltin_U16:
            outTypeRef->flags = SLMirTypeScalar_I32;
            outTypeRef->aux = SLMirTypeAuxMakeScalarInt(SLMirIntKind_U16);
            return 1;
        case SLBuiltin_I16:
            outTypeRef->flags = SLMirTypeScalar_I32;
            outTypeRef->aux = SLMirTypeAuxMakeScalarInt(SLMirIntKind_I16);
            return 1;
        case SLBuiltin_U32:
            outTypeRef->flags = SLMirTypeScalar_I32;
            outTypeRef->aux = SLMirTypeAuxMakeScalarInt(SLMirIntKind_U32);
            return 1;
        case SLBuiltin_I32:
        case SLBuiltin_USIZE:
        case SLBuiltin_ISIZE:
            outTypeRef->flags = SLMirTypeScalar_I32;
            outTypeRef->aux = SLMirTypeAuxMakeScalarInt(SLMirIntKind_I32);
            return 1;
        case SLBuiltin_U64:
        case SLBuiltin_I64:    outTypeRef->flags = SLMirTypeScalar_I64; return 1;
        case SLBuiltin_F32:    outTypeRef->flags = SLMirTypeScalar_F32; return 1;
        case SLBuiltin_F64:    outTypeRef->flags = SLMirTypeScalar_F64; return 1;
        case SLBuiltin_TYPE:
        case SLBuiltin_RAWPTR: outTypeRef->flags = SLMirTypeFlag_OPAQUE_PTR; return 1;
        default:               return 0;
    }
}

static int MirAstNodeIsTypeNodeForSearch(SLAstKind kind) {
    return kind == SLAst_TYPE_NAME || kind == SLAst_TYPE_PTR || kind == SLAst_TYPE_REF
        || kind == SLAst_TYPE_MUTREF || kind == SLAst_TYPE_ARRAY || kind == SLAst_TYPE_VARRAY
        || kind == SLAst_TYPE_SLICE || kind == SLAst_TYPE_MUTSLICE || kind == SLAst_TYPE_OPTIONAL
        || kind == SLAst_TYPE_FN || kind == SLAst_TYPE_TUPLE || kind == SLAst_TYPE_ANON_STRUCT
        || kind == SLAst_TYPE_ANON_UNION;
}

static int32_t MirFunctionTypeParamIndexByTypeNode(
    const SLAst* ast, SLStrView src, int32_t fnNode, int32_t typeNode) {
    int32_t child;
    int32_t index = 0;
    if (ast == NULL || fnNode < 0 || typeNode < 0 || (uint32_t)fnNode >= ast->len
        || (uint32_t)typeNode >= ast->len || ast->nodes[typeNode].kind != SLAst_TYPE_NAME
        || ast->nodes[typeNode].firstChild >= 0)
    {
        return -1;
    }
    child = ast->nodes[fnNode].firstChild;
    while (child >= 0) {
        if (ast->nodes[child].kind == SLAst_TYPE_PARAM) {
            if (SLNameEqSlice(
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
    SLMirProgramBuilder*  builder,
    const SLTypeCheckCtx* tc,
    uint32_t              sourceRef,
    int32_t               typeId,
    uint32_t*             outTypeRef) {
    uint32_t     i;
    SLMirTypeRef typeRef = { 0 };
    if (outTypeRef != NULL) {
        *outTypeRef = UINT32_MAX;
    }
    if (builder == NULL || tc == NULL || outTypeRef == NULL || typeId < 0) {
        return 0;
    }
    typeId = SLTCResolveAliasBaseType((SLTypeCheckCtx*)tc, typeId);
    if (typeId < 0 || (uint32_t)typeId >= tc->typeLen) {
        return 0;
    }
    if (MirBuiltinTypeRefFromTC(tc, typeId, &typeRef)) {
        return SLMirProgramBuilderAddType(builder, &typeRef, outTypeRef) == 0 ? 1 : -1;
    }
    for (i = 0; i < tc->ast->len; i++) {
        int32_t resolved = -1;
        if (!MirAstNodeIsTypeNodeForSearch(tc->ast->nodes[i].kind)) {
            continue;
        }
        if (SLTCResolveTypeNode((SLTypeCheckCtx*)tc, (int32_t)i, &resolved) == 0
            && resolved == typeId)
        {
            typeRef = (SLMirTypeRef){ .astNode = i, .sourceRef = sourceRef };
            return SLMirProgramBuilderAddType(builder, &typeRef, outTypeRef) == 0 ? 1 : -1;
        }
    }
    return 0;
}

static int EnsureMirBuilderBuiltinTypeRefFromTC(
    SLMirProgramBuilder* builder, const SLTypeCheckCtx* tc, int32_t typeId, uint32_t* outTypeRef) {
    SLMirTypeRef typeRef = { 0 };
    if (outTypeRef != NULL) {
        *outTypeRef = UINT32_MAX;
    }
    if (builder == NULL || tc == NULL || outTypeRef == NULL || typeId < 0) {
        return 0;
    }
    typeId = SLTCResolveAliasBaseType((SLTypeCheckCtx*)tc, typeId);
    if (typeId < 0 || (uint32_t)typeId >= tc->typeLen) {
        return 0;
    }
    if (!MirBuiltinTypeRefFromTC(tc, typeId, &typeRef)) {
        return 0;
    }
    return SLMirProgramBuilderAddType(builder, &typeRef, outTypeRef) == 0 ? 1 : -1;
}

static int32_t MirFindTCNamedTypeIndex(const SLTypeCheckCtx* tc, int32_t typeId) {
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

static int MirBuiltinTypeNodeMatchesTC(const SLTypeCheckCtx* tc, int32_t typeId, int32_t nodeId) {
    const SLAstNode* n;
    if (tc == NULL || typeId < 0 || (uint32_t)typeId >= tc->typeLen || nodeId < 0
        || (uint32_t)nodeId >= tc->ast->len || tc->types[typeId].kind != SLTCType_BUILTIN)
    {
        return 0;
    }
    n = &tc->ast->nodes[nodeId];
    if (n->kind != SLAst_TYPE_NAME || n->firstChild >= 0) {
        return 0;
    }
    switch (tc->types[typeId].builtin) {
        case SLBuiltin_BOOL: return SliceEqCStr(tc->src.ptr, n->dataStart, n->dataEnd, "bool");
        case SLBuiltin_U8:   return SliceEqCStr(tc->src.ptr, n->dataStart, n->dataEnd, "u8");
        case SLBuiltin_U16:  return SliceEqCStr(tc->src.ptr, n->dataStart, n->dataEnd, "u16");
        case SLBuiltin_U32:  return SliceEqCStr(tc->src.ptr, n->dataStart, n->dataEnd, "u32");
        case SLBuiltin_U64:  return SliceEqCStr(tc->src.ptr, n->dataStart, n->dataEnd, "u64");
        case SLBuiltin_I8:   return SliceEqCStr(tc->src.ptr, n->dataStart, n->dataEnd, "i8");
        case SLBuiltin_I16:  return SliceEqCStr(tc->src.ptr, n->dataStart, n->dataEnd, "i16");
        case SLBuiltin_I32:
            return SliceEqCStr(tc->src.ptr, n->dataStart, n->dataEnd, "i32")
                || SliceEqCStr(tc->src.ptr, n->dataStart, n->dataEnd, "int");
        case SLBuiltin_I64:    return SliceEqCStr(tc->src.ptr, n->dataStart, n->dataEnd, "i64");
        case SLBuiltin_USIZE:  return SliceEqCStr(tc->src.ptr, n->dataStart, n->dataEnd, "usize");
        case SLBuiltin_ISIZE:  return SliceEqCStr(tc->src.ptr, n->dataStart, n->dataEnd, "isize");
        case SLBuiltin_TYPE:   return SliceEqCStr(tc->src.ptr, n->dataStart, n->dataEnd, "type");
        case SLBuiltin_RAWPTR: return SliceEqCStr(tc->src.ptr, n->dataStart, n->dataEnd, "rawptr");
        case SLBuiltin_F32:    return SliceEqCStr(tc->src.ptr, n->dataStart, n->dataEnd, "f32");
        case SLBuiltin_F64:    return SliceEqCStr(tc->src.ptr, n->dataStart, n->dataEnd, "f64");
        default:               return 0;
    }
}

static int MirTypeNodeMatchesTCType(const SLTypeCheckCtx* tc, int32_t typeId, int32_t nodeId) {
    int32_t  namedIndex;
    int32_t  child;
    uint16_t argIndex;
    if (tc == NULL || typeId < 0 || (uint32_t)typeId >= tc->typeLen || nodeId < 0
        || (uint32_t)nodeId >= tc->ast->len)
    {
        return 0;
    }
    if (tc->types[typeId].kind == SLTCType_BUILTIN) {
        return MirBuiltinTypeNodeMatchesTC(tc, typeId, nodeId);
    }
    if (tc->types[typeId].kind != SLTCType_NAMED) {
        return 0;
    }
    if (tc->ast->nodes[nodeId].kind != SLAst_TYPE_NAME) {
        return 0;
    }
    if (!SLNameEqSlice(
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
    SLMirProgramBuilder*  builder,
    const SLTypeCheckCtx* tc,
    uint32_t              sourceRef,
    int32_t               typeId,
    uint32_t*             outTypeRef) {
    uint32_t     i;
    SLMirTypeRef typeRef = { 0 };
    if (outTypeRef != NULL) {
        *outTypeRef = UINT32_MAX;
    }
    if (builder == NULL || tc == NULL || outTypeRef == NULL || typeId < 0
        || (uint32_t)typeId >= tc->typeLen || tc->types[typeId].kind != SLTCType_NAMED)
    {
        return 0;
    }
    for (i = 0; i < tc->ast->len; i++) {
        if (!MirAstNodeIsTypeNodeForSearch(tc->ast->nodes[i].kind)) {
            continue;
        }
        if (MirTypeNodeMatchesTCType(tc, typeId, (int32_t)i)) {
            typeRef = (SLMirTypeRef){ .astNode = i, .sourceRef = sourceRef };
            return SLMirProgramBuilderAddType(builder, &typeRef, outTypeRef) == 0 ? 1 : -1;
        }
    }
    return 0;
}

static int PatchMirFunctionTypeRefsFromTC(
    SLArena*               arena,
    const SLPackageLoader* loader,
    SLMirProgramBuilder*   builder,
    const SLPackage*       pkg,
    const SLParsedFile*    file,
    const SLTypeCheckCtx*  tc,
    uint32_t               tcFnIndex,
    uint32_t               mirFnIndex) {
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
        const SLMirTypeRef* retTypeRef = &builder->types[builder->funcs[mirFnIndex].typeRef];
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
                const SLMirTypeRef* localTypeRef =
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

static int32_t FindVarLikeTypeNode(const SLAst* ast, int32_t nodeId) {
    int32_t child;
    if (ast == NULL || nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    child = ast->nodes[nodeId].firstChild;
    if (child >= 0 && ast->nodes[child].kind == SLAst_NAME_LIST) {
        child = ast->nodes[child].nextSibling;
    }
    while (child >= 0) {
        if (ast->nodes[child].kind == SLAst_TYPE_NAME || ast->nodes[child].kind == SLAst_TYPE_PTR
            || ast->nodes[child].kind == SLAst_TYPE_REF
            || ast->nodes[child].kind == SLAst_TYPE_MUTREF
            || ast->nodes[child].kind == SLAst_TYPE_ARRAY
            || ast->nodes[child].kind == SLAst_TYPE_VARRAY
            || ast->nodes[child].kind == SLAst_TYPE_SLICE
            || ast->nodes[child].kind == SLAst_TYPE_MUTSLICE
            || ast->nodes[child].kind == SLAst_TYPE_OPTIONAL
            || ast->nodes[child].kind == SLAst_TYPE_FN
            || ast->nodes[child].kind == SLAst_TYPE_ANON_STRUCT
            || ast->nodes[child].kind == SLAst_TYPE_ANON_UNION
            || ast->nodes[child].kind == SLAst_TYPE_TUPLE)
        {
            return child;
        }
        child = ast->nodes[child].nextSibling;
    }
    return -1;
}

static int32_t FindFnReturnTypeNode(const SLAst* ast, int32_t fnNode) {
    int32_t child;
    if (ast == NULL || fnNode < 0 || (uint32_t)fnNode >= ast->len) {
        return -1;
    }
    child = ast->nodes[fnNode].firstChild;
    while (child >= 0) {
        if (ast->nodes[child].kind == SLAst_TYPE_NAME || ast->nodes[child].kind == SLAst_TYPE_PTR
            || ast->nodes[child].kind == SLAst_TYPE_REF
            || ast->nodes[child].kind == SLAst_TYPE_MUTREF
            || ast->nodes[child].kind == SLAst_TYPE_ARRAY
            || ast->nodes[child].kind == SLAst_TYPE_VARRAY
            || ast->nodes[child].kind == SLAst_TYPE_SLICE
            || ast->nodes[child].kind == SLAst_TYPE_MUTSLICE
            || ast->nodes[child].kind == SLAst_TYPE_OPTIONAL
            || ast->nodes[child].kind == SLAst_TYPE_FN || ast->nodes[child].kind == SLAst_TYPE_TUPLE
            || ast->nodes[child].kind == SLAst_TYPE_ANON_STRUCT
            || ast->nodes[child].kind == SLAst_TYPE_ANON_UNION)
        {
            return child;
        }
        if (ast->nodes[child].kind == SLAst_BLOCK) {
            break;
        }
        child = ast->nodes[child].nextSibling;
    }
    return -1;
}

static int DeclHasDirective(
    const SLParsedFile* file,
    int32_t             nodeId,
    const char*         name,
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
    SLMirProgramBuilder* builder,
    SLArena*             arena,
    const SLParsedFile*  file,
    int32_t              nodeId,
    uint32_t*            outFunctionIndex) {
    const SLAst*     ast = &file->ast;
    const SLAstNode* n = &ast->nodes[nodeId];
    SLMirSourceRef   sourceRef = { .src = { file->source, file->sourceLen } };
    SLMirFunction    fn = { 0 };
    uint32_t         sourceIndex = 0;
    int32_t          typeNode = -1;
    int32_t          child;
    if (builder == NULL || arena == NULL || file == NULL || outFunctionIndex == NULL) {
        return -1;
    }
    *outFunctionIndex = UINT32_MAX;
    if (SLMirProgramBuilderAddSource(builder, &sourceRef, &sourceIndex) != 0) {
        return -1;
    }
    fn.nameStart = n->dataStart;
    fn.nameEnd = n->dataEnd;
    fn.sourceRef = sourceIndex;
    fn.typeRef = UINT32_MAX;
    typeNode =
        n->kind == SLAst_FN ? FindFnReturnTypeNode(ast, nodeId) : FindVarLikeTypeNode(ast, nodeId);
    if (typeNode >= 0) {
        SLMirTypeRef typeRef = {
            .astNode = (uint32_t)typeNode,
            .sourceRef = sourceIndex,
            .flags = 0,
            .aux = 0,
        };
        if (SLMirProgramBuilderAddType(builder, &typeRef, &fn.typeRef) != 0) {
            return -1;
        }
    }
    if (SLMirProgramBuilderBeginFunction(builder, &fn, outFunctionIndex) != 0) {
        return -1;
    }
    if (n->kind == SLAst_FN) {
        child = n->firstChild;
        while (child >= 0) {
            if (ast->nodes[child].kind == SLAst_PARAM) {
                SLMirLocal   local = { 0 };
                SLMirTypeRef typeRef = { 0 };
                int32_t      paramTypeNode = ast->nodes[child].firstChild;
                local.typeRef = UINT32_MAX;
                local.flags = SLMirLocalFlag_PARAM | SLMirLocalFlag_MUTABLE;
                local.nameStart = ast->nodes[child].dataStart;
                local.nameEnd = ast->nodes[child].dataEnd;
                if (paramTypeNode >= 0) {
                    typeRef.astNode = (uint32_t)paramTypeNode;
                    typeRef.sourceRef = sourceIndex;
                    typeRef.flags = 0;
                    typeRef.aux = 0;
                    if (SLMirProgramBuilderAddType(builder, &typeRef, &local.typeRef) != 0) {
                        return -1;
                    }
                }
                if (SLMirProgramBuilderAddLocal(builder, &local, NULL) != 0) {
                    return -1;
                }
                builder->funcs[*outFunctionIndex].paramCount++;
                if ((ast->nodes[child].flags & SLAstFlag_PARAM_VARIADIC) != 0u) {
                    builder->funcs[*outFunctionIndex].flags |= SLMirFunctionFlag_VARIADIC;
                }
            }
            child = ast->nodes[child].nextSibling;
        }
    }
    return SLMirProgramBuilderEndFunction(builder);
}

static int AppendMirPlaybitEntryHookWrapper(
    SLMirProgramBuilder* builder,
    SLArena*             arena,
    const SLParsedFile*  file,
    int32_t              nodeId,
    uint32_t             entryMainFunctionIndex,
    uint32_t*            outFunctionIndex) {
    const SLAst*     ast;
    const SLAstNode* n;
    SLMirFunction    fn = { 0 };
    SLMirSourceRef   sourceRef = { 0 };
    uint32_t         sourceIndex = 0;
    if (builder == NULL || arena == NULL || file == NULL || outFunctionIndex == NULL || nodeId < 0
        || (uint32_t)nodeId >= file->ast.len)
    {
        return -1;
    }
    *outFunctionIndex = UINT32_MAX;
    ast = &file->ast;
    n = &ast->nodes[nodeId];
    sourceRef.src = (SLStrView){ file->source, file->sourceLen };
    if (SLMirProgramBuilderAddSource(builder, &sourceRef, &sourceIndex) != 0) {
        return -1;
    }
    fn.nameStart = n->dataStart;
    fn.nameEnd = n->dataEnd;
    fn.sourceRef = sourceIndex;
    fn.typeRef = UINT32_MAX;
    if (SLMirProgramBuilderBeginFunction(builder, &fn, outFunctionIndex) != 0) {
        return -1;
    }
    if (SLMirProgramBuilderAppendInst(
            builder,
            &(SLMirInst){
                .op = SLMirOp_CALL_FN,
                .tok = 0u,
                ._reserved = 0u,
                .aux = entryMainFunctionIndex,
                .start = n->start,
                .end = n->end,
            })
            != 0
        || SLMirProgramBuilderAppendInst(
               builder,
               &(SLMirInst){
                   .op = SLMirOp_DROP,
                   .tok = 0u,
                   ._reserved = 0u,
                   .aux = 0u,
                   .start = n->start,
                   .end = n->end,
               })
               != 0
        || SLMirProgramBuilderAppendInst(
               builder,
               &(SLMirInst){
                   .op = SLMirOp_RETURN_VOID,
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
    return SLMirProgramBuilderEndFunction(builder);
}

static const SLMirResolvedDecl* _Nullable FindMirResolvedDeclBySlice(
    const SLMirResolvedDeclMap* map,
    const SLPackage*            pkg,
    const char*                 src,
    uint32_t                    nameStart,
    uint32_t                    nameEnd,
    SLMirDeclKind               kind) {
    uint32_t i;
    if (map == NULL || pkg == NULL || src == NULL || nameEnd <= nameStart) {
        return NULL;
    }
    for (i = 0; i < map->len; i++) {
        const SLMirResolvedDecl* entry = &map->v[i];
        if (entry->pkg != pkg || entry->kind != (uint8_t)kind) {
            continue;
        }
        if (SliceEqSlice(entry->src, entry->nameStart, entry->nameEnd, src, nameStart, nameEnd)) {
            return entry;
        }
    }
    return NULL;
}

static const SLMirResolvedDecl* _Nullable FindMirResolvedDeclByCStr(
    const SLMirResolvedDeclMap* map, const SLPackage* pkg, const char* name, SLMirDeclKind kind) {
    uint32_t i;
    size_t   nameLen;
    if (map == NULL || pkg == NULL || name == NULL) {
        return NULL;
    }
    nameLen = strlen(name);
    for (i = 0; i < map->len; i++) {
        const SLMirResolvedDecl* entry = &map->v[i];
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

static const SLMirResolvedDecl* _Nullable FindMirResolvedValueBySlice(
    const SLMirResolvedDeclMap* map,
    const SLPackage*            pkg,
    const char*                 src,
    uint32_t                    nameStart,
    uint32_t                    nameEnd) {
    const SLMirResolvedDecl* entry = FindMirResolvedDeclBySlice(
        map, pkg, src, nameStart, nameEnd, SLMirDeclKind_CONST);
    if (entry != NULL) {
        return entry;
    }
    return FindMirResolvedDeclBySlice(map, pkg, src, nameStart, nameEnd, SLMirDeclKind_VAR);
}

static const SLMirResolvedDecl* _Nullable FindMirResolvedValueByCStr(
    const SLMirResolvedDeclMap* map, const SLPackage* pkg, const char* name) {
    const SLMirResolvedDecl* entry = FindMirResolvedDeclByCStr(map, pkg, name, SLMirDeclKind_CONST);
    if (entry != NULL) {
        return entry;
    }
    return FindMirResolvedDeclByCStr(map, pkg, name, SLMirDeclKind_VAR);
}

static int MirAstDeclHasTypeParams(const SLAst* ast, int32_t nodeId) {
    int32_t child;
    if (ast == NULL || nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return 0;
    }
    child = ast->nodes[nodeId].firstChild;
    while (child >= 0) {
        if (ast->nodes[child].kind == SLAst_TYPE_PARAM) {
            return 1;
        }
        child = ast->nodes[child].nextSibling;
    }
    return 0;
}

static int AppendMirDeclsFromFile(
    const SLPackageLoader* loader,
    const SLPackage*       entryPkg,
    SLMirProgramBuilder*   builder,
    SLArena*               arena,
    const SLPackage*       pkg,
    const SLParsedFile*    file,
    SLMirResolvedDeclMap* _Nullable declMap,
    SLMirTcFunctionMap* _Nullable tcFnMap) {
    int32_t         child = ASTFirstChild(&file->ast, file->ast.root);
    SLTypeCheckCtx* tc = file->hasTypecheckCtx ? (SLTypeCheckCtx*)file->typecheckCtx : NULL;
    while (child >= 0) {
        const SLAstNode* n = &file->ast.nodes[child];
        if (n->kind == SLAst_FN) {
            uint32_t     outFunctionIndex = UINT32_MAX;
            int32_t      bodyNode;
            int32_t      wasmImportNode = -1;
            SLDiag       diag = { 0 };
            int          supported = 0;
            SLStrView    src = { file->source, file->sourceLen };
            const SLAst* ast = &file->ast;
            if (MirAstDeclHasTypeParams(ast, child)) {
                child = ASTNextSibling(ast, child);
                continue;
            }
            bodyNode = FindFunctionBodyNode(file, child);
            if (bodyNode >= 0) {
                if (SLMirLowerAppendSimpleFunction(
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
                    return PrintSLDiagLineCol(file->path, file->source, &diag, 0);
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
                           SLMirDeclKind_FN)
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
                && StrEq(loader->platformTarget, SL_PLAYBIT_PLATFORM_TARGET)
                && SliceEqCStr(file->source, n->dataStart, n->dataEnd, SL_PLAYBIT_ENTRY_HOOK_NAME))
            {
                const SLMirResolvedDecl* entryMain =
                    declMap != NULL
                        ? FindMirResolvedDeclByCStr(declMap, entryPkg, "main", SLMirDeclKind_FN)
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
                           SLMirDeclKind_FN)
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
                           SLMirDeclKind_FN)
                           != 0)
                {
                    return ErrorSimple("out of memory");
                }
            }
        } else if (n->kind == SLAst_VAR || n->kind == SLAst_CONST) {
            const SLAst* ast = &file->ast;
            const char*  kindName = n->kind == SLAst_CONST ? "top-level const" : "top-level var";
            int32_t      firstChild = ASTFirstChild(ast, child);
            SLStrView    src = { file->source, file->sourceLen };
            SLDiag       diag = { 0 };
            int          supported = 0;
            if (firstChild >= 0 && ast->nodes[firstChild].kind == SLAst_NAME_LIST) {
                uint32_t i;
                uint32_t nameCount = AstListCount(ast, firstChild);
                for (i = 0; i < nameCount; i++) {
                    uint32_t         outFunctionIndex = UINT32_MAX;
                    int32_t          nameNode = AstListItemAt(ast, firstChild, i);
                    const SLAstNode* nameAst =
                        (nameNode >= 0 && (uint32_t)nameNode < ast->len)
                            ? &ast->nodes[nameNode]
                            : NULL;
                    if (nameAst == NULL) {
                        return ErrorMirUnsupported(file, n, kindName, NULL);
                    }
                    diag = (SLDiag){ 0 };
                    supported = 0;
                    if (SLMirLowerAppendNamedVarLikeTopInitFunctionBySlice(
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
                        return PrintSLDiagLineCol(file->path, file->source, &diag, 0);
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
                               n->kind == SLAst_CONST ? SLMirDeclKind_CONST : SLMirDeclKind_VAR)
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
                    if (SLMirLowerAppendNamedVarLikeTopInitFunctionBySlice(
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
                        return PrintSLDiagLineCol(file->path, file->source, &diag, 0);
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
                           n->kind == SLAst_CONST ? SLMirDeclKind_CONST : SLMirDeclKind_VAR)
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
    const SLPackageLoader* loader,
    SLMirProgramBuilder*   builder,
    SLArena*               arena,
    const SLPackage*       pkg,
    const SLParsedFile*    file,
    SLMirTcFunctionMap*    tcFnMap) {
    SLTypeCheckCtx* tc =
        file != NULL && file->hasTypecheckCtx ? (SLTypeCheckCtx*)file->typecheckCtx : NULL;
    uint32_t i;
    if (loader == NULL || builder == NULL || arena == NULL || pkg == NULL || file == NULL
        || tcFnMap == NULL || tc == NULL)
    {
        return 0;
    }
    for (i = 0; i < tc->funcLen; i++) {
        const SLTCFunction* fn = &tc->funcs[i];
        uint32_t            outFunctionIndex = UINT32_MAX;
        int32_t             bodyNode;
        SLDiag              diag = { 0 };
        int                 supported = 0;
        if ((fn->flags & SLTCFunctionFlag_TEMPLATE_INSTANCE) == 0 || fn->defNode < 0) {
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
        if (SLMirLowerAppendSimpleFunction(
                builder,
                arena,
                &file->ast,
                (SLStrView){ file->source, file->sourceLen },
                fn->declNode,
                bodyNode,
                &outFunctionIndex,
                &supported,
                &diag)
            != 0)
        {
            return PrintSLDiagLineCol(file->path, file->source, &diag, 0);
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
    SLMirProgramBuilder* builder,
    SLArena*             arena,
    const SLPackage*     pkg,
    const SLParsedFile*  file,
    const char*          name,
    SLMirResolvedDeclMap* _Nullable declMap) {
    int32_t child = ASTFirstChild(&file->ast, file->ast.root);
    while (child >= 0) {
        const SLAstNode* n = &file->ast.nodes[child];
        int32_t          wasmImportNode = -1;
        if (n->kind == SLAst_FN && name != NULL
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
                       SLMirDeclKind_FN)
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
    SLMirProgramBuilder* builder,
    SLArena*             arena,
    const SLPackage*     pkg,
    const SLParsedFile*  file,
    SLMirResolvedDeclMap* _Nullable declMap) {
    return AppendMirSelectedPlatformNamedImportDeclsFromFile(
        builder, arena, pkg, file, "panic", declMap);
}

static int AppendMirSelectedPlatformConsoleLogDeclsFromFile(
    SLMirProgramBuilder* builder,
    SLArena*             arena,
    const SLPackage*     pkg,
    const SLParsedFile*  file,
    SLMirResolvedDeclMap* _Nullable declMap) {
    return AppendMirSelectedPlatformNamedImportDeclsFromFile(
        builder, arena, pkg, file, "console_log", declMap);
}

static int IsPlatformImportPath(const char* _Nullable path) {
    return path != NULL && (StrEq(path, "platform") || strncmp(path, "platform/", 9u) == 0);
}

static int IsSelectedPlatformImportPath(const SLPackageLoader* loader, const char* _Nullable path) {
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

int PackageHasPlatformImport(const SLPackage* _Nullable pkg) {
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

static const SLPackage* _Nullable EffectiveMirImportTargetPackage(
    const SLPackageLoader* loader, const SLImportRef* imp) {
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
    const SLPackageLoader* loader,
    uint32_t               pkgIndex,
    uint8_t*               state,
    uint32_t*              order,
    uint32_t*              orderLen) {
    const SLPackage* pkg = &loader->packages[pkgIndex];
    uint32_t         i;
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

int PackageUsesPlatformImport(const SLPackageLoader* loader);

static int BuildEntryPackageMirOrder(
    const SLPackageLoader* loader,
    const SLPackage*       entryPkg,
    int                    includeSelectedPlatform,
    uint32_t*              outOrder,
    uint32_t               outOrderCap,
    uint32_t*              outOrderLen) {
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

void FreeForeignLinkageInfo(SLForeignLinkageInfo* info) {
    uint32_t               i;
    SLForeignLinkageEntry* entries;
    if (info == NULL || info->entries == NULL) {
        if (info != NULL) {
            *info = (SLForeignLinkageInfo){ 0 };
        }
        return;
    }
    entries = (SLForeignLinkageEntry*)(uintptr_t)info->entries;
    for (i = 0; i < info->len; i++) {
        free(entries[i].arg0.bytes);
        free(entries[i].arg1.bytes);
    }
    free(entries);
    *info = (SLForeignLinkageInfo){ 0 };
}

static int ForeignLinkageBuilderAppend(
    SLForeignLinkageBuilder* b, const SLForeignLinkageEntry* entry) {
    SLForeignLinkageEntry* newEntries;
    uint32_t               newCap;
    if (b == NULL || entry == NULL) {
        return -1;
    }
    if (b->len >= b->cap) {
        newCap = b->cap >= 8u ? b->cap * 2u : 8u;
        newEntries = (SLForeignLinkageEntry*)realloc(
            b->entries, sizeof(SLForeignLinkageEntry) * newCap);
        if (newEntries == NULL) {
            return -1;
        }
        b->entries = newEntries;
        b->cap = newCap;
    }
    b->entries[b->len++] = *entry;
    return 0;
}

static void ForeignLinkageBuilderFree(SLForeignLinkageBuilder* b) {
    if (b == NULL) {
        return;
    }
    FreeForeignLinkageInfo((SLForeignLinkageInfo*)b);
    b->cap = 0;
}

static int BuildMirForeignLinkageInfo(
    const SLPackageLoader*      loader,
    const SLMirResolvedDeclMap* declMap,
    SLForeignLinkageInfo*       outInfo,
    SLDiag* _Nullable diag) {
    SLForeignLinkageBuilder b = { 0 };
    uint32_t                pkgIndex;
    if (outInfo == NULL) {
        return -1;
    }
    *outInfo = (SLForeignLinkageInfo){ 0 };
    if (loader == NULL || declMap == NULL) {
        return 0;
    }
    for (pkgIndex = 0; pkgIndex < loader->packageLen; pkgIndex++) {
        const SLPackage* pkg = &loader->packages[pkgIndex];
        uint32_t         fileIndex;
        for (fileIndex = 0; fileIndex < pkg->fileLen; fileIndex++) {
            const SLParsedFile* file = &pkg->files[fileIndex];
            int32_t             child = ASTFirstChild(&file->ast, file->ast.root);
            while (child >= 0) {
                const SLAstNode* decl = &file->ast.nodes[child];
                int32_t          cImportNode = -1;
                int32_t          wasmImportNode = -1;
                int32_t          exportNode = -1;
                if (DeclHasDirective(file, child, "c_import", &cImportNode)) {
                    if (diag != NULL) {
                        *diag = (SLDiag){
                            .code = SLDiag_WASM_BACKEND_UNSUPPORTED_MIR,
                            .type = SLDiagTypeOfCode(SLDiag_WASM_BACKEND_UNSUPPORTED_MIR),
                            .start = file->ast.nodes[cImportNode].start,
                            .end = file->ast.nodes[cImportNode].end,
                            .detail = "@c_import is not supported by the direct Wasm backend",
                        };
                    }
                    ForeignLinkageBuilderFree(&b);
                    return -1;
                }
                if (DeclHasDirective(file, child, "wasm_import", &wasmImportNode)) {
                    int32_t                  arg0 = DirectiveArgAt(&file->ast, wasmImportNode, 0u);
                    int32_t                  arg1 = DirectiveArgAt(&file->ast, wasmImportNode, 1u);
                    const SLMirResolvedDecl* resolved = NULL;
                    SLForeignLinkageEntry    entry = { 0 };
                    SLStringLitErr           litErr = { 0 };
                    if (arg0 < 0 || arg1 < 0) {
                        ForeignLinkageBuilderFree(&b);
                        return -1;
                    }
                    if (decl->kind == SLAst_FN) {
                        resolved = FindMirResolvedDeclBySlice(
                            declMap,
                            pkg,
                            file->source,
                            decl->dataStart,
                            decl->dataEnd,
                            SLMirDeclKind_FN);
                        entry.kind = SLForeignLinkage_WASM_IMPORT_FN;
                    } else if (decl->kind == SLAst_CONST) {
                        resolved = FindMirResolvedDeclBySlice(
                            declMap,
                            pkg,
                            file->source,
                            decl->dataStart,
                            decl->dataEnd,
                            SLMirDeclKind_CONST);
                        entry.kind = SLForeignLinkage_WASM_IMPORT_CONST;
                    } else if (decl->kind == SLAst_VAR) {
                        resolved = FindMirResolvedDeclBySlice(
                            declMap,
                            pkg,
                            file->source,
                            decl->dataStart,
                            decl->dataEnd,
                            SLMirDeclKind_VAR);
                        entry.kind = SLForeignLinkage_WASM_IMPORT_VAR;
                    }
                    if (resolved == NULL) {
                        child = ASTNextSibling(&file->ast, child);
                        continue;
                    }
                    entry.functionIndex = resolved->functionIndex;
                    entry.start = file->ast.nodes[wasmImportNode].start;
                    entry.end = file->ast.nodes[wasmImportNode].end;
                    if (loader->selectedPlatformPkg == pkg && decl->kind == SLAst_FN
                        && SliceEqCStr(file->source, decl->dataStart, decl->dataEnd, "panic"))
                    {
                        entry.flags = SLForeignLinkageFlag_PLATFORM_PANIC;
                    }
                    if (SLDecodeStringLiteralMalloc(
                            file->source,
                            file->ast.nodes[arg0].start,
                            file->ast.nodes[arg0].end,
                            &entry.arg0.bytes,
                            &entry.arg0.len,
                            &litErr)
                            != 0
                        || SLDecodeStringLiteralMalloc(
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
                    DeclHasDirective(file, child, "export", &exportNode) && decl->kind == SLAst_FN)
                {
                    int32_t                  arg0 = DirectiveArgAt(&file->ast, exportNode, 0u);
                    const SLMirResolvedDecl* resolved = FindMirResolvedDeclBySlice(
                        declMap,
                        pkg,
                        file->source,
                        decl->dataStart,
                        decl->dataEnd,
                        SLMirDeclKind_FN);
                    SLForeignLinkageEntry entry = { 0 };
                    SLStringLitErr        litErr = { 0 };
                    if (arg0 >= 0 && resolved != NULL) {
                        entry.kind = SLForeignLinkage_EXPORT_FN;
                        entry.functionIndex = resolved->functionIndex;
                        entry.start = file->ast.nodes[exportNode].start;
                        entry.end = file->ast.nodes[exportNode].end;
                        if (SLDecodeStringLiteralMalloc(
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

static const SLParsedFile* _Nullable FindLoaderFileByMirSource(
    const SLPackageLoader* loader,
    const SLMirProgram*    program,
    uint32_t               sourceRef,
    const SLPackage** _Nullable outPkg) {
    uint32_t pkgIndex;
    if (outPkg != NULL) {
        *outPkg = NULL;
    }
    if (loader == NULL || program == NULL || sourceRef >= program->sourceLen) {
        return NULL;
    }
    for (pkgIndex = 0; pkgIndex < loader->packageLen; pkgIndex++) {
        const SLPackage* pkg = &loader->packages[pkgIndex];
        uint32_t         fileIndex;
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
    const SLParsedFile* file,
    int32_t             nodeId,
    int32_t*            outTypeNode,
    int32_t*            outCountNode,
    int32_t*            outInitNode,
    int32_t*            outAllocNode) {
    const SLAstNode* n;
    int32_t          nextNode;
    int              hasCount;
    int              hasInit;
    int              hasAlloc;
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
    if (n->kind != SLAst_NEW) {
        return 0;
    }
    hasCount = (n->flags & SLAstFlag_NEW_HAS_COUNT) != 0;
    hasInit = (n->flags & SLAstFlag_NEW_HAS_INIT) != 0;
    hasAlloc = (n->flags & SLAstFlag_NEW_HAS_ALLOC) != 0;
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

static const SLSymbolDecl* _Nullable FindPackageTypeDeclBySlice(
    const SLPackage* pkg, const char* src, uint32_t start, uint32_t end);

static int ResolvePackageEnumVariantConstValue(
    const SLPackage* pkg,
    const char*      typeSrc,
    uint32_t         typeStart,
    uint32_t         typeEnd,
    const char*      memberSrc,
    uint32_t         memberStart,
    uint32_t         memberEnd,
    int64_t*         outValue) {
    const SLSymbolDecl* decl;
    const SLParsedFile* file;
    const SLAst*        ast;
    int32_t             variantNode;
    int64_t             nextValue = 0;
    if (outValue != NULL) {
        *outValue = 0;
    }
    if (pkg == NULL || typeSrc == NULL || memberSrc == NULL || outValue == NULL) {
        return 0;
    }
    decl = FindPackageTypeDeclBySlice(pkg, typeSrc, typeStart, typeEnd);
    if (decl == NULL || decl->kind != SLAst_ENUM || decl->fileIndex >= pkg->fileLen
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
        && (ast->nodes[variantNode].kind == SLAst_TYPE_NAME
            || ast->nodes[variantNode].kind == SLAst_TYPE_PTR
            || ast->nodes[variantNode].kind == SLAst_TYPE_REF
            || ast->nodes[variantNode].kind == SLAst_TYPE_MUTREF
            || ast->nodes[variantNode].kind == SLAst_TYPE_ARRAY
            || ast->nodes[variantNode].kind == SLAst_TYPE_VARRAY
            || ast->nodes[variantNode].kind == SLAst_TYPE_SLICE
            || ast->nodes[variantNode].kind == SLAst_TYPE_MUTSLICE
            || ast->nodes[variantNode].kind == SLAst_TYPE_OPTIONAL
            || ast->nodes[variantNode].kind == SLAst_TYPE_FN
            || ast->nodes[variantNode].kind == SLAst_TYPE_TUPLE
            || ast->nodes[variantNode].kind == SLAst_TYPE_ANON_STRUCT
            || ast->nodes[variantNode].kind == SLAst_TYPE_ANON_UNION))
    {
        variantNode = ast->nodes[variantNode].nextSibling;
    }
    while (variantNode >= 0) {
        const SLAstNode* variant = &ast->nodes[variantNode];
        int64_t          value = nextValue;
        int32_t          child = variant->firstChild;
        if (variant->kind == SLAst_FIELD) {
            while (child >= 0 && ast->nodes[child].kind == SLAst_FIELD) {
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
    const SLMirProgram* program, const char* src, uint32_t srcLen);
static int FindMirTypeRefByAstNode(
    const SLMirProgram* program, uint32_t sourceRef, int32_t astNode, uint32_t* outTypeRef);
static int AppendMirInst(
    SLMirInst* outInsts, uint32_t outCap, uint32_t* outLen, const SLMirInst* inst);
static int MirTypeNodeKind(SLAstKind kind);
static int ResolveMirAggregateTypeRefForTypeNode(
    const SLPackageLoader* loader,
    const SLMirProgram*    program,
    const SLParsedFile*    file,
    int32_t                typeNode,
    uint32_t*              outTypeRef);
static int EnsureMirAstTypeRef(
    SLArena*               arena,
    const SLPackageLoader* loader,
    SLMirProgram*          program,
    uint32_t               astNode,
    uint32_t               sourceRef,
    uint32_t* _Nonnull outTypeRef);
static int EnsureMirScalarTypeRef(
    SLArena* arena, SLMirProgram* program, SLMirTypeScalar scalar, uint32_t* _Nonnull outTypeRef);

static uint32_t MirIntKindByteWidth(SLMirIntKind intKind) {
    switch (intKind) {
        case SLMirIntKind_U8:
        case SLMirIntKind_I8:
        case SLMirIntKind_BOOL: return 1u;
        case SLMirIntKind_U16:
        case SLMirIntKind_I16:  return 2u;
        case SLMirIntKind_U32:
        case SLMirIntKind_I32:  return 4u;
        default:                return 0u;
    }
}

static int ResolveMirAllocNewPointeeTypeRef(
    const SLPackageLoader* loader,
    SLArena*               arena,
    SLMirProgram*          program,
    const SLParsedFile*    file,
    const SLMirInst*       allocInst,
    uint32_t*              outTypeRef) {
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
    SLArena* arena, SLMirProgram* program, int64_t value, uint32_t* _Nonnull outIndex) {
    SLMirConst* newConsts;
    uint32_t    i;
    if (arena == NULL || program == NULL || outIndex == NULL) {
        return -1;
    }
    for (i = 0; i < program->constLen; i++) {
        if (program->consts[i].kind == SLMirConst_INT && (int64_t)program->consts[i].bits == value)
        {
            *outIndex = i;
            return 0;
        }
    }
    newConsts = (SLMirConst*)SLArenaAlloc(
        arena, sizeof(SLMirConst) * (program->constLen + 1u), (uint32_t)_Alignof(SLMirConst));
    if (newConsts == NULL) {
        return -1;
    }
    if (program->constLen != 0u) {
        memcpy(newConsts, program->consts, sizeof(SLMirConst) * program->constLen);
    }
    newConsts[program->constLen] = (SLMirConst){
        .kind = SLMirConst_INT,
        .aux = 0u,
        .bits = (uint64_t)value,
        .bytes = { 0 },
    };
    program->consts = newConsts;
    *outIndex = program->constLen++;
    return 0;
}

static int EnsureMirNullConst(SLArena* arena, SLMirProgram* program, uint32_t* _Nonnull outIndex) {
    SLMirConst* newConsts;
    uint32_t    i;
    if (arena == NULL || program == NULL || outIndex == NULL) {
        return -1;
    }
    for (i = 0; i < program->constLen; i++) {
        if (program->consts[i].kind == SLMirConst_NULL) {
            *outIndex = i;
            return 0;
        }
    }
    newConsts = (SLMirConst*)SLArenaAlloc(
        arena, sizeof(SLMirConst) * (program->constLen + 1u), (uint32_t)_Alignof(SLMirConst));
    if (newConsts == NULL) {
        return -1;
    }
    if (program->constLen != 0u) {
        memcpy(newConsts, program->consts, sizeof(SLMirConst) * program->constLen);
    }
    newConsts[program->constLen] =
        (SLMirConst){ .kind = SLMirConst_NULL, .aux = 0u, .bits = 0u, .bytes = { 0 } };
    program->consts = newConsts;
    *outIndex = program->constLen++;
    return 0;
}

static int EnsureMirBoolConst(
    SLArena* arena, SLMirProgram* program, bool value, uint32_t* _Nonnull outIndex) {
    SLMirConst* newConsts;
    uint32_t    i;
    if (arena == NULL || program == NULL || outIndex == NULL) {
        return -1;
    }
    for (i = 0; i < program->constLen; i++) {
        if (program->consts[i].kind == SLMirConst_BOOL
            && ((program->consts[i].bits != 0u) == value))
        {
            *outIndex = i;
            return 0;
        }
    }
    newConsts = (SLMirConst*)SLArenaAlloc(
        arena, sizeof(SLMirConst) * (program->constLen + 1u), (uint32_t)_Alignof(SLMirConst));
    if (newConsts == NULL) {
        return -1;
    }
    if (program->constLen != 0u) {
        memcpy(newConsts, program->consts, sizeof(SLMirConst) * program->constLen);
    }
    newConsts[program->constLen] = (SLMirConst){
        .kind = SLMirConst_BOOL,
        .aux = 0u,
        .bits = value ? 1u : 0u,
        .bytes = { 0 },
    };
    program->consts = newConsts;
    *outIndex = program->constLen++;
    return 0;
}

static int EnsureMirStringConst(
    SLArena* arena, SLMirProgram* program, SLStrView value, uint32_t* _Nonnull outIndex) {
    SLMirConst* newConsts;
    uint32_t    i;
    if (arena == NULL || program == NULL || outIndex == NULL) {
        return -1;
    }
    for (i = 0; i < program->constLen; i++) {
        if (program->consts[i].kind == SLMirConst_STRING
            && program->consts[i].bytes.len == value.len
            && (value.len == 0u || memcmp(program->consts[i].bytes.ptr, value.ptr, value.len) == 0))
        {
            *outIndex = i;
            return 0;
        }
    }
    newConsts = (SLMirConst*)SLArenaAlloc(
        arena, sizeof(SLMirConst) * (program->constLen + 1u), (uint32_t)_Alignof(SLMirConst));
    if (newConsts == NULL) {
        return -1;
    }
    if (program->constLen != 0u) {
        memcpy(newConsts, program->consts, sizeof(SLMirConst) * program->constLen);
    }
    newConsts[program->constLen] =
        (SLMirConst){ .kind = SLMirConst_STRING, .aux = 0u, .bits = 0u, .bytes = value };
    program->consts = newConsts;
    *outIndex = program->constLen++;
    return 0;
}

static int EnsureMirFunctionConst(
    SLArena* arena, SLMirProgram* program, uint32_t functionIndex, uint32_t* _Nonnull outIndex) {
    SLMirConst* newConsts;
    uint32_t    i;
    if (arena == NULL || program == NULL || outIndex == NULL || functionIndex >= program->funcLen) {
        return -1;
    }
    for (i = 0; i < program->constLen; i++) {
        if (program->consts[i].kind == SLMirConst_FUNCTION
            && program->consts[i].aux == functionIndex)
        {
            *outIndex = i;
            return 0;
        }
    }
    newConsts = (SLMirConst*)SLArenaAlloc(
        arena, sizeof(SLMirConst) * (program->constLen + 1u), (uint32_t)_Alignof(SLMirConst));
    if (newConsts == NULL) {
        return -1;
    }
    if (program->constLen != 0u) {
        memcpy(newConsts, program->consts, sizeof(SLMirConst) * program->constLen);
    }
    newConsts[program->constLen] = (SLMirConst){
        .kind = SLMirConst_FUNCTION,
        .aux = functionIndex,
        .bits = functionIndex,
        .bytes = { 0 },
    };
    program->consts = newConsts;
    *outIndex = program->constLen++;
    return 0;
}

static bool MirSourceSliceEq(
    const SLMirProgram* program,
    uint32_t            sourceRefA,
    uint32_t            startA,
    uint32_t            endA,
    uint32_t            sourceRefB,
    uint32_t            startB,
    uint32_t            endB) {
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
    const SLMirProgram* program, const char* src, uint32_t srcLen) {
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
    const SLMirProgram* program, uint32_t sourceRef, int32_t astNode, uint32_t* outTypeRef) {
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
    const SLMirProgram* program,
    uint32_t            ownerTypeRef,
    uint32_t            sourceRef,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    uint32_t*           outFieldIndex) {
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
    const SLMirProgram* program, const char* name, uint32_t* outFieldIndex) {
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
    const SLMirProgram*  program,
    const SLMirFunction* fn,
    const char*          src,
    uint32_t             nameStart,
    uint32_t             nameEnd,
    uint32_t*            outLocalIndex) {
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
        const SLMirLocal* local = &program->locals[fn->localStart + i];
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
    const SLParsedFile* file, int32_t exprNode, uint32_t* _Nonnull outCount) {
    const SLAstNode* n;
    uint32_t         leftCount = 0u;
    uint32_t         rightCount = 0u;
    if (outCount != NULL) {
        *outCount = 0u;
    }
    if (file == NULL || outCount == NULL || exprNode < 0 || (uint32_t)exprNode >= file->ast.len) {
        return 0;
    }
    n = &file->ast.nodes[exprNode];
    switch (n->kind) {
        case SLAst_INT:
        case SLAst_IDENT: *outCount = 1u; return 1;
        case SLAst_UNARY:
            if (!CountMirAllocNewCountExprInsts(file, n->firstChild, &leftCount)) {
                return 0;
            }
            *outCount = leftCount + 1u;
            return 1;
        case SLAst_BINARY: {
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
    const SLParsedFile* file, int32_t exprNode, uint32_t* _Nonnull outCount) {
    const SLAstNode* n;
    if (outCount != NULL) {
        *outCount = 0u;
    }
    if (file == NULL || outCount == NULL || exprNode < 0 || (uint32_t)exprNode >= file->ast.len) {
        return 0;
    }
    n = &file->ast.nodes[exprNode];
    switch (n->kind) {
        case SLAst_IDENT:
        case SLAst_NULL:  *outCount = 1u; return 1;
        case SLAst_CAST:  {
            int32_t lhsNode = n->firstChild;
            if (lhsNode < 0 || (uint32_t)lhsNode >= file->ast.len) {
                return 0;
            }
            return CountMirAllocNewAllocExprInsts(file, lhsNode, outCount);
        }
        case SLAst_FIELD_EXPR: {
            int32_t baseNode = n->firstChild;
            if (baseNode < 0 || (uint32_t)baseNode >= file->ast.len) {
                return 0;
            }
            if (file->ast.nodes[baseNode].kind == SLAst_IDENT
                && SliceEqCStr(
                    file->source,
                    file->ast.nodes[baseNode].dataStart,
                    file->ast.nodes[baseNode].dataEnd,
                    "context")
                && (SliceEqCStr(file->source, n->dataStart, n->dataEnd, "mem")
                    || SliceEqCStr(file->source, n->dataStart, n->dataEnd, "temp_mem")))
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
    const SLMirProgram*  program,
    SLMirProgram*        mutableProgram,
    const SLMirFunction* fn,
    const SLParsedFile*  file,
    int32_t              exprNode,
    SLArena*             arena,
    SLMirInst*           outInsts,
    uint32_t             outCap,
    uint32_t*            outLen) {
    const SLAstNode* n;
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
        case SLAst_INT: {
            int64_t  value = 0;
            uint32_t constIndex = UINT32_MAX;
            if (outCap < 1u || !ParseMirIntLiteral(file->source, n->dataStart, n->dataEnd, &value)
                || EnsureMirIntConst(arena, mutableProgram, value, &constIndex) != 0)
            {
                return 0;
            }
            outInsts[0] = (SLMirInst){
                .op = SLMirOp_PUSH_CONST,
                .tok = 0u,
                ._reserved = 0u,
                .aux = constIndex,
                .start = n->start,
                .end = n->end,
            };
            *outLen = 1u;
            return 1;
        }
        case SLAst_IDENT: {
            uint32_t localIndex = UINT32_MAX;
            if (outCap < 1u
                || !FindMirFunctionLocalBySlice(
                    program, fn, file->source, n->dataStart, n->dataEnd, &localIndex))
            {
                return 0;
            }
            outInsts[0] = (SLMirInst){
                .op = SLMirOp_LOCAL_LOAD,
                .tok = 0u,
                ._reserved = 0u,
                .aux = localIndex,
                .start = n->start,
                .end = n->end,
            };
            *outLen = 1u;
            return 1;
        }
        case SLAst_UNARY: {
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
            outInsts[innerLen] = (SLMirInst){
                .op = SLMirOp_UNARY,
                .tok = (uint16_t)n->op,
                ._reserved = 0u,
                .aux = 0u,
                .start = n->start,
                .end = n->end,
            };
            *outLen = innerLen + 1u;
            return 1;
        }
        case SLAst_BINARY: {
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
            outInsts[leftLen + rightLen] = (SLMirInst){
                .op = SLMirOp_BINARY,
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
    const SLParsedFile* file,
    int32_t             initNode,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    int32_t*            outValueNode) {
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
        const SLAstNode* field = &file->ast.nodes[child];
        if (field->kind == SLAst_COMPOUND_FIELD && field->dataEnd >= field->dataStart
            && field->dataEnd - field->dataStart == nameEnd - nameStart
            && memcmp(
                   file->source + field->dataStart, file->source + nameStart, nameEnd - nameStart)
                   == 0)
        {
            *outValueNode = field->firstChild;
            return *outValueNode >= 0 || (field->flags & SLAstFlag_COMPOUND_FIELD_SHORTHAND) != 0u;
        }
        child = field->nextSibling;
    }
    return 0;
}

static int FindCompoundFieldValueNodeByText(
    const SLParsedFile* file, int32_t initNode, const char* name, int32_t* outValueNode) {
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
        const SLAstNode* field = &file->ast.nodes[child];
        if (field->kind == SLAst_COMPOUND_FIELD && field->dataEnd >= field->dataStart
            && (size_t)(field->dataEnd - field->dataStart) == nameLen
            && memcmp(file->source + field->dataStart, name, nameLen) == 0)
        {
            *outValueNode = field->firstChild;
            return *outValueNode >= 0 || (field->flags & SLAstFlag_COMPOUND_FIELD_SHORTHAND) != 0u;
        }
        child = field->nextSibling;
    }
    return 0;
}

static int AppendMirBinaryInst(
    SLMirInst* outInsts, uint32_t outCap, uint32_t* outLen, SLTokenKind tok) {
    return AppendMirInst(
        outInsts,
        outCap,
        outLen,
        &(SLMirInst){
            .op = SLMirOp_BINARY,
            .tok = (uint16_t)tok,
            ._reserved = 0u,
            .aux = 0u,
            .start = 0u,
            .end = 0u,
        });
}

static int AppendMirIntConstInst(
    SLArena*      arena,
    SLMirProgram* program,
    SLMirInst*    outInsts,
    uint32_t      outCap,
    uint32_t*     outLen,
    int64_t       value) {
    uint32_t constIndex = UINT32_MAX;
    return EnsureMirIntConst(arena, program, value, &constIndex) == 0
        && AppendMirInst(
               outInsts,
               outCap,
               outLen,
               &(SLMirInst){
                   .op = SLMirOp_PUSH_CONST,
                   .tok = 0u,
                   ._reserved = 0u,
                   .aux = constIndex,
                   .start = 0u,
                   .end = 0u,
               });
}

static int MirStaticTypeByteSize(const SLMirProgram* program, uint32_t typeRefIndex) {
    const SLMirTypeRef* typeRef;
    uint32_t            i;
    uint32_t            offset = 0u;
    uint32_t            maxAlign = 1u;
    if (program == NULL || typeRefIndex == UINT32_MAX || typeRefIndex >= program->typeLen) {
        return -1;
    }
    typeRef = &program->types[typeRefIndex];
    if (SLMirTypeRefIsAggregate(typeRef)) {
        for (i = 0; i < program->fieldLen; i++) {
            int fieldSize;
            int fieldAlign;
            if (program->fields[i].ownerTypeRef != typeRefIndex) {
                continue;
            }
            if (program->fields[i].typeRef >= program->typeLen) {
                return -1;
            }
            if (SLMirTypeRefIsVArrayView(&program->types[program->fields[i].typeRef])) {
                fieldSize = 0;
                fieldAlign = 1;
            } else if (SLMirTypeRefIsStrObj(&program->types[program->fields[i].typeRef])) {
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
    if (SLMirTypeRefIsStrObj(typeRef) || SLMirTypeRefIsStrRef(typeRef)
        || SLMirTypeRefIsSliceView(typeRef) || SLMirTypeRefIsAggSliceView(typeRef))
    {
        return 8;
    }
    if (SLMirTypeRefIsFixedArray(typeRef)) {
        return (int)(MirIntKindByteWidth(SLMirTypeRefIntKind(typeRef))
                     * SLMirTypeRefFixedArrayCount(typeRef));
    }
    if (SLMirTypeRefIsFixedArrayView(typeRef) || SLMirTypeRefIsStrPtr(typeRef)
        || SLMirTypeRefIsOpaquePtr(typeRef) || SLMirTypeRefIsU8Ptr(typeRef)
        || SLMirTypeRefIsI8Ptr(typeRef) || SLMirTypeRefIsU16Ptr(typeRef)
        || SLMirTypeRefIsI16Ptr(typeRef) || SLMirTypeRefIsU32Ptr(typeRef)
        || SLMirTypeRefIsI32Ptr(typeRef) || SLMirTypeRefIsFuncRef(typeRef))
    {
        return 4;
    }
    if (SLMirTypeRefScalarKind(typeRef) == SLMirTypeScalar_I32) {
        return (int)MirIntKindByteWidth(SLMirTypeRefIntKind(typeRef));
    }
    return -1;
}

static int LowerMirVarSizeAllocNewSizeExpr(
    const SLMirProgram*  program,
    SLMirProgram*        mutableProgram,
    const SLMirFunction* fn,
    const SLParsedFile*  file,
    const SLMirInst*     allocInst,
    uint32_t             pointeeTypeRef,
    SLArena*             arena,
    SLMirInst*           outInsts,
    uint32_t             outCap,
    uint32_t*            outLen) {
    int32_t             typeNode = -1;
    int32_t             countNode = -1;
    int32_t             initNode = -1;
    int32_t             allocNode = -1;
    uint32_t            len = 0u;
    uint32_t            i;
    const SLMirTypeRef* pointee;
    int                 hasDynamic = 0;
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
    if (SLMirTypeRefIsStrObj(pointee)) {
        int32_t lenNode = -1;
        if (initNode < 0 || !FindCompoundFieldValueNodeByText(file, initNode, "len", &lenNode)) {
            /* caller handles unsupported shape */
            return 0;
        }
        if (!LowerMirAllocNewCountExpr(
                program, mutableProgram, fn, file, lenNode, arena, outInsts, outCap, &len)
            || !AppendMirIntConstInst(arena, mutableProgram, outInsts, outCap, &len, 9)
            || !AppendMirBinaryInst(outInsts, outCap, &len, SLTok_ADD))
        {
            return 0;
        }
        *outLen = len;
        return 1;
    }
    if (!SLMirTypeRefIsAggregate(pointee)) {
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
        const SLMirField*   fieldRef;
        const SLMirTypeRef* fieldType;
        int32_t             valueNode = -1;
        uint32_t            elemSize;
        uint32_t            align;
        if (program->fields[i].ownerTypeRef != pointeeTypeRef) {
            continue;
        }
        fieldRef = &program->fields[i];
        if (fieldRef->typeRef >= program->typeLen) {
            return 0;
        }
        fieldType = &program->types[fieldRef->typeRef];
        if (SLMirTypeRefIsStrObj(fieldType)) {
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
                || !AppendMirBinaryInst(outInsts, outCap, &len, SLTok_ADD)
                || !AppendMirBinaryInst(outInsts, outCap, &len, SLTok_ADD))
            {
                return 0;
            }
            continue;
        }
        if (!SLMirTypeRefIsVArrayView(fieldType)) {
            continue;
        }
        hasDynamic = 1;
        elemSize = MirIntKindByteWidth(SLMirTypeRefIntKind(fieldType));
        align = elemSize >= 4u ? 4u : elemSize;
        if (align > 1u
            && (!AppendMirIntConstInst(
                    arena, mutableProgram, outInsts, outCap, &len, (int64_t)(align - 1u))
                || !AppendMirBinaryInst(outInsts, outCap, &len, SLTok_ADD)
                || !AppendMirIntConstInst(
                    arena, mutableProgram, outInsts, outCap, &len, -(int64_t)align)
                || !AppendMirBinaryInst(outInsts, outCap, &len, SLTok_AND)))
        {
            return 0;
        }
        if (SLMirTypeRefVArrayCountField(fieldType) == UINT32_MAX
            || SLMirTypeRefVArrayCountField(fieldType) >= program->fieldLen)
        {
            return 0;
        }
        {
            const SLMirField* countField =
                &program->fields[SLMirTypeRefVArrayCountField(fieldType)];
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
                || !AppendMirBinaryInst(outInsts, outCap, &len, SLTok_MUL)))
        {
            return 0;
        }
        if (!AppendMirBinaryInst(outInsts, outCap, &len, SLTok_ADD)) {
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
    const SLMirProgram* program,
    const SLParsedFile* file,
    const SLMirInst*    allocInst,
    uint32_t            pointeeTypeRef,
    uint32_t*           outCount) {
    int32_t             typeNode = -1;
    int32_t             countNode = -1;
    int32_t             initNode = -1;
    int32_t             allocNode = -1;
    uint32_t            count = 0u;
    uint32_t            i;
    int                 hasDynamic = 0;
    const SLMirTypeRef* pointee;
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
    if (SLMirTypeRefIsStrObj(pointee)) {
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
    if (!SLMirTypeRefIsAggregate(pointee)) {
        return 0;
    }
    count = 1u;
    for (i = 0; i < program->fieldLen; i++) {
        const SLMirField*   fieldRef;
        const SLMirTypeRef* fieldType;
        if (program->fields[i].ownerTypeRef != pointeeTypeRef) {
            continue;
        }
        fieldRef = &program->fields[i];
        if (fieldRef->typeRef >= program->typeLen) {
            return 0;
        }
        fieldType = &program->types[fieldRef->typeRef];
        if (SLMirTypeRefIsStrObj(fieldType)) {
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
        if (SLMirTypeRefIsVArrayView(fieldType)) {
            const SLMirField* countField;
            int32_t           countValueNode = -1;
            uint32_t          exprCount = 0u;
            uint32_t          elemSize = MirIntKindByteWidth(SLMirTypeRefIntKind(fieldType));
            uint32_t          countFieldRef = SLMirTypeRefVArrayCountField(fieldType);
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
    const SLMirProgram*  program,
    SLMirProgram*        mutableProgram,
    const SLMirFunction* fn,
    const SLParsedFile*  file,
    int32_t              exprNode,
    SLArena*             arena,
    SLMirInst*           outInsts,
    uint32_t             outCap,
    uint32_t*            outLen) {
    const SLAstNode* n;
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
        case SLAst_IDENT: {
            uint32_t localIndex = UINT32_MAX;
            if (!FindMirFunctionLocalBySlice(
                    program, fn, file->source, n->dataStart, n->dataEnd, &localIndex))
            {
                return 0;
            }
            outInsts[0] = (SLMirInst){
                .op = SLMirOp_LOCAL_LOAD,
                .tok = 0u,
                ._reserved = 0u,
                .aux = localIndex,
                .start = n->start,
                .end = n->end,
            };
            *outLen = 1u;
            return 1;
        }
        case SLAst_NULL: {
            uint32_t constIndex = UINT32_MAX;
            if (EnsureMirNullConst(arena, mutableProgram, &constIndex) != 0) {
                return 0;
            }
            outInsts[0] = (SLMirInst){
                .op = SLMirOp_PUSH_CONST,
                .tok = 0u,
                ._reserved = 0u,
                .aux = constIndex,
                .start = n->start,
                .end = n->end,
            };
            *outLen = 1u;
            return 1;
        }
        case SLAst_CAST:
            return LowerMirAllocNewAllocExpr(
                program, mutableProgram, fn, file, n->firstChild, arena, outInsts, outCap, outLen);
        case SLAst_FIELD_EXPR: {
            int32_t  baseNode = n->firstChild;
            uint32_t field = SLMirContextField_INVALID;
            if (baseNode < 0 || (uint32_t)baseNode >= file->ast.len
                || file->ast.nodes[baseNode].kind != SLAst_IDENT
                || !SliceEqCStr(
                    file->source,
                    file->ast.nodes[baseNode].dataStart,
                    file->ast.nodes[baseNode].dataEnd,
                    "context"))
            {
                return 0;
            }
            if (SliceEqCStr(file->source, n->dataStart, n->dataEnd, "mem")) {
                field = SLMirContextField_MEM;
            } else if (SliceEqCStr(file->source, n->dataStart, n->dataEnd, "temp_mem")) {
                field = SLMirContextField_TEMP_MEM;
            } else {
                return 0;
            }
            outInsts[0] = (SLMirInst){
                .op = SLMirOp_CTX_GET,
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
    SLMirInst* outInsts, uint32_t outCap, uint32_t* _Nonnull ioLen, const SLMirInst* inst) {
    if (outInsts == NULL || ioLen == NULL || inst == NULL || *ioLen >= outCap) {
        return 0;
    }
    outInsts[*ioLen] = *inst;
    (*ioLen)++;
    return 1;
}

static uint32_t MirInitOwnerTypeRefForType(const SLMirProgram* program, uint32_t typeRef) {
    if (program == NULL || typeRef >= program->typeLen) {
        return UINT32_MAX;
    }
    if (SLMirTypeRefIsAggregate(&program->types[typeRef])) {
        return typeRef;
    }
    if (SLMirTypeRefIsOpaquePtr(&program->types[typeRef])) {
        return SLMirTypeRefOpaquePointeeTypeRef(&program->types[typeRef]);
    }
    return UINT32_MAX;
}

static int MirTypeNodeKind(SLAstKind kind) {
    return IsFnReturnTypeNodeKind(kind) || kind == SLAst_TYPE_ANON_STRUCT
        || kind == SLAst_TYPE_ANON_UNION;
}

static int LowerMirHeapInitValueExpr(
    const SLPackageLoader* loader,
    const SLMirProgram*    program,
    SLMirProgram*          mutableProgram,
    const SLMirFunction*   fn,
    const SLParsedFile*    fnFile,
    const SLParsedFile*    exprFile,
    int32_t                exprNode,
    SLArena*               arena,
    uint32_t               currentLocalIndex,
    uint32_t               currentOwnerTypeRef,
    uint32_t               expectedTypeRef,
    SLMirInst*             outInsts,
    uint32_t               outCap,
    uint32_t*              outLen,
    uint32_t*              outTypeRef);

static const SLAstNode* _Nullable ResolveMirAggregateDeclNode(
    const SLPackageLoader* loader,
    const SLMirProgram*    program,
    const SLMirTypeRef*    typeRef,
    const SLParsedFile** _Nullable outFile,
    uint32_t* _Nullable outSourceRef);

static int EnsureMirAggregateFieldRef(
    SLArena*      arena,
    SLMirProgram* program,
    uint32_t      nameStart,
    uint32_t      nameEnd,
    uint32_t      sourceRef,
    uint32_t      ownerTypeRef,
    uint32_t      typeRef,
    uint32_t* _Nonnull outFieldRef);

static const SLSymbolDecl* _Nullable FindPackageTypeDeclBySlice(
    const SLPackage* pkg, const char* src, uint32_t start, uint32_t end);

static int ResolveMirAggregateTypeRefForTypeNode(
    const SLPackageLoader* loader,
    const SLMirProgram*    program,
    const SLParsedFile*    file,
    int32_t                typeNode,
    uint32_t*              outTypeRef) {
    const SLPackage*    pkg = NULL;
    const SLAstNode*    node;
    const SLSymbolDecl* decl;
    const SLParsedFile* declFile;
    uint32_t            sourceRef;
    uint32_t            declSourceRef;
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
        && *outTypeRef < program->typeLen && SLMirTypeRefIsAggregate(&program->types[*outTypeRef]))
    {
        return 1;
    }
    node = &file->ast.nodes[typeNode];
    if (node->kind != SLAst_TYPE_NAME) {
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
        || !SLMirTypeRefIsAggregate(&program->types[*outTypeRef]))
    {
        return 0;
    }
    return 1;
}

static int LowerMirHeapInitValueBySlice(
    const SLMirProgram*  program,
    const SLMirFunction* fn,
    const SLParsedFile*  fnFile,
    const SLParsedFile*  exprFile,
    uint32_t             nameStart,
    uint32_t             nameEnd,
    uint32_t             start,
    uint32_t             end,
    uint32_t             currentLocalIndex,
    uint32_t             currentOwnerTypeRef,
    SLMirInst*           outInsts,
    uint32_t             outCap,
    uint32_t*            outLen,
    uint32_t*            outTypeRef) {
    uint32_t localIndex = UINT32_MAX;
    uint32_t fieldIndex = UINT32_MAX;
    if (outLen == NULL || outTypeRef == NULL || program == NULL || fn == NULL || exprFile == NULL
        || nameEnd < nameStart)
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
                &(SLMirInst){
                    .op = SLMirOp_LOCAL_LOAD,
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
        && FindMirFieldByOwnerAndSlice(
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
                &(SLMirInst){
                    .op = SLMirOp_LOCAL_LOAD,
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
                &(SLMirInst){
                    .op = SLMirOp_AGG_GET,
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
    const SLPackageLoader* loader,
    const SLMirProgram*    program,
    SLMirProgram*          mutableProgram,
    const SLMirFunction*   fn,
    const SLParsedFile*    fnFile,
    const SLParsedFile*    exprFile,
    int32_t                exprNode,
    SLArena*               arena,
    uint32_t               currentLocalIndex,
    uint32_t               currentOwnerTypeRef,
    uint32_t               expectedTypeRef,
    SLMirInst*             outInsts,
    uint32_t               outCap,
    uint32_t*              outLen,
    uint32_t*              outTypeRef) {
    const SLAstNode* lit;
    int32_t          child;
    int32_t          typeNode = -1;
    uint32_t         ownerTypeRef = expectedTypeRef;
    uint32_t         exprSourceRef;
    if (outLen == NULL || outTypeRef == NULL || program == NULL || exprFile == NULL || exprNode < 0
        || (uint32_t)exprNode >= exprFile->ast.len)
    {
        return 0;
    }
    lit = &exprFile->ast.nodes[exprNode];
    if (lit->kind != SLAst_COMPOUND_LIT) {
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
    if (ownerTypeRef >= program->typeLen || !SLMirTypeRefIsAggregate(&program->types[ownerTypeRef]))
    {
        return 0;
    }
    if (!AppendMirInst(
            outInsts,
            outCap,
            outLen,
            &(SLMirInst){
                .op = SLMirOp_AGG_ZERO,
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
        const SLAstNode* field = &exprFile->ast.nodes[child];
        int32_t          valueNode = field->firstChild;
        uint32_t         fieldIndex = UINT32_MAX;
        uint32_t         fieldTypeRef = UINT32_MAX;
        uint32_t         valueTypeRef = UINT32_MAX;
        uint32_t         beforeLen;
        if (field->kind != SLAst_COMPOUND_FIELD || field->dataEnd < field->dataStart
            || field->nextSibling == child)
        {
            return 0;
        }
        if (memchr(exprFile->source + field->dataStart, '.', field->dataEnd - field->dataStart)
                != NULL
            || !FindMirFieldByOwnerAndSlice(
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
        } else if ((field->flags & SLAstFlag_COMPOUND_FIELD_SHORTHAND) != 0u) {
            if (!LowerMirHeapInitValueBySlice(
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
                &(SLMirInst){
                    .op = SLMirOp_AGG_SET,
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
    const SLPackageLoader* loader,
    const SLMirProgram*    program,
    SLMirProgram*          mutableProgram,
    const SLMirFunction*   fn,
    const SLParsedFile*    fnFile,
    const SLParsedFile*    exprFile,
    int32_t                exprNode,
    SLArena*               arena,
    uint32_t               currentLocalIndex,
    uint32_t               currentOwnerTypeRef,
    uint32_t               expectedTypeRef,
    SLMirInst*             outInsts,
    uint32_t               outCap,
    uint32_t*              outLen,
    uint32_t*              outTypeRef) {
    const SLAstNode* n;
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
        case SLAst_INT: {
            int64_t  value = 0;
            uint32_t constIndex = UINT32_MAX;
            if (!ParseMirIntLiteral(exprFile->source, n->dataStart, n->dataEnd, &value)
                || EnsureMirIntConst(arena, mutableProgram, value, &constIndex) != 0
                || !AppendMirInst(
                    outInsts,
                    outCap,
                    outLen,
                    &(SLMirInst){
                        .op = SLMirOp_PUSH_CONST,
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
        case SLAst_BOOL: {
            uint32_t constIndex = UINT32_MAX;
            bool     value = SliceEqCStr(exprFile->source, n->dataStart, n->dataEnd, "true");
            if ((!value && !SliceEqCStr(exprFile->source, n->dataStart, n->dataEnd, "false"))
                || EnsureMirBoolConst(arena, mutableProgram, value, &constIndex) != 0
                || !AppendMirInst(
                    outInsts,
                    outCap,
                    outLen,
                    &(SLMirInst){
                        .op = SLMirOp_PUSH_CONST,
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
        case SLAst_STRING: {
            uint8_t*       bytes = NULL;
            uint32_t       len = 0u;
            SLStringLitErr litErr = { 0 };
            uint32_t       constIndex = UINT32_MAX;
            if (SLDecodeStringLiteralArena(
                    arena, exprFile->source, n->dataStart, n->dataEnd, &bytes, &len, &litErr)
                    != 0
                || EnsureMirStringConst(
                       arena,
                       mutableProgram,
                       (SLStrView){ .ptr = (const char*)bytes, .len = len },
                       &constIndex)
                       != 0
                || !AppendMirInst(
                    outInsts,
                    outCap,
                    outLen,
                    &(SLMirInst){
                        .op = SLMirOp_PUSH_CONST,
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
        case SLAst_NULL: {
            uint32_t constIndex = UINT32_MAX;
            if (EnsureMirNullConst(arena, mutableProgram, &constIndex) != 0
                || !AppendMirInst(
                    outInsts,
                    outCap,
                    outLen,
                    &(SLMirInst){
                        .op = SLMirOp_PUSH_CONST,
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
        case SLAst_IDENT:
            return LowerMirHeapInitValueBySlice(
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
        case SLAst_UNARY: {
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
                    &(SLMirInst){
                        .op = SLMirOp_UNARY,
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
        case SLAst_BINARY: {
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
                    &(SLMirInst){
                        .op = SLMirOp_BINARY,
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
        case SLAst_FIELD_EXPR: {
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
                || !FindMirFieldByOwnerAndSlice(
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
                    &(SLMirInst){
                        .op = SLMirOp_AGG_GET,
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
        case SLAst_COMPOUND_LIT:
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
    const SLPackageLoader* loader,
    const SLMirProgram*    program,
    SLMirProgram*          mutableProgram,
    const SLMirFunction*   fn,
    const SLParsedFile*    fnFile,
    const SLParsedFile*    typeFile,
    const SLParsedFile*    newFile,
    const SLMirInst*       allocInst,
    const SLMirInst*       storeInst,
    uint32_t               pointeeTypeRef,
    SLArena*               arena,
    SLMirInst*             outInsts,
    uint32_t               outCap,
    uint32_t*              outLen,
    bool*                  outClearedInitFlag) {
    int32_t          typeNode = -1;
    int32_t          countNode = -1;
    int32_t          initNode = -1;
    int32_t          allocNode = -1;
    const SLAstNode* structNode;
    const SLAstNode* astNode;
    uint32_t         fieldSourceRef;
    uint32_t         typeSourceRef = UINT32_MAX;
    uint32_t         emittedLen = 0u;
    uint32_t         localIndex = storeInst->aux;
    bool             clearedInitFlag = false;
    bool             explicitDirect[256] = { 0 };
    uint32_t         directFieldIndices[256];
    uint32_t         directFieldCount = 0u;
    uint32_t         i;
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
    if (astNode->kind != SLAst_NEW || fieldSourceRef == UINT32_MAX) {
        return 0;
    }
    if ((astNode->flags & SLAstFlag_NEW_HAS_INIT) != 0u) {
        const SLAstNode* initLit;
        int32_t          child;
        if (initNode < 0 || (uint32_t)initNode >= newFile->ast.len) {
            return 0;
        }
        initLit = &newFile->ast.nodes[initNode];
        if (initLit->kind != SLAst_COMPOUND_LIT) {
            return 0;
        }
        child = initLit->firstChild;
        if (child >= 0 && (uint32_t)child < newFile->ast.len
            && MirTypeNodeKind(newFile->ast.nodes[child].kind))
        {
            child = newFile->ast.nodes[child].nextSibling;
        }
        while (child >= 0) {
            const SLAstNode* field = &newFile->ast.nodes[child];
            int32_t          valueNode = field->firstChild;
            uint32_t         fieldIndex = UINT32_MAX;
            uint32_t         fieldTypeRef = UINT32_MAX;
            uint32_t         valueTypeRef = UINT32_MAX;
            const char*      dot;
            if (field->kind != SLAst_COMPOUND_FIELD || field->dataEnd < field->dataStart) {
                return 0;
            }
            dot = memchr(
                newFile->source + field->dataStart, '.', field->dataEnd - field->dataStart);
            if (SLMirTypeRefIsStrObj(&program->types[pointeeTypeRef])) {
                uint32_t pseudoFieldRef = UINT32_MAX;
                uint32_t expectedTypeRef = UINT32_MAX;
                if (dot != NULL) {
                    return 0;
                }
                if (SliceEqCStr(newFile->source, field->dataStart, field->dataEnd, "len")) {
                    if (EnsureMirScalarTypeRef(
                            arena, mutableProgram, SLMirTypeScalar_I32, &expectedTypeRef)
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
                        &(SLMirInst){
                            .op = SLMirOp_LOCAL_LOAD,
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
                        &(SLMirInst){
                            .op = SLMirOp_AGG_SET,
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
                        &(SLMirInst){
                            .op = SLMirOp_DROP,
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
                    || !SLMirTypeRefIsStrObj(
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
                            arena, mutableProgram, SLMirTypeScalar_I32, &expectedTypeRef)
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
                        &(SLMirInst){
                            .op = SLMirOp_LOCAL_LOAD,
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
                        &(SLMirInst){
                            .op = SLMirOp_AGG_GET,
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
                        &(SLMirInst){
                            .op = SLMirOp_AGG_SET,
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
                        &(SLMirInst){
                            .op = SLMirOp_DROP,
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
                    &(SLMirInst){
                        .op = SLMirOp_LOCAL_LOAD,
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
            } else if ((field->flags & SLAstFlag_COMPOUND_FIELD_SHORTHAND) != 0u) {
                if (!LowerMirHeapInitValueBySlice(
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
                    &(SLMirInst){
                        .op = SLMirOp_AGG_SET,
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
                    &(SLMirInst){
                        .op = SLMirOp_DROP,
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
    if (!SLMirTypeRefIsAggregate(&program->types[pointeeTypeRef])) {
        *outLen = emittedLen;
        *outClearedInitFlag = clearedInitFlag;
        return emittedLen != 0u || clearedInitFlag;
    }
    structNode = ResolveMirAggregateDeclNode(
        loader, program, &program->types[pointeeTypeRef], &typeFile, &typeSourceRef);
    if (structNode == NULL || structNode->kind != SLAst_STRUCT) {
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
            const SLAstNode* fieldDecl = &typeFile->ast.nodes[fieldNode];
            int32_t          typeChild;
            int32_t          defaultExprNode;
            uint32_t         fieldIndex = UINT32_MAX;
            uint32_t         fieldTypeRef = UINT32_MAX;
            uint32_t         valueTypeRef = UINT32_MAX;
            bool             alreadyExplicit = false;
            uint32_t         directIndex;
            if (fieldDecl->kind != SLAst_FIELD) {
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
                    &(SLMirInst){
                        .op = SLMirOp_LOCAL_LOAD,
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
                    &(SLMirInst){
                        .op = SLMirOp_AGG_SET,
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
                    &(SLMirInst){
                        .op = SLMirOp_DROP,
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
    const SLPackageLoader* loader, SLArena* arena, SLMirProgram* program) {
    SLMirInst*     insts = NULL;
    SLMirFunction* funcs = NULL;
    uint32_t       instOutLen = 0u;
    uint32_t       totalExtraLen = 0u;
    uint32_t       funcIndex;
    if (loader == NULL || arena == NULL || program == NULL) {
        return -1;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const SLMirFunction* fn = &program->funcs[funcIndex];
        const SLParsedFile*  fnFile = FindLoaderFileByMirSource(
            loader, program, fn->sourceRef, NULL);
        uint32_t pc;
        if (fnFile == NULL) {
            continue;
        }
        for (pc = 0u; pc + 1u < fn->instLen; pc++) {
            const SLMirInst*    allocInst = &program->insts[fn->instStart + pc];
            const SLMirInst*    storeInst = &program->insts[fn->instStart + pc + 1u];
            const SLMirLocal*   local;
            uint32_t            pointeeTypeRef;
            const SLParsedFile* typeFile;
            SLMirInst*          tempInsts;
            uint32_t            tempLen = 0u;
            bool                clearedInit = false;
            uint32_t            tempCap;
            if (allocInst->op != SLMirOp_ALLOC_NEW || storeInst->op != SLMirOp_LOCAL_STORE
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
            tempInsts = tempCap != 0u ? (SLMirInst*)malloc(sizeof(SLMirInst) * tempCap) : NULL;
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
    funcs = (SLMirFunction*)SLArenaAlloc(
        arena, sizeof(SLMirFunction) * program->funcLen, (uint32_t)_Alignof(SLMirFunction));
    insts = (SLMirInst*)SLArenaAlloc(
        arena,
        sizeof(SLMirInst) * (program->instLen + totalExtraLen),
        (uint32_t)_Alignof(SLMirInst));
    if ((funcs == NULL && program->funcLen != 0u)
        || (insts == NULL && program->instLen + totalExtraLen != 0u))
    {
        return -1;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const SLMirFunction* fn = &program->funcs[funcIndex];
        const SLParsedFile*  fnFile = FindLoaderFileByMirSource(
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
                const SLMirInst*    allocInst = &program->insts[fn->instStart + pc];
                const SLMirInst*    storeInst = &program->insts[fn->instStart + pc + 1u];
                const SLMirLocal*   local;
                uint32_t            pointeeTypeRef;
                const SLParsedFile* typeFile;
                SLMirInst*          tempInsts;
                uint32_t            tempLen = 0u;
                bool                cleared = false;
                uint32_t            tempCap;
                if (allocInst->op != SLMirOp_ALLOC_NEW || storeInst->op != SLMirOp_LOCAL_STORE
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
                tempInsts = tempCap != 0u ? (SLMirInst*)malloc(sizeof(SLMirInst) * tempCap) : NULL;
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
            const SLMirInst* srcInst = &program->insts[fn->instStart + pc];
            insts[instOutLen] = *srcInst;
            if (clearInit != NULL && clearInit[pc] && insts[instOutLen].op == SLMirOp_ALLOC_NEW) {
                insts[instOutLen].tok &= (uint16_t)~SLAstFlag_NEW_HAS_INIT;
            }
            if ((srcInst->op == SLMirOp_JUMP || srcInst->op == SLMirOp_JUMP_IF_FALSE)
                && srcInst->aux < fn->instLen)
            {
                insts[instOutLen].aux = pcMap[srcInst->aux];
            }
            instOutLen++;
            if (insertCounts != NULL && insertCounts[pc] != 0u) {
                const SLMirInst*    allocInst = &program->insts[fn->instStart + pc - 1u];
                const SLMirInst*    storeInst = &program->insts[fn->instStart + pc];
                const SLMirLocal*   local;
                uint32_t            pointeeTypeRef;
                const SLParsedFile* typeFile;
                uint32_t            emittedLen = 0u;
                SLMirInst*          tempInsts = insts + instOutLen;
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
    const SLPackageLoader* loader, SLArena* arena, SLMirProgram* program) {
    SLMirInst*     insts = NULL;
    SLMirFunction* funcs = NULL;
    uint32_t       instOutLen = 0u;
    uint32_t       totalExtraLen = 0u;
    uint32_t       funcIndex;
    if (loader == NULL || arena == NULL || program == NULL) {
        return -1;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const SLMirFunction* fn = &program->funcs[funcIndex];
        const SLParsedFile*  ownerFile = FindLoaderFileByMirSource(
            loader, program, fn->sourceRef, NULL);
        uint32_t pc;
        if (ownerFile != NULL) {
            for (pc = 0u; pc < fn->instLen; pc++) {
                const SLMirInst* inst = &program->insts[fn->instStart + pc];
                const SLMirInst* nextInst;
                uint32_t         localTypeRef;
                uint32_t         countInstLen = 0u;
                int32_t          typeNode = -1;
                int32_t          countNode = -1;
                int32_t          initNode = -1;
                int32_t          allocNode = -1;
                if (inst->op != SLMirOp_ALLOC_NEW || (inst->tok & SLAstFlag_NEW_HAS_COUNT) == 0u
                    || pc + 1u >= fn->instLen || ownerFile->source == NULL)
                {
                    continue;
                }
                nextInst = &program->insts[fn->instStart + pc + 1u];
                if (nextInst->op != SLMirOp_LOCAL_STORE || nextInst->aux >= fn->localCount) {
                    continue;
                }
                localTypeRef = program->locals[fn->localStart + nextInst->aux].typeRef;
                if (localTypeRef >= program->typeLen
                    || (!SLMirTypeRefIsSliceView(&program->types[localTypeRef])
                        && !SLMirTypeRefIsAggSliceView(&program->types[localTypeRef])))
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
    funcs = (SLMirFunction*)SLArenaAlloc(
        arena, sizeof(SLMirFunction) * program->funcLen, (uint32_t)_Alignof(SLMirFunction));
    insts = (SLMirInst*)SLArenaAlloc(
        arena,
        sizeof(SLMirInst) * (program->instLen + totalExtraLen),
        (uint32_t)_Alignof(SLMirInst));
    if ((funcs == NULL && program->funcLen != 0u)
        || (insts == NULL && program->instLen + totalExtraLen != 0u))
    {
        return -1;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const SLMirFunction* fn = &program->funcs[funcIndex];
        const SLParsedFile*  ownerFile = FindLoaderFileByMirSource(
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
                const SLMirInst* inst = &program->insts[fn->instStart + pc];
                const SLMirInst* nextInst;
                uint32_t         localTypeRef;
                int32_t          typeNode = -1;
                int32_t          countNode = -1;
                int32_t          initNode = -1;
                int32_t          allocNode = -1;
                if (inst->op != SLMirOp_ALLOC_NEW || (inst->tok & SLAstFlag_NEW_HAS_COUNT) == 0u
                    || pc + 1u >= fn->instLen || ownerFile->source == NULL)
                {
                    continue;
                }
                nextInst = &program->insts[fn->instStart + pc + 1u];
                if (nextInst->op != SLMirOp_LOCAL_STORE || nextInst->aux >= fn->localCount) {
                    continue;
                }
                localTypeRef = program->locals[fn->localStart + nextInst->aux].typeRef;
                if (localTypeRef >= program->typeLen
                    || (!SLMirTypeRefIsSliceView(&program->types[localTypeRef])
                        && !SLMirTypeRefIsAggSliceView(&program->types[localTypeRef])))
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
            const SLMirInst* srcInst = &program->insts[fn->instStart + pc];
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
            if ((srcInst->op == SLMirOp_JUMP || srcInst->op == SLMirOp_JUMP_IF_FALSE)
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
    const SLPackageLoader* loader, SLArena* arena, SLMirProgram* program) {
    SLMirInst*     insts = NULL;
    SLMirFunction* funcs = NULL;
    uint32_t       instOutLen = 0u;
    uint32_t       totalExtraLen = 0u;
    uint32_t       funcIndex;
    if (loader == NULL || arena == NULL || program == NULL) {
        return -1;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const SLMirFunction* fn = &program->funcs[funcIndex];
        const SLParsedFile*  ownerFile = FindLoaderFileByMirSource(
            loader, program, fn->sourceRef, NULL);
        uint32_t pc;
        if (ownerFile == NULL || ownerFile->source == NULL) {
            continue;
        }
        for (pc = 0u; pc + 1u < fn->instLen; pc++) {
            const SLMirInst* inst = &program->insts[fn->instStart + pc];
            const SLMirInst* nextInst = &program->insts[fn->instStart + pc + 1u];
            uint32_t         localTypeRef;
            uint32_t         pointeeTypeRef = UINT32_MAX;
            uint32_t         countInstLen = 0u;
            uint32_t         tempCap;
            SLMirInst*       tempInsts = NULL;
            if (inst->op != SLMirOp_ALLOC_NEW || (inst->tok & SLAstFlag_NEW_HAS_COUNT) != 0u
                || nextInst->op != SLMirOp_LOCAL_STORE || nextInst->aux >= fn->localCount)
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
            tempInsts = tempCap != 0u ? (SLMirInst*)malloc(sizeof(SLMirInst) * tempCap) : NULL;
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
    funcs = (SLMirFunction*)SLArenaAlloc(
        arena, sizeof(SLMirFunction) * program->funcLen, (uint32_t)_Alignof(SLMirFunction));
    insts = (SLMirInst*)SLArenaAlloc(
        arena,
        sizeof(SLMirInst) * (program->instLen + totalExtraLen),
        (uint32_t)_Alignof(SLMirInst));
    if ((funcs == NULL && program->funcLen != 0u)
        || (insts == NULL && program->instLen + totalExtraLen != 0u))
    {
        return -1;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const SLMirFunction* fn = &program->funcs[funcIndex];
        const SLParsedFile*  ownerFile = FindLoaderFileByMirSource(
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
                const SLMirInst* inst = &program->insts[fn->instStart + pc];
                const SLMirInst* nextInst = &program->insts[fn->instStart + pc + 1u];
                uint32_t         localTypeRef;
                uint32_t         pointeeTypeRef = UINT32_MAX;
                uint32_t         tempCap;
                SLMirInst*       tempInsts = NULL;
                if (inst->op != SLMirOp_ALLOC_NEW || (inst->tok & SLAstFlag_NEW_HAS_COUNT) != 0u
                    || nextInst->op != SLMirOp_LOCAL_STORE || nextInst->aux >= fn->localCount)
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
                tempInsts = tempCap != 0u ? (SLMirInst*)malloc(sizeof(SLMirInst) * tempCap) : NULL;
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
            const SLMirInst* srcInst = &program->insts[fn->instStart + pc];
            if (insertCounts != NULL && insertCounts[pc] != 0u) {
                const SLMirInst* nextInst =
                    pc + 1u < fn->instLen ? &program->insts[fn->instStart + pc + 1u] : NULL;
                uint32_t pointeeTypeRef = UINT32_MAX;
                uint32_t emittedLen = 0u;
                if (nextInst == NULL || nextInst->op != SLMirOp_LOCAL_STORE
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
                insts[instOutLen].tok |= SLAstFlag_NEW_HAS_COUNT;
            }
            if ((srcInst->op == SLMirOp_JUMP || srcInst->op == SLMirOp_JUMP_IF_FALSE)
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
    const SLPackageLoader* loader, SLArena* arena, SLMirProgram* program) {
    SLMirInst*     insts = NULL;
    SLMirFunction* funcs = NULL;
    uint32_t       instOutLen = 0u;
    uint32_t       totalExtraLen = 0u;
    uint32_t       funcIndex;
    if (loader == NULL || arena == NULL || program == NULL) {
        return -1;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const SLMirFunction* fn = &program->funcs[funcIndex];
        const SLParsedFile*  ownerFile = FindLoaderFileByMirSource(
            loader, program, fn->sourceRef, NULL);
        uint32_t pc;
        if (ownerFile == NULL || ownerFile->source == NULL) {
            continue;
        }
        for (pc = 0u; pc < fn->instLen; pc++) {
            const SLMirInst* inst = &program->insts[fn->instStart + pc];
            uint32_t         allocInstLen = 0u;
            int32_t          typeNode = -1;
            int32_t          countNode = -1;
            int32_t          initNode = -1;
            int32_t          allocNode = -1;
            if (inst->op != SLMirOp_ALLOC_NEW || (inst->tok & SLAstFlag_NEW_HAS_ALLOC) == 0u) {
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
    funcs = (SLMirFunction*)SLArenaAlloc(
        arena, sizeof(SLMirFunction) * program->funcLen, (uint32_t)_Alignof(SLMirFunction));
    insts = (SLMirInst*)SLArenaAlloc(
        arena,
        sizeof(SLMirInst) * (program->instLen + totalExtraLen),
        (uint32_t)_Alignof(SLMirInst));
    if ((funcs == NULL && program->funcLen != 0u)
        || (insts == NULL && program->instLen + totalExtraLen != 0u))
    {
        return -1;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const SLMirFunction* fn = &program->funcs[funcIndex];
        const SLParsedFile*  ownerFile = FindLoaderFileByMirSource(
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
                const SLMirInst* inst = &program->insts[fn->instStart + pc];
                int32_t          typeNode = -1;
                int32_t          countNode = -1;
                int32_t          initNode = -1;
                int32_t          allocNode = -1;
                if (inst->op != SLMirOp_ALLOC_NEW || (inst->tok & SLAstFlag_NEW_HAS_ALLOC) == 0u) {
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
            const SLMirInst* srcInst = &program->insts[fn->instStart + pc];
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
            if ((srcInst->op == SLMirOp_JUMP || srcInst->op == SLMirOp_JUMP_IF_FALSE)
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

static const SLImportSymbolRef* _Nullable FindImportValueSymbolBySlice(
    const SLPackage* pkg, const char* src, uint32_t start, uint32_t end) {
    uint32_t i;
    if (pkg == NULL || src == NULL || end <= start) {
        return NULL;
    }
    for (i = 0; i < pkg->importSymbolLen; i++) {
        const SLImportSymbolRef* sym = &pkg->importSymbols[i];
        size_t                   nameLen;
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

static const SLImportSymbolRef* _Nullable FindImportFunctionSymbolBySlice(
    const SLPackage* pkg, const char* src, uint32_t start, uint32_t end) {
    uint32_t i;
    if (pkg == NULL || src == NULL || end <= start) {
        return NULL;
    }
    for (i = 0; i < pkg->importSymbolLen; i++) {
        const SLImportSymbolRef* sym = &pkg->importSymbols[i];
        size_t                   nameLen;
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

static const SLMirResolvedDecl* _Nullable FindResolvedImportValueBySlice(
    const SLPackageLoader*      loader,
    const SLMirResolvedDeclMap* map,
    const SLPackage*            pkg,
    const char*                 src,
    uint32_t                    start,
    uint32_t                    end) {
    const SLImportSymbolRef* sym = FindImportValueSymbolBySlice(pkg, src, start, end);
    const SLPackage*         depPkg;
    if (sym == NULL || sym->importIndex >= pkg->importLen) {
        return NULL;
    }
    depPkg = EffectiveMirImportTargetPackage(loader, &pkg->imports[sym->importIndex]);
    return depPkg != NULL ? FindMirResolvedValueByCStr(map, depPkg, sym->sourceName) : NULL;
}

static const SLMirResolvedDecl* _Nullable FindResolvedImportFunctionBySlice(
    const SLPackageLoader*      loader,
    const SLMirResolvedDeclMap* map,
    const SLPackage*            pkg,
    const char*                 src,
    uint32_t                    start,
    uint32_t                    end) {
    const SLImportSymbolRef* sym = FindImportFunctionSymbolBySlice(pkg, src, start, end);
    const SLPackage*         depPkg;
    if (sym == NULL || sym->importIndex >= pkg->importLen) {
        return NULL;
    }
    depPkg = EffectiveMirImportTargetPackage(loader, &pkg->imports[sym->importIndex]);
    return depPkg != NULL
             ? FindMirResolvedDeclByCStr(map, depPkg, sym->sourceName, SLMirDeclKind_FN)
             : NULL;
}

static int MirExprInstStackDelta(const SLMirInst* inst, int32_t* outDelta) {
    uint32_t elemCount = 0;
    if (inst == NULL || outDelta == NULL) {
        return 0;
    }
    switch (inst->op) {
        case SLMirOp_PUSH_CONST:
        case SLMirOp_PUSH_INT:
        case SLMirOp_PUSH_FLOAT:
        case SLMirOp_PUSH_BOOL:
        case SLMirOp_PUSH_STRING:
        case SLMirOp_PUSH_NULL:
        case SLMirOp_LOAD_IDENT:
        case SLMirOp_LOCAL_LOAD:
        case SLMirOp_LOCAL_ADDR:
        case SLMirOp_ADDR_OF:
        case SLMirOp_AGG_ZERO:
        case SLMirOp_ARRAY_ZERO:
        case SLMirOp_CTX_GET:         *outDelta = 1; return 1;
        case SLMirOp_UNARY:
        case SLMirOp_CAST:
        case SLMirOp_COERCE:
        case SLMirOp_SEQ_LEN:
        case SLMirOp_STR_CSTR:
        case SLMirOp_OPTIONAL_WRAP:
        case SLMirOp_OPTIONAL_UNWRAP:
        case SLMirOp_DEREF_LOAD:
        case SLMirOp_AGG_GET:
        case SLMirOp_AGG_ADDR:
        case SLMirOp_ARRAY_GET:
        case SLMirOp_ARRAY_ADDR:
        case SLMirOp_TAGGED_TAG:
        case SLMirOp_TAGGED_PAYLOAD:  *outDelta = 0; return 1;
        case SLMirOp_BINARY:
        case SLMirOp_INDEX:
        case SLMirOp_LOCAL_STORE:
        case SLMirOp_STORE_IDENT:
        case SLMirOp_DROP:
        case SLMirOp_ASSERT:
        case SLMirOp_CTX_SET:
        case SLMirOp_DEREF_STORE:
        case SLMirOp_ARRAY_SET:
        case SLMirOp_AGG_SET:         *outDelta = -1; return 1;
        case SLMirOp_CALL:
        case SLMirOp_CALL_FN:
        case SLMirOp_CALL_HOST:
        case SLMirOp_CALL_INDIRECT:
            *outDelta = 1 - (int32_t)SLMirCallArgCountFromTok(inst->tok);
            return 1;
        case SLMirOp_TUPLE_MAKE:
        case SLMirOp_AGG_MAKE:
            elemCount = (uint32_t)inst->tok;
            *outDelta = 1 - (int32_t)elemCount;
            return 1;
        case SLMirOp_SLICE_MAKE:
            *outDelta = 0 - (((inst->tok & SLAstFlag_INDEX_HAS_START) != 0u) ? 1 : 0)
                      - (((inst->tok & SLAstFlag_INDEX_HAS_END) != 0u) ? 1 : 0);
            return 1;
        case SLMirOp_TAGGED_MAKE:   *outDelta = 0; return 1;
        case SLMirOp_RETURN:
        case SLMirOp_JUMP_IF_FALSE: *outDelta = -1; return 1;
        case SLMirOp_RETURN_VOID:
        case SLMirOp_LOCAL_ZERO:
        case SLMirOp_JUMP:          return ((*outDelta = 0), 1);
        default:                    return 0;
    }
}

static int FindCallArgStartInFunction(
    const SLMirProgram*  program,
    const SLMirFunction* fn,
    uint32_t             callIndex,
    uint32_t             argCount,
    uint32_t*            outArgStart) {
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
    const SLMirProgram*  program,
    const SLMirFunction* fn,
    uint32_t             callIndex,
    uint32_t             argCount,
    uint32_t*            outArgEnd) {
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
    const SLMirProgram* program, const SLMirFunction* fn, const SLMirInst* inst) {
    if (program == NULL || fn == NULL || inst == NULL) {
        return UINT32_MAX;
    }
    switch (inst->op) {
        case SLMirOp_LOCAL_LOAD:
            return inst->aux < fn->localCount
                     ? program->locals[fn->localStart + inst->aux].typeRef
                     : UINT32_MAX;
        case SLMirOp_AGG_GET:
        case SLMirOp_AGG_ADDR:
            return inst->aux < program->fieldLen ? program->fields[inst->aux].typeRef : UINT32_MAX;
        default: return UINT32_MAX;
    }
}

static uint32_t MirAggregateOwnerTypeRef(const SLMirProgram* program, uint32_t typeRef) {
    if (program == NULL || typeRef >= program->typeLen) {
        return UINT32_MAX;
    }
    if (SLMirTypeRefIsAggregate(&program->types[typeRef])) {
        return typeRef;
    }
    if (SLMirTypeRefIsOpaquePtr(&program->types[typeRef])) {
        return SLMirTypeRefOpaquePointeeTypeRef(&program->types[typeRef]);
    }
    return UINT32_MAX;
}

static uint32_t FindMirFieldNamed(
    const SLMirProgram* program,
    uint32_t            ownerTypeRef,
    const char*         src,
    uint32_t            start,
    uint32_t            end) {
    uint32_t i;
    uint32_t nameLen;
    if (program == NULL || src == NULL || ownerTypeRef >= program->typeLen || end < start) {
        return UINT32_MAX;
    }
    nameLen = end - start;
    for (i = 0; i < program->fieldLen; i++) {
        const SLMirField* field = &program->fields[i];
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
    const SLMirProgram* program, const char* src, uint32_t start, uint32_t end) {
    uint32_t i;
    uint32_t nameLen;
    if (program == NULL || src == NULL || end < start) {
        return UINT32_MAX;
    }
    nameLen = end - start;
    for (i = 0; i < program->fieldLen; i++) {
        const SLMirField* field = &program->fields[i];
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
    SLArena*      arena,
    SLMirProgram* program,
    SLMirHostKind kind,
    uint32_t      flags,
    uint32_t      target,
    uint32_t      nameStart,
    uint32_t      nameEnd,
    uint32_t* _Nonnull outIndex) {
    uint32_t      i;
    SLMirHostRef* newHosts;
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
    newHosts = (SLMirHostRef*)SLArenaAlloc(
        arena, sizeof(SLMirHostRef) * (program->hostLen + 1u), (uint32_t)_Alignof(SLMirHostRef));
    if (newHosts == NULL) {
        return -1;
    }
    if (program->hostLen != 0u) {
        memcpy(newHosts, program->hosts, sizeof(SLMirHostRef) * program->hostLen);
    }
    newHosts[program->hostLen] = (SLMirHostRef){
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

static int ResolvePackageMirProgram(
    const SLPackageLoader*      loader,
    const SLMirResolvedDeclMap* declMap,
    const SLMirTcFunctionMap*   tcFnMap,
    SLArena*                    arena,
    const SLMirProgram*         program,
    SLMirProgram*               outProgram) {
    SLMirInst*     insts = NULL;
    SLMirFunction* funcs = NULL;
    uint32_t       instOutLen = 0;
    uint32_t       funcIndex;
    if (loader == NULL || declMap == NULL || arena == NULL || program == NULL || outProgram == NULL)
    {
        return -1;
    }
    *outProgram = *program;
    insts = (SLMirInst*)SLArenaAlloc(
        arena, sizeof(SLMirInst) * program->instLen, (uint32_t)_Alignof(SLMirInst));
    funcs = (SLMirFunction*)SLArenaAlloc(
        arena, sizeof(SLMirFunction) * program->funcLen, (uint32_t)_Alignof(SLMirFunction));
    if ((program->instLen != 0u && insts == NULL) || (program->funcLen != 0u && funcs == NULL)) {
        return -1;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const SLMirFunction* fn = &program->funcs[funcIndex];
        const SLPackage*     ownerPkg = NULL;
        const SLParsedFile*  ownerFile = FindLoaderFileByMirSource(
            loader, program, fn->sourceRef, &ownerPkg);
        SLTypeCheckCtx* ownerTc =
            ownerFile != NULL && ownerFile->hasTypecheckCtx
                ? (SLTypeCheckCtx*)ownerFile->typecheckCtx
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
                uint32_t         instIndex = fn->instStart + localIndex;
                const SLMirInst* inst = &program->insts[instIndex];
                if (inst->op == SLMirOp_CALL && inst->aux < program->symbolLen) {
                    const SLMirSymbolRef*    sym = &program->symbols[inst->aux];
                    const SLMirResolvedDecl* target = NULL;
                    if (sym->kind != SLMirSymbol_CALL) {
                        continue;
                    }
                    if ((sym->flags & SLMirSymbolFlag_CALL_RECEIVER_ARG0) != 0u) {
                        uint32_t argc = SLMirCallArgCountFromTok(inst->tok);
                        uint32_t argStart = UINT32_MAX;
                        if (argc == 0u
                            || !FindCallArgStartInFunction(program, fn, instIndex, argc, &argStart)
                            || argStart < fn->instStart || argStart >= fn->instStart + fn->instLen
                            || program->insts[argStart].op != SLMirOp_LOAD_IDENT)
                        {
                            continue;
                        }
                        {
                            const SLMirInst*   recvInst = &program->insts[argStart];
                            const SLImportRef* imp = FindImportByAliasSlice(
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
                                SLMirDeclKind_FN);
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
                            SLMirDeclKind_FN);
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
                } else if (inst->op == SLMirOp_AGG_GET && localIndex > 0u) {
                    const SLMirInst*         recvInst = &program->insts[instIndex - 1u];
                    const SLImportRef*       imp;
                    const SLMirResolvedDecl* target;
                    int64_t                  enumValue = 0;
                    if (recvInst->op != SLMirOp_LOAD_IDENT) {
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
            uint32_t  instIndex = fn->instStart + localIndex;
            SLMirInst inst = program->insts[instIndex];
            if (omit != NULL && omit[localIndex] != 0u) {
                continue;
            }
            if (ownerPkg != NULL && ownerFile != NULL) {
                if (inst.op == SLMirOp_LOAD_IDENT) {
                    const SLMirResolvedDecl* target = FindMirResolvedValueBySlice(
                        declMap, ownerPkg, ownerFile->source, inst.start, inst.end);
                    if (target == NULL) {
                        target = FindResolvedImportValueBySlice(
                            loader, declMap, ownerPkg, ownerFile->source, inst.start, inst.end);
                    }
                    if (target != NULL) {
                        inst.op = SLMirOp_CALL_FN;
                        inst.tok = 0;
                        inst.aux = target->functionIndex;
                    } else {
                        target = FindMirResolvedDeclBySlice(
                            declMap,
                            ownerPkg,
                            ownerFile->source,
                            inst.start,
                            inst.end,
                            SLMirDeclKind_FN);
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
                            inst.op = SLMirOp_PUSH_CONST;
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
                            inst.op = SLMirOp_PUSH_CONST;
                            inst.tok = 0u;
                            inst.aux = constIndex;
                        }
                    }
                } else if (inst.op == SLMirOp_CALL && inst.aux < program->symbolLen) {
                    const SLMirSymbolRef*    sym = &program->symbols[inst.aux];
                    const SLMirResolvedDecl* target = NULL;
                    if (sym->kind == SLMirSymbol_CALL) {
                        int32_t  targetTcFn = -1;
                        uint32_t targetMirFn = UINT32_MAX;
                        if (SliceEqCStr(ownerFile->source, inst.start, inst.end, "typeof")) {
                            uint32_t constIndex = UINT32_MAX;
                            if (EnsureMirIntConst(arena, outProgram, 0, &constIndex) != 0) {
                                free(omit);
                                return -1;
                            }
                            inst.op = SLMirOp_PUSH_CONST;
                            inst.tok = 0u;
                            inst.aux = constIndex;
                        } else if (
                            ownerTc != NULL && ownerTcFnIndex != UINT32_MAX
                            && sym->target < ownerTc->ast->len
                            && SLTCFindCallTarget(
                                ownerTc, (int32_t)ownerTcFnIndex, (int32_t)sym->target, &targetTcFn)
                            && targetTcFn >= 0 && (uint32_t)targetTcFn < ownerTc->funcLen
                            && (ownerTc->funcs[targetTcFn].flags
                                & SLTCFunctionFlag_TEMPLATE_INSTANCE)
                                   != 0
                            && FindMirTcFunctionDecl(
                                tcFnMap, ownerPkg, ownerFile, (uint32_t)targetTcFn, &targetMirFn))
                        {
                            inst.op = SLMirOp_CALL_FN;
                            inst.tok = (uint16_t)(SLMirCallArgCountFromTok(inst.tok)
                                                  | (inst.tok & SLMirCallArgFlag_SPREAD_LAST));
                            inst.aux = targetMirFn;
                        } else if ((sym->flags & SLMirSymbolFlag_CALL_RECEIVER_ARG0) != 0u) {
                            uint32_t argc = SLMirCallArgCountFromTok(inst.tok);
                            uint32_t argStart = UINT32_MAX;
                            if (argc != 0u
                                && FindCallArgStartInFunction(
                                    program, fn, instIndex, argc, &argStart)
                                && argStart < instIndex
                                && program->insts[argStart].op == SLMirOp_LOAD_IDENT)
                            {
                                const SLMirInst*   recvInst = &program->insts[argStart];
                                const SLImportRef* imp = FindImportByAliasSlice(
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
                                        SLMirDeclKind_FN);
                                    if (target != NULL) {
                                        inst.op = SLMirOp_CALL_FN;
                                        inst.tok =
                                            (uint16_t)((SLMirCallArgCountFromTok(inst.tok) - 1u)
                                                       | (inst.tok & SLMirCallArgFlag_SPREAD_LAST));
                                        inst.aux = target->functionIndex;
                                    }
                                }
                            }
                        } else {
                            target = FindMirResolvedDeclBySlice(
                                declMap,
                                ownerPkg,
                                ownerFile->source,
                                inst.start,
                                inst.end,
                                SLMirDeclKind_FN);
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
                                inst.op = SLMirOp_CALL_FN;
                                inst.aux = target->functionIndex;
                            }
                        }
                    }
                } else if (inst.op == SLMirOp_AGG_GET && localIndex > 0u) {
                    const SLMirInst*         recvInst = &program->insts[instIndex - 1u];
                    const SLImportRef*       imp;
                    const SLMirResolvedDecl* target;
                    if (recvInst->op == SLMirOp_LOAD_IDENT) {
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
                            inst.op = SLMirOp_PUSH_CONST;
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
                                    inst.op = SLMirOp_CALL_FN;
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

static uint32_t FindMirFuncRefFieldByName(
    const SLMirProgram* program, const char* src, uint32_t start, uint32_t end) {
    uint32_t i;
    if (program == NULL || src == NULL) {
        return UINT32_MAX;
    }
    for (i = 0; i < program->fieldLen; i++) {
        const SLMirField* field = &program->fields[i];
        if (field->typeRef < program->typeLen
            && SLMirTypeRefIsFuncRef(&program->types[field->typeRef])
            && SliceEqSlice(src, field->nameStart, field->nameEnd, src, start, end))
        {
            return i;
        }
    }
    return UINT32_MAX;
}

static int ClassifyMirFuncFieldCall(
    const SLPackageLoader* loader,
    const SLMirProgram*    program,
    const SLMirFunction*   fn,
    uint32_t               localIndex,
    uint32_t* _Nullable outInsertAfterPc,
    uint32_t* _Nullable outImplFieldRef) {
    const SLParsedFile*   ownerFile;
    const SLMirInst*      inst;
    const SLMirSymbolRef* sym;
    uint32_t              argc;
    uint32_t              argStartPc = UINT32_MAX;
    uint32_t              recvEndPc = UINT32_MAX;
    uint32_t              fieldRef = UINT32_MAX;
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
    if (ownerFile == NULL || ownerFile->source == NULL || inst->op != SLMirOp_CALL
        || inst->aux >= program->symbolLen)
    {
        return 0;
    }
    sym = &program->symbols[inst->aux];
    argc = SLMirCallArgCountFromTok(inst->tok);
    if (sym->kind != SLMirSymbol_CALL || argc < 2u
        || (sym->flags & SLMirSymbolFlag_CALL_RECEIVER_ARG0) == 0u
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

static int LoaderTargetNeedsSelectedPlatformMir(const SLPackageLoader* _Nullable loader) {
    return loader != NULL && loader->platformTarget != NULL
        && (StrEq(loader->platformTarget, SL_WASM_MIN_PLATFORM_TARGET)
            || StrEq(loader->platformTarget, SL_PLAYBIT_PLATFORM_TARGET));
}

static int BuilderHasHostPrintCall(const SLMirProgramBuilder* builder) {
    uint32_t i;
    if (builder == NULL) {
        return 0;
    }
    for (i = 0; i < builder->instLen; i++) {
        const SLMirInst* inst = &builder->insts[i];
        if (inst->op == SLMirOp_CALL_HOST && inst->aux < builder->hostLen
            && builder->hosts[inst->aux].target == SLMirHostTarget_PRINT)
        {
            return 1;
        }
    }
    return 0;
}

static int FindMirIntConstIndex(const SLMirProgram* program, uint64_t bits, uint32_t* outIndex) {
    uint32_t i;
    if (outIndex != NULL) {
        *outIndex = UINT32_MAX;
    }
    if (program == NULL || outIndex == NULL) {
        return 0;
    }
    for (i = 0; i < program->constLen; i++) {
        if (program->consts[i].kind == SLMirConst_INT && program->consts[i].bits == bits) {
            *outIndex = i;
            return 1;
        }
    }
    return 0;
}

static int EnsureMirIntConstIndex(
    SLArena* arena, SLMirProgram* program, uint64_t bits, uint32_t* outIndex) {
    SLMirConst* consts;
    uint32_t    i;
    if (outIndex != NULL) {
        *outIndex = UINT32_MAX;
    }
    if (arena == NULL || program == NULL || outIndex == NULL) {
        return 0;
    }
    for (i = 0; i < program->constLen; i++) {
        if (program->consts[i].kind == SLMirConst_INT && program->consts[i].bits == bits) {
            *outIndex = i;
            return 1;
        }
    }
    consts = (SLMirConst*)SLArenaAlloc(
        arena, sizeof(SLMirConst) * (program->constLen + 1u), (uint32_t)_Alignof(SLMirConst));
    if (consts == NULL) {
        return 0;
    }
    for (i = 0; i < program->constLen; i++) {
        consts[i] = program->consts[i];
    }
    consts[program->constLen] = (SLMirConst){
        .kind = SLMirConst_INT,
        .bits = bits,
    };
    *outIndex = program->constLen;
    program->consts = consts;
    program->constLen++;
    return 1;
}

static int RewriteMirWasmPrintHostcalls(
    const SLPackageLoader*      loader,
    const SLMirResolvedDeclMap* declMap,
    SLArena*                    arena,
    SLMirProgram*               program) {
    const SLMirResolvedDecl* consoleLogDecl;
    const SLMirFunction*     consoleLogFn;
    SLMirFunction*           funcs;
    SLMirInst*               insts;
    uint32_t                 zeroConstIndex = UINT32_MAX;
    uint32_t                 flagsTypeRef;
    uint32_t                 totalExtraLen = 0u;
    uint32_t                 funcIndex;
    uint32_t                 instOutLen = 0u;
    if (!LoaderTargetNeedsSelectedPlatformMir(loader)) {
        return 0;
    }
    if (loader == NULL || loader->selectedPlatformPkg == NULL || declMap == NULL || arena == NULL
        || program == NULL)
    {
        return -1;
    }
    consoleLogDecl = FindMirResolvedDeclByCStr(
        declMap, loader->selectedPlatformPkg, "console_log", SLMirDeclKind_FN);
    if (consoleLogDecl == NULL || consoleLogDecl->functionIndex >= program->funcLen) {
        return 0;
    }
    consoleLogFn = &program->funcs[consoleLogDecl->functionIndex];
    if (consoleLogFn->paramCount < 2u || consoleLogFn->localStart + 1u >= program->localLen) {
        return -1;
    }
    flagsTypeRef = program->locals[consoleLogFn->localStart + 1u].typeRef;
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const SLMirFunction* fn = &program->funcs[funcIndex];
        uint32_t             pc;
        for (pc = 0; pc < fn->instLen; pc++) {
            const SLMirInst* inst = &program->insts[fn->instStart + pc];
            if (inst->op == SLMirOp_CALL_HOST && inst->aux < program->hostLen
                && program->hosts[inst->aux].target == SLMirHostTarget_PRINT)
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
    funcs = (SLMirFunction*)SLArenaAlloc(
        arena, sizeof(SLMirFunction) * program->funcLen, (uint32_t)_Alignof(SLMirFunction));
    insts = (SLMirInst*)SLArenaAlloc(
        arena,
        sizeof(SLMirInst) * (program->instLen + totalExtraLen),
        (uint32_t)_Alignof(SLMirInst));
    if ((funcs == NULL && program->funcLen != 0u)
        || (insts == NULL && program->instLen + totalExtraLen != 0u))
    {
        return -1;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const SLMirFunction* fn = &program->funcs[funcIndex];
        uint32_t*            insertCounts = NULL;
        uint32_t*            pcMap = NULL;
        uint32_t             extraLen = 0u;
        uint32_t             delta = 0u;
        uint32_t             pc;
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
            const SLMirInst* inst = &program->insts[fn->instStart + pc];
            if (inst->op == SLMirOp_CALL_HOST && inst->aux < program->hostLen
                && program->hosts[inst->aux].target == SLMirHostTarget_PRINT)
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
            const SLMirInst* srcInst = &program->insts[fn->instStart + pc];
            if (insertCounts[pc] != 0u) {
                insts[instOutLen++] = (SLMirInst){
                    .op = SLMirOp_PUSH_CONST,
                    .aux = zeroConstIndex,
                    .start = srcInst->start,
                    .end = srcInst->end,
                };
                insts[instOutLen++] = (SLMirInst){
                    .op = SLMirOp_CAST,
                    .tok = SLMirCastTarget_INT,
                    .aux = flagsTypeRef,
                    .start = srcInst->start,
                    .end = srcInst->end,
                };
            }
            insts[instOutLen] = *srcInst;
            if ((srcInst->op == SLMirOp_JUMP || srcInst->op == SLMirOp_JUMP_IF_FALSE)
                && srcInst->aux < fn->instLen)
            {
                insts[instOutLen].aux = pcMap[srcInst->aux];
            } else if (
                srcInst->op == SLMirOp_CALL_HOST && srcInst->aux < program->hostLen
                && program->hosts[srcInst->aux].target == SLMirHostTarget_PRINT)
            {
                insts[instOutLen].op = SLMirOp_CALL_FN;
                insts[instOutLen].aux = consoleLogDecl->functionIndex;
                insts[instOutLen].tok = (uint16_t)((srcInst->tok & SLMirCallArgFlag_MASK) | 2u);
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
    const SLPackageLoader* loader, SLArena* arena, SLMirProgram* program) {
    SLMirInst*     insts = NULL;
    SLMirFunction* funcs = NULL;
    uint32_t       instOutLen = 0u;
    uint32_t       totalExtraLen = 0u;
    uint32_t       funcIndex;
    if (loader == NULL || arena == NULL || program == NULL) {
        return -1;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const SLMirFunction* fn = &program->funcs[funcIndex];
        uint32_t             pc;
        for (pc = 0u; pc < fn->instLen; pc++) {
            if (ClassifyMirFuncFieldCall(loader, program, fn, pc, NULL, NULL)) {
                totalExtraLen++;
            }
        }
    }
    if (totalExtraLen == 0u) {
        return 0;
    }
    funcs = (SLMirFunction*)SLArenaAlloc(
        arena, sizeof(SLMirFunction) * program->funcLen, (uint32_t)_Alignof(SLMirFunction));
    insts = (SLMirInst*)SLArenaAlloc(
        arena,
        sizeof(SLMirInst) * (program->instLen + totalExtraLen),
        (uint32_t)_Alignof(SLMirInst));
    if ((funcs == NULL && program->funcLen != 0u)
        || (insts == NULL && program->instLen + totalExtraLen != 0u))
    {
        return -1;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const SLMirFunction* fn = &program->funcs[funcIndex];
        uint32_t*            insertCounts = NULL;
        uint32_t*            insertFieldRefs = NULL;
        uint32_t*            pcMap = NULL;
        uint32_t             extraLen = 0u;
        uint32_t             delta = 0u;
        uint32_t             pc;
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
            SLMirInst inst = program->insts[fn->instStart + pc];
            if ((inst.op == SLMirOp_JUMP || inst.op == SLMirOp_JUMP_IF_FALSE)
                && inst.aux < fn->instLen)
            {
                inst.aux = pcMap[inst.aux];
            } else if (inst.op == SLMirOp_CALL && inst.aux < program->symbolLen) {
                const SLMirSymbolRef* sym = &program->symbols[inst.aux];
                if (sym->kind == SLMirSymbol_CALL
                    && (sym->flags & SLMirSymbolFlag_CALL_RECEIVER_ARG0) != 0u
                    && SLMirCallArgCountFromTok(inst.tok) > 0u)
                {
                    inst.op = SLMirOp_CALL_INDIRECT;
                    inst.tok = (uint16_t)((SLMirCallArgCountFromTok(inst.tok) - 1u)
                                          | (inst.tok & SLMirCallArgFlag_SPREAD_LAST));
                    inst.aux = 0u;
                }
            }
            insts[instOutLen++] = inst;
            if (insertCounts != NULL && insertCounts[pc] != 0u) {
                insts[instOutLen++] = (SLMirInst){
                    .op = SLMirOp_AGG_GET,
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

static void SpecializeMirDirectFunctionFieldStores(SLArena* arena, SLMirProgram* program) {
    uint32_t funcIndex;
    if (arena == NULL || program == NULL) {
        return;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const SLMirFunction* fn = &program->funcs[funcIndex];
        uint32_t             pc;
        for (pc = 3u; pc < fn->instLen; pc++) {
            const SLMirInst*  storeInst = &program->insts[fn->instStart + pc];
            const SLMirInst*  addrInst = &program->insts[fn->instStart + pc - 1u];
            const SLMirInst*  valueInst = &program->insts[fn->instStart + pc - 3u];
            const SLMirField* fieldRef;
            SLMirField*       field = NULL;
            uint32_t          typeRef = UINT32_MAX;
            if (storeInst->op != SLMirOp_DEREF_STORE || addrInst->op != SLMirOp_AGG_ADDR
                || addrInst->aux >= program->fieldLen || valueInst->op != SLMirOp_PUSH_CONST
                || valueInst->aux >= program->constLen
                || program->consts[valueInst->aux].kind != SLMirConst_FUNCTION)
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
                field = (SLMirField*)&program->fields[fieldIndex];
            }
            if (field->typeRef >= program->typeLen
                || !SLMirTypeRefIsFuncRef(&program->types[field->typeRef])
                || SLMirTypeRefFuncRefFunctionIndex(&program->types[field->typeRef]) != UINT32_MAX)
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
    const SLParsedFile* fileA, int32_t nodeA, const SLParsedFile* fileB, int32_t nodeB) {
    const SLAstNode* astNodeA;
    const SLAstNode* astNodeB;
    int32_t          childA;
    int32_t          childB;
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

static int32_t FindMirFunctionDeclNode(const SLParsedFile* file, const SLMirFunction* fn) {
    int32_t child;
    if (file == NULL || fn == NULL) {
        return -1;
    }
    child = ASTFirstChild(&file->ast, file->ast.root);
    while (child >= 0) {
        const SLAstNode* n = &file->ast.nodes[child];
        if (n->kind == SLAst_FN
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
    const SLParsedFile* typeFile, int32_t typeNode, const SLParsedFile* fnFile, int32_t fnNode) {
    int32_t typeChild;
    int32_t fnChild;
    int32_t typeReturnNode = -1;
    int32_t fnReturnNode = -1;
    if (typeFile == NULL || fnFile == NULL || typeNode < 0 || fnNode < 0
        || (uint32_t)typeNode >= typeFile->ast.len || (uint32_t)fnNode >= fnFile->ast.len
        || typeFile->ast.nodes[typeNode].kind != SLAst_TYPE_FN
        || fnFile->ast.nodes[fnNode].kind != SLAst_FN)
    {
        return false;
    }
    typeChild = ASTFirstChild(&typeFile->ast, typeNode);
    fnChild = ASTFirstChild(&fnFile->ast, fnNode);
    while (fnChild >= 0 && fnFile->ast.nodes[fnChild].kind == SLAst_PARAM) {
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
    const SLPackageLoader* loader, const SLMirProgram* program, const SLMirTypeRef* typeRef) {
    const SLParsedFile* typeFile;
    uint32_t            functionIndex;
    if (loader == NULL || program == NULL || typeRef == NULL || !SLMirTypeRefIsFuncRef(typeRef)
        || typeRef->astNode == UINT32_MAX)
    {
        return UINT32_MAX;
    }
    typeFile = FindLoaderFileByMirSource(loader, program, typeRef->sourceRef, NULL);
    if (typeFile == NULL) {
        return UINT32_MAX;
    }
    for (functionIndex = 0; functionIndex < program->funcLen; functionIndex++) {
        const SLMirFunction* fn = &program->funcs[functionIndex];
        const SLParsedFile*  fnFile = FindLoaderFileByMirSource(
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
    const SLPackageLoader* loader, SLMirProgram* program) {
    uint32_t i;
    if (loader == NULL || program == NULL) {
        return;
    }
    for (i = 0; i < program->typeLen; i++) {
        SLMirTypeRef* typeRef = (SLMirTypeRef*)&program->types[i];
        uint32_t      functionIndex;
        if (!SLMirTypeRefIsFuncRef(typeRef) || typeRef->aux != 0u || typeRef->astNode == UINT32_MAX)
        {
            continue;
        }
        functionIndex = FindMirRepresentativeFunctionForFuncType(loader, program, typeRef);
        if (functionIndex < program->funcLen) {
            typeRef->aux = functionIndex + 1u;
        }
    }
}

int PackageUsesPlatformImport(const SLPackageLoader* loader) {
    uint32_t pkgIndex;
    if (loader == NULL) {
        return 0;
    }
    for (pkgIndex = 0; pkgIndex < loader->packageLen; pkgIndex++) {
        const SLPackage* pkg = &loader->packages[pkgIndex];
        uint32_t         importIndex;
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

static SLMirTypeScalar ClassifyMirScalarType(
    const SLParsedFile* file, const SLMirTypeRef* typeRef) {
    const SLAstNode* node;
    const SLAstNode* child;
    if (file == NULL || typeRef == NULL || typeRef->astNode >= file->ast.len) {
        return SLMirTypeScalar_NONE;
    }
    node = &file->ast.nodes[typeRef->astNode];
    switch (node->kind) {
        case SLAst_TYPE_PTR:
        case SLAst_TYPE_MUTREF: return SLMirTypeScalar_I32;
        case SLAst_TYPE_REF:
            if (node->firstChild >= 0 && (uint32_t)node->firstChild < file->ast.len) {
                child = &file->ast.nodes[node->firstChild];
                if (child->kind == SLAst_TYPE_NAME
                    && SliceEqCStr(file->source, child->dataStart, child->dataEnd, "str"))
                {
                    return SLMirTypeScalar_NONE;
                }
            }
            return SLMirTypeScalar_I32;
        case SLAst_TYPE_NAME:
            if (SliceEqCStr(file->source, node->dataStart, node->dataEnd, "rawptr")) {
                return SLMirTypeScalar_NONE;
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
                return SLMirTypeScalar_I32;
            }
            if (SliceEqCStr(file->source, node->dataStart, node->dataEnd, "u64")
                || SliceEqCStr(file->source, node->dataStart, node->dataEnd, "i64"))
            {
                return SLMirTypeScalar_I64;
            }
            if (SliceEqCStr(file->source, node->dataStart, node->dataEnd, "f32")) {
                return SLMirTypeScalar_F32;
            }
            if (SliceEqCStr(file->source, node->dataStart, node->dataEnd, "f64")) {
                return SLMirTypeScalar_F64;
            }
            return SLMirTypeScalar_NONE;
        default: return SLMirTypeScalar_NONE;
    }
}

static SLMirIntKind ClassifyMirIntKindFromTypeNode(
    const SLParsedFile* file, const SLAstNode* node) {
    if (file == NULL || node == NULL) {
        return SLMirIntKind_NONE;
    }
    if (node->kind != SLAst_TYPE_NAME) {
        return SLMirIntKind_NONE;
    }
    if (SliceEqCStr(file->source, node->dataStart, node->dataEnd, "bool")) {
        return SLMirIntKind_BOOL;
    }
    if (SliceEqCStr(file->source, node->dataStart, node->dataEnd, "u8")) {
        return SLMirIntKind_U8;
    }
    if (SliceEqCStr(file->source, node->dataStart, node->dataEnd, "i8")) {
        return SLMirIntKind_I8;
    }
    if (SliceEqCStr(file->source, node->dataStart, node->dataEnd, "u16")) {
        return SLMirIntKind_U16;
    }
    if (SliceEqCStr(file->source, node->dataStart, node->dataEnd, "i16")) {
        return SLMirIntKind_I16;
    }
    if (SliceEqCStr(file->source, node->dataStart, node->dataEnd, "u32")) {
        return SLMirIntKind_U32;
    }
    if (SliceEqCStr(file->source, node->dataStart, node->dataEnd, "i32")
        || SliceEqCStr(file->source, node->dataStart, node->dataEnd, "uint")
        || SliceEqCStr(file->source, node->dataStart, node->dataEnd, "int"))
    {
        return SLMirIntKind_I32;
    }
    return SLMirIntKind_NONE;
}

static uint32_t ParseMirArrayLen(const SLParsedFile* file, const SLAstNode* node) {
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
    const SLMirProgram* program, const SLParsedFile* file, uint32_t defaultSourceRef) {
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

static const SLSymbolDecl* _Nullable FindPackageTypeDeclBySlice(
    const SLPackage* pkg, const char* src, uint32_t start, uint32_t end) {
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

static const SLAstNode* _Nullable ResolveMirTypeAliasTargetNode(
    const SLPackageLoader* loader,
    const SLMirProgram*    program,
    const SLMirTypeRef*    typeRef,
    const SLParsedFile** _Nullable outFile,
    uint32_t* _Nullable outSourceRef) {
    const SLPackage*    pkg = NULL;
    const SLParsedFile* file;
    const SLAstNode*    node;
    const SLSymbolDecl* decl;
    const SLParsedFile* declFile;
    uint32_t            sourceRef;
    int32_t             targetNode;
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
    if (node->kind != SLAst_TYPE_NAME) {
        return NULL;
    }
    decl = FindPackageTypeDeclBySlice(pkg, file->source, node->dataStart, node->dataEnd);
    if (decl == NULL || decl->kind != SLAst_TYPE_ALIAS || decl->nodeId < 0
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

static const SLAstNode* _Nullable ResolveMirEnumUnderlyingTypeNode(
    const SLPackageLoader* loader,
    const SLMirProgram*    program,
    const SLMirTypeRef*    typeRef,
    const SLParsedFile** _Nullable outFile,
    uint32_t* _Nullable outSourceRef) {
    const SLPackage*    pkg = NULL;
    const SLParsedFile* file;
    const SLAstNode*    node;
    const SLSymbolDecl* decl;
    const SLParsedFile* declFile;
    uint32_t            sourceRef;
    int32_t             underTypeNode;
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
    if (node->kind != SLAst_TYPE_NAME) {
        return NULL;
    }
    decl = FindPackageTypeDeclBySlice(pkg, file->source, node->dataStart, node->dataEnd);
    if (decl == NULL || decl->kind != SLAst_ENUM || decl->nodeId < 0
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
            declFile->ast.nodes[underTypeNode].kind == SLAst_TYPE_NAME
            || declFile->ast.nodes[underTypeNode].kind == SLAst_TYPE_PTR
            || declFile->ast.nodes[underTypeNode].kind == SLAst_TYPE_REF
            || declFile->ast.nodes[underTypeNode].kind == SLAst_TYPE_MUTREF
            || declFile->ast.nodes[underTypeNode].kind == SLAst_TYPE_ARRAY
            || declFile->ast.nodes[underTypeNode].kind == SLAst_TYPE_VARRAY
            || declFile->ast.nodes[underTypeNode].kind == SLAst_TYPE_SLICE
            || declFile->ast.nodes[underTypeNode].kind == SLAst_TYPE_MUTSLICE
            || declFile->ast.nodes[underTypeNode].kind == SLAst_TYPE_OPTIONAL
            || declFile->ast.nodes[underTypeNode].kind == SLAst_TYPE_FN
            || declFile->ast.nodes[underTypeNode].kind == SLAst_TYPE_ANON_STRUCT
            || declFile->ast.nodes[underTypeNode].kind == SLAst_TYPE_ANON_UNION
            || declFile->ast.nodes[underTypeNode].kind == SLAst_TYPE_TUPLE))
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

static const SLAstNode* _Nullable ResolveMirAggregateDeclNode(
    const SLPackageLoader* loader,
    const SLMirProgram*    program,
    const SLMirTypeRef*    typeRef,
    const SLParsedFile** _Nullable outFile,
    uint32_t* _Nullable outSourceRef) {
    const SLPackage*    pkg = NULL;
    const SLParsedFile* file;
    const SLAstNode*    node;
    const SLSymbolDecl* decl;
    const SLParsedFile* declFile;
    uint32_t            sourceRef;
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
    if (node->kind == SLAst_TYPE_ANON_STRUCT || node->kind == SLAst_TYPE_ANON_UNION) {
        if (outFile != NULL) {
            *outFile = file;
        }
        if (outSourceRef != NULL) {
            *outSourceRef = typeRef->sourceRef;
        }
        return node;
    }
    if (node->kind != SLAst_TYPE_NAME) {
        return NULL;
    }
    decl = FindPackageTypeDeclBySlice(pkg, file->source, node->dataStart, node->dataEnd);
    if (decl == NULL || decl->nodeId < 0 || (uint32_t)decl->fileIndex >= pkg->fileLen) {
        return NULL;
    }
    declFile = &pkg->files[decl->fileIndex];
    if ((decl->kind != SLAst_STRUCT && decl->kind != SLAst_UNION)
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

static uint32_t ClassifyMirTypeFlags(
    const SLPackageLoader* loader,
    const SLMirProgram*    program,
    const SLParsedFile*    file,
    const SLMirTypeRef*    typeRef) {
    uint32_t         flags = ClassifyMirScalarType(file, typeRef);
    const SLAstNode* node;
    const SLAstNode* child;
    if (file == NULL || typeRef == NULL || typeRef->astNode >= file->ast.len) {
        return flags;
    }
    node = &file->ast.nodes[typeRef->astNode];
    if (node->kind == SLAst_TYPE_OPTIONAL && node->firstChild >= 0
        && (uint32_t)node->firstChild < file->ast.len)
    {
        SLMirTypeRef childTypeRef = {
            .astNode = (uint32_t)node->firstChild,
            .sourceRef = typeRef->sourceRef,
            .flags = 0u,
            .aux = 0u,
        };
        return SLMirTypeFlag_OPTIONAL | ClassifyMirTypeFlags(loader, program, file, &childTypeRef);
    }
    if ((node->kind == SLAst_TYPE_REF || node->kind == SLAst_TYPE_PTR) && node->firstChild >= 0
        && (uint32_t)node->firstChild < file->ast.len)
    {
        SLMirTypeRef childTypeRef = {
            .astNode = (uint32_t)node->firstChild,
            .sourceRef = typeRef->sourceRef,
            .flags = 0u,
            .aux = 0u,
        };
        child = &file->ast.nodes[node->firstChild];
        if (child->kind == SLAst_TYPE_NAME) {
            if (SliceEqCStr(file->source, child->dataStart, child->dataEnd, "str")) {
                flags |=
                    node->kind == SLAst_TYPE_REF ? SLMirTypeFlag_STR_REF : SLMirTypeFlag_STR_PTR;
            } else if (SliceEqCStr(file->source, child->dataStart, child->dataEnd, "u8")) {
                flags |= SLMirTypeFlag_U8_PTR;
            } else if (SliceEqCStr(file->source, child->dataStart, child->dataEnd, "i8")) {
                flags |= SLMirTypeFlag_I8_PTR;
            } else if (SliceEqCStr(file->source, child->dataStart, child->dataEnd, "u16")) {
                flags |= SLMirTypeFlag_U16_PTR;
            } else if (SliceEqCStr(file->source, child->dataStart, child->dataEnd, "i16")) {
                flags |= SLMirTypeFlag_I16_PTR;
            } else if (SliceEqCStr(file->source, child->dataStart, child->dataEnd, "u32")) {
                flags |= SLMirTypeFlag_U32_PTR;
            } else if (
                SliceEqCStr(file->source, child->dataStart, child->dataEnd, "bool")
                || SliceEqCStr(file->source, child->dataStart, child->dataEnd, "i32")
                || SliceEqCStr(file->source, child->dataStart, child->dataEnd, "uint")
                || SliceEqCStr(file->source, child->dataStart, child->dataEnd, "int"))
            {
                flags |= SLMirTypeFlag_I32_PTR;
            } else if (
                loader != NULL && program != NULL
                && ResolveMirAggregateDeclNode(loader, program, &childTypeRef, NULL, NULL) != NULL)
            {
                flags |= SLMirTypeFlag_OPAQUE_PTR;
            }
        } else if (
            child->kind == SLAst_TYPE_ARRAY
            && ClassifyMirIntKindFromTypeNode(
                   file,
                   child->firstChild >= 0 && (uint32_t)child->firstChild < file->ast.len
                       ? &file->ast.nodes[child->firstChild]
                       : NULL)
                   != SLMirIntKind_NONE
            && ParseMirArrayLen(file, child) != 0u)
        {
            flags |= SLMirTypeFlag_FIXED_ARRAY_VIEW;
        } else if (
            child->kind == SLAst_TYPE_SLICE && child->firstChild >= 0
            && (uint32_t)child->firstChild < file->ast.len
            && (ClassifyMirIntKindFromTypeNode(file, &file->ast.nodes[child->firstChild])
                    != SLMirIntKind_NONE
                || (loader != NULL && program != NULL
                    && ResolveMirAggregateDeclNode(
                           loader,
                           program,
                           &(SLMirTypeRef){
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
                          != SLMirIntKind_NONE
                       ? SLMirTypeFlag_SLICE_VIEW
                       : SLMirTypeFlag_AGG_SLICE_VIEW;
        } else if (
            child->kind == SLAst_TYPE_VARRAY && child->firstChild >= 0
            && (uint32_t)child->firstChild < file->ast.len
            && (ClassifyMirIntKindFromTypeNode(file, &file->ast.nodes[child->firstChild])
                    != SLMirIntKind_NONE
                || (loader != NULL && program != NULL
                    && ResolveMirAggregateDeclNode(
                           loader,
                           program,
                           &(SLMirTypeRef){
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
                          != SLMirIntKind_NONE
                       ? SLMirTypeFlag_VARRAY_VIEW
                       : SLMirTypeFlag_AGG_SLICE_VIEW;
        }
    } else if (
        node->kind == SLAst_TYPE_ARRAY && node->firstChild >= 0
        && (uint32_t)node->firstChild < file->ast.len)
    {
        child = &file->ast.nodes[node->firstChild];
        if (ClassifyMirIntKindFromTypeNode(file, child) != SLMirIntKind_NONE
            && ParseMirArrayLen(file, node) != 0u)
        {
            flags |= SLMirTypeFlag_FIXED_ARRAY;
        }
    } else if (node->kind == SLAst_TYPE_FN) {
        flags |= SLMirTypeFlag_FUNC_REF;
    } else if (
        node->kind == SLAst_TYPE_VARRAY && node->firstChild >= 0
        && (uint32_t)node->firstChild < file->ast.len
        && ClassifyMirIntKindFromTypeNode(file, &file->ast.nodes[node->firstChild])
               != SLMirIntKind_NONE)
    {
        flags |= SLMirTypeFlag_VARRAY_VIEW;
    } else if (
        node->kind == SLAst_TYPE_NAME
        && SliceEqCStr(file->source, node->dataStart, node->dataEnd, "str"))
    {
        flags |= SLMirTypeFlag_STR_OBJ;
    } else if (
        node->kind == SLAst_TYPE_NAME
        && SliceEqCStr(file->source, node->dataStart, node->dataEnd, "rawptr"))
    {
        flags |= SLMirTypeFlag_OPAQUE_PTR;
    } else if (node->kind == SLAst_TYPE_NAME) {
        const SLParsedFile* aliasFile = NULL;
        uint32_t            aliasSourceRef = UINT32_MAX;
        const SLAstNode*    aliasTarget = ResolveMirTypeAliasTargetNode(
            loader, program, typeRef, &aliasFile, &aliasSourceRef);
        if (aliasTarget != NULL && aliasFile != NULL) {
            SLMirTypeRef aliasTypeRef = {
                .astNode = (uint32_t)(aliasTarget - aliasFile->ast.nodes),
                .sourceRef = aliasSourceRef,
                .flags = 0u,
                .aux = 0u,
            };
            flags |= ClassifyMirTypeFlags(loader, program, aliasFile, &aliasTypeRef);
        }
        if (flags == SLMirTypeScalar_NONE) {
            const SLParsedFile* enumFile = NULL;
            uint32_t            enumSourceRef = UINT32_MAX;
            const SLAstNode*    enumType = ResolveMirEnumUnderlyingTypeNode(
                loader, program, typeRef, &enumFile, &enumSourceRef);
            if (enumType != NULL && enumFile != NULL) {
                SLMirTypeRef enumTypeRef = {
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
        flags |= SLMirTypeFlag_AGGREGATE;
    }
    return flags;
}

static uint32_t ClassifyMirTypeAux(
    const SLPackageLoader* loader,
    const SLMirProgram*    program,
    const SLParsedFile*    file,
    const SLMirTypeRef*    typeRef) {
    const SLAstNode* node;
    const SLAstNode* child;
    SLMirIntKind     intKind;
    uint32_t         arrayCount;
    if (file == NULL || typeRef == NULL || typeRef->astNode >= file->ast.len) {
        return 0u;
    }
    node = &file->ast.nodes[typeRef->astNode];
    switch (node->kind) {
        case SLAst_TYPE_OPTIONAL:
            if (node->firstChild < 0 || (uint32_t)node->firstChild >= file->ast.len) {
                return 0u;
            }
            return ClassifyMirTypeAux(
                loader,
                program,
                file,
                &(SLMirTypeRef){
                    .astNode = (uint32_t)node->firstChild,
                    .sourceRef = typeRef->sourceRef,
                    .flags = 0u,
                    .aux = 0u,
                });
        case SLAst_TYPE_NAME:
            intKind = ClassifyMirIntKindFromTypeNode(file, node);
            if (intKind != SLMirIntKind_NONE) {
                return SLMirTypeAuxMakeScalarInt(intKind);
            }
            if (loader != NULL && program != NULL) {
                const SLParsedFile* aliasFile = NULL;
                uint32_t            aliasSourceRef = UINT32_MAX;
                const SLAstNode*    aliasTarget = ResolveMirTypeAliasTargetNode(
                    loader, program, typeRef, &aliasFile, &aliasSourceRef);
                if (aliasTarget != NULL && aliasFile != NULL) {
                    return ClassifyMirTypeAux(
                        loader,
                        program,
                        aliasFile,
                        &(SLMirTypeRef){
                            .astNode = (uint32_t)(aliasTarget - aliasFile->ast.nodes),
                            .sourceRef = aliasSourceRef,
                            .flags = 0u,
                            .aux = 0u,
                        });
                }
                {
                    const SLParsedFile* enumFile = NULL;
                    uint32_t            enumSourceRef = UINT32_MAX;
                    const SLAstNode*    enumType = ResolveMirEnumUnderlyingTypeNode(
                        loader, program, typeRef, &enumFile, &enumSourceRef);
                    if (enumType != NULL && enumFile != NULL) {
                        return ClassifyMirTypeAux(
                            loader,
                            program,
                            enumFile,
                            &(SLMirTypeRef){
                                .astNode = (uint32_t)(enumType - enumFile->ast.nodes),
                                .sourceRef = enumSourceRef,
                                .flags = 0u,
                                .aux = 0u,
                            });
                    }
                }
            }
            return 0u;
        case SLAst_TYPE_PTR:
        case SLAst_TYPE_REF:
        case SLAst_TYPE_MUTREF:
            if (node->firstChild < 0 || (uint32_t)node->firstChild >= file->ast.len) {
                return 0u;
            }
            child = &file->ast.nodes[node->firstChild];
            if (child->kind == SLAst_TYPE_ARRAY && child->firstChild >= 0
                && (uint32_t)child->firstChild < file->ast.len)
            {
                intKind = ClassifyMirIntKindFromTypeNode(file, &file->ast.nodes[child->firstChild]);
                arrayCount = ParseMirArrayLen(file, child);
                if (intKind != SLMirIntKind_NONE && arrayCount != 0u) {
                    return SLMirTypeAuxMakeFixedArray(intKind, arrayCount);
                }
                return 0u;
            }
            if (child->kind == SLAst_TYPE_SLICE && child->firstChild >= 0
                && (uint32_t)child->firstChild < file->ast.len)
            {
                intKind = ClassifyMirIntKindFromTypeNode(file, &file->ast.nodes[child->firstChild]);
                if (intKind != SLMirIntKind_NONE) {
                    return SLMirTypeAuxMakeScalarInt(intKind);
                }
                return SLMirTypeAuxMakeAggSliceView(UINT32_MAX);
            }
            if (child->kind == SLAst_TYPE_VARRAY && child->firstChild >= 0
                && (uint32_t)child->firstChild < file->ast.len)
            {
                intKind = ClassifyMirIntKindFromTypeNode(file, &file->ast.nodes[child->firstChild]);
                if (intKind != SLMirIntKind_NONE) {
                    return SLMirTypeAuxMakeScalarInt(intKind);
                }
                return SLMirTypeAuxMakeAggSliceView(UINT32_MAX);
            }
            intKind = ClassifyMirIntKindFromTypeNode(file, child);
            return intKind != SLMirIntKind_NONE ? SLMirTypeAuxMakeScalarInt(intKind) : 0u;
        case SLAst_TYPE_ARRAY:
            if (node->firstChild < 0 || (uint32_t)node->firstChild >= file->ast.len) {
                return 0u;
            }
            child = &file->ast.nodes[node->firstChild];
            intKind = ClassifyMirIntKindFromTypeNode(file, child);
            arrayCount = ParseMirArrayLen(file, node);
            if (intKind == SLMirIntKind_NONE || arrayCount == 0u) {
                return 0u;
            }
            return SLMirTypeAuxMakeFixedArray(intKind, arrayCount);
        case SLAst_TYPE_VARRAY:
            if (node->firstChild < 0 || (uint32_t)node->firstChild >= file->ast.len) {
                return 0u;
            }
            intKind = ClassifyMirIntKindFromTypeNode(file, &file->ast.nodes[node->firstChild]);
            return intKind != SLMirIntKind_NONE ? SLMirTypeAuxMakeScalarInt(intKind) : 0u;
        default: return 0u;
    }
}

static void EnrichMirTypeFlags(const SLPackageLoader* loader, SLMirProgram* program) {
    uint32_t i;
    if (loader == NULL || program == NULL || program->types == NULL) {
        return;
    }
    for (i = 0; i < program->typeLen; i++) {
        SLMirTypeRef*       typeRef = (SLMirTypeRef*)&program->types[i];
        const SLParsedFile* file = FindLoaderFileByMirSource(
            loader, program, typeRef->sourceRef, NULL);
        if (typeRef->astNode == UINT32_MAX && typeRef->sourceRef == UINT32_MAX) {
            continue;
        }
        typeRef->flags =
            (typeRef->flags
             & ~(SLMirTypeFlag_SCALAR_MASK | SLMirTypeFlag_STR_REF | SLMirTypeFlag_STR_PTR
                 | SLMirTypeFlag_STR_OBJ | SLMirTypeFlag_U8_PTR | SLMirTypeFlag_I32_PTR
                 | SLMirTypeFlag_I8_PTR | SLMirTypeFlag_U16_PTR | SLMirTypeFlag_I16_PTR
                 | SLMirTypeFlag_U32_PTR | SLMirTypeFlag_FIXED_ARRAY
                 | SLMirTypeFlag_FIXED_ARRAY_VIEW | SLMirTypeFlag_SLICE_VIEW
                 | SLMirTypeFlag_VARRAY_VIEW | SLMirTypeFlag_AGG_SLICE_VIEW
                 | SLMirTypeFlag_AGGREGATE | SLMirTypeFlag_OPAQUE_PTR | SLMirTypeFlag_OPTIONAL
                 | SLMirTypeFlag_FUNC_REF))
            | ClassifyMirTypeFlags(loader, program, file, typeRef);
        typeRef->aux = ClassifyMirTypeAux(loader, program, file, typeRef);
    }
}

static int EnsureMirAstTypeRef(
    SLArena*               arena,
    const SLPackageLoader* loader,
    SLMirProgram*          program,
    uint32_t               astNode,
    uint32_t               sourceRef,
    uint32_t* _Nonnull outTypeRef) {
    uint32_t            i;
    SLMirTypeRef*       newTypes;
    const SLParsedFile* file;
    if (arena == NULL || loader == NULL || program == NULL || outTypeRef == NULL) {
        return -1;
    }
    for (i = 0; i < program->typeLen; i++) {
        if (program->types[i].astNode == astNode && program->types[i].sourceRef == sourceRef) {
            *outTypeRef = i;
            return 0;
        }
    }
    newTypes = (SLMirTypeRef*)SLArenaAlloc(
        arena, sizeof(SLMirTypeRef) * (program->typeLen + 1u), (uint32_t)_Alignof(SLMirTypeRef));
    if (newTypes == NULL) {
        return -1;
    }
    if (program->typeLen != 0u) {
        memcpy(newTypes, program->types, sizeof(SLMirTypeRef) * program->typeLen);
    }
    newTypes[program->typeLen] =
        (SLMirTypeRef){ .astNode = astNode, .sourceRef = sourceRef, .flags = 0u, .aux = 0u };
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
    SLArena*      arena,
    SLMirProgram* program,
    uint32_t      nameStart,
    uint32_t      nameEnd,
    uint32_t      sourceRef,
    uint32_t      ownerTypeRef,
    uint32_t      typeRef,
    uint32_t* _Nonnull outFieldRef) {
    uint32_t    i;
    SLMirField* newFields;
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
    newFields = (SLMirField*)SLArenaAlloc(
        arena, sizeof(SLMirField) * (program->fieldLen + 1u), (uint32_t)_Alignof(SLMirField));
    if (newFields == NULL) {
        return -1;
    }
    if (program->fieldLen != 0u) {
        memcpy(newFields, program->fields, sizeof(SLMirField) * program->fieldLen);
    }
    newFields[program->fieldLen] = (SLMirField){
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
    const SLPackageLoader* loader, SLArena* arena, SLMirProgram* program, uint32_t ownerTypeRef) {
    const SLParsedFile* file = NULL;
    const SLParsedFile* ownerFile = NULL;
    const SLAstNode*    declNode;
    const SLAstNode*    ownerNode = NULL;
    uint32_t            ownerSourceRef = UINT32_MAX;
    int32_t             childNode;
    if (loader == NULL || arena == NULL || program == NULL || ownerTypeRef >= program->typeLen) {
        return -1;
    }
    declNode = ResolveMirAggregateDeclNode(
        loader, program, &program->types[ownerTypeRef], &file, &ownerSourceRef);
    ownerFile = FindLoaderFileByMirSource(
        loader, program, program->types[ownerTypeRef].sourceRef, NULL);
    if (ownerFile != NULL && program->types[ownerTypeRef].astNode < ownerFile->ast.len) {
        ownerNode = &ownerFile->ast.nodes[program->types[ownerTypeRef].astNode];
    }
    if (declNode == NULL || file == NULL) {
        return 0;
    }
    ((SLMirTypeRef*)&program->types[ownerTypeRef])->flags |= SLMirTypeFlag_AGGREGATE;
    childNode = declNode->firstChild;
    while (childNode >= 0 && (uint32_t)childNode < file->ast.len) {
        const SLAstNode* fieldNode = &file->ast.nodes[childNode];
        uint32_t         fieldTypeRef = UINT32_MAX;
        uint32_t         fieldRef = UINT32_MAX;
        int32_t          typeNode = fieldNode->firstChild;
        uint32_t         typeSourceRef = ownerSourceRef;
        if (fieldNode->kind != SLAst_FIELD) {
            childNode = fieldNode->nextSibling;
            continue;
        }
        if (typeNode < 0 || (uint32_t)typeNode >= file->ast.len) {
            return -1;
        }
        if (ownerFile != NULL && ownerNode != NULL && ownerNode->kind == SLAst_TYPE_NAME
            && file->ast.nodes[typeNode].kind == SLAst_TYPE_NAME)
        {
            int32_t typeParamNode = declNode->firstChild;
            int32_t typeArgNode = ownerNode->firstChild;
            while (typeParamNode >= 0 && typeArgNode >= 0) {
                const SLAstNode* param = &file->ast.nodes[typeParamNode];
                if (param->kind == SLAst_TYPE_PARAM
                    && SLNameEqSlice(
                        (SLStrView){ file->source, file->sourceLen },
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
    const SLPackageLoader* loader, SLArena* arena, SLMirProgram* program) {
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

static int EnrichMirVArrayCountFields(const SLPackageLoader* loader, SLMirProgram* program) {
    uint32_t i;
    if (loader == NULL || program == NULL) {
        return -1;
    }
    for (i = 0; i < program->fieldLen; i++) {
        SLMirTypeRef*       typeRef;
        const SLParsedFile* file;
        const SLAstNode*    typeNode;
        uint32_t            countFieldRef = UINT32_MAX;
        if (program->fields[i].typeRef >= program->typeLen) {
            continue;
        }
        typeRef = (SLMirTypeRef*)&program->types[program->fields[i].typeRef];
        if (!SLMirTypeRefIsVArrayView(typeRef)) {
            continue;
        }
        file = FindLoaderFileByMirSource(loader, program, typeRef->sourceRef, NULL);
        if (file == NULL || typeRef->astNode >= file->ast.len) {
            continue;
        }
        typeNode = &file->ast.nodes[typeRef->astNode];
        if (typeNode->kind != SLAst_TYPE_VARRAY
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
        typeRef->aux = SLMirTypeAuxMakeVArrayView(SLMirTypeRefIntKind(typeRef), countFieldRef);
    }
    return 0;
}

static int EnrichMirAggSliceElemTypes(
    const SLPackageLoader* loader, SLArena* arena, SLMirProgram* program) {
    uint32_t i = 0;
    if (loader == NULL || arena == NULL || program == NULL) {
        return -1;
    }
    while (i < program->typeLen) {
        SLMirTypeRef*       typeRef = (SLMirTypeRef*)&program->types[i];
        const SLParsedFile* file;
        const SLAstNode*    node;
        const SLAstNode*    child;
        uint32_t            elemTypeRef = UINT32_MAX;
        uint32_t            sourceRef;
        if (!SLMirTypeRefIsAggSliceView(typeRef)) {
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
        if ((node->kind != SLAst_TYPE_PTR && node->kind != SLAst_TYPE_REF
             && node->kind != SLAst_TYPE_MUTREF)
            || node->firstChild < 0 || (uint32_t)node->firstChild >= file->ast.len)
        {
            i++;
            continue;
        }
        child = &file->ast.nodes[node->firstChild];
        if ((child->kind != SLAst_TYPE_SLICE && child->kind != SLAst_TYPE_VARRAY)
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
        typeRef = (SLMirTypeRef*)&program->types[i];
        typeRef->aux = SLMirTypeAuxMakeAggSliceView(elemTypeRef);
        if (EnsureMirAggregateFieldsForType(loader, arena, program, elemTypeRef) != 0) {
            return -1;
        }
        i++;
    }
    return 0;
}

static int EnrichMirOpaquePtrPointees(
    const SLPackageLoader* loader, SLArena* arena, SLMirProgram* program) {
    uint32_t i = 0;
    if (loader == NULL || arena == NULL || program == NULL) {
        return -1;
    }
    while (i < program->typeLen) {
        SLMirTypeRef*       typeRef = (SLMirTypeRef*)&program->types[i];
        const SLParsedFile* file;
        const SLAstNode*    node;
        SLMirTypeRef        childTypeRef;
        uint32_t            pointeeTypeRef = UINT32_MAX;
        uint32_t            sourceRef;
        if (!SLMirTypeRefIsOpaquePtr(typeRef)) {
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
        if (node->kind != SLAst_TYPE_PTR || node->firstChild < 0
            || (uint32_t)node->firstChild >= file->ast.len)
        {
            i++;
            continue;
        }
        childTypeRef = (SLMirTypeRef){
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
        typeRef = (SLMirTypeRef*)&program->types[i];
        typeRef->aux = pointeeTypeRef;
        if (EnsureMirAggregateFieldsForType(loader, arena, program, pointeeTypeRef) != 0) {
            return -1;
        }
        i++;
    }
    return 0;
}

static int EnsureMirScalarTypeRef(
    SLArena* arena, SLMirProgram* program, SLMirTypeScalar scalar, uint32_t* _Nonnull outTypeRef) {
    uint32_t      i;
    SLMirTypeRef* newTypes;
    if (arena == NULL || program == NULL || outTypeRef == NULL || scalar == SLMirTypeScalar_NONE) {
        return -1;
    }
    for (i = 0; i < program->typeLen; i++) {
        if (SLMirTypeRefScalarKind(&program->types[i]) == scalar) {
            *outTypeRef = i;
            return 0;
        }
    }
    newTypes = (SLMirTypeRef*)SLArenaAlloc(
        arena, sizeof(SLMirTypeRef) * (program->typeLen + 1u), (uint32_t)_Alignof(SLMirTypeRef));
    if (newTypes == NULL) {
        return -1;
    }
    if (program->typeLen != 0u) {
        memcpy(newTypes, program->types, sizeof(SLMirTypeRef) * program->typeLen);
    }
    newTypes[program->typeLen] = (SLMirTypeRef){
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

static MirInferredType MirInferredTypeFromTypeRef(const SLMirTypeRef* typeRef) {
    uint32_t flags;
    if (typeRef == NULL) {
        return MirInferredType_NONE;
    }
    flags = typeRef->flags;
    if ((flags & SLMirTypeFlag_STR_REF) != 0) {
        return MirInferredType_STR_REF;
    }
    if ((flags & SLMirTypeFlag_STR_PTR) != 0) {
        return MirInferredType_STR_PTR;
    }
    if ((flags & SLMirTypeFlag_STR_OBJ) != 0) {
        return MirInferredType_STR_PTR;
    }
    if ((flags & SLMirTypeFlag_U8_PTR) != 0) {
        return MirInferredType_U8_PTR;
    }
    if ((flags & SLMirTypeFlag_I8_PTR) != 0) {
        return MirInferredType_I8_PTR;
    }
    if ((flags & SLMirTypeFlag_U16_PTR) != 0) {
        return MirInferredType_U16_PTR;
    }
    if ((flags & SLMirTypeFlag_I16_PTR) != 0) {
        return MirInferredType_I16_PTR;
    }
    if ((flags & SLMirTypeFlag_U32_PTR) != 0) {
        return MirInferredType_U32_PTR;
    }
    if ((flags & SLMirTypeFlag_I32_PTR) != 0) {
        return MirInferredType_I32_PTR;
    }
    if ((flags & SLMirTypeFlag_OPAQUE_PTR) != 0) {
        return MirInferredType_OPAQUE_PTR;
    }
    if ((flags & SLMirTypeFlag_FUNC_REF) != 0) {
        return MirInferredType_FUNC_REF;
    }
    if ((flags & (SLMirTypeFlag_FIXED_ARRAY | SLMirTypeFlag_FIXED_ARRAY_VIEW)) != 0) {
        switch (SLMirTypeRefIntKind(typeRef)) {
            case SLMirIntKind_U8:   return MirInferredType_ARRAY_U8;
            case SLMirIntKind_I8:   return MirInferredType_ARRAY_I8;
            case SLMirIntKind_U16:  return MirInferredType_ARRAY_U16;
            case SLMirIntKind_I16:  return MirInferredType_ARRAY_I16;
            case SLMirIntKind_U32:  return MirInferredType_ARRAY_U32;
            case SLMirIntKind_BOOL:
            case SLMirIntKind_I32:  return MirInferredType_ARRAY_I32;
            default:                return MirInferredType_NONE;
        }
    }
    if ((flags & (SLMirTypeFlag_SLICE_VIEW | SLMirTypeFlag_VARRAY_VIEW)) != 0) {
        switch (SLMirTypeRefIntKind(typeRef)) {
            case SLMirIntKind_U8:   return MirInferredType_SLICE_U8;
            case SLMirIntKind_I8:   return MirInferredType_SLICE_I8;
            case SLMirIntKind_U16:  return MirInferredType_SLICE_U16;
            case SLMirIntKind_I16:  return MirInferredType_SLICE_I16;
            case SLMirIntKind_U32:  return MirInferredType_SLICE_U32;
            case SLMirIntKind_BOOL:
            case SLMirIntKind_I32:  return MirInferredType_SLICE_I32;
            default:                return MirInferredType_NONE;
        }
    }
    if ((flags & SLMirTypeFlag_AGG_SLICE_VIEW) != 0) {
        return MirInferredType_SLICE_AGG;
    }
    if ((flags & SLMirTypeFlag_AGGREGATE) != 0) {
        return MirInferredType_AGG;
    }
    switch ((SLMirTypeScalar)(flags & SLMirTypeFlag_SCALAR_MASK)) {
        case SLMirTypeScalar_I32: return MirInferredType_I32;
        case SLMirTypeScalar_I64: return MirInferredType_I64;
        case SLMirTypeScalar_F32: return MirInferredType_F32;
        case SLMirTypeScalar_F64: return MirInferredType_F64;
        default:                  return MirInferredType_NONE;
    }
}

static MirInferredType MirProgramTypeKind(const SLMirProgram* program, uint32_t typeRefIndex) {
    if (program == NULL || typeRefIndex == UINT32_MAX || typeRefIndex >= program->typeLen) {
        return MirInferredType_NONE;
    }
    return MirInferredTypeFromTypeRef(&program->types[typeRefIndex]);
}

static MirInferredType MirConstTypeKind(const SLMirConst* value) {
    if (value == NULL) {
        return MirInferredType_NONE;
    }
    switch (value->kind) {
        case SLMirConst_INT:
        case SLMirConst_BOOL:
        case SLMirConst_NULL:     return MirInferredType_I32;
        case SLMirConst_STRING:   return MirInferredType_STR_REF;
        case SLMirConst_FUNCTION: return MirInferredType_FUNC_REF;
        default:                  return MirInferredType_NONE;
    }
}

static MirInferredType MirFunctionResultTypeKind(
    const SLMirProgram* program, uint32_t functionIndex) {
    if (program == NULL || functionIndex >= program->funcLen) {
        return MirInferredType_NONE;
    }
    return MirProgramTypeKind(program, program->funcs[functionIndex].typeRef);
}

static int EnsureMirStrRefTypeRef(
    SLArena* arena, SLMirProgram* program, uint32_t* _Nonnull outTypeRef) {
    uint32_t      i;
    SLMirTypeRef* newTypes;
    if (arena == NULL || program == NULL || outTypeRef == NULL) {
        return -1;
    }
    for (i = 0; i < program->typeLen; i++) {
        if (SLMirTypeRefIsStrRef(&program->types[i])) {
            *outTypeRef = i;
            return 0;
        }
    }
    newTypes = (SLMirTypeRef*)SLArenaAlloc(
        arena, sizeof(SLMirTypeRef) * (program->typeLen + 1u), (uint32_t)_Alignof(SLMirTypeRef));
    if (newTypes == NULL) {
        return -1;
    }
    if (program->typeLen != 0u) {
        memcpy(newTypes, program->types, sizeof(SLMirTypeRef) * program->typeLen);
    }
    newTypes[program->typeLen] = (SLMirTypeRef){
        .astNode = UINT32_MAX,
        .sourceRef = 0,
        .flags = SLMirTypeFlag_STR_REF,
        .aux = 0,
    };
    program->types = newTypes;
    *outTypeRef = program->typeLen++;
    return 0;
}

static int EnsureMirFlaggedTypeRef(
    SLArena* arena, SLMirProgram* program, uint32_t flags, uint32_t* _Nonnull outTypeRef) {
    uint32_t      i;
    SLMirTypeRef* newTypes;
    if (arena == NULL || program == NULL || outTypeRef == NULL || flags == 0u) {
        return -1;
    }
    for (i = 0; i < program->typeLen; i++) {
        if (program->types[i].flags == flags) {
            *outTypeRef = i;
            return 0;
        }
    }
    newTypes = (SLMirTypeRef*)SLArenaAlloc(
        arena, sizeof(SLMirTypeRef) * (program->typeLen + 1u), (uint32_t)_Alignof(SLMirTypeRef));
    if (newTypes == NULL) {
        return -1;
    }
    if (program->typeLen != 0u) {
        memcpy(newTypes, program->types, sizeof(SLMirTypeRef) * program->typeLen);
    }
    newTypes[program->typeLen] =
        (SLMirTypeRef){ .astNode = UINT32_MAX, .sourceRef = 0, .flags = flags, .aux = 0 };
    program->types = newTypes;
    *outTypeRef = program->typeLen++;
    return 0;
}

static int EnsureMirFunctionRefTypeRef(
    SLArena* arena, SLMirProgram* program, uint32_t functionIndex, uint32_t* _Nonnull outTypeRef) {
    uint32_t      i;
    SLMirTypeRef* newTypes;
    uint32_t      aux;
    if (arena == NULL || program == NULL || outTypeRef == NULL || functionIndex >= program->funcLen)
    {
        return -1;
    }
    aux = functionIndex + 1u;
    for (i = 0; i < program->typeLen; i++) {
        if ((program->types[i].flags & SLMirTypeFlag_FUNC_REF) != 0u
            && program->types[i].aux == aux)
        {
            *outTypeRef = i;
            return 0;
        }
    }
    newTypes = (SLMirTypeRef*)SLArenaAlloc(
        arena, sizeof(SLMirTypeRef) * (program->typeLen + 1u), (uint32_t)_Alignof(SLMirTypeRef));
    if (newTypes == NULL) {
        return -1;
    }
    if (program->typeLen != 0u) {
        memcpy(newTypes, program->types, sizeof(SLMirTypeRef) * program->typeLen);
    }
    newTypes[program->typeLen] = (SLMirTypeRef){
        .astNode = UINT32_MAX,
        .sourceRef = 0,
        .flags = SLMirTypeFlag_FUNC_REF,
        .aux = aux,
    };
    program->types = newTypes;
    *outTypeRef = program->typeLen++;
    return 0;
}

static int EnsureMirInferredTypeRef(
    SLArena* arena, SLMirProgram* program, MirInferredType type, uint32_t* _Nonnull outTypeRef) {
    switch (type) {
        case MirInferredType_I32:
            return EnsureMirScalarTypeRef(arena, program, SLMirTypeScalar_I32, outTypeRef);
        case MirInferredType_I64:
            return EnsureMirScalarTypeRef(arena, program, SLMirTypeScalar_I64, outTypeRef);
        case MirInferredType_F32:
            return EnsureMirScalarTypeRef(arena, program, SLMirTypeScalar_F32, outTypeRef);
        case MirInferredType_F64:
            return EnsureMirScalarTypeRef(arena, program, SLMirTypeScalar_F64, outTypeRef);
        case MirInferredType_STR_REF: return EnsureMirStrRefTypeRef(arena, program, outTypeRef);
        case MirInferredType_STR_PTR:
            return EnsureMirFlaggedTypeRef(arena, program, SLMirTypeFlag_STR_PTR, outTypeRef);
        case MirInferredType_U8_PTR:
            return EnsureMirFlaggedTypeRef(arena, program, SLMirTypeFlag_U8_PTR, outTypeRef);
        case MirInferredType_I8_PTR:
            return EnsureMirFlaggedTypeRef(arena, program, SLMirTypeFlag_I8_PTR, outTypeRef);
        case MirInferredType_U16_PTR:
            return EnsureMirFlaggedTypeRef(arena, program, SLMirTypeFlag_U16_PTR, outTypeRef);
        case MirInferredType_I16_PTR:
            return EnsureMirFlaggedTypeRef(arena, program, SLMirTypeFlag_I16_PTR, outTypeRef);
        case MirInferredType_U32_PTR:
            return EnsureMirFlaggedTypeRef(arena, program, SLMirTypeFlag_U32_PTR, outTypeRef);
        case MirInferredType_I32_PTR:
            return EnsureMirFlaggedTypeRef(arena, program, SLMirTypeFlag_I32_PTR, outTypeRef);
        case MirInferredType_OPAQUE_PTR:
            return EnsureMirFlaggedTypeRef(arena, program, SLMirTypeFlag_OPAQUE_PTR, outTypeRef);
        case MirInferredType_FUNC_REF:
            return EnsureMirFlaggedTypeRef(arena, program, SLMirTypeFlag_FUNC_REF, outTypeRef);
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

static void RewriteMirAggregateMake(SLMirProgram* program) {
    uint32_t funcIndex;
    if (program == NULL) {
        return;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const SLMirFunction* fn = &program->funcs[funcIndex];
        uint32_t             pc;
        for (pc = 0; pc < fn->instLen; pc++) {
            SLMirInst* inst = (SLMirInst*)&program->insts[fn->instStart + pc];
            uint32_t   typeRef = UINT32_MAX;
            uint32_t   scanPc;
            int32_t    depth = 1;
            if (inst->op == SLMirOp_AGG_ZERO && fn->typeRef < program->typeLen
                && SLMirTypeRefIsAggregate(&program->types[fn->typeRef]))
            {
                for (scanPc = pc + 1u; scanPc < fn->instLen; scanPc++) {
                    SLMirInst* next = (SLMirInst*)&program->insts[fn->instStart + scanPc];
                    if (next->op == SLMirOp_LOCAL_STORE) {
                        break;
                    }
                    if (next->op == SLMirOp_COERCE && next->aux == inst->aux) {
                        next->aux = fn->typeRef;
                        continue;
                    }
                    if (next->op == SLMirOp_RETURN) {
                        inst->aux = fn->typeRef;
                        break;
                    }
                }
            }
            if (inst->op != SLMirOp_AGG_MAKE) {
                continue;
            }
            for (scanPc = pc + 1u; scanPc < fn->instLen && depth > 0; scanPc++) {
                const SLMirInst* next = &program->insts[fn->instStart + scanPc];
                int32_t          delta = 0;
                if (depth == 1) {
                    if (next->op == SLMirOp_COERCE && next->aux < program->typeLen
                        && SLMirTypeRefIsAggregate(&program->types[next->aux]))
                    {
                        typeRef = next->aux;
                        break;
                    }
                    if (next->op == SLMirOp_RETURN && fn->typeRef < program->typeLen
                        && SLMirTypeRefIsAggregate(&program->types[fn->typeRef]))
                    {
                        typeRef = fn->typeRef;
                        break;
                    }
                    if (next->op == SLMirOp_LOCAL_STORE && next->aux < fn->localCount) {
                        uint32_t localTypeRef = program->locals[fn->localStart + next->aux].typeRef;
                        if (localTypeRef < program->typeLen
                            && SLMirTypeRefIsAggregate(&program->types[localTypeRef]))
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
                inst->op = SLMirOp_AGG_ZERO;
                inst->tok = 0u;
                inst->aux = typeRef;
            }
        }
    }
}

static void InferMirStraightLineLocalTypes(SLArena* arena, SLMirProgram* program) {
    uint32_t funcIndex;
    if (arena == NULL || program == NULL) {
        return;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const SLMirFunction* fn = &program->funcs[funcIndex];
        MirInferredType      localTypes[256] = { 0 };
        MirInferredType      stackTypes[512] = { 0 };
        uint32_t             localTypeRefs[256];
        uint32_t             stackTypeRefs[512];
        uint32_t             stackLen = 0;
        uint32_t             localIndex;
        uint32_t             pc;
        int                  supported = 1;
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
            const SLMirLocal* local = &program->locals[fn->localStart + localIndex];
            localTypes[localIndex] = MirProgramTypeKind(program, local->typeRef);
            localTypeRefs[localIndex] = local->typeRef;
        }
        for (pc = 0; pc < fn->instLen && supported; pc++) {
            const SLMirInst* inst = &program->insts[fn->instStart + pc];
            switch (inst->op) {
                case SLMirOp_PUSH_CONST:
                    if (inst->aux >= program->constLen || stackLen >= 512u) {
                        supported = 0;
                        break;
                    }
                    stackTypes[stackLen] = MirConstTypeKind(&program->consts[inst->aux]);
                    stackTypeRefs[stackLen] = UINT32_MAX;
                    if (program->consts[inst->aux].kind == SLMirConst_FUNCTION
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
                case SLMirOp_AGG_ZERO:
                    if (inst->aux >= program->typeLen || stackLen >= 512u) {
                        supported = 0;
                        break;
                    }
                    stackTypes[stackLen] = MirInferredType_AGG;
                    stackTypeRefs[stackLen++] = inst->aux;
                    break;
                case SLMirOp_LOCAL_ZERO:
                    if (inst->aux >= fn->localCount) {
                        supported = 0;
                    }
                    break;
                case SLMirOp_CTX_GET:
                    if (stackLen >= 512u) {
                        supported = 0;
                        break;
                    }
                    stackTypes[stackLen] =
                        (inst->aux == SLMirContextField_MEM
                         || inst->aux == SLMirContextField_TEMP_MEM)
                            ? MirInferredType_OPAQUE_PTR
                            : MirInferredType_NONE;
                    stackTypeRefs[stackLen++] = UINT32_MAX;
                    break;
                case SLMirOp_LOCAL_LOAD:
                    if (inst->aux >= fn->localCount || stackLen >= 512u) {
                        supported = 0;
                        break;
                    }
                    stackTypes[stackLen] = localTypes[inst->aux];
                    stackTypeRefs[stackLen++] = localTypeRefs[inst->aux];
                    break;
                case SLMirOp_LOCAL_ADDR:
                    if (inst->aux >= fn->localCount || stackLen >= 512u) {
                        supported = 0;
                        break;
                    }
                    stackTypes[stackLen] = localTypes[inst->aux];
                    stackTypeRefs[stackLen++] = localTypeRefs[inst->aux];
                    break;
                case SLMirOp_LOCAL_STORE: {
                    SLMirLocal*     local;
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
                                    || !SLMirTypeRefIsAggregate(
                                        &program->types[localTypeRefs[inst->aux]])
                                    || !SLMirTypeRefIsAggregate(&program->types[typeRef]))))
                        {
                            supported = 0;
                            break;
                        }
                        localTypes[inst->aux] = MirInferredType_AGG;
                        if (typeRef != UINT32_MAX) {
                            localTypeRefs[inst->aux] = typeRef;
                        }
                        local = (SLMirLocal*)&program->locals[fn->localStart + inst->aux];
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
                    local = (SLMirLocal*)&program->locals[fn->localStart + inst->aux];
                    if (srcType == MirInferredType_FUNC_REF && typeRef != UINT32_MAX
                        && local->typeRef < program->typeLen
                        && SLMirTypeRefIsFuncRef(&program->types[local->typeRef])
                        && SLMirTypeRefFuncRefFunctionIndex(&program->types[local->typeRef])
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
                case SLMirOp_AGG_SET:
                    if (stackLen < 2u || stackTypes[stackLen - 2u] != MirInferredType_AGG) {
                        supported = 0;
                        break;
                    }
                    stackLen--;
                    break;
                case SLMirOp_UNARY:
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
                case SLMirOp_BINARY:
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
                        switch ((SLTokenKind)inst->tok) {
                            case SLTok_EQ:
                            case SLTok_NEQ:
                            case SLTok_LT:
                            case SLTok_GT:
                            case SLTok_LTE:
                            case SLTok_GTE:
                            case SLTok_LOGICAL_AND:
                            case SLTok_LOGICAL_OR:
                                stackTypes[stackLen - 1u] = MirInferredType_I32;
                                stackTypeRefs[stackLen - 1u] = UINT32_MAX;
                                break;
                            default: stackTypes[stackLen - 1u] = lhsType; break;
                        }
                    }
                    break;
                case SLMirOp_CAST:
                case SLMirOp_COERCE:
                    if (stackLen == 0u) {
                        supported = 0;
                        break;
                    }
                    stackTypes[stackLen - 1u] = MirProgramTypeKind(program, inst->aux);
                    stackTypeRefs[stackLen - 1u] = inst->aux;
                    break;
                case SLMirOp_SEQ_LEN:
                    if (stackLen == 0u) {
                        supported = 0;
                        break;
                    }
                    stackTypes[stackLen - 1u] = MirInferredType_I32;
                    stackTypeRefs[stackLen - 1u] = UINT32_MAX;
                    break;
                case SLMirOp_STR_CSTR:
                    if (stackLen == 0u) {
                        supported = 0;
                        break;
                    }
                    stackTypes[stackLen - 1u] = MirInferredType_U8_PTR;
                    stackTypeRefs[stackLen - 1u] = UINT32_MAX;
                    break;
                case SLMirOp_CALL_FN: {
                    uint32_t argc = SLMirCallArgCountFromTok(inst->tok);
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
                case SLMirOp_ARRAY_ADDR:
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
                                    ? SLMirTypeRefAggSliceElemTypeRef(
                                          &program->types[stackTypeRefs[stackLen - 1u]])
                                    : UINT32_MAX;
                            if (stackTypeRefs[stackLen - 1u] == UINT32_MAX) {
                                supported = 0;
                            }
                            break;
                        default: supported = 0; break;
                    }
                    break;
                case SLMirOp_DEREF_LOAD:
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
                case SLMirOp_DEREF_STORE:
                    if (stackLen < 2u) {
                        supported = 0;
                        break;
                    }
                    stackLen -= 2u;
                    break;
                case SLMirOp_SLICE_MAKE:
                    if (stackLen == 0u) {
                        supported = 0;
                        break;
                    }
                    if ((inst->tok & SLAstFlag_INDEX_HAS_END) != 0u) {
                        if (stackLen == 0u || stackTypes[stackLen - 1u] != MirInferredType_I32) {
                            supported = 0;
                            break;
                        }
                        stackLen--;
                    }
                    if ((inst->tok & SLAstFlag_INDEX_HAS_START) != 0u) {
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
                case SLMirOp_INDEX:
                    if (stackLen < 2u) {
                        supported = 0;
                        break;
                    }
                    stackLen--;
                    if (stackTypes[stackLen - 1u] == MirInferredType_SLICE_AGG) {
                        stackTypes[stackLen - 1u] = MirInferredType_OPAQUE_PTR;
                        stackTypeRefs[stackLen - 1u] =
                            stackTypeRefs[stackLen - 1u] < program->typeLen
                                ? SLMirTypeRefAggSliceElemTypeRef(
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
                case SLMirOp_DROP:
                case SLMirOp_ASSERT:
                case SLMirOp_RETURN:
                    if (stackLen == 0u) {
                        supported = 0;
                        break;
                    }
                    stackLen--;
                    break;
                case SLMirOp_RETURN_VOID: break;
                default:                  supported = 0; break;
            }
        }
    }
}

int BuildPackageMirProgram(
    const SLPackageLoader* loader,
    const SLPackage*       entryPkg,
    int                    includeSelectedPlatform,
    SLArena*               arena,
    SLMirProgram*          outProgram,
    SLForeignLinkageInfo* _Nullable outForeignLinkage,
    SLDiag* _Nullable diag) {
    SLMirProgramBuilder  builder;
    SLMirResolvedDeclMap declMap = { 0 };
    SLMirTcFunctionMap   tcFnMap = { 0 };
    uint32_t*            topoOrder = NULL;
    uint32_t             topoOrderLen = 0;
    uint32_t             orderIndex;
    int                  autoIncludeSelectedPlatformPanicOnly = 0;
    if (diag != NULL) {
        *diag = (SLDiag){ 0 };
    }
    if (outForeignLinkage != NULL) {
        *outForeignLinkage = (SLForeignLinkageInfo){ 0 };
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
        && StrEq(loader->platformTarget, SL_WASM_MIN_PLATFORM_TARGET);
    SLMirProgramBuilderInit(&builder, arena);
    for (orderIndex = 0; orderIndex < topoOrderLen; orderIndex++) {
        const SLPackage* pkg = &loader->packages[topoOrder[orderIndex]];
        uint32_t         fileIndex;
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
        const SLPackage* pkg = &loader->packages[topoOrder[orderIndex]];
        uint32_t         fileIndex;
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
        && StrEq(loader->platformTarget, SL_WASM_MIN_PLATFORM_TARGET)
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
    SLMirProgramBuilderFinish(&builder, outProgram);
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
        if (diag != NULL && diag->code != SLDiag_NONE) {
            return -1;
        }
        return ErrorSimple("out of memory");
    }
    free(tcFnMap.v);
    free(declMap.v);
    free(topoOrder);
    return SLMirValidateProgram(outProgram, diag);
}

int DumpMIR(
    const char* entryPath,
    const char* _Nullable platformTarget,
    const char* _Nullable archTarget,
    int testingBuild) {
    uint8_t         arenaStorage[4096];
    SLArena         arena;
    SLPackageLoader loader = { 0 };
    SLPackage*      entryPkg = NULL;
    SLMirProgram    program = { 0 };
    SLDiag          diag = { 0 };
    SLWriter        writer;
    SLStrView       fallbackSrc = { 0 };
    int             includeSelectedPlatform = 0;

    SLArenaInit(&arena, arenaStorage, sizeof(arenaStorage));
    SLArenaSetAllocator(&arena, NULL, CodegenArenaGrow, CodegenArenaFree);

    if (LoadAndCheckPackage(entryPath, platformTarget, archTarget, testingBuild, &loader, &entryPkg)
        != 0)
    {
        SLArenaDispose(&arena);
        return -1;
    }
    includeSelectedPlatform =
        PackageUsesPlatformImport(&loader)
        || (platformTarget != NULL && StrEq(platformTarget, SL_PLAYBIT_PLATFORM_TARGET));
    if (BuildPackageMirProgram(
            &loader, entryPkg, includeSelectedPlatform, &arena, &program, NULL, &diag)
        != 0)
    {
        if (diag.code != SLDiag_NONE && entryPkg->fileLen == 1 && entryPkg->files[0].source != NULL)
        {
            (void)PrintSLDiagLineCol(entryPkg->files[0].path, entryPkg->files[0].source, &diag, 0);
        } else if (diag.code != SLDiag_NONE) {
            (void)ErrorSimple("invalid MIR program");
        }
        FreeLoader(&loader);
        SLArenaDispose(&arena);
        return -1;
    }

    if (entryPkg->fileLen == 1 && entryPkg->files[0].source != NULL) {
        fallbackSrc.ptr = entryPkg->files[0].source;
        fallbackSrc.len = entryPkg->files[0].sourceLen;
    }

    writer.ctx = NULL;
    writer.write = StdoutWrite;
    if (SLMirDumpProgram(&program, fallbackSrc, &writer, &diag) != 0) {
        FreeLoader(&loader);
        SLArenaDispose(&arena);
        return -1;
    }

    FreeLoader(&loader);
    SLArenaDispose(&arena);
    return 0;
}

SL_API_END
