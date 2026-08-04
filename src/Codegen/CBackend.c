#include "Codegen/CBackend.h"

#include "Codegen/CodegenBackend.h"
#include "Codegen/TypeRegistry.h"
#include "Codegen/TypeUtil.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    const char* typeName;
    const char* cName;
    bool indirect;
} CSymbol;

typedef struct
{
    const Module* mod;
    DiagnosticEngine* diag;
    Arena* arena;
    TypeRegistry types;
    StrMap symbols;
    Sb out;
    Vec exports;
    Vec externs;
    bool jitMode;
    unsigned indent;
    const SourceManager* sources;
    size_t sourceCount;
    Vec boxVars;               /* in-scope box-local OwnEntry* (current function) */
    const char* currentReturn; /* current function's return type name */
    unsigned retCounter;
    unsigned boxTmpCounter; /* unique names for inline struct-init field boxing */
} CEmitter;

typedef struct
{
    const char* cName;
    const char* typeName;
    bool byRef; /* by-ref box param: cName is a pointer to the caller's box slot */
} OwnEntry;

static const char* DropHelperName(CEmitter* emitter, const char* structName);

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

static const char* TypeNameC(CEmitter* emitter, const char* name)
{
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
        if (mapped.vec > 1)
        {
            return Encode(emitter, "strata__vec_", name);
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
        const char* _inner = OwningInnerCStr(emitter->arena, baseName);
        if (_inner)
        {
            baseName = _inner;
        }

        const StructType* type = TypeRegistryFind(&emitter->types, baseName);
        int index = type ? TypeRegistryFieldIndex(&emitter->types, baseName, member->member) : -1;

        return index >= 0 ? ((const FieldDecl*)VecGet(&type->fields, (size_t)index))->type.name : "";
    }
    case NodeStructInit:
        return ((const StructInitExpr*)node)->typeName;
    default:
        return "";
    }
}

static bool IsLValue(const Node* node)
{
    return node && (node->kind == NodeIdent || node->kind == NodeMember);
}

/* Unwraps casts to find the lvalue being moved (identifier or field chain),
   for nulling via EmitLValue. */
static const Node* MovableBoxSource(const Node* n)
{
    while (n && n->kind == NodeCast)
    {
        n = ((const CastExpr*)n)->operand;
    }

    return IsLValue(n) ? n : NULL;
}

static void EmitExpr(CEmitter* emitter, const Node* node);
static void EmitScalarValue(CEmitter* emitter, const Node* node);

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
        bool throughBox = IsOwningType(baseType);
        SbPutc(&emitter->out, '(');
        EmitLValue(emitter, member->base_node);
        SbPuts(&emitter->out, throughBox ? ")->" : ").");
        SbPuts(&emitter->out, FieldName(emitter, member->member));

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

            if (valueType[0] != '\0' && !IsOwningType(valueType))
            {
                /* box<T> field initialized from a bare T value/literal -
                   heap-box it inline. A raw `.field = (T){...}` here would
                   be a type mismatch in C (the field is really T*), and
                   silently truncating/reinterpreting it would corrupt the
                   struct - so allocate storage for it via a GNU statement
                   expression, which is valid in any expression position
                   (including a nested struct-init field). */
                const char* inner = OwningInnerCStr(emitter->arena, declaration->type.name);
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
                EmitExpr(emitter, field->value);
                SbPuts(&emitter->out, "; ");
                SbPuts(&emitter->out, tmp);
                SbPuts(&emitter->out, "; })");

                continue;
            }
        }

        EmitExpr(emitter, field->value);
    }

    SbPuts(&emitter->out, " }");
}

static void EmitPseudoCall(CEmitter* emitter, const CallExpr* call)
{
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

    if (emitter->jitMode && function && function->isExtern)
    {
        callee = ExternSlotName(emitter, function->name);
    }

    SbPuts(&emitter->out, callee);
    SbPutc(&emitter->out, '(');

    for (size_t i = 0; i < call->args.count; ++i)
    {
        if (i > 0)
        {
            SbPuts(&emitter->out, ", ");
        }

        const Node* argument = (const Node*)VecGet(&call->args, i);
        const ParamDecl* parameter
            = function && i < function->params.count ? (const ParamDecl*)VecGet(&function->params, i) : NULL;

        if (parameter && ParamIsIndirect(emitter, parameter))
        {
            /* box<T> coerced to T: if the param is a plain struct (not box),
               and the arg is a box, the heap pointer IS the T* the param wants. */
            bool paramIsBoxType = IsOwningType(parameter->type.name);
            const char* argType = ExprType(emitter, argument);
            bool argIsBox = IsOwningType(argType);

            if (!paramIsBoxType && argIsBox)
            {
                EmitExpr(emitter, argument);
            }
            else if (IsLValue(argument))
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
                EmitExpr(emitter, argument);
                SbPuts(&emitter->out, " })[0]");
            }
        }
        else
        {
            /* A box arg passed to a by-value (non-indirect) param - e.g. a
               plain handle - must be dereferenced to its value, not passed
               as the box's own heap pointer. */
            EmitScalarValue(emitter, argument);
        }
    }

    SbPutc(&emitter->out, ')');
}

/* Emits a box-typed identifier dereferenced to its value, for use as a
   plain unary/binary operand (as opposed to a member-access base or a
   box-to-box move, which need the raw pointer). */
static void EmitScalarValue(CEmitter* emitter, const Node* node)
{
    if (node && node->kind == NodeIdent && IsOwningType(ExprType(emitter, node)))
    {
        SbPuts(&emitter->out, "(*");
        EmitLValue(emitter, node);
        SbPutc(&emitter->out, ')');

        return;
    }

    EmitExpr(emitter, node);
}

static void EmitExpr(CEmitter* emitter, const Node* node)
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
        bool throughBox = IsOwningType(baseType);
        SbPutc(&emitter->out, '(');
        EmitExpr(emitter, member->base_node);
        SbPuts(&emitter->out, throughBox ? ")->" : ").");
        SbPuts(&emitter->out, FieldName(emitter, member->member));

        return;
    }
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
                EmitExpr(emitter, assign->value);
                SbPutc(&emitter->out, ')');
            }
            else
            {
                EmitExpr(emitter, assign->value);
            }
            SbPuts(&emitter->out, ")");

            const Node* movedSource = MovableBoxSource(assign->value);

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
        EmitExpr(emitter, cast->operand);
        SbPuts(&emitter->out, "))");

        return;
    }
    default:
        DiagError(emitter->diag, node->range, "C backend encountered an unsupported expression");
        SbPuts(&emitter->out, "0");

        return;
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

        bool fieldNeedsBoxing = IsOwningType(fd->type.name);

        if (fieldNeedsBoxing)
        {
            const char* valueType = ExprType(emitter, sf->value);
            fieldNeedsBoxing = valueType[0] != '\0' && !IsOwningType(valueType);
        }

        if (fieldNeedsBoxing)
        {
            /* box<T> field initialized from a bare T value/literal - heap-box
               it, same as a top-level `box<T> x = T{...};` local. Without
               this, the field (really a T*) would be assigned a raw T
               value: a C type mismatch that, if silently coerced, would
               corrupt the struct. */
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
            EmitExpr(emitter, sf->value);
            SbPuts(&emitter->out, ";\n");
        }
        else
        {
            Pad(emitter);
            SbPuts(&emitter->out, cName);
            SbPuts(&emitter->out, "->");
            SbPuts(&emitter->out, FieldName(emitter, fd->name));
            SbPuts(&emitter->out, " = ");
            EmitExpr(emitter, sf->value);
            SbPuts(&emitter->out, ";\n");
        }

        /* A moved-into box field nulls its source. */
        const Node* movedFieldSource = IsOwningType(fd->type.name) ? MovableBoxSource(sf->value) : NULL;

        if (movedFieldSource)
        {
            Pad(emitter);
            EmitLValue(emitter, movedFieldSource);
            SbPuts(&emitter->out, " = 0;\n");
        }
    }
}

/* Initializes an already-declared box global (EmitGlobals left it null). */
static void EmitBoxInitStmt(CEmitter* emitter, const char* cName, const char* innerC, const char* inner,
                            const Node* init)
{
    const char* initType = ExprType(emitter, (Node*)init);

    if (initType[0] != '\0' && IsOwningType(initType))
    {
        /* A box-returning call: assign the returned pointer directly. */
        Pad(emitter);
        SbPuts(&emitter->out, cName);
        SbPuts(&emitter->out, " = ");
        EmitExpr(emitter, (Node*)init);
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

    Pad(emitter);
    SbPuts(&emitter->out, cName);
    SbPuts(&emitter->out, " = strata_alloc(sizeof(");
    SbPuts(&emitter->out, innerC);
    SbPuts(&emitter->out, "));\n");

    Pad(emitter);
    SbPutc(&emitter->out, '*');
    SbPuts(&emitter->out, cName);
    SbPuts(&emitter->out, " = ");
    EmitExpr(emitter, (Node*)init);
    SbPuts(&emitter->out, ";\n");
}

static void EmitVarDecl(CEmitter* emitter, const VarDeclStmt* declaration, bool semicolon)
{
    const char* cName = VarName(emitter, declaration->name);

    if (IsOwningType(declaration->type.name))
    {
        bool isString = strcmp(declaration->type.name, "string") == 0;

        if (isString)
        {
            const char* initType = declaration->init ? ExprType(emitter, declaration->init) : "";

            SbPuts(&emitter->out, "char * ");
            SbPuts(&emitter->out, cName);

            if (initType[0] != '\0' && IsOwningType(initType) && declaration->init->kind != NodeStrLiteral)
            {
                SbPuts(&emitter->out, " = ");
                EmitExpr(emitter, declaration->init);
            }
            else if (declaration->init && declaration->init->kind == NodeStrLiteral)
            {
                SbPuts(&emitter->out, " = strata_strdup(");
                EmitExpr(emitter, declaration->init);
                SbPutc(&emitter->out, ')');
            }
            else
            {
                SbPuts(&emitter->out, " = 0");
            }

            if (semicolon)
            {
                SbPutc(&emitter->out, ';');
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

        if (initType[0] != '\0' && IsOwningType(initType))
        {
            /* Move from another box<T>: take its pointer, null the source. */
            SbPuts(&emitter->out, " = ");
            EmitExpr(emitter, declaration->init);

            if (semicolon)
            {
                SbPutc(&emitter->out, ';');
            }

            const Node* movedDeclSource = MovableBoxSource(declaration->init);

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
        else
        {
            /* Box a value of the inner type. (var already declared above) */
            if (declaration->init && TypeRegistryIsOwningStruct(&emitter->types, inner)
                && declaration->init->kind == NodeStructInit)
            {
                EmitBoxedStructInit(emitter, cName, innerC, inner, (const StructInitExpr*)declaration->init);
            }
            else
            {
                SbPuts(&emitter->out, " = strata_alloc(sizeof(");
                SbPuts(&emitter->out, innerC);
                SbPuts(&emitter->out, "));");

                if (semicolon)
                {
                    SbPutc(&emitter->out, '\n');
                }

                if (declaration->init)
                {
                    if (semicolon)
                    {
                        Pad(emitter);
                    }

                    SbPutc(&emitter->out, '*');
                    SbPuts(&emitter->out, cName);
                    SbPuts(&emitter->out, " = ");
                    EmitExpr(emitter, declaration->init);

                    if (semicolon)
                    {
                        SbPutc(&emitter->out, ';');
                    }
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

        EmitExpr(emitter, declaration->init);
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
        const char* inner = OwningInnerCStr(emitter->arena, e->typeName);
        bool owningInner = inner && TypeRegistryIsOwningStruct(&emitter->types, inner);

        /* For a by-ref box param, the box pointer lives at *var (caller's slot). */
        const char* star = e->byRef ? "*" : "";

        Pad(emitter);
        SbPuts(&emitter->out, "if (");
        SbPuts(&emitter->out, star);
        SbPuts(&emitter->out, var);
        SbPuts(&emitter->out, ") { ");

        if (owningInner)
        {
            SbPuts(&emitter->out, DropHelperName(emitter, inner));
            SbPuts(&emitter->out, "(");
            SbPuts(&emitter->out, star);
            SbPuts(&emitter->out, var);
            SbPuts(&emitter->out, "); ");
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

static void EmitStmt(CEmitter* emitter, const Node* node);

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

        for (size_t i = 0; i < block->statements.count; ++i)
        {
            EmitStmt(emitter, (const Node*)VecGet(&block->statements, i));
        }

        EmitDrops(emitter, boxMark);
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

            return;
        }

        /* A bare box<T> identifier is only a move if the function returns
           box<T>; otherwise it's a deref-read. */
        bool returnsBoxIdent = statement->value && statement->value->kind == NodeIdent && IsOwningType(valueType);

        const Node* movedReturnSource = IsOwningType(valueType) ? MovableBoxSource(statement->value) : NULL;
        bool movesBox = movedReturnSource && targetIsBox && strcmp(emitter->currentReturn, valueType) == 0;

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

            if (returnsBoxIdent && !movesBox)
            {
                SbPuts(&emitter->out, "(*");
                EmitLValue(emitter, statement->value);
                SbPutc(&emitter->out, ')');
            }
            else
            {
                EmitExpr(emitter, statement->value);
            }

            SbPuts(&emitter->out, ";\n");

            /* Returning a box moves it out: null the source so the drop below
               does not free it. */
            if (movesBox)
            {
                Pad(emitter);
                EmitLValue(emitter, movedReturnSource);
                SbPuts(&emitter->out, " = 0;\n");
            }

            EmitDrops(emitter, 0);

            Pad(emitter);
            SbPuts(&emitter->out, "return ");
            SbPuts(&emitter->out, tmp);
            SbPuts(&emitter->out, ";\n");
        }
        else
        {
            Pad(emitter);
            SbPuts(&emitter->out, "return");

            if (statement->value)
            {
                SbPutc(&emitter->out, ' ');

                if (returnsBoxIdent && !movesBox)
                {
                    SbPuts(&emitter->out, "(*");
                    EmitLValue(emitter, statement->value);
                    SbPutc(&emitter->out, ')');
                }
                else
                {
                    EmitExpr(emitter, statement->value);
                }
            }

            SbPuts(&emitter->out, ";\n");
        }

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
            EmitExpr(emitter, statement->expr);
        }

        SbPuts(&emitter->out, ";\n");

        return;
    }
    case NodeIf:
    {
        const IfStmt* statement = (const IfStmt*)node;
        Pad(emitter);
        SbPuts(&emitter->out, "if (");
        EmitExpr(emitter, statement->condition);
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
        EmitExpr(emitter, statement->condition);
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
                EmitExpr(emitter, statement->init);
            }
        }

        SbPuts(&emitter->out, "; ");

        if (statement->condition)
        {
            EmitExpr(emitter, statement->condition);
        }

        SbPuts(&emitter->out, "; ");

        if (statement->update)
        {
            EmitExpr(emitter, statement->update);
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
        EmitExpr(emitter, node);
        SbPuts(&emitter->out, ";\n");

        return;
    }
}

static void EmitParam(CEmitter* emitter, const ParamDecl* param)
{
    EmitType(emitter, &param->type);

    if (ParamIsIndirect(emitter, param))
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

            EmitParam(emitter, (const ParamDecl*)VecGet(&function->params, i));
        }
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
            EmitType(emitter, &param->type);

            if (ParamIsIndirect(emitter, param))
            {
                SbPutc(&emitter->out, '*');
            }
        }
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

    for (size_t p = 0; p < sizeof(primitive) / sizeof(primitive[0]); ++p)
    {
        for (int lanes = 2; lanes <= 4; ++lanes)
        {
            const char* vectorName = arena_format(emitter->arena, "%s%d", primitive[p], lanes);
            const char* elementName = TypeNameC(emitter, primitive[p]);

            SbPuts(&emitter->out, "typedef struct { ");
            SbPuts(&emitter->out, elementName);
            SbPrintf(&emitter->out, " lane[%d]; } ", lanes);
            SbPuts(&emitter->out, TypeNameC(emitter, vectorName));
            SbPuts(&emitter->out, ";\n");
        }
    }

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

        if (IsOwningType(global->type.name))
        {
            /* Boxing requires a runtime alloc, so the real init runs in
               __strata_module_init; the declared storage starts null. */
            SbPuts(&emitter->out, "0");
        }
        else if (global->init)
        {
            EmitExpr(emitter, global->init);
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

        if (function->isExtern && emitter->jitMode)
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

            /* An owned (non-ref) box parameter is consumed: freed at return. */
            if (IsOwningType(param->type.name) && param->mod == ModNone)
            {
                OwnEntry* entry = (OwnEntry*)arena_alloc(emitter->arena, sizeof(OwnEntry));
                entry->cName = VarName(emitter, param->name);
                entry->typeName = param->type.name;
                entry->byRef = true;
                VecPush(&emitter->boxVars, entry);
            }
        }

        EmitLineDirective(emitter, function->base.range);
        EmitFunctionSignature(emitter, function, FunctionName(emitter, function->mangledName));
        SbPuts(&emitter->out, " {\n");

        emitter->indent++;

        const Block* body = (const Block*)function->body;
        for (size_t j = 0; j < body->statements.count; ++j)
        {
            EmitStmt(emitter, (const Node*)VecGet(&body->statements, j));
        }

        EmitDrops(emitter, 0);
        emitter->boxVars.count = 0;

        Pad(emitter);

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

        if (!IsOwningType(global->type.name) || !global->init)
        {
            continue;
        }

        const char* inner = OwningInnerCStr(emitter->arena, global->type.name);
        const char* innerC = TypeNameC(emitter, inner);
        const char* cName = GlobalName(emitter, global->name);

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
                                     const SourceManager* sources, size_t sourceCount, bool jitMode)
{
    BuiltCModule result;
    BuiltCModuleInit(&result);

    if (!ast)
    {
        DiagError(diag, SRC_INVALID, "C backend received a null module");
        return result;
    }

    CEmitter emitter = {0};
    emitter.mod = ast;
    emitter.diag = diag;
    emitter.arena = arena;
    emitter.jitMode = jitMode;
    emitter.sources = sources;
    emitter.sourceCount = sourceCount;
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
                         "extern void* strata_alloc(unsigned long);\n"
                         "extern void strata_free(void*);\n"
                         "static char* strata_strdup(const char* s) {\n"
                         "  unsigned long n = 0; while (s[n]) n++;\n"
                         "  char* d = (char*)strata_alloc(n + 1);\n"
                         "  if (d) { unsigned long i; for (i = 0; i <= n; i++) d[i] = s[i]; }\n"
                         "  return d;\n"
                         "}\n"
                         "extern float fmodf(float, float);\n"
                         "extern double fmod(double, double);\n\n");
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

BuiltCModule BuildCModule(const Module* ast, DiagnosticEngine* diag, Arena* arena, bool jitMode)
{
    return BuildCModuleWithSources(ast, diag, arena, NULL, 0, jitMode);
}

CodegenResult GenerateC(const Module* mod)
{
    CodegenResult result = {0};
    result.moduleName = mod ? mod->name : NULL;

    Arena arena;
    arena_init(&arena, 0);

    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);

    BuiltCModule module = BuildCModule(mod, &diag, &arena, false);

    result.ok = !DiagHasErrors(&diag);
    result.output = DupString(module.source ? module.source : "");

    BuiltCModuleDispose(&module);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);

    return result;
}
