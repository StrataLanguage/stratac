#include "Codegen/CBackend.h"

#include "strata/strata.h"

#include "AST/AST.h"
#include "Codegen/CSimd.h"
#include "Codegen/CodegenBackend.h"
#include "Codegen/TypeRegistry.h"
#include "Codegen/TypeUtil.h"

#include <assert.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define MAKE_TEMP_ID(buffer_) GenerateId(buffer_, 8)

typedef struct
{
    const char* typeName;
    const char* cName;
    bool indirect;
    bool aliased; /* ref T... rest: element slots hold pointers to sources */
} CSymbol;

typedef struct
{
    const char* cName;
    const char* typeName;
    bool byRef; /* by-ref box param: cName is a pointer to the caller's box slot */
    bool stackBuffer; /* T[] rest param: array backing is the caller's stack */
} OwnEntry;

static const char* DropHelperName(CEmitter* emitter, const char* structName);

static StrataArch ResolveArch(StrataArch arch)
{
    // If the arch is set to AUTO, use the hosts architecture
    if (arch == STRATA_ARCH_AUTO)
    {
#if defined(STRATA_HOST_ARM64)
        return STRATA_ARCH_ARM64;
#elif defined(STRATA_HOST_X64)
        return STRATA_ARCH_X64;
#endif
    }

    return arch;
}

static void DisposeMap(StrMap* map)
{
    StrMapFree(map);
}

static void Pad(CEmitter* emitter)
{
    SbPutr(&emitter->out, ' ', emitter->indent * 4);
}

static void EmitEscapedFileName(CEmitter* emitter, const char* name)
{
    for (const char* cursor = name; *cursor; ++cursor)
    {
        if (*cursor == '\\' || *cursor == '"')
        {
            SbPutc(&emitter->out, '\\');
        }

        SbPutc(&emitter->out, *cursor);
    }
}

static void EmitLineDirective(CEmitter* emitter, SourceRange range)
{
    if (!emitter->sources || !SourceRangeValid(range) || range.fileId >= emitter->sourceCount)
    {
        return;
    }

    const SourceManager* source = &emitter->sources[range.fileId];

    LineCol location = SourceManagerLineCol(source, range.start);
    SbPrintf(&emitter->out, "#line %u \"", location.line);

    EmitEscapedFileName(emitter, source->m_name ? source->m_name : "<string>");

    SbPuts(&emitter->out, "\"\n");
}

static SourceRange TypeSourceRange(CEmitter* emitter, const char* name)
{
    for (size_t i = 0; i < emitter->mod->structs.count; ++i)
    {
        const StructDecl* declaration = (const StructDecl*)VecGet(&emitter->mod->structs, i);
        if (strcmp(declaration->name, name) == 0)
        {
            return declaration->base.range;
        }
    }
    for (size_t i = 0; i < emitter->mod->handles.count; ++i)
    {
        const HandleDecl* declaration = (const HandleDecl*)VecGet(&emitter->mod->handles, i);
        if (strcmp(declaration->name, name) == 0)
        {
            return declaration->base.range;
        }
    }
    return SRC_INVALID;
}

static const char* Encode(CEmitter* emitter, const char* prefix, const char* name)
{
    Sb sb;
    SbInit(&sb);
    SbPuts(&sb, prefix);

    for (const unsigned char* p = (const unsigned char*)name; *p; ++p)
    {
        if (isalnum(*p) || *p == '_')
        {
            SbPutc(&sb, (char)*p);
        }
        else
        {
            SbPrintf(&sb, "_x%02x_", (unsigned)*p);
        }
    }

    return SbFinish(&sb, emitter->arena);
}

static bool IsPlainIdentifier(const char* name)
{
    static const char* keywords[]
        = {"alignas",  "alignof",  "auto",       "break",     "case",           "char",         "const",    "continue",
           "default",  "do",       "double",     "else",      "enum",           "extern",       "float",    "for",
           "goto",     "if",       "inline",     "int",       "long",           "register",     "restrict", "return",
           "short",    "signed",   "sizeof",     "static",    "struct",         "switch",       "typedef",  "union",
           "unsigned", "void",     "volatile",   "while",     "_Alignas",       "_Alignof",     "_Atomic",  "_Bool",
           "_Complex", "_Generic", "_Imaginary", "_Noreturn", "_Static_assert", "_Thread_local"};
    if (!name || !(isalpha((unsigned char)name[0]) || name[0] == '_'))
    {
        return false;
    }

    for (const unsigned char* p = (const unsigned char*)name + 1; *p; ++p)
    {
        if (!(isalnum(*p) || *p == '_'))
        {
            return false;
        }
    }

    if (name[0] == '_' && (name[1] == '_' || isupper((unsigned char)name[1])))
    {
        return false;
    }

    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); ++i)
    {
        if (strcmp(name, keywords[i]) == 0)
        {
            return false;
        }
    }

    return true;
}

static const char* FunctionName(CEmitter* emitter, const char* name)
{
    if (IsPlainIdentifier(name))
    {
        return arena_strdup(emitter->arena, name);
    }

    return Encode(emitter, "strata__fn_", name);
}

static const char* GetSimdTypeName(CEmitter* emitter, const char* name)
{
    /* When SIMD intrinsics are disabled (e.g. the TCC JIT, or --no-simd), the
       operation emitters produce __strata_float128 struct values - so the
       declared type must match or the generated C is inconsistent. Only use
       the native vector types when intrinsics are actually emitted. */
    if ((emitter->emitFlags & CEmitEnableSIMD) == 0)
    {
        return "__strata_float128";
    }

    StrataArch arch = ResolveArch(emitter->arch);

    if ((emitter->emitFlags & CEmitEnableSIMD) == 0)
    {
        return CSIMD_FALLBACK_VECTOR_NAME;
    }

    if (arch == STRATA_ARCH_ARM64)
    {
        return "float32x4_t";
    }
    else if (arch == STRATA_ARCH_X64)
    {
        return "__m128";
    }

    return CSIMD_FALLBACK_VECTOR_NAME;
}

static const char* TypeNameC(CEmitter* emitter, const char* name)
{
    if (IsArrayType(name))
    {
        /* Every T[] shares one fat {data, len} C struct; the element type
           only matters when indexing (the data pointer is cast then). */
        return "strata__arr";
    }

    if (IsOwningType(name))
    {
        if (strcmp(name, "string") == 0)
        {
            return "char *";
        }

        const char* inner = OwningInnerCStr(emitter->arena, name);

        if (inner)
        {
            const char* innerC = TypeNameC(emitter, inner);
            Sb sb;
            SbInit(&sb);
            SbPuts(&sb, innerC);
            SbPuts(&sb, " *");

            return SbFinish(&sb, emitter->arena);
        }

        return "void*";
    }

    MappedType mapped;
    TypeName type = {0};
    type.name = (char*)name;
    mapped = MapType(&type);

    if (mapped.valid)
    {
        if (mapped.isSimdVector)
        {
            return GetSimdTypeName(emitter, name);
        }

        if (mapped.isVoid)
        {
            return "void";
        }

        if (mapped.isFloat && mapped.bits == 32)
        {
            return "float";
        }

        if (mapped.isFloat && mapped.bits == 64)
        {
            return "double";
        }

        if (mapped.bits == 1)
        {
            return "_Bool";
        }

        if (mapped.bits == 8)
        {
            return mapped.isUnsigned ? "unsigned char" : "signed char";
        }

        if (mapped.bits == 16)
        {
            return mapped.isUnsigned ? "unsigned short" : "short";
        }

        if (mapped.bits == 64)
        {
            return mapped.isUnsigned ? "unsigned long long" : "long long";
        }

        if (mapped.isUnsigned)
        {
            return "unsigned int";
        }

        return "int";
    }

    if (TypeRegistryIsUserType(&emitter->types, name))
    {
        return Encode(emitter, "strata__type_", name);
    }

    DiagErrorFmt(emitter->diag, SRC_INVALID, "don't know '%s'", name);

    return "void*";
}

static const char* FieldName(CEmitter* emitter, const char* name)
{
    return Encode(emitter, "strata__field_", name);
}

static const char* VarName(CEmitter* emitter, const char* name)
{
    return Encode(emitter, "strata__var_", name);
}

static const char* GlobalName(CEmitter* emitter, const char* name)
{
    return Encode(emitter, "strata__global_", name);
}

static const char* ExternSlotName(CEmitter* emitter, const char* name)
{
    return Encode(emitter, "strata__ext_", name);
}

static bool IsStructValue(CEmitter* emitter, const char* name)
{
    return TypeRegistryIsUserType(&emitter->types, name) && !TypeRegistryIsOpaque(&emitter->types, name);
}

static bool ParamIsIndirect(CEmitter* emitter, const ParamDecl* param)
{
    return ByRef(param->mod) || IsStructValue(emitter, param->type.name) || IsOwningType(param->type.name);
}

/* Param pass convention for a specific function. Extern variadic functions
   (bare `...`) receive string params as const char* by value, matching the
   LLVM backend and the C varargs ABI the host expects. */
static bool ParamIsIndirectFor(CEmitter* emitter, const FunctionDecl* function, const ParamDecl* param)
{
    /* Extern string params cross as const char* by value, matching the LLVM
       backend and libc-style hosts (puts, printf, strlen, ...). */
    if (function && function->isExtern && strcmp(param->type.name, "string") == 0)
    {
        return false;
    }

    return ParamIsIndirect(emitter, param);
}

/* A `ref T... rest` with non-owning elements collects POINTERS to the source
   arguments, so element writes through the ref propagate back to the caller
   (true ref semantics). Owning (box<T>) element rests stay a borrow. */
static bool ParamIsAliasedRest(CEmitter* emitter, const ParamDecl* param)
{
    if (!param->isVarargRest || param->mod != ModRef)
    {
        return false;
    }

    Str inner = ArrayInnerStr(param->type.name);

    if (!inner.data)
    {
        return false;
    }

    const char* elem = StrNew(emitter->arena, inner.data, inner.len).data;

    return !IsOwningType(elem);
}

static void EmitType(CEmitter* emitter, const TypeName* type)
{
    if (type->isConst)
    {
        SbPuts(&emitter->out, "const ");
    }

    SbPuts(&emitter->out, TypeNameC(emitter, type->name));
}

static void AddSymbol(CEmitter* emitter, const char* name, const char* typeName, const char* cName, bool indirect)
{
    CSymbol* symbol = (CSymbol*)arena_alloc(emitter->arena, sizeof(CSymbol));
    symbol->typeName = typeName;
    symbol->cName = cName;
    symbol->indirect = indirect;

    StrMapPut(&emitter->symbols, name, symbol);
}

static const char* ExprTypePseudoCall(CEmitter* emitter, const CallExpr* call)
{
    // The callee name is preserved for pseudo function calls in contrast to the mangled name for user defined calls.
    if (IsSimdVector(call->callee))
    {
        return call->callee;
    }

    return NULL;
}

static const char* ExprType(CEmitter* emitter, const Node* node)
{
    if (!node)
    {
        return "";
    }

    switch (node->kind)
    {
    case NodeIntLiteral:
    {
        const IntLiteral* lit = (const IntLiteral*)node;

        if (lit->value > 0xFFFFFFFFULL)
        {
            return lit->isUnsigned ? "ulong" : "long";
        }

        return lit->isUnsigned ? "uint" : "int";
    }
    case NodeFloatLiteral:
        return "float";
    case NodeBoolLiteral:
        return "bool";
    case NodeStrLiteral:
        return "string";
    case NodeIdent:
    {
        const CSymbol* symbol = (const CSymbol*)StrMapGet(&emitter->symbols, ((const IdentExpr*)node)->name);
        return symbol ? symbol->typeName : "";
    }
    case NodeUnary:
        return ((const UnaryExpr*)node)->op == UnNot ? "bool" : ExprType(emitter, ((const UnaryExpr*)node)->operand);
    case NodeBinary:
    {
        const BinaryExpr* expression = (const BinaryExpr*)node;

        if (expression->op >= BinEqEq)
        {
            return "bool";
        }

        const char* lhs = ExprType(emitter, expression->lhs);
        const char* rhs = ExprType(emitter, expression->rhs);

        if (strcmp(lhs, "double") == 0 || strcmp(rhs, "double") == 0)
        {
            return "double";
        }

        if (strcmp(lhs, "float") == 0 || strcmp(rhs, "float") == 0)
        {
            return "float";
        }

        if (strcmp(lhs, "ulong") == 0 || strcmp(rhs, "ulong") == 0)
        {
            return "ulong";
        }

        if (strcmp(lhs, "long") == 0 || strcmp(rhs, "long") == 0)
        {
            return "long";
        }

        if (strcmp(lhs, "uint") == 0 || strcmp(rhs, "uint") == 0)
        {
            return "uint";
        }

        if (strcmp(lhs, "float3") == 0 && (strcmp(lhs, "float3") == 0 || strcmp(rhs, "float") == 0))
        {
            return "float3";
        }

        if (strcmp(lhs, "float4") == 0 && (strcmp(lhs, "float4") == 0 || strcmp(rhs, "float") == 0))
        {
            return "float4";
        }

        return "int";
    }
    case NodeAssign:
        return ExprType(emitter, ((const AssignExpr*)node)->target);
    case NodeIncDec:
        return ExprType(emitter, ((const IncDecExpr*)node)->operand);
    case NodeCast:
        return ((const CastExpr*)node)->type.name;
    case NodeCall:
    {
        const CallExpr* call = (const CallExpr*)node;

        const char* pseudoCallType = ExprTypePseudoCall(emitter, call);

        if (pseudoCallType)
        {
            return pseudoCallType;
        }

        if (call->resolvedDecl)
        {
            return call->resolvedDecl->returnType.name;
        }

        return TypeRegistryIsUserType(&emitter->types, call->callee) ? call->callee : "";
    }
    case NodeMember:
    {
        const MemberExpr* member = (const MemberExpr*)node;

        const char* baseName = ExprType(emitter, member->base_node);

        if (IsSimdVector(baseName))
        {
            size_t laneCount = strlen(member->member);
            switch (laneCount)
            {
            case 1:
                return "float";
            case 3:
                return "float3";
            case 4:
                return "float4";
            default:
                DiagErrorFmt(emitter->diag, node->range, "cannot destructure into larger vector");
                break;
            }

            return "float";
        }

        /* array.length is a u64 query on the fat struct. */
        if (IsArrayType(baseName) && strcmp(member->member, "length") == 0)
        {
            return "ulong";
        }

        const char* _inner = OwningInnerCStr(emitter->arena, baseName);
        if (_inner)
        {
            baseName = _inner;
        }

        /* If unwrapping a box revealed a SIMD vector, the member is a lane
           or swizzle, not a struct field. */
        if (IsSimdVector(baseName))
        {
            size_t laneCount = strlen(member->member);
            switch (laneCount)
            {
            case 1:
                return "float";
            case 3:
                return "float3";
            case 4:
                return "float4";
            default:
                DiagErrorFmt(emitter->diag, node->range, "cannot destructure into larger vector");
                break;
            }

            return "float";
        }

        const StructType* type = TypeRegistryFind(&emitter->types, baseName);
        int index = type ? TypeRegistryFieldIndex(&emitter->types, baseName, member->member) : -1;

        return index >= 0 ? ((const FieldDecl*)VecGet(&type->fields, (size_t)index))->type.name : "";
    }
    case NodeStructInit:
        return ((const StructInitExpr*)node)->typeName;
    case NodeIndex:
    {
        const char* baseName = ExprType(emitter, ((const IndexExpr*)node)->base_node);
        Str inner = ArrayInnerStr(baseName);

        return inner.data ? StrNew(emitter->arena, inner.data, inner.len).data : "";
    }
    case NodeArrayInit:
    {
        const ArrayInitExpr* ai = (const ArrayInitExpr*)node;

        return arena_format(emitter->arena, "%s[]", ai->elementType);
    }
    default:
        return "";
    }
}

static void EmitScalarValue(CEmitter* emitter, const Node* node);
static void CEmitOwnedValue(CEmitter* emitter, const Node* init, const char* innerType);

/* True if 'base' is a `ref T...` rest array whose slots hold pointers to the
   source arguments (element access must deref the slot). */
static bool IsAliasedRestArray(const CEmitter* emitter, const Node* base)
{
    if (base && base->kind == NodeIdent)
    {
        const CSymbol* sym = (const CSymbol*)StrMapGet(&emitter->symbols, ((const IdentExpr*)base)->name);

        return sym && sym->aliased;
    }

    return false;
}

static void EmitLValue(CEmitter* emitter, const Node* node)
{
    if (!node)
    {
        SbPuts(&emitter->out, "0");

        return;
    }

    if (node->kind == NodeIdent)
    {
        const IdentExpr* ident = (const IdentExpr*)node;

        CSymbol* symbol = (CSymbol*)StrMapGet(&emitter->symbols, ident->name);
        if (!symbol)
        {
            DiagErrorFmt(emitter->diag, node->range, "don't know '%s'", ident->name);
            SbPuts(&emitter->out, "0");

            return;
        }

        if (symbol->indirect)
        {
            SbPuts(&emitter->out, "(*");
        }

        SbPuts(&emitter->out, symbol->cName);

        if (symbol->indirect)
        {
            SbPutc(&emitter->out, ')');
        }

        return;
    }

    if (node->kind == NodeMember)
    {
        const MemberExpr* member = (const MemberExpr*)node;
        const char* baseType = ExprType(emitter, member->base_node);

        /* array.length is a u64 query on the fat struct. */
        if (IsArrayType(baseType) && strcmp(member->member, "length") == 0)
        {
            SbPutc(&emitter->out, '(');
            EmitLValue(emitter, member->base_node);
            SbPuts(&emitter->out, ").len");

            return;
        }

        bool throughBox = IsOwningType(baseType);
        SbPutc(&emitter->out, '(');
        EmitLValue(emitter, member->base_node);
        SbPuts(&emitter->out, throughBox ? ")->" : ").");
        SbPuts(&emitter->out, FieldName(emitter, member->member));

        return;
    }

    if (node->kind == NodeIndex)
    {
        const IndexExpr* idx = (const IndexExpr*)node;
        const char* baseType = ExprType(emitter, idx->base_node);
        Str inner = ArrayInnerStr(baseType);
        const char* elemC
            = TypeNameC(emitter, inner.data ? StrNew(emitter->arena, inner.data, inner.len).data : "void");

        if (IsAliasedRestArray(emitter, idx->base_node))
        {
            /* Aliased ref rest: the data is a T* array; each slot points at
               the source argument, so deref the slot. */
            SbPuts(&emitter->out, "(*(( ");
            SbPuts(&emitter->out, elemC);
            SbPuts(&emitter->out, " **)(( ");
            EmitLValue(emitter, idx->base_node);
            SbPuts(&emitter->out, ").data))[({ unsigned long long _bi = ");
            CEmitExpr(emitter, idx->index);
            SbPuts(&emitter->out, "; ");
            
            if (emitter->boundsCheck)
            {
                SbPuts(&emitter->out, "if (_bi >= (");
                EmitLValue(emitter, idx->base_node);
                SbPuts(&emitter->out, ").len) strata_panic(\"array index out of bounds\"); ");
            }

            SbPuts(&emitter->out, "_bi; })])");
        }
        else
        {
            SbPuts(&emitter->out, "((");
            SbPuts(&emitter->out, elemC);
            SbPuts(&emitter->out, " *)(( ");
            EmitLValue(emitter, idx->base_node);
            SbPuts(&emitter->out, ").data))[({ unsigned long long _bi = ");
            CEmitExpr(emitter, idx->index);
            SbPuts(&emitter->out, "; ");
            
            if (emitter->boundsCheck)
            {
                SbPuts(&emitter->out, "if (_bi >= (");
                EmitLValue(emitter, idx->base_node);
                SbPuts(&emitter->out, ").len) strata_panic(\"array index out of bounds\"); ");
            }

            SbPuts(&emitter->out, "_bi; })]");
        }

        return;
    }

    DiagError(emitter->diag, node->range, "C backend expected an assignable expression");
    SbPuts(&emitter->out, "0");
}

static const char* UnarySpelling(UnaryOp op)
{
    switch (op)
    {
    case UnNeg:
        return "-";
    case UnPos:
        return "+";
    case UnNot:
        return "!";
    case UnBitNot:
        return "~";
    }

    return "";
}

static const char* BinarySpelling(BinaryOp op)
{
    switch (op)
    {
    case BinAdd:
        return "+";
    case BinSub:
        return "-";
    case BinMul:
        return "*";
    case BinDiv:
        return "/";
    case BinMod:
        return "%";
    case BinBitAnd:
        return "&";
    case BinBitOr:
        return "|";
    case BinBitXor:
        return "^";
    case BinShl:
        return "<<";
    case BinShr:
        return ">>";
    case BinEqEq:
        return "==";
    case BinNotEq:
        return "!=";
    case BinLt:
        return "<";
    case BinLtEq:
        return "<=";
    case BinGt:
        return ">";
    case BinGtEq:
        return ">=";
    case BinLogicAnd:
        return "&&";
    case BinLogicOr:
        return "||";
    }

    return "";
}

static const char* AssignSpelling(AssignOp op)
{
    switch (op)
    {
    case AssignSet:
        return "=";
    case AssignAdd:
        return "+=";
    case AssignSub:
        return "-=";
    case AssignMul:
        return "*=";
    case AssignDiv:
        return "/=";
    case AssignMod:
        return "%=";
    }
    return "=";
}

static void EmitStructInit(CEmitter* emitter, const char* typeName, const Vec* fields)
{
    const StructType* type = TypeRegistryFind(&emitter->types, typeName);
    SbPutc(&emitter->out, '(');
    SbPuts(&emitter->out, TypeNameC(emitter, typeName));
    SbPuts(&emitter->out, "){ ");

    size_t positional = 0;
    for (size_t i = 0; i < fields->count; ++i)
    {
        StructInitField* field = (StructInitField*)VecGet(fields, i);

        size_t index;

        if (field->name && field->name[0])
        {
            int named = TypeRegistryFieldIndex(&emitter->types, typeName, field->name);
            index = named >= 0 ? (size_t)named : 0;
        }
        else
        {
            index = positional++;
        }

        if (i > 0)
        {
            SbPuts(&emitter->out, ", ");
        }

        FieldDecl* declaration = (type && index < type->fields.count) ? (FieldDecl*)VecGet(&type->fields, index) : NULL;

        if (declaration)
        {
            SbPutc(&emitter->out, '.');
            SbPuts(&emitter->out, FieldName(emitter, declaration->name));
            SbPuts(&emitter->out, " = ");
        }

        if (declaration && IsOwningType(declaration->type.name))
        {
            const char* valueType = ExprType(emitter, field->value);
            bool sameOwningType = valueType[0] != '\0' && IsOwningType(valueType)
                && strcmp(valueType, declaration->type.name) == 0;
            const char* inner = OwningInnerCStr(emitter->arena, declaration->type.name);

            if (sameOwningType && field->value->kind != NodeStrLiteral)
            {
                /* Same owning type: move (take pointer, null source). */
                CEmitExpr(emitter, field->value);

                const Node* moved = MovableBoxSourceNode(field->value);

                if (moved)
                {
                    SbPuts(&emitter->out, ", (");
                    EmitLValue(emitter, moved);
                    SbPuts(&emitter->out, " = 0)");
                }
            }
            else if (inner)
            {
                /* box<T> field from a non-box<T> value: heap-box it inline
                   via a GNU statement expression. CEmitOwnedValue handles
                   strdup for literals, move for owning sources. */
                const char* innerC = TypeNameC(emitter, inner);
                char tmp[32];
                snprintf(tmp, sizeof tmp, "strata__boxtmp%u", emitter->boxTmpCounter++);

                SbPuts(&emitter->out, "({ ");
                SbPuts(&emitter->out, innerC);
                SbPuts(&emitter->out, " *");
                SbPuts(&emitter->out, tmp);
                SbPuts(&emitter->out, " = strata_alloc(sizeof(");
                SbPuts(&emitter->out, innerC);
                SbPuts(&emitter->out, ")); *");
                SbPuts(&emitter->out, tmp);
                SbPuts(&emitter->out, " = ");
                CEmitOwnedValue(emitter, field->value, inner);
                SbPuts(&emitter->out, "; ");
                SbPuts(&emitter->out, tmp);
                SbPuts(&emitter->out, "; })");
            }
            else
            {
                /* string field: construct owned value (strdup literal,
                   move source). */
                CEmitOwnedValue(emitter, field->value, declaration->type.name);
            }

            continue;
        }

        CEmitExpr(emitter, field->value);
    }

    SbPuts(&emitter->out, " }");
}

/* Emits an inline array helper (array_push / array_pop / array_resize) as a GNU
   statement-expression that mutates the array in place through its lvalue. */
static void EmitCopyBuiltin(CEmitter* emitter, const CallExpr* call);
static void EmitArrayBuiltin(CEmitter* emitter, const CallExpr* call)
{
    const Node* arg0 = (const Node*)VecGet(&call->args, 0);
    Str innerRaw = ArrayInnerStr(ExprType(emitter, arg0));
    const char* elemType = StrNew(emitter->arena, innerRaw.data, innerRaw.len).data;
    const char* elemC = TypeNameC(emitter, elemType);
    bool elemOwning = IsOwningType(elemType);

    SbPuts(&emitter->out, "({ strata__arr* _a = &(");
    EmitLValue(emitter, arg0);
    SbPuts(&emitter->out, "); ");

    if (strcmp(call->callee, "array_push") == 0)
    {
        const Node* val = (const Node*)VecGet(&call->args, 1);

        SbPuts(&emitter->out, "unsigned long long _n = _a->len + 1; ");
        SbPuts(&emitter->out, elemC);
        SbPuts(&emitter->out, "* _d = strata_alloc(_n * sizeof(");
        SbPuts(&emitter->out, elemC);
        SbPuts(&emitter->out, ")); ");
        SbPuts(&emitter->out, "{ unsigned long long _i; for (_i = 0; _i < _a->len; _i++) _d[_i] = ((");
        SbPuts(&emitter->out, elemC);
        SbPuts(&emitter->out, "*)_a->data)[_i]; } ");

        SbPuts(&emitter->out, "((");
        SbPuts(&emitter->out, elemC);
        SbPuts(&emitter->out, "*)_d)[_a->len] = ");

        if (elemOwning)
        {
            const char* vt = ExprType(emitter, val);

            if (vt[0] != '\0' && IsOwningType(vt) && strcmp(vt, elemType) == 0
                && val->kind != NodeStrLiteral)
            {
                /* Same owning type, not a literal: move (take pointer, null source). */
                CEmitExpr(emitter, val);

                const Node* moved = MovableBoxSourceNode(val);

                if (moved)
                {
                    SbPuts(&emitter->out, ", (");
                    EmitLValue(emitter, moved);
                    SbPuts(&emitter->out, " = 0)");
                }
            }
            else if (elemType && IsArrayType(elemType) == false && OwningInnerCStr(emitter->arena, elemType))
            {
                /* box<T> element from a non-box<T> value: box it up.
                   CEmitOwnedValue handles strdup for literals, move for
                   owning sources. */
                const char* inner = OwningInnerCStr(emitter->arena, elemType);
                const char* innerC = TypeNameC(emitter, inner);

                SbPuts(&emitter->out, "({ ");
                SbPuts(&emitter->out, innerC);
                SbPuts(&emitter->out, " *_b = strata_alloc(sizeof(");
                SbPuts(&emitter->out, innerC);
                SbPuts(&emitter->out, ")); *_b = ");
                CEmitOwnedValue(emitter, val, inner);
                SbPuts(&emitter->out, "; _b; })");
            }
            else
            {
                /* string element or other owning primitive. */
                CEmitOwnedValue(emitter, val, elemType);
            }
        }
        else
        {
            CEmitExpr(emitter, val);
        }

        SbPuts(&emitter->out, "; strata_free(_a->data); _a->data = _d; _a->len = _n; _n; })");
    }
    else if (strcmp(call->callee, "array_pop") == 0)
    {
        SbPuts(&emitter->out, elemC);
        SbPuts(&emitter->out, " _v = ((");
        SbPuts(&emitter->out, elemC);
        SbPuts(&emitter->out, "*)_a->data)[_a->len - 1]; _a->len = _a->len - 1; _v; })");
    }
    else /* array_resize */
    {
        const Node* newLen = (const Node*)VecGet(&call->args, 1);

        SbPuts(&emitter->out, "unsigned long long _n = ");
        CEmitExpr(emitter, newLen);
        SbPuts(&emitter->out, "; if (_n != _a->len) { ");

        if (elemOwning)
        {
            /* Free the owning elements being truncated away. */
            SbPuts(&emitter->out, "if (_n < _a->len) { unsigned long long _i; for (_i = _n; _i < _a->len; _i++) ");
            SbPuts(&emitter->out, "strata_free(((");
            SbPuts(&emitter->out, elemC);
            SbPuts(&emitter->out, "*)_a->data)[_i]); } ");
        }

        SbPuts(&emitter->out, elemC);
        SbPuts(&emitter->out, "* _d = strata_alloc((_n ? _n : 1) * sizeof(");
        SbPuts(&emitter->out, elemC);
        SbPuts(&emitter->out, ")); ");
        SbPuts(&emitter->out, "{ unsigned long long _i, _c = _a->len < _n ? _a->len : _n; ");
        SbPuts(&emitter->out, "for (_i = 0; _i < _c; _i++) _d[_i] = ((");
        SbPuts(&emitter->out, elemC);
        SbPuts(&emitter->out, "*)_a->data)[_i]; ");
        SbPuts(&emitter->out, "for (; _i < _n; _i++) _d[_i] = 0; } ");
        SbPuts(&emitter->out, "strata_free(_a->data); _a->data = _d; _a->len = _n; } })");
    }
}

static void EmitPseudoCall(CEmitter* emitter, const CallExpr* call)
{

    if (IsSimdVector(call->callee))
    {
        CSimdVectorConstruct(emitter, &call->args);
        return;
    }

    if (strcmp(call->callee, "array_push") == 0 || strcmp(call->callee, "array_pop") == 0
        || strcmp(call->callee, "array_resize") == 0)
    {
        EmitArrayBuiltin(emitter, call);
        return;
    }

    if (strcmp(call->callee, "copy") == 0)
    {
        EmitCopyBuiltin(emitter, call);
    }
}

/* Deep copy */
static void CEmitCopyText(CEmitter* emitter, const char* type, const char* accessor)
{
    if (strcmp(type, "string") == 0)
    {
        SbPuts(&emitter->out, "strata_strdup(");
        SbPuts(&emitter->out, accessor);
        SbPutc(&emitter->out, ')');
        return;
    }

    if (IsOwningType(type) && !IsArrayType(type))
    {
        const char* inner = OwningInnerCStr(emitter->arena, type);
        const char* innerC = TypeNameC(emitter, inner);
        char* innerAccessor = arena_format(emitter->arena, "(*%s)", accessor);

        SbPuts(&emitter->out, "({ ");
        SbPuts(&emitter->out, innerC);
        SbPuts(&emitter->out, " *_c = strata_alloc(sizeof(");
        SbPuts(&emitter->out, innerC);
        SbPuts(&emitter->out, ")); *_c = ");
        CEmitCopyText(emitter, inner, innerAccessor);
        SbPuts(&emitter->out, "; _c; })");
        return;
    }

    if (IsArrayType(type))
    {
        Str innerRaw = ArrayInnerStr(type);
        const char* elemType = StrNew(emitter->arena, innerRaw.data, innerRaw.len).data;
        const char* elemC = TypeNameC(emitter, elemType);
        char* elemAccessor = arena_format(emitter->arena, "((%s*)(%s).data)[_i]", elemC, accessor);

        SbPuts(&emitter->out, "({ strata__arr _c; _c.len = (");
        SbPuts(&emitter->out, accessor);
        SbPuts(&emitter->out, ").len; _c.data = strata_alloc(_c.len * sizeof(");
        SbPuts(&emitter->out, elemC);
        SbPuts(&emitter->out, ")); { unsigned long long _i; for (_i = 0; _i < _c.len; _i++) ((");
        SbPuts(&emitter->out, elemC);
        SbPuts(&emitter->out, "*)_c.data)[_i] = ");

        if (IsOwningType(elemType))
        {
            CEmitCopyText(emitter, elemType, elemAccessor);
        }
        else
        {
            SbPuts(&emitter->out, elemAccessor);
        }

        SbPuts(&emitter->out, "; } _c; })");
        return;
    }

    SbPuts(&emitter->out, accessor);
}

/* Emits copy(arg) returning a deep copy of an owning value
   (string/box<T>/T[]). */
static void EmitCopyBuiltin(CEmitter* emitter, const CallExpr* call)
{
    const Node* arg0 = (const Node*)VecGet(&call->args, 0);
    const char* type = ExprType(emitter, arg0);

    /* Capture the source expression's C text so the recursive copy below can
       reference it (a nested statement-expression references it in-place). */
    Sb saved = emitter->out;
    Sb tmp;
    SbInit(&tmp);
    emitter->out = tmp;
    CEmitExpr(emitter, arg0);
    tmp = emitter->out;
    emitter->out = saved;

    char* srcText = SbFinish(&tmp, emitter->arena);
    CEmitCopyText(emitter, type, srcText);
}

static void EmitArrayInitExpr(CEmitter* emitter, const ArrayInitExpr* ai);

/* Emits one element expression for a stack-backed vararg rest collection.
   The backing array is a compound literal at the call site; each element is a
   plain C expression. Owning elements are moved (pointer captured, source
   nulled) unless borrow is set (ref rest), in which case they're borrowed. */
static void EmitRestElement(CEmitter* emitter, const Node* eNode, const char* elemType, bool borrow)
{
    bool elemOwning = IsOwningType(elemType);
    const char* elemC = TypeNameC(emitter, elemType);

    if (elemOwning)
    {
        if (borrow)
        {
            /* ref rest: store the pointer, keep the source alive. */
            CEmitExpr(emitter, eNode);

            return;
        }

        const char* valueType = ExprType(emitter, eNode);

        if (strcmp(elemType, "string") == 0 && eNode->kind == NodeStrLiteral)
        {
            SbPuts(&emitter->out, "strata_strdup(");
            CEmitExpr(emitter, eNode);
            SbPutc(&emitter->out, ')');

            return;
        }

        if (valueType[0] != '\0' && IsOwningType(valueType))
        {
            /* Move an owning source: capture its pointer, null the source. */
            SbPrintf(&emitter->out, "({ %s _t = ", elemC);
            CEmitExpr(emitter, eNode);
            SbPuts(&emitter->out, "; ");

            const Node* moved = MovableBoxSourceNode(eNode);

            if (moved)
            {
                EmitLValue(emitter, moved);
                SbPuts(&emitter->out, " = 0");
            }

            SbPuts(&emitter->out, "; _t; })");

            return;
        }

        /* box<T> from a bare T: heap-box it inline. */
        {
            const char* inner = OwningInnerCStr(emitter->arena, elemType);
            const char* innerC = inner ? TypeNameC(emitter, inner) : "void";

            SbPuts(&emitter->out, "({ ");
            SbPuts(&emitter->out, innerC);
            SbPuts(&emitter->out, " *_b = strata_alloc(sizeof(");
            SbPuts(&emitter->out, innerC);
            SbPuts(&emitter->out, ")); *_b = ");
            CEmitExpr(emitter, eNode);
            SbPuts(&emitter->out, "; _b; })");
        }

        return;
    }

    const char* valueType = ExprType(emitter, eNode);

    if (borrow)
    {
        /* Aliased `ref T...`: the slot holds a POINTER to the source storage
           so element writes through the ref propagate to the caller. */
        if (valueType[0] != '\0' && IsOwningType(valueType))
        {
            /* box<T> arg: the box pointer already IS the address of the value. */
            CEmitExpr(emitter, eNode);
        }
        else if (IsLValueNode(eNode))
        {
            SbPuts(&emitter->out, "&(");
            EmitLValue(emitter, eNode);
            SbPutc(&emitter->out, ')');
        }
        else
        {
            SbPuts(&emitter->out, "&((");
            SbPuts(&emitter->out, elemC);
            SbPuts(&emitter->out, "){ ");
            CEmitExpr(emitter, eNode);
            SbPuts(&emitter->out, " })");
        }

        return;
    }

    /* box<T> stored in a T[] element: unbox the pointer. */
    if (valueType[0] != '\0' && IsOwningType(valueType))
    {
        SbPuts(&emitter->out, "(*");
        CEmitExpr(emitter, eNode);
        SbPutc(&emitter->out, ')');
    }
    else
    {
        CEmitExpr(emitter, eNode);
    }
}

static void EmitCall(CEmitter* emitter, const CallExpr* call)
{
    if (call->isPseudoCall)
    {
        EmitPseudoCall(emitter, call);
        return;
    }

    if (!call->resolvedDecl && TypeRegistryIsUserType(&emitter->types, call->callee))
    {
        Vec fields;
        VecInit(&fields);

        for (size_t i = 0; i < call->args.count; ++i)
        {
            StructInitField* field = (StructInitField*)arena_alloc(emitter->arena, sizeof(StructInitField));
            field->name = NULL;
            field->value = (Node*)VecGet(&call->args, i);
            VecPush(&fields, field);
        }

        EmitStructInit(emitter, call->callee, &fields);
        free(fields.items);

        return;
    }

    const FunctionDecl* function = call->resolvedDecl;
    const char* callee = FunctionName(emitter, call->callee);
    bool nullCheck = false;

    if ((emitter->emitFlags & CEmitJIT) != 0 && function && function->isExtern)
    {
        callee = ExternSlotName(emitter, function->name);
        nullCheck = emitter->nullExternCall;
    }

    if (nullCheck)
    {
        /* JIT extern slots start null; panic on calling an unbound extern
           instead of jumping to address 0. Guarded via a comma expression so
           the call's result (of any type) flows through, and the form is
           accepted by both TCC and clang. */
        SbPuts(&emitter->out, "(strata__ext_check(");
        SbPuts(&emitter->out, callee);
        SbPuts(&emitter->out, ", \"call to null extern function '");
        SbPuts(&emitter->out, function->name);
        SbPuts(&emitter->out, "'\"), ");
    }

    SbPuts(&emitter->out, callee);
    SbPutc(&emitter->out, '(');

    /* A typed rest param collects the trailing args into one T[] array
       passed in the rest param's slot, mirroring the LLVM backend. */
    bool typedRest = function && function->isVariadic && !function->isCVararg && function->params.count > 0;
    bool cVararg = function && function->isCVararg;

    size_t namedCount = typedRest ? function->params.count - 1 : call->args.count;
    size_t emitted = 0;

    for (size_t i = 0; i < namedCount; ++i)
    {
        if (emitted > 0)
        {
            SbPuts(&emitter->out, ", ");
        }

        emitted++;

        const Node* argument = (const Node*)VecGet(&call->args, i);
        const ParamDecl* parameter
            = function && i < function->params.count ? (const ParamDecl*)VecGet(&function->params, i) : NULL;

        /* Extern string params cross by value (const char*), so they must not
           go through the indirect (char**) path. */
        bool isStringByValue = function && function->isExtern && parameter
            && strcmp(parameter->type.name, "string") == 0;

        if (parameter && !isStringByValue && ParamIsIndirectFor(emitter, function, parameter))
        {
            /* box<T> coerced to T: if the param is a plain struct (not box),
               and the arg is a box, the heap pointer IS the T* the param wants. */
            bool paramIsBoxType = IsOwningType(parameter->type.name);
            const char* argType = ExprType(emitter, argument);
            bool argIsBox = IsOwningType(argType);

            if (!paramIsBoxType && argIsBox)
            {
                CEmitExpr(emitter, argument);
            }
            else if (paramIsBoxType && argument->kind == NodeStrLiteral
                     && strcmp(parameter->type.name, "string") == 0)
            {
                /* An owning string param receiving a literal must get a
                   heap copy the callee can free (mirrors the LLVM backend). */
                SbPuts(&emitter->out, "&((char*){ strata_strdup(");
                CEmitExpr(emitter, argument);
                SbPuts(&emitter->out, ") })");
            }
            else if (IsLValueNode(argument))
            {
                SbPuts(&emitter->out, "&(");
                EmitLValue(emitter, argument);
                SbPutc(&emitter->out, ')');
            }
            else
            {
                const char* typeName = TypeNameC(emitter, parameter->type.name);

                SbPuts(&emitter->out, "&((");
                SbPuts(&emitter->out, typeName);
                SbPuts(&emitter->out, "[]){ ");
                CEmitExpr(emitter, argument);
                SbPuts(&emitter->out, " })[0]");
            }
        }
        else
        {
            const char* argType = ExprType(emitter, argument);

            if (isStringByValue)
            {
                /* Extern string param: a plain string arg passes its char*
                   value; a box<string> derefs to its char*. */
                if (strcmp(argType, "string") == 0)
                {
                    CEmitExpr(emitter, argument);
                }
                else
                {
                    EmitScalarValue(emitter, argument);
                }
            }
            else if (cVararg && strcmp(argType, "string") == 0)
            {
                /* Variadic extern: string args pass their char* value. */
                CEmitExpr(emitter, argument);
            }
            else
            {
                /* A box arg passed to a by-value (non-indirect) param - e.g. a
                   plain handle - must be dereferenced to its value, not passed
                   as the box's own heap pointer. */
                EmitScalarValue(emitter, argument);
            }
        }
    }

    if (typedRest)
    {
        if (emitted > 0)
        {
            SbPuts(&emitter->out, ", ");
        }

        const ParamDecl* restParam = (const ParamDecl*)VecGet(&function->params, function->params.count - 1);
        Str inner = ArrayInnerStr(restParam->type.name);
        const char* elemName = inner.data ? StrNew(emitter->arena, inner.data, inner.len).data : NULL;
        const char* elemC = TypeNameC(emitter, elemName);
        bool borrow = restParam->mod == ModRef;
        bool aliased = borrow && !IsOwningType(elemName);

        /* A stack-allocated {data, len}: the backing buffer is a compound
           literal array (automatic storage, alive through the call). An
           aliased ref rest stores T* (pointers to the source arguments). */
        SbPuts(&emitter->out, "&((strata__arr){ .data = (");
        SbPuts(&emitter->out, elemC);

        if (aliased)
        {
            SbPuts(&emitter->out, "*");
        }

        SbPuts(&emitter->out, "[]){ ");

        size_t tail = call->args.count - (function->params.count - 1);

        if (tail == 0)
        {
            SbPuts(&emitter->out, "0");
        }
        else
        {
            for (size_t i = function->params.count - 1; i < call->args.count; ++i)
            {
                if (i > function->params.count - 1)
                {
                    SbPuts(&emitter->out, ", ");
                }

                EmitRestElement(emitter, (const Node*)VecGet(&call->args, i), elemName, borrow);
            }
        }

        SbPuts(&emitter->out, " }, .len = ");
        SbPrintf(&emitter->out, "%llu", (unsigned long long)tail);
        SbPuts(&emitter->out, " })");
    }

    SbPutc(&emitter->out, ')');

    if (nullCheck)
    {
        SbPutc(&emitter->out, ')');
    }
}

/* Emits a box-typed identifier dereferenced to its value, for use as a
   plain unary/binary operand (as opposed to a member-access base or a
   box-to-box move, which need the raw pointer). */
static void EmitScalarValue(CEmitter* emitter, const Node* node);
static void EmitArrayInitExpr(CEmitter* emitter, const ArrayInitExpr* ai);

/* Emits an array literal as a GNU statement-expression returning a
   strata__arr { data, len }. Elements are stored into a freshly allocated
   buffer; owning elements (string/box) are heap-boxed or moved in. */
static void EmitArrayInitExpr(CEmitter* emitter, const ArrayInitExpr* ai)
{
    const char* elemType = ai->elementType;
    const char* elemC = TypeNameC(emitter, elemType);
    size_t count = ai->elements.count;
    bool elemOwning = IsOwningType(elemType);

    /* Pointer-to-element cast, e.g. "int *", "char * *", "Vec3 *". */
    SbPuts(&emitter->out, "({ strata__arr _a; _a.data = strata_alloc(");
    SbPrintf(&emitter->out, "%llu", (unsigned long long)(count ? count : 1));
    SbPuts(&emitter->out, "*sizeof(");
    SbPuts(&emitter->out, elemC);
    SbPuts(&emitter->out, ")); _a.len = ");
    SbPrintf(&emitter->out, "%llu", (unsigned long long)count);
    SbPutc(&emitter->out, ';');

    for (size_t i = 0; i < count; i++)
    {
        const Node* eNode = (const Node*)VecGet(&ai->elements, i);

        SbPuts(&emitter->out, " ((");
        SbPuts(&emitter->out, elemC);
        SbPuts(&emitter->out, "*)_a.data)[");
        SbPrintf(&emitter->out, "%llu", (unsigned long long)i);
        SbPuts(&emitter->out, "] = ");

        if (elemOwning)
        {
            const char* valueType = ExprType(emitter, eNode);

            if (valueType[0] != '\0' && IsOwningType(valueType) && strcmp(valueType, elemType) == 0
                && eNode->kind != NodeStrLiteral)
            {
                /* Same owning type, not a literal: move. */
                CEmitExpr(emitter, eNode);

                const Node* moved = MovableBoxSourceNode(eNode);

                if (moved)
                {
                    SbPuts(&emitter->out, ", (");
                    EmitLValue(emitter, moved);
                    SbPuts(&emitter->out, " = 0)");
                }
            }
            else if (OwningInnerCStr(emitter->arena, elemType))
            {
                /* box<T> element from a non-box<T> value: box it up. */
                const char* inner = OwningInnerCStr(emitter->arena, elemType);
                const char* innerC = TypeNameC(emitter, inner);

                SbPuts(&emitter->out, "({ ");
                SbPuts(&emitter->out, innerC);
                SbPuts(&emitter->out, " *_b = strata_alloc(sizeof(");
                SbPuts(&emitter->out, innerC);
                SbPuts(&emitter->out, ")); *_b = ");
                CEmitOwnedValue(emitter, eNode, inner);
                SbPuts(&emitter->out, "; _b; })");
            }
            else
            {
                /* string element: construct owned value (strdup literal). */
                CEmitOwnedValue(emitter, eNode, elemType);
            }
        }
        else
        {
            const char* valueType = ExprType(emitter, eNode);

            /* box<T> stored in a T[] array element: unbox the pointer. */
            if (valueType[0] != '\0' && IsOwningType(valueType) && !elemOwning)
            {
                SbPuts(&emitter->out, "(*");
                CEmitExpr(emitter, eNode);
                SbPutc(&emitter->out, ')');
            }
            else
            {
                CEmitExpr(emitter, eNode);
            }
        }

        SbPutc(&emitter->out, ';');
    }

    SbPuts(&emitter->out, " _a; })");
}

static void EmitScalarValue(CEmitter* emitter, const Node* node)
{
    /* A box-typed expression used as a plain value (by-value call arg,
       unary/binary operand, assignment RHS) is dereferenced to its pointee.
       Handles idents, array elements, member chains, and box-returning calls
       uniformly; for an ident CEmitExpr already yields the box pointer. */
    if (node && IsOwningType(ExprType(emitter, node)))
    {
        SbPuts(&emitter->out, "(*");
        CEmitExpr(emitter, node);
        SbPutc(&emitter->out, ')');

        return;
    }

    CEmitExpr(emitter, node);
}

static void EmitSimdBinaryExpr(CEmitter* emitter, const BinaryExpr* binary)
{
    StrataArch arch = ResolveArch(emitter->arch);

    if (arch == STRATA_ARCH_ARM64)
    {
    }
}

void CEmitExpr(CEmitter* emitter, const Node* node)
{
    if (!node)
    {
        SbPuts(&emitter->out, "0");

        return;
    }

    switch (node->kind)
    {
    case NodeIntLiteral:
    {
        const IntLiteral* literal = (const IntLiteral*)node;

        if (literal->value > 0xFFFFFFFFULL)
        {
            SbPrintf(&emitter->out, "%llu%s", (unsigned long long)literal->value, literal->isUnsigned ? "ULL" : "LL");
        }
        else
        {
            SbPrintf(&emitter->out, "%llu%s", (unsigned long long)literal->value, literal->isUnsigned ? "u" : "");
        }

        return;
    }
    case NodeFloatLiteral:
        SbPrintf(&emitter->out, "%.9ef", ((const FloatLiteral*)node)->value);
        return;
    case NodeBoolLiteral:
        SbPuts(&emitter->out, ((const BoolLiteral*)node)->value ? "1" : "0");
        return;
    case NodeStrLiteral:
    {
        const StrLiteral* literal = (const StrLiteral*)node;
        SbPutc(&emitter->out, '"');
        for (const char* s = literal->value; *s; s++)
        {
            if (*s == '\\')
            {
                SbPuts(&emitter->out, "\\\\");
            }
            else if (*s == '"')
            {
                SbPuts(&emitter->out, "\\\"");
            }
            else if (*s == '\n')
            {
                SbPuts(&emitter->out, "\\n");
            }
            else if (*s == '\t')
            {
                SbPuts(&emitter->out, "\\t");
            }
            else if (*s == '\r')
            {
                SbPuts(&emitter->out, "\\r");
            }
            else if (*s != '\0')
            {
                SbPutc(&emitter->out, *s);
            }
        }
        SbPutc(&emitter->out, '"');
        return;
    }
    case NodeIdent:
        EmitLValue(emitter, node);
        return;
    case NodeMember:
    {
        const MemberExpr* member = (const MemberExpr*)node;
        const char* baseType = ExprType(emitter, member->base_node);

        if (IsSimdVector(baseType))
        {
            CSimdVectorDestructure(emitter, member, false);
            return;
        }

        /* array.length is a u64 query on the fat struct. */
        if (IsArrayType(baseType) && strcmp(member->member, "length") == 0)
        {
            SbPutc(&emitter->out, '(');
            CEmitExpr(emitter, member->base_node);
            SbPuts(&emitter->out, ").len");

            return;
        }

        /* box<simd>.swizzle or box<simd>.lane: unpack the box and route
           through the SIMD destructure. */
        {
            bool throughBox = IsOwningType(baseType);
            const char* inner = throughBox ? OwningInnerCStr(emitter->arena, baseType) : NULL;

            if (inner && IsSimdVector(inner))
            {
                CSimdVectorDestructure(emitter, member, true);
                return;
            }
        }

        bool throughBox = IsOwningType(baseType);
        SbPutc(&emitter->out, '(');
        CEmitExpr(emitter, member->base_node);
        SbPuts(&emitter->out, throughBox ? ")->" : ").");
        SbPuts(&emitter->out, FieldName(emitter, member->member));

        return;
    }
    case NodeIndex:
        EmitLValue(emitter, node);
        return;
    case NodeArrayInit:
        EmitArrayInitExpr(emitter, (const ArrayInitExpr*)node);
        return;
    case NodeUnary:
    {
        const UnaryExpr* unary = (const UnaryExpr*)node;
        SbPutc(&emitter->out, '(');
        SbPuts(&emitter->out, UnarySpelling(unary->op));
        EmitScalarValue(emitter, unary->operand);
        SbPutc(&emitter->out, ')');

        return;
    }
    case NodeBinary:
    {
        const BinaryExpr* binary = (const BinaryExpr*)node;

        const char* resultType = ExprType(emitter, node);

        if (binary->op == BinMod && IsFloatType(resultType))
        {
            SbPuts(&emitter->out, strcmp(resultType, "double") == 0 ? "fmod(" : "fmodf(");
            EmitScalarValue(emitter, binary->lhs);
            SbPuts(&emitter->out, ", ");
            EmitScalarValue(emitter, binary->rhs);
            SbPutc(&emitter->out, ')');

            return;
        }

        if (IsSimdVector(resultType))
        {
            CSimdVectorBinExpr(emitter, binary);
            return;
        }

        SbPutc(&emitter->out, '(');
        EmitScalarValue(emitter, binary->lhs);
        SbPutc(&emitter->out, ' ');
        SbPuts(&emitter->out, BinarySpelling(binary->op));
        SbPutc(&emitter->out, ' ');
        EmitScalarValue(emitter, binary->rhs);
        SbPutc(&emitter->out, ')');

        return;
    }
    case NodeAssign:
    {
        const AssignExpr* assign = (const AssignExpr*)node;

        const char* targetType = ExprType(emitter, assign->target);

        /* Whole-array rebind: free the old buffer (and owning elements), take
           the new fat struct, and null a moved source. */
        if (assign->op == AssignSet && IsArrayType(targetType)
            && strcmp(ExprType(emitter, assign->value), targetType) == 0)
        {
            Str inner = ArrayInnerStr(targetType);
            const char* elemType = StrNew(emitter->arena, inner.data, inner.len).data;
            const char* elemC = TypeNameC(emitter, elemType);
            bool elemOwning = IsOwningType(elemType);

            SbPuts(&emitter->out, "({ if (");
            EmitLValue(emitter, assign->target);
            SbPuts(&emitter->out, ".data) { ");

            if (elemOwning)
            {
                SbPuts(&emitter->out, "{ unsigned long long _i; for (_i = 0; _i < ");
                EmitLValue(emitter, assign->target);
                SbPuts(&emitter->out, ".len; _i++) { ");

                if (TypeRegistryIsOwningStruct(&emitter->types, elemType))
                {
                    SbPuts(&emitter->out, DropHelperName(emitter, elemType));
                    SbPuts(&emitter->out, "(&((");
                    SbPuts(&emitter->out, elemC);
                    SbPuts(&emitter->out, "*)");
                    EmitLValue(emitter, assign->target);
                    SbPuts(&emitter->out, ".data)[_i]); ");
                }
                else
                {
                    SbPuts(&emitter->out, "strata_free(((");
                    SbPuts(&emitter->out, elemC);
                    SbPuts(&emitter->out, "*)");
                    EmitLValue(emitter, assign->target);
                    SbPuts(&emitter->out, ".data)[_i]); ");
                }

                SbPuts(&emitter->out, "} } ");
            }

            SbPuts(&emitter->out, "strata_free(");
            EmitLValue(emitter, assign->target);
            SbPuts(&emitter->out, ".data); } ");

            EmitLValue(emitter, assign->target);
            SbPuts(&emitter->out, " = ");
            CEmitExpr(emitter, assign->value);
            SbPutc(&emitter->out, ';');

            const Node* movedSource = MovableBoxSourceNode(assign->value);

            if (movedSource)
            {
                SbPuts(&emitter->out, " ");
                EmitLValue(emitter, movedSource);
                SbPuts(&emitter->out, " = (strata__arr){0};");
            }

            SbPutc(&emitter->out, ' ');
            EmitLValue(emitter, assign->target);
            SbPuts(&emitter->out, "; })");

            return;
        }

        /* `=` rebinds the box only when the value is itself a box of the
           same type; any other assignment into a box<T> - including `=`
           with a plain T value, e.g. `x = 5;` - mutates its contents. */
        bool boxMove = assign->op == AssignSet && IsOwningType(targetType)
                       && strcmp(ExprType(emitter, assign->value), targetType) == 0;

        if (boxMove)
        {
            bool isStrLit = strcmp(targetType, "string") == 0 && assign->value->kind == NodeStrLiteral;

            SbPuts(&emitter->out, "(strata_free(");
            EmitLValue(emitter, assign->target);
            SbPuts(&emitter->out, "), (");
            EmitLValue(emitter, assign->target);
            SbPuts(&emitter->out, " = ");
            if (isStrLit)
            {
                SbPuts(&emitter->out, "strata_strdup(");
                CEmitExpr(emitter, assign->value);
                SbPutc(&emitter->out, ')');
            }
            else
            {
                CEmitExpr(emitter, assign->value);
            }
            SbPuts(&emitter->out, ")");

            const Node* movedSource = MovableBoxSourceNode(assign->value);

            if (movedSource)
            {
                SbPuts(&emitter->out, ", (");
                EmitLValue(emitter, movedSource);
                SbPuts(&emitter->out, " = 0)");
            }

            SbPuts(&emitter->out, ")");

            return;
        }

        if (!boxMove && IsOwningType(targetType))
        {
            /* Assigning a plain T (or compound-assigning) into a box<T>
               mutates its contents in place - not a move, so `x = 5;` and
               `val -= amt;` both work even through a `ref box<T>` param or
               a box global. */
            const char* inner = OwningInnerCStr(emitter->arena, targetType);

            if (IsOwningType(inner))
            {
                /* Content-assigning an OWNING inner (box<string> = "x" /
                   someString): drop only the old inner value in place (free
                   it), then store a freshly owned inner - strata_strdup a
                   literal, move a movable source. The box allocation itself
                   is kept, mirroring `box<int> = 5` except the replaced
                   inner is owning so it's freed first. */
                SbPuts(&emitter->out, "({ if (*");
                EmitLValue(emitter, assign->target);
                SbPuts(&emitter->out, ") strata_free(*");
                EmitLValue(emitter, assign->target);
                SbPuts(&emitter->out, "); *");
                EmitLValue(emitter, assign->target);
                SbPuts(&emitter->out, " = ");
                CEmitOwnedValue(emitter, assign->value, inner);
                SbPuts(&emitter->out, "; *");
                EmitLValue(emitter, assign->target);
                SbPuts(&emitter->out, "; })");
                return;
            }

            if (assign->op == AssignMod && IsFloatType(inner))
            {
                SbPuts(&emitter->out, "(*");
                EmitLValue(emitter, assign->target);
                SbPuts(&emitter->out, " = ");
                SbPuts(&emitter->out, strcmp(inner, "double") == 0 ? "fmod(*" : "fmodf(*");
                EmitLValue(emitter, assign->target);
                SbPuts(&emitter->out, ", ");
                EmitScalarValue(emitter, assign->value);
                SbPuts(&emitter->out, "))");

                return;
            }

            SbPuts(&emitter->out, "(*");
            EmitLValue(emitter, assign->target);
            SbPutc(&emitter->out, ' ');
            SbPuts(&emitter->out, AssignSpelling(assign->op));
            SbPutc(&emitter->out, ' ');
            EmitScalarValue(emitter, assign->value);
            SbPutc(&emitter->out, ')');

            return;
        }

        if (assign->op == AssignMod && IsFloatType(targetType))
        {
            SbPutc(&emitter->out, '(');
            EmitLValue(emitter, assign->target);
            SbPuts(&emitter->out, " = ");
            SbPuts(&emitter->out, strcmp(targetType, "double") == 0 ? "fmod(" : "fmodf(");
            EmitLValue(emitter, assign->target);
            SbPuts(&emitter->out, ", ");
            EmitScalarValue(emitter, assign->value);
            SbPuts(&emitter->out, "))");

            return;
        }

        SbPutc(&emitter->out, '(');
        EmitLValue(emitter, assign->target);
        SbPutc(&emitter->out, ' ');
        SbPuts(&emitter->out, AssignSpelling(assign->op));
        SbPutc(&emitter->out, ' ');
        EmitScalarValue(emitter, assign->value);
        SbPutc(&emitter->out, ')');

        return;
    }
    case NodeCall:
        EmitCall(emitter, (const CallExpr*)node);
        return;
    case NodeStructInit:
    {
        const StructInitExpr* init = (const StructInitExpr*)node;
        EmitStructInit(emitter, init->typeName, &init->fields);
        return;
    }
    case NodeIncDec:
    {
        const IncDecExpr* increment = (const IncDecExpr*)node;
        const char* operandType = ExprType(emitter, increment->operand);
        bool throughBox = IsOwningType(operandType);

        SbPutc(&emitter->out, '(');

        if (increment->isPrefix)
        {
            SbPuts(&emitter->out, increment->isDec ? "--" : "++");
        }

        /* Parenthesize the box dereference as a unit so postfix ++/--
           binds to the boxed int lvalue, not to the box pointer itself -
           `*(*inVal)++` would parse as `*((*inVal)++)` and increment the
           pointer instead of the pointee. */
        if (throughBox)
        {
            SbPuts(&emitter->out, "(*");
        }

        EmitLValue(emitter, increment->operand);

        if (throughBox)
        {
            SbPutc(&emitter->out, ')');
        }

        if (!increment->isPrefix)
        {
            SbPuts(&emitter->out, increment->isDec ? "--" : "++");
        }

        SbPutc(&emitter->out, ')');

        return;
    }
    case NodeCast:
    {
        const CastExpr* cast = (const CastExpr*)node;
        SbPuts(&emitter->out, "((");
        SbPuts(&emitter->out, TypeNameC(emitter, cast->type.name));
        SbPuts(&emitter->out, ")(");
        CEmitExpr(emitter, cast->operand);
        SbPuts(&emitter->out, "))");

        return;
    }
    default:
        DiagError(emitter->diag, node->range, "C backend encountered an unsupported expression");
        SbPuts(&emitter->out, "0");

        return;
    }
}

/* Emits a C expression that produces an owned value of 'innerType' from
   'init'. This is the C-backend mirror of the LLVM backend's EmitOwnedValue:
   - Non-owning inner: evaluate the expression as-is.
   - Owning inner + string literal: wrap in strata_strdup (heap-copy).
   - Owning inner + movable source: evaluate, then null the source via a
     comma expression:  (expr, (src = 0))
   - Owning inner + non-movable (call result): evaluate as-is.
   Used for string vars, box<T> inners, struct fields, return values, etc. */
static void CEmitOwnedValue(CEmitter* emitter, const Node* init, const char* innerType)
{
    if (!IsOwningType(innerType))
    {
        CEmitExpr(emitter, init);
        return;
    }

    if (init->kind == NodeStrLiteral)
    {
        SbPuts(&emitter->out, "strata_strdup(");
        CEmitExpr(emitter, init);
        SbPutc(&emitter->out, ')');
        return;
    }

    CEmitExpr(emitter, init);

    const Node* moved = MovableBoxSourceNode(init);

    if (moved)
    {
        SbPuts(&emitter->out, ", (");
        EmitLValue(emitter, moved);
        SbPuts(&emitter->out, " = 0)");
    }
}

/* Box a struct value by allocating it and assigning each init field, so that
   box<T> fields move their source (nulling it). Omitted fields stay zero/null. */
static void EmitBoxedStructInit(CEmitter* emitter, const char* cName, const char* innerC, const char* inner,
                                const StructInitExpr* si)
{
    SbPuts(&emitter->out, " = strata_alloc(sizeof(");
    SbPuts(&emitter->out, innerC);
    SbPuts(&emitter->out, "));\n");

    Pad(emitter);
    SbPutc(&emitter->out, '*');
    SbPuts(&emitter->out, cName);
    SbPuts(&emitter->out, " = (");
    SbPuts(&emitter->out, innerC);
    SbPuts(&emitter->out, "){0};\n");

    const StructType* st = TypeRegistryFind(&emitter->types, inner);
    size_t positional = 0;

    for (size_t k = 0; k < si->fields.count; ++k)
    {
        StructInitField* sf = (StructInitField*)VecGet(&si->fields, k);

        size_t idx;

        if (sf->name && sf->name[0] != '\0')
        {
            int named = TypeRegistryFieldIndex(&emitter->types, inner, sf->name);
            idx = named >= 0 ? (size_t)named : 0;
        }
        else
        {
            idx = positional++;
        }

        FieldDecl* fd = st ? (FieldDecl*)VecGet(&st->fields, idx) : NULL;

        if (!fd)
        {
            continue;
        }

        /* For a box<T> field, determine whether we need to box up the value
           (alloc + store) or can do a direct pointer move. Boxing is needed
           whenever the value type differs from the field type — a bare T
           into box<T>, a string literal into box<string>, etc. Only
           box<T> from the same box<T> is a direct move. */
        const char* valueType = ExprType(emitter, sf->value);
        bool fieldIsOwning = IsOwningType(fd->type.name);
        bool sameOwningType = fieldIsOwning && valueType[0] != '\0'
            && strcmp(valueType, fd->type.name) == 0;

        if (fieldIsOwning && !sameOwningType)
        {
            /* box<T> field from a non-box<T> value: box it up. */
            const char* fieldInner = OwningInnerCStr(emitter->arena, fd->type.name);
            const char* fieldInnerC = TypeNameC(emitter, fieldInner);

            Pad(emitter);
            SbPuts(&emitter->out, cName);
            SbPuts(&emitter->out, "->");
            SbPuts(&emitter->out, FieldName(emitter, fd->name));
            SbPuts(&emitter->out, " = strata_alloc(sizeof(");
            SbPuts(&emitter->out, fieldInnerC);
            SbPuts(&emitter->out, "));\n");

            Pad(emitter);
            SbPutc(&emitter->out, '*');
            SbPuts(&emitter->out, cName);
            SbPuts(&emitter->out, "->");
            SbPuts(&emitter->out, FieldName(emitter, fd->name));
            SbPuts(&emitter->out, " = ");
            CEmitOwnedValue(emitter, sf->value, fieldInner);
            SbPuts(&emitter->out, ";\n");
        }
        else
        {
            /* Direct assignment: same box<T> move, string field, or non-owning.
               For owning fields, CEmitOwnedValue handles strdup for literals
               and move+null for variables (the comma-expression works because
               = binds tighter than ,). */
            Pad(emitter);
            SbPuts(&emitter->out, cName);
            SbPuts(&emitter->out, "->");
            SbPuts(&emitter->out, FieldName(emitter, fd->name));
            SbPuts(&emitter->out, " = ");

            if (fieldIsOwning)
            {
                CEmitOwnedValue(emitter, sf->value, fd->type.name);
            }
            else
            {
                CEmitExpr(emitter, sf->value);
            }

            SbPuts(&emitter->out, ";\n");
        }

        /* CEmitOwnedValue already handles source nulling for owning fields. */
    }
}

/* Initializes an already-declared box global (EmitGlobals left it null). */
static void EmitBoxInitStmt(CEmitter* emitter, const char* cName, const char* innerC, const char* inner,
                            const Node* init)
{
    const char* initType = ExprType(emitter, (Node*)init);

    /* Direct move from another box<T> of the same kind: take pointer. */
    if (initType[0] != '\0' && IsOwningType(initType) && OwningInnerStr(initType).data)
    {
        Pad(emitter);
        SbPuts(&emitter->out, cName);
        SbPuts(&emitter->out, " = ");
        CEmitExpr(emitter, (Node*)init);
        SbPuts(&emitter->out, ";\n");

        return;
    }

    if (TypeRegistryIsOwningStruct(&emitter->types, inner) && init->kind == NodeStructInit)
    {
        Pad(emitter);
        SbPuts(&emitter->out, cName);
        EmitBoxedStructInit(emitter, cName, innerC, inner, (const StructInitExpr*)init);

        return;
    }

    /* Box up the inner value (generic for any T, owning or not). */
    Pad(emitter);
    SbPuts(&emitter->out, cName);
    SbPuts(&emitter->out, " = strata_alloc(sizeof(");
    SbPuts(&emitter->out, innerC);
    SbPuts(&emitter->out, "));\n");

    Pad(emitter);
    SbPutc(&emitter->out, '*');
    SbPuts(&emitter->out, cName);
    SbPuts(&emitter->out, " = ");
    CEmitOwnedValue(emitter, init, inner);
    SbPuts(&emitter->out, ";\n");
}

static void EmitVarDecl(CEmitter* emitter, const VarDeclStmt* declaration, bool semicolon)
{
    const char* cName = VarName(emitter, declaration->name);

    if (IsArrayType(declaration->type.name))
    {
        SbPuts(&emitter->out, "strata__arr ");
        SbPuts(&emitter->out, cName);
        SbPuts(&emitter->out, " = ");

        if (declaration->init)
        {
            const char* initType = ExprType(emitter, declaration->init);

            if (initType[0] != '\0' && IsArrayType(initType))
            {
                /* Move from another array: copy the fat struct, then null the
                   source so it isn't freed twice. */
                CEmitExpr(emitter, declaration->init);

                if (semicolon)
                {
                    SbPutc(&emitter->out, ';');
                }

                const Node* movedSource = MovableBoxSourceNode(declaration->init);

                if (movedSource)
                {
                    if (semicolon)
                    {
                        SbPutc(&emitter->out, '\n');
                        Pad(emitter);
                    }

                    EmitLValue(emitter, movedSource);
                    SbPuts(&emitter->out, " = (strata__arr){0}");

                    if (semicolon)
                    {
                        SbPutc(&emitter->out, ';');
                    }
                }
            }
            else
            {
                CEmitExpr(emitter, declaration->init);

                if (semicolon)
                {
                    SbPutc(&emitter->out, ';');
                }
            }
        }
        else
        {
            SbPuts(&emitter->out, "(strata__arr){0}");

            if (semicolon)
            {
                SbPutc(&emitter->out, ';');
            }
        }

        AddSymbol(emitter, declaration->name, declaration->type.name, cName, false);
        {
            OwnEntry* entry = (OwnEntry*)arena_alloc(emitter->arena, sizeof(OwnEntry));
            entry->cName = cName;
            entry->typeName = declaration->type.name;
            entry->byRef = false;
            VecPush(&emitter->boxVars, entry);
        }

        return;
    }

    if (IsOwningType(declaration->type.name))
    {
        bool isString = strcmp(declaration->type.name, "string") == 0;

        if (isString)
        {
            SbPuts(&emitter->out, "char * ");
            SbPuts(&emitter->out, cName);

            if (declaration->init)
            {
                SbPuts(&emitter->out, " = ");
                if (declaration->init->kind == NodeStrLiteral)
                {
                    SbPuts(&emitter->out, "strata_strdup(");
                    CEmitExpr(emitter, declaration->init);
                    SbPutc(&emitter->out, ')');
                }
                else
                {
                    /* Emit the owning source value. The move-out (nulling the
                       source so it isn't double-freed) is emitted below as a
                       separate statement — a bare comma here would parse as a
                       second declarator: `char * s = a, (b = 0);`. */
                    CEmitExpr(emitter, declaration->init);
                }
            }
            else
            {
                SbPuts(&emitter->out, " = 0");
            }

            if (semicolon)
            {
                SbPutc(&emitter->out, ';');
            }

            if (declaration->init && declaration->init->kind != NodeStrLiteral)
            {
                const Node* movedSrc = MovableBoxSourceNode(declaration->init);
                if (movedSrc)
                {
                    if (semicolon)
                    {
                        SbPutc(&emitter->out, '\n');
                        Pad(emitter);
                    }

                    EmitLValue(emitter, movedSrc);
                    SbPuts(&emitter->out, " = 0");

                    if (semicolon)
                    {
                        SbPutc(&emitter->out, ';');
                    }
                }
            }

            AddSymbol(emitter, declaration->name, declaration->type.name, cName, false);
            {
                OwnEntry* entry = (OwnEntry*)arena_alloc(emitter->arena, sizeof(OwnEntry));
                entry->cName = cName;
                entry->typeName = declaration->type.name;
                VecPush(&emitter->boxVars, entry);
            }

            return;
        }

        const char* inner = OwningInnerCStr(emitter->arena, declaration->type.name);
        const char* innerC = TypeNameC(emitter, inner);
        const char* initType = declaration->init ? ExprType(emitter, declaration->init) : "";

        SbPuts(&emitter->out, innerC);
        SbPuts(&emitter->out, " *");
        SbPutc(&emitter->out, ' ');
        SbPuts(&emitter->out, cName);

        if (initType[0] != '\0' && IsOwningType(initType) && strcmp(initType, declaration->type.name) == 0)
        {
            /* Direct move from the same box<T> type: take pointer, null source. */
            SbPuts(&emitter->out, " = ");
            CEmitExpr(emitter, declaration->init);

            if (semicolon)
            {
                SbPutc(&emitter->out, ';');
            }

            const Node* movedDeclSource = MovableBoxSourceNode(declaration->init);

            if (movedDeclSource)
            {
                if (semicolon)
                {
                    SbPutc(&emitter->out, '\n');
                    Pad(emitter);
                }

                EmitLValue(emitter, movedDeclSource);
                SbPuts(&emitter->out, " = 0");

                if (semicolon)
                {
                    SbPutc(&emitter->out, ';');
                }
            }
        }
        else if (declaration->init && TypeRegistryIsOwningStruct(&emitter->types, inner)
                 && declaration->init->kind == NodeStructInit)
        {
            EmitBoxedStructInit(emitter, cName, innerC, inner, (const StructInitExpr*)declaration->init);
        }
        else
        {
            /* Box up the inner value. For box<string> this is:
               construct a string (strdup literal / move source), then store
               into the allocated slot — identical to box<int> but the inner
               happens to be owning. */
            SbPuts(&emitter->out, " = strata_alloc(sizeof(");
            SbPuts(&emitter->out, innerC);
            SbPuts(&emitter->out, "));");

            if (declaration->init)
            {
                if (semicolon)
                {
                    SbPutc(&emitter->out, '\n');
                }

                Pad(emitter);
                SbPutc(&emitter->out, '*');
                SbPuts(&emitter->out, cName);
                SbPuts(&emitter->out, " = ");
                CEmitOwnedValue(emitter, declaration->init, inner);

                if (semicolon)
                {
                    SbPutc(&emitter->out, ';');
                }
            }
        }

        AddSymbol(emitter, declaration->name, declaration->type.name, cName, false);
        {
            OwnEntry* entry = (OwnEntry*)arena_alloc(emitter->arena, sizeof(OwnEntry));
            entry->cName = cName;
            entry->typeName = declaration->type.name;
            VecPush(&emitter->boxVars, entry);
        }

        return;
    }

    EmitType(emitter, &declaration->type);
    SbPutc(&emitter->out, ' ');

    SbPuts(&emitter->out, cName);
    SbPuts(&emitter->out, " = ");

    if (declaration->init)
    {
        /* box<T> -> T coercion: dereference the box pointer. */
        const char* initType = ExprType(emitter, declaration->init);

        if (IsOwningType(initType))
        {
            SbPutc(&emitter->out, '*');
        }

        CEmitExpr(emitter, declaration->init);
    }
    else
    {
        SbPutc(&emitter->out, '(');
        SbPuts(&emitter->out, TypeNameC(emitter, declaration->type.name));
        SbPuts(&emitter->out, "){0}");
    }

    if (semicolon)
    {
        SbPutc(&emitter->out, ';');
    }

    AddSymbol(emitter, declaration->name, declaration->type.name, cName, false);
}

static void EmitStmt(CEmitter* emitter, const Node* node);

/* Emit `if (v) { strata_free(v); v = 0; }` for box locals [fromIndex, count). */
static void EmitDrops(CEmitter* emitter, size_t fromIndex)
{
    for (size_t i = fromIndex; i < emitter->boxVars.count; ++i)
    {
        OwnEntry* e = (OwnEntry*)VecGet(&emitter->boxVars, i);
        const char* var = e->cName;

        if (IsArrayType(e->typeName))
        {
            /* T[]: the data pointer is freed; owning elements drop first.
               For a by-ref array param the struct lives at (*var). */
            const char* ref = e->byRef ? "(*" : "";
            const char* closeRef = e->byRef ? ")" : "";
            Str arrInner = ArrayInnerStr(e->typeName);
            const char* elemType = arrInner.data ? StrNew(emitter->arena, arrInner.data, arrInner.len).data : "void";
            const char* elemC = TypeNameC(emitter, elemType);

            Pad(emitter);
            SbPuts(&emitter->out, "if (");
            SbPuts(&emitter->out, ref);
            SbPuts(&emitter->out, var);
            SbPuts(&emitter->out, closeRef);
            SbPuts(&emitter->out, ".data) { ");

            if (IsOwningType(elemType))
            {
                SbPrintf(&emitter->out, "{ unsigned long long _i; for (_i = 0; _i < %s%s%s.len; _i++) { ", ref, var,
                         closeRef);

                if (TypeRegistryIsOwningStruct(&emitter->types, elemType))
                {
                    SbPuts(&emitter->out, DropHelperName(emitter, elemType));
                    SbPuts(&emitter->out, "&((");
                    SbPuts(&emitter->out, elemC);
                    SbPuts(&emitter->out, "*)");
                    SbPuts(&emitter->out, ref);
                    SbPuts(&emitter->out, var);
                    SbPuts(&emitter->out, closeRef);
                    SbPuts(&emitter->out, ".data)[_i]); ");
                }
                else
                {
                    SbPuts(&emitter->out, "strata_free(((");
                    SbPuts(&emitter->out, elemC);
                    SbPuts(&emitter->out, "*)");
                    SbPuts(&emitter->out, ref);
                    SbPuts(&emitter->out, var);
                    SbPuts(&emitter->out, closeRef);
                    SbPuts(&emitter->out, ".data)[_i]); ");
                }

                SbPuts(&emitter->out, "} } ");
            }

            if (!e->stackBuffer)
            {
                SbPuts(&emitter->out, "strata_free(");
                SbPuts(&emitter->out, ref);
                SbPuts(&emitter->out, var);
                SbPuts(&emitter->out, closeRef);
                SbPuts(&emitter->out, ".data); ");
            }

            SbPutc(&emitter->out, '}');
            SbPutc(&emitter->out, '\n');

            continue;
        }

        const char* inner = OwningInnerCStr(emitter->arena, e->typeName);

        /* For a by-ref box param, the box pointer lives at *var (caller's slot). */
        const char* star = e->byRef ? "*" : "";

        Pad(emitter);
        SbPuts(&emitter->out, "if (");
        SbPuts(&emitter->out, star);
        SbPuts(&emitter->out, var);
        SbPuts(&emitter->out, ") { ");

        /* A box of an owning inner owns that inner too. An owning struct
           drops its fields recursively; an owning primitive (string) is a
           single heap pointer freed directly; a plain value (int) needs no
           inner drop. */
        if (inner)
        {
            if (TypeRegistryIsOwningStruct(&emitter->types, inner))
            {
                SbPuts(&emitter->out, DropHelperName(emitter, inner));
                SbPuts(&emitter->out, "(");
                SbPuts(&emitter->out, star);
                SbPuts(&emitter->out, var);
                SbPuts(&emitter->out, "); ");
            }
            else if (IsOwningType(inner))
            {
                SbPuts(&emitter->out, "strata_free(*");
                SbPuts(&emitter->out, star);
                SbPuts(&emitter->out, var);
                SbPuts(&emitter->out, "); ");
            }
        }

        SbPuts(&emitter->out, "strata_free(");
        SbPuts(&emitter->out, star);
        SbPuts(&emitter->out, var);
        SbPuts(&emitter->out, "); ");
        SbPuts(&emitter->out, star);
        SbPuts(&emitter->out, var);
        SbPuts(&emitter->out, " = 0; }\n");
    }
}

static void EmitControlledStmt(CEmitter* emitter, const Node* node)
{
    if (node && node->kind == NodeBlock)
    {
        EmitStmt(emitter, node);

        return;
    }

    SbPuts(&emitter->out, "{\n");
    emitter->indent++;
    EmitStmt(emitter, node);
    emitter->indent--;
    Pad(emitter);
    SbPutc(&emitter->out, '}');
}

static void EmitStmt(CEmitter* emitter, const Node* node)
{
    if (!node)
    {
        return;
    }

    switch (node->kind)
    {
    case NodeBlock:
    {
        const Block* block = (const Block*)node;
        SbPuts(&emitter->out, "{\n");

        emitter->indent++;

        size_t boxMark = emitter->boxVars.count;
        emitter->terminated = false;

        for (size_t i = 0; i < block->statements.count; ++i)
        {
            const Node* stmt = (const Node*)VecGet(&block->statements, i);

            if (emitter->terminated)
            {
                break;
            }

            EmitStmt(emitter, stmt);

            if (stmt->kind == NodeIf || stmt->kind == NodeWhile || stmt->kind == NodeFor)
            {
                emitter->terminated = false;
            }
        }

        if (!emitter->terminated)
        {
            EmitDrops(emitter, boxMark);
        }

        emitter->boxVars.count = boxMark;

        emitter->indent--;

        Pad(emitter);
        SbPutc(&emitter->out, '}');

        return;
    }
    case NodeReturn:
    {
        const ReturnStmt* statement = (const ReturnStmt*)node;

        const char* valueType = statement->value ? ExprType(emitter, statement->value) : "";
        bool targetIsBox = emitter->currentReturn && IsOwningType(emitter->currentReturn);

        /* The return type is box<T> but the expression produces a plain T
           (e.g. `return Pistol{...};` from a box<Pistol>-returning
           function): box it into a temporary, same as a `box<T> x = ...;`
           local, then return the pointer. */
        if (statement->value && targetIsBox
            && !(valueType[0] != '\0' && strcmp(valueType, emitter->currentReturn) == 0))
        {
            const char* inner = OwningInnerCStr(emitter->arena, emitter->currentReturn);
            const char* innerC = TypeNameC(emitter, inner);
            char tmp[32];
            snprintf(tmp, sizeof tmp, "strata__ret%u", emitter->retCounter++);

            Pad(emitter);
            SbPuts(&emitter->out, innerC);
            SbPuts(&emitter->out, " *");
            SbPuts(&emitter->out, tmp);
            SbPuts(&emitter->out, ";\n");

            EmitBoxInitStmt(emitter, tmp, innerC, inner, statement->value);
            EmitDrops(emitter, 0);

            Pad(emitter);
            SbPuts(&emitter->out, "return ");
            SbPuts(&emitter->out, tmp);
            SbPuts(&emitter->out, ";\n");

            emitter->terminated = true;
            return;
        }

        /* A bare box<T> identifier is only a move if the function returns
           box<T>; otherwise it's a deref-read. */
        const Node* movedReturnSource = IsOwningType(valueType) ? MovableBoxSourceNode(statement->value) : NULL;
        bool movesBox = movedReturnSource && targetIsBox && strcmp(emitter->currentReturn, valueType) == 0;

        /* An owning value returned as a non-owning type (e.g. box<int>
           field access returned as int) must be dereferenced to read.
           Arrays are excluded: they're returned by value as a {ptr, len}
           struct, not through a pointer deref. */
        bool needsBoxDeref = statement->value && valueType[0] != '\0'
            && IsOwningType(valueType) && !IsArrayType(valueType) && !movesBox;

        if (statement->value && emitter->boxVars.count > 0)
        {
            /* Keep box members alive across the drop by materializing the
               return value first. */
            char tmp[32];
            snprintf(tmp, sizeof tmp, "strata__ret%u", emitter->retCounter++);

            Pad(emitter);
            SbPuts(&emitter->out, TypeNameC(emitter, emitter->currentReturn ? emitter->currentReturn : "int"));
            SbPutc(&emitter->out, ' ');
            SbPuts(&emitter->out, tmp);
            SbPuts(&emitter->out, " = ");

            if (needsBoxDeref)
            {
                SbPuts(&emitter->out, "(*");
                CEmitExpr(emitter, statement->value);
                SbPutc(&emitter->out, ')');
            }
            else
            {
                CEmitExpr(emitter, statement->value);
            }

            SbPuts(&emitter->out, ";\n");

            /* Returning an owning value moves it out: null the source so the
               drop below does not free it. */
            if (movesBox)
            {
                Pad(emitter);
                EmitLValue(emitter, movedReturnSource);
                if (IsArrayType(valueType))
                {
                    /* An array is a {ptr, len} struct: zero the whole struct,
                       not just the pointer. */
                    SbPuts(&emitter->out, " = (strata__arr){0};\n");
                }
                else
                {
                    SbPuts(&emitter->out, " = 0;\n");
                }
            }

            EmitDrops(emitter, 0);

            Pad(emitter);
            SbPuts(&emitter->out, "return ");
            SbPuts(&emitter->out, tmp);
            SbPuts(&emitter->out, ";\n");

            emitter->terminated = true;
        }
        else
        {
            /* Drop live owning locals before a plain return (a return inside
               a nested block must not fall through to a later block-level
               drop). */
            if (emitter->boxVars.count > 0)
            {
                EmitDrops(emitter, 0);
            }

            Pad(emitter);
            SbPuts(&emitter->out, "return");

            if (statement->value)
            {
                SbPutc(&emitter->out, ' ');

                if (needsBoxDeref)
                {
                    SbPuts(&emitter->out, "(*");
                    CEmitExpr(emitter, statement->value);
                    SbPutc(&emitter->out, ')');
                }
                else
                {
                    CEmitExpr(emitter, statement->value);
                }
            }

            SbPuts(&emitter->out, ";\n");
        }

        emitter->terminated = true;
        return;
    }
    case NodeVarDecl:
        Pad(emitter);
        EmitVarDecl(emitter, (const VarDeclStmt*)node, true);
        SbPutc(&emitter->out, '\n');

        return;
    case NodeExprStmt:
    {
        Pad(emitter);
        const ExprStmt* statement = (const ExprStmt*)node;

        if (statement->expr)
        {
            CEmitExpr(emitter, statement->expr);
        }

        SbPuts(&emitter->out, ";\n");

        return;
    }
    case NodeIf:
    {
        const IfStmt* statement = (const IfStmt*)node;
        Pad(emitter);
        SbPuts(&emitter->out, "if (");
        CEmitExpr(emitter, statement->condition);
        SbPuts(&emitter->out, ") ");
        EmitControlledStmt(emitter, statement->thenBranch);

        if (statement->elseBranch)
        {
            SbPuts(&emitter->out, " else ");
            EmitControlledStmt(emitter, statement->elseBranch);
        }

        SbPutc(&emitter->out, '\n');

        return;
    }
    case NodeWhile:
    {
        const WhileStmt* statement = (const WhileStmt*)node;
        Pad(emitter);
        SbPuts(&emitter->out, "while (");
        CEmitExpr(emitter, statement->condition);
        SbPuts(&emitter->out, ") ");
        EmitControlledStmt(emitter, statement->body);
        SbPutc(&emitter->out, '\n');

        return;
    }
    case NodeFor:
    {
        const ForStmt* statement = (const ForStmt*)node;
        Pad(emitter);
        SbPuts(&emitter->out, "for (");

        if (statement->init)
        {
            if (statement->init->kind == NodeVarDecl)
            {
                EmitVarDecl(emitter, (const VarDeclStmt*)statement->init, false);
            }
            else
            {
                CEmitExpr(emitter, statement->init);
            }
        }

        SbPuts(&emitter->out, "; ");

        if (statement->condition)
        {
            CEmitExpr(emitter, statement->condition);
        }

        SbPuts(&emitter->out, "; ");

        if (statement->update)
        {
            CEmitExpr(emitter, statement->update);
        }

        SbPuts(&emitter->out, ") ");
        EmitControlledStmt(emitter, statement->body);
        SbPutc(&emitter->out, '\n');

        return;
    }
    case NodeBreak:
        Pad(emitter);
        SbPuts(&emitter->out, "break;\n");

        return;
    case NodeContinue:
        Pad(emitter);
        SbPuts(&emitter->out, "continue;\n");

        return;
    default:
        Pad(emitter);
        CEmitExpr(emitter, node);
        SbPuts(&emitter->out, ";\n");

        return;
    }
}

static void EmitParam(CEmitter* emitter, const FunctionDecl* function, const ParamDecl* param)
{
    /* Extern string params cross as const char* by value, matching the LLVM
       backend and libc-style hosts. */
    if (function && function->isExtern && strcmp(param->type.name, "string") == 0)
    {
        SbPuts(&emitter->out, "const char * ");
        SbPuts(&emitter->out, VarName(emitter, param->name));

        return;
    }

    /* A typed rest param crosses as a strata__arr* (stack-backed). Emitted
       without `const` even for `const T... rest` - the read-only rule is
       enforced by sema, and the owned-drop mutates the fat struct. */
    if (param->isVarargRest)
    {
        SbPuts(&emitter->out, "strata__arr* ");
        SbPuts(&emitter->out, VarName(emitter, param->name));

        return;
    }

    EmitType(emitter, &param->type);

    if (ParamIsIndirectFor(emitter, function, param))
    {
        SbPutc(&emitter->out, '*');
    }

    SbPutc(&emitter->out, ' ');
    SbPuts(&emitter->out, VarName(emitter, param->name));
}

static const char* DropHelperName(CEmitter* emitter, const char* structName)
{
    return Encode(emitter, "strata__drop__", structName);
}

static void EmitDropField(CEmitter* emitter, const char* fieldC, const char* fieldType)
{
    if (IsOwningType(fieldType))
    {
        const char* inner = OwningInnerCStr(emitter->arena, fieldType);

        SbPrintf(&emitter->out, "    if (p->%s) { ", fieldC);

        if (inner && TypeRegistryIsOwningStruct(&emitter->types, inner))
        {
            SbPrintf(&emitter->out, "%s(p->%s); ", DropHelperName(emitter, inner), fieldC);
        }
        else if (inner && IsOwningType(inner))
        {
            SbPrintf(&emitter->out, "strata_free(*(p->%s)); ", fieldC);
        }

        SbPrintf(&emitter->out, "strata_free(p->%s); p->%s = 0; }\n", fieldC, fieldC);
    }
    else if (TypeRegistryIsOwningStruct(&emitter->types, fieldType))
    {
        SbPrintf(&emitter->out, "    %s(&(p->%s));\n", DropHelperName(emitter, fieldType), fieldC);
    }
}

static void EmitDropHelpers(CEmitter* emitter)
{
    /* Forward declarations first so helpers may reference each other. */
    for (size_t i = 0; i < emitter->types.count; ++i)
    {
        const StructType* t = &emitter->types.types[i];

        if (TypeRegistryIsOwningStruct(&emitter->types, t->name))
        {
            SbPrintf(&emitter->out, "static void %s(%s*);\n", DropHelperName(emitter, t->name),
                     TypeNameC(emitter, t->name));
        }
    }

    for (size_t i = 0; i < emitter->types.count; ++i)
    {
        const StructType* t = &emitter->types.types[i];

        if (!TypeRegistryIsOwningStruct(&emitter->types, t->name))
        {
            continue;
        }

        SbPrintf(&emitter->out,
                 "static void %s(%s* p) {\n"
                 "    if (!p) return;\n",
                 DropHelperName(emitter, t->name), TypeNameC(emitter, t->name));

        for (size_t j = 0; j < t->fields.count; ++j)
        {
            FieldDecl* f = (FieldDecl*)VecGet(&t->fields, j);
            EmitDropField(emitter, FieldName(emitter, f->name), f->type.name);
        }

        SbPuts(&emitter->out, "}\n");
    }

    SbPutc(&emitter->out, '\n');
}

static void EmitFunctionSignature(CEmitter* emitter, const FunctionDecl* function, const char* name)
{
    EmitType(emitter, &function->returnType);
    SbPutc(&emitter->out, ' ');
    SbPuts(&emitter->out, name);
    SbPutc(&emitter->out, '(');

    if (function->params.count == 0)
    {
        SbPuts(&emitter->out, "void");
    }
    else
    {
        for (size_t i = 0; i < function->params.count; ++i)
        {
            if (i > 0)
            {
                SbPuts(&emitter->out, ", ");
            }

            EmitParam(emitter, function, (const ParamDecl*)VecGet(&function->params, i));
        }
    }

    if (function->isCVararg)
    {
        SbPuts(&emitter->out, ", ...");
    }

    SbPutc(&emitter->out, ')');
}

static void EmitExternSlot(CEmitter* emitter, const FunctionDecl* function)
{
    EmitType(emitter, &function->returnType);
    SbPuts(&emitter->out, " (*");

    const char* slotName = ExternSlotName(emitter, function->name);
    SbPuts(&emitter->out, slotName);

    SbPuts(&emitter->out, ")(");

    if (function->params.count == 0)
    {
        SbPuts(&emitter->out, "void");
    }
    else
    {
        for (size_t i = 0; i < function->params.count; ++i)
        {
            if (i > 0)
            {
                SbPuts(&emitter->out, ", ");
            }

            const ParamDecl* param = (const ParamDecl*)VecGet(&function->params, i);

            /* Extern string params cross as const char* by value. */
            if (function->isExtern && strcmp(param->type.name, "string") == 0)
            {
                SbPuts(&emitter->out, "const char *");
            }
            else if (param->isVarargRest)
            {
                SbPuts(&emitter->out, "strata__arr*");
            }
            else
            {
                EmitType(emitter, &param->type);

                if (ParamIsIndirectFor(emitter, function, param))
                {
                    SbPutc(&emitter->out, '*');
                }
            }
        }
    }

    if (function->isCVararg)
    {
        SbPuts(&emitter->out, ", ...");
    }

    SbPuts(&emitter->out, ") = 0;\n");

    CBackendSymbol* symbol = (CBackendSymbol*)arena_alloc(emitter->arena, sizeof(CBackendSymbol));
    symbol->strataName = function->name;
    symbol->cName = slotName;
    symbol->isIntVoid = false;
    VecPush(&emitter->externs, symbol);
}

static void EmitStructBody(CEmitter* emitter, size_t index, unsigned char* states)
{
    const StructType* type = &emitter->types.types[index];
    if (type->opaque || states[index] == 2)
    {
        return;
    }

    if (states[index] == 1)
    {
        DiagErrorFmt(emitter->diag, TypeSourceRange(emitter, type->name), "struct '%s' has a by-value dependency cycle",
                     type->name);
        return;
    }

    states[index] = 1;

    for (size_t fieldIndex = 0; fieldIndex < type->fields.count; ++fieldIndex)
    {
        const FieldDecl* field = (const FieldDecl*)VecGet(&type->fields, fieldIndex);
        const StructType* dependency = TypeRegistryFind(&emitter->types, field->type.name);

        if (dependency && !dependency->opaque)
        {
            size_t dependencyIndex = (size_t)(dependency - emitter->types.types);
            EmitStructBody(emitter, dependencyIndex, states);
        }
    }

    states[index] = 2;

    const char* cName = TypeNameC(emitter, type->name);
    EmitLineDirective(emitter, TypeSourceRange(emitter, type->name));
    SbPuts(&emitter->out, "struct ");
    SbPuts(&emitter->out, cName);
    SbPuts(&emitter->out, " {\n");

    for (size_t j = 0; j < type->fields.count; ++j)
    {
        FieldDecl* field = (FieldDecl*)VecGet(&type->fields, j);
        SbPuts(&emitter->out, "    ");
        EmitType(emitter, &field->type);
        SbPutc(&emitter->out, ' ');
        SbPuts(&emitter->out, FieldName(emitter, field->name));
        SbPuts(&emitter->out, ";\n");
    }

    SbPuts(&emitter->out, "};\n");
}

static void EmitTypes(CEmitter* emitter)
{
    static const char* primitive[] = {"bool", "int", "uint", "float", "double"};

    // for (size_t p = 0; p < sizeof(primitive) / sizeof(primitive[0]); ++p)
    // {
    //     for (int lanes = 2; lanes <= 4; ++lanes)
    //     {
    //         const char* vectorName = arena_format(emitter->arena, "%s%d", primitive[p], lanes);
    //         const char* elementName = TypeNameC(emitter, primitive[p]);

    //         SbPuts(&emitter->out, "typedef struct { ");
    //         SbPuts(&emitter->out, elementName);
    //         SbPrintf(&emitter->out, " lane[%d]; } ", lanes);
    //         SbPuts(&emitter->out, TypeNameC(emitter, vectorName));
    //         SbPuts(&emitter->out, ";\n");
    //     }
    // }

    for (size_t i = 0; i < emitter->types.count; ++i)
    {
        const StructType* type = &emitter->types.types[i];
        const char* cName = TypeNameC(emitter, type->name);

        EmitLineDirective(emitter, TypeSourceRange(emitter, type->name));

        if (type->opaque && !type->incomplete)
        {
            SbPuts(&emitter->out, "typedef struct ");
            SbPuts(&emitter->out, Encode(emitter, "strata__handle_tag_", type->name));
            SbPuts(&emitter->out, "* ");
            SbPuts(&emitter->out, cName);
            SbPuts(&emitter->out, ";\n");
        }
        else
        {
            SbPuts(&emitter->out, "typedef struct ");
            SbPuts(&emitter->out, cName);
            SbPutc(&emitter->out, ' ');
            SbPuts(&emitter->out, cName);
            SbPuts(&emitter->out, ";\n");
        }
    }

    // NOTE: arena_alloc zeros it out
    unsigned char* states
        = (unsigned char*)arena_alloc(emitter->arena, emitter->types.count ? emitter->types.count : 1);

    for (size_t i = 0; i < emitter->types.count; ++i)
    {
        EmitStructBody(emitter, i, states);
    }

    SbPutc(&emitter->out, '\n');
}

static void EmitGlobals(CEmitter* emitter)
{
    for (size_t i = 0; i < emitter->mod->globals.count; ++i)
    {
        const GlobalDecl* global = (const GlobalDecl*)VecGet(&emitter->mod->globals, i);
        EmitLineDirective(emitter, global->base.range);
        EmitType(emitter, &global->type);
        SbPutc(&emitter->out, ' ');

        const char* cName = GlobalName(emitter, global->name);
        SbPuts(&emitter->out, cName);
        SbPuts(&emitter->out, " = ");

        if (IsArrayType(global->type.name))
        {
            /* Array globals start empty (no braced initializer supported at
               module scope). The module_init/teardown helpers handle alloc/free. */
            SbPuts(&emitter->out, "(strata__arr){0}");
        }
        else if (IsOwningType(global->type.name))
        {
            /* Boxing requires a runtime alloc, so the real init runs in
               __strata_module_init; the declared storage starts null. */
            SbPuts(&emitter->out, "0");
        }
        else if (global->init)
        {
            CEmitExpr(emitter, global->init);
        }
        else
        {
            SbPutc(&emitter->out, '(');
            SbPuts(&emitter->out, TypeNameC(emitter, global->type.name));
            SbPuts(&emitter->out, "){0}");
        }

        SbPuts(&emitter->out, ";\n");
        AddSymbol(emitter, global->name, global->type.name, cName, false);
    }

    if (emitter->mod->globals.count)
    {
        SbPutc(&emitter->out, '\n');
    }
}

static void EmitDeclarations(CEmitter* emitter)
{
    for (size_t i = 0; i < emitter->mod->functions.count; ++i)
    {
        const FunctionDecl* function = (const FunctionDecl*)VecGet(&emitter->mod->functions, i);
        EmitLineDirective(emitter, function->base.range);

        if (function->isExtern && (emitter->emitFlags & CEmitJIT) != 0)
        {
            EmitExternSlot(emitter, function);
        }
        else
        {
            if (function->isExtern)
            {
                SbPuts(&emitter->out, "extern ");
            }
            EmitFunctionSignature(emitter, function, FunctionName(emitter, function->mangledName));
            SbPuts(&emitter->out, ";\n");
        }

        if (!function->isExtern)
        {
            CBackendSymbol* symbol = (CBackendSymbol*)arena_alloc(emitter->arena, sizeof(CBackendSymbol));
            symbol->strataName = function->mangledName;
            symbol->cName = FunctionName(emitter, function->mangledName);
            symbol->isIntVoid = strcmp(function->returnType.name, "int") == 0 && function->params.count == 0;

            VecPush(&emitter->exports, symbol);
        }
    }

    SbPutc(&emitter->out, '\n');
}

/**
 * @brief Find a `NodeKind` in a given `Node` and check recursively through nested blocks. This only checks for definite
 * statements, ignoring any conditional nodes.
 *
 * @returns The node of type `kind` if it was found, or NULL otherwise.
 */
static const Node* FindDefiniteNodeKind(const Node* node, NodeKind kind)
{
    if (node->kind == kind)
    {
        return node;
    }

    if (node->kind == NodeBlock)
    {
        const Block* block = (const Block*)node;

        /* Work backwards for faster return block checks. */
        for (long long i = (long long)block->statements.count - 1; i >= 0; i--)
        {
            const Node* result = FindDefiniteNodeKind(block->statements.items[i], kind);
            if (result != NULL)
            {
                return result;
            }
        }
    }

    return NULL;
}

static bool HasConcludingReturnStmt(const Block* body)
{
    /* No statements in definition */
    if (body->statements.count < 1)
    {
        return false;
    }

    /* Check if there are any return statements that are guaranteed to occur. Note that we cannot just check the final
       node in the block as there may be a terminating return statement above with dead code below. */
    const Node* returnStmt = FindDefiniteNodeKind((const Node*)body, NodeReturn);

    return (returnStmt != NULL);
}

static void EmitDefinitions(CEmitter* emitter)
{
    for (size_t i = 0; i < emitter->mod->functions.count; ++i)
    {
        const FunctionDecl* function = (const FunctionDecl*)VecGet(&emitter->mod->functions, i);

        if (!function->body)
        {
            continue;
        }

        DisposeMap(&emitter->symbols);
        StrMapInit(&emitter->symbols);
        emitter->boxVars.count = 0;
        emitter->retCounter = 0;
        emitter->boxTmpCounter = 0;
        emitter->currentReturn = function->returnType.name;

        for (size_t j = 0; j < emitter->mod->globals.count; ++j)
        {
            const GlobalDecl* global = (const GlobalDecl*)VecGet(&emitter->mod->globals, j);

            AddSymbol(emitter, global->name, global->type.name, GlobalName(emitter, global->name), false);
        }
        for (size_t j = 0; j < function->params.count; ++j)
        {
            const ParamDecl* param = (const ParamDecl*)VecGet(&function->params, j);

            AddSymbol(emitter, param->name, param->type.name, VarName(emitter, param->name),
                      ParamIsIndirect(emitter, param));

            {
                CSymbol* sym = (CSymbol*)StrMapGet(&emitter->symbols, param->name);
                if (sym)
                {
                    sym->aliased = ParamIsAliasedRest(emitter, param);
                }
            }

            /* An owned (non-ref) box parameter is consumed: freed at return.
               A vararg rest array is stack-backed: drop its owning elements
               but not the caller's stack buffer. */
            if (IsOwningType(param->type.name) && param->mod == ModNone)
            {
                OwnEntry* entry = (OwnEntry*)arena_alloc(emitter->arena, sizeof(OwnEntry));
                entry->cName = VarName(emitter, param->name);
                entry->typeName = param->type.name;
                entry->byRef = true;
                entry->stackBuffer = param->isVarargRest;
                VecPush(&emitter->boxVars, entry);
            }
        }

        EmitLineDirective(emitter, function->base.range);
        EmitFunctionSignature(emitter, function, FunctionName(emitter, function->mangledName));
        SbPuts(&emitter->out, " {\n");

        emitter->indent++;

        const Block* body = (const Block*)function->body;
        emitter->terminated = false;

        for (size_t j = 0; j < body->statements.count; ++j)
        {
            const Node* stmt = (const Node*)VecGet(&body->statements, j);

            if (emitter->terminated)
            {
                break;
            }

            EmitStmt(emitter, stmt);

            if (stmt->kind == NodeIf || stmt->kind == NodeWhile || stmt->kind == NodeFor)
            {
                emitter->terminated = false;
            }
        }

        if (!emitter->terminated)
        {
            EmitDrops(emitter, 0);

            Pad(emitter);

            /* Add a trailing return statment if no returns were provided */
            if (!HasConcludingReturnStmt(body))
            {

                if (strcmp(function->returnType.name, "void") == 0)
                {
                    SbPuts(&emitter->out, "return;\n");
                }
                else
                {
                    SbPuts(&emitter->out, "return (");
                    SbPuts(&emitter->out, TypeNameC(emitter, function->returnType.name));
                    SbPuts(&emitter->out, "){0};\n");
                }
            }
        }

        emitter->boxVars.count = 0;

        emitter->indent--;

        SbPuts(&emitter->out, "}\n");
        SbPutc(&emitter->out, '\n');
    }
}

static bool ModuleHasBoxGlobals(const CEmitter* emitter)
{
    for (size_t i = 0; i < emitter->mod->globals.count; ++i)
    {
        const GlobalDecl* global = (const GlobalDecl*)VecGet(&emitter->mod->globals, i);

        if (IsOwningType(global->type.name))
        {
            return true;
        }
    }

    return false;
}

/* Resets per-function scratch state for a synthetic module-level function. */
static void ResetEmitterForSyntheticFunction(CEmitter* emitter)
{
    DisposeMap(&emitter->symbols);
    StrMapInit(&emitter->symbols);
    emitter->boxVars.count = 0;
    emitter->retCounter = 0;
    emitter->boxTmpCounter = 0;
    emitter->currentReturn = "void";

    for (size_t j = 0; j < emitter->mod->globals.count; ++j)
    {
        const GlobalDecl* global = (const GlobalDecl*)VecGet(&emitter->mod->globals, j);

        AddSymbol(emitter, global->name, global->type.name, GlobalName(emitter, global->name), false);
    }
}

/* Boxes each global, called once after the JIT/host loads the module. */
static void EmitModuleInit(CEmitter* emitter)
{
    if (!ModuleHasBoxGlobals(emitter))
    {
        return;
    }

    ResetEmitterForSyntheticFunction(emitter);

    SbPuts(&emitter->out, "void __strata_module_init(void) {\n");
    emitter->indent++;

    for (size_t i = 0; i < emitter->mod->globals.count; ++i)
    {
        const GlobalDecl* global = (const GlobalDecl*)VecGet(&emitter->mod->globals, i);

        if (IsArrayType(global->type.name) && global->init)
        {
            /* Array literals need a runtime alloc, so the braced initializer
               runs here against the (zeroed) global storage. */
            const char* cName = GlobalName(emitter, global->name);

            AddSymbol(emitter, global->name, global->type.name, cName, false);
            Pad(emitter);
            SbPuts(&emitter->out, cName);
            SbPuts(&emitter->out, " = ");
            CEmitExpr(emitter, global->init);
            SbPuts(&emitter->out, ";\n");
            continue;
        }

        if (!IsOwningType(global->type.name) || !global->init)
        {
            continue;
        }

        const char* cName = GlobalName(emitter, global->name);
        AddSymbol(emitter, global->name, global->type.name, cName, false);

        if (strcmp(global->type.name, "string") == 0)
        {
            /* A string global owns its buffer: copy a literal (so teardown can
               free it) or assign a fresh call result. */
            Pad(emitter);
            SbPuts(&emitter->out, cName);
            SbPuts(&emitter->out, " = ");

            if (global->init->kind == NodeStrLiteral)
            {
                SbPuts(&emitter->out, "strata_strdup(");
                CEmitExpr(emitter, global->init);
                SbPutc(&emitter->out, ')');
            }
            else
            {
                CEmitExpr(emitter, global->init);
            }

            SbPuts(&emitter->out, ";\n");
            continue;
        }

        const char* inner = OwningInnerCStr(emitter->arena, global->type.name);
        const char* innerC = TypeNameC(emitter, inner);

        EmitBoxInitStmt(emitter, cName, innerC, inner, global->init);
    }

    emitter->indent--;
    SbPuts(&emitter->out, "}\n\n");
}

/* Frees every box global, called once before the module/JIT is unloaded. */
static void EmitModuleTeardown(CEmitter* emitter)
{
    if (!ModuleHasBoxGlobals(emitter))
    {
        return;
    }

    ResetEmitterForSyntheticFunction(emitter);

    for (size_t i = 0; i < emitter->mod->globals.count; ++i)
    {
        const GlobalDecl* global = (const GlobalDecl*)VecGet(&emitter->mod->globals, i);

        if (!IsOwningType(global->type.name))
        {
            continue;
        }

        OwnEntry* entry = (OwnEntry*)arena_alloc(emitter->arena, sizeof(OwnEntry));
        entry->cName = GlobalName(emitter, global->name);
        entry->typeName = global->type.name;
        entry->byRef = false;
        VecPush(&emitter->boxVars, entry);
    }

    SbPuts(&emitter->out, "void __strata_module_teardown(void) {\n");
    emitter->indent++;

    EmitDrops(emitter, 0);
    emitter->boxVars.count = 0;

    emitter->indent--;
    SbPuts(&emitter->out, "}\n\n");
}

void BuiltCModuleInit(BuiltCModule* module)
{
    module->source = NULL;
    VecInit(&module->exports);
    VecInit(&module->externs);
}

void BuiltCModuleDispose(BuiltCModule* module)
{
    free(module->exports.items);
    free(module->externs.items);
    BuiltCModuleInit(module);
}

BuiltCModule BuildCModuleWithSources(const Module* ast, DiagnosticEngine* diag, Arena* arena,
                                     const SourceManager* sources, size_t sourceCount, CBackendEmitFlags emitFlags,
                                     int arch, const StrataProfile* profile)
{
    BuiltCModule result;
    BuiltCModuleInit(&result);

    if (!ast)
    {
        DiagError(diag, SRC_INVALID, "C backend received a null module");
        return result;
    }

    if (arch == STRATA_ARCH_AUTO)
    {
        arch = ResolveArch(arch);
    }

    CEmitter emitter = {0};
    emitter.mod = ast;
    emitter.diag = diag;
    emitter.arena = arena;
    emitter.emitFlags = emitFlags;
    emitter.arch = arch;
    emitter.sources = sources;
    emitter.sourceCount = sourceCount;
    emitter.boundsCheck = !profile || profile->boundsCheck;
    emitter.nullExternCall = !profile || profile->nullExternCall;
    TypeRegistryInit(&emitter.types);
    TypeRegistryBuild(&emitter.types, ast);
    StrMapInit(&emitter.symbols);
    SbInit(&emitter.out);
    VecInit(&emitter.exports);
    VecInit(&emitter.externs);
    VecInit(&emitter.boxVars);
    emitter.currentReturn = "int";
    emitter.retCounter = 0;
    emitter.boxTmpCounter = 0;

    SbPuts(&emitter.out, "/* Generated by Strata. */\n"
                         "_Static_assert(sizeof(int) == 4, \"Strata requires 32-bit int\");\n"
                         "extern void* strata_alloc(unsigned long long);\n"
                         "extern void strata_free(void*);\n"
                         "extern void strata_panic(const char* msg);\n"
                         "static char* strata_strdup(const char* s) {\n"
                         "  unsigned long n = 0; while (s[n]) n++;\n"
                         "  char* d = (char*)strata_alloc(n + 1);\n"
                         "  if (d) { unsigned long i; for (i = 0; i <= n; i++) d[i] = s[i]; }\n"
                         "  return d;\n"
                         "}\n"
                         "extern float fmodf(float, float);\n"
                         "extern double fmod(double, double);\n\n");

    if ((emitFlags & CEmitJIT) != 0 && emitter.nullExternCall)
    {
        /* Guards a JIT extern call against an unbound (null) slot. */
        SbPuts(&emitter.out,
               "static void strata__ext_check(void* _slot, const char* _name) { if (!_slot) "
               "strata_panic(_name); }\n\n");
    }

    if ((emitFlags & CEmitEnableSIMD) != 0)
    {
        /* Emit includes to SIMD headers */
        switch (arch)
        {
        case STRATA_ARCH_X64:
            SbPuts(&emitter.out, "#include <immintrin.h>\n");
            break;
        case STRATA_ARCH_ARM64:
            SbPuts(&emitter.out, "#include <arm_neon.h>\n");
            break;
        case STRATA_ARCH_AUTO:
        default:;
        }
    }
    else
    {
        SbPuts(&emitter.out,
               "typedef struct __strata_float128 { float x; float y; float z; float w; } __strata_float128;\n");
    }

    /* Fat {data, len} struct shared by every T[]. */
    SbPuts(&emitter.out, "typedef struct { void* data; unsigned long long len; } strata__arr;\n\n");

    EmitTypes(&emitter);
    EmitDropHelpers(&emitter);
    EmitGlobals(&emitter);
    EmitDeclarations(&emitter);
    EmitDefinitions(&emitter);
    EmitModuleInit(&emitter);
    EmitModuleTeardown(&emitter);

    result.source = SbFinish(&emitter.out, arena);
    result.exports = emitter.exports;
    result.externs = emitter.externs;

    DisposeMap(&emitter.symbols);
    free(emitter.boxVars.items);
    TypeRegistryFree(&emitter.types);

    return result;
}

BuiltCModule BuildCModule(const Module* ast, DiagnosticEngine* diag, Arena* arena, CBackendEmitFlags emitFlags,
                          int arch, const StrataProfile* profile)
{
    return BuildCModuleWithSources(ast, diag, arena, NULL, 0, emitFlags, arch, profile);
}

CodegenResult GenerateC(const Module* mod, int arch)
{
    CodegenResult result = {0};
    result.moduleName = mod ? mod->name : NULL;

    Arena arena;
    arena_init(&arena, 0);

    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);

    StrataProfile profile = strataProfileDefault();

    BuiltCModule module = BuildCModule(mod, &diag, &arena, CEmitNone, arch, &profile);

    result.ok = !DiagHasErrors(&diag);
    result.output = DupString(module.source ? module.source : "");

    BuiltCModuleDispose(&module);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);

    return result;
}
