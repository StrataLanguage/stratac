#include "Codegen/CBackend.h"

#include "Codegen/CodegenBackend.h"
#include "Codegen/TypeRegistry.h"
#include "Codegen/TypeUtil.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char* typeName;
    const char* cName;
    bool indirect;
} CSymbol;

typedef struct {
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
} CEmitter;

static void DisposeMap(StrMap* map)
{
    free(map->keys);
    free(map->values);
    StrMapInit(map);
}

static void Pad(CEmitter* emitter)
{
    SbPutr(&emitter->out, ' ', emitter->indent * 4);
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

    return true;
}

static const char* FunctionName(CEmitter* emitter, const char* name)
{
    if (IsPlainIdentifier(name))
    {
        return arena_strdup(emitter->arena, name);
    }
    return Encode(emitter, "__strata_fn_", name);
}

static const char* TypeNameC(CEmitter* emitter, const char* name)
{
    MappedType mapped;
    TypeName type = {0};
    type.name = (char*)name;
    mapped = MapType(&type);

    if (mapped.valid)
    {
        if (mapped.vec > 1)
        {
            return Encode(emitter, "__strata_vec_", name);
        }
        if (mapped.isVoid) return "void";
        if (mapped.isFloat && mapped.bits == 32) return "float";
        if (mapped.isFloat && mapped.bits == 64) return "double";
        if (mapped.bits == 1) return "_Bool";
        if (mapped.isUnsigned) return "unsigned int";
        return "int";
    }

    if (TypeRegistryIsUserType(&emitter->types, name))
    {
        return Encode(emitter, "__strata_type_", name);
    }

    DiagErrorFmt(emitter->diag, SRC_INVALID, "C backend does not know type '%s'", name);
    return "void*";
}

static const char* FieldName(CEmitter* emitter, const char* name)
{
    return Encode(emitter, "__strata_field_", name);
}

static const char* VarName(CEmitter* emitter, const char* name)
{
    return Encode(emitter, "__strata_var_", name);
}

static const char* GlobalName(CEmitter* emitter, const char* name)
{
    return Encode(emitter, "__strata_global_", name);
}

static const char* ExternSlotName(CEmitter* emitter, const char* name)
{
    return Encode(emitter, "__strata_ext_", name);
}

static bool IsStructValue(CEmitter* emitter, const char* name)
{
    return TypeRegistryIsUserType(&emitter->types, name)
        && !TypeRegistryIsOpaque(&emitter->types, name);
}

static bool ParamIsIndirect(CEmitter* emitter, const ParamDecl* param)
{
    return ByRef(param->mod) || IsStructValue(emitter, param->type.name);
}

static void EmitType(CEmitter* emitter, const TypeName* type)
{
    if (type->isConst)
    {
        SbPuts(&emitter->out, "const ");
    }
    SbPuts(&emitter->out, TypeNameC(emitter, type->name));
}

static void AddSymbol(CEmitter* emitter, const char* name, const char* typeName,
                      const char* cName, bool indirect)
{
    CSymbol* symbol = (CSymbol*)arena_alloc(emitter->arena, sizeof(CSymbol));
    symbol->typeName = typeName;
    symbol->cName = cName;
    symbol->indirect = indirect;
    StrMapPut(&emitter->symbols, name, symbol);
}

static bool IsLValue(const Node* node)
{
    return node && (node->kind == NodeIdent || node->kind == NodeMember);
}

static void EmitExpr(CEmitter* emitter, const Node* node);

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
            DiagErrorFmt(emitter->diag, node->range, "C backend cannot resolve variable '%s'", ident->name);
            SbPuts(&emitter->out, "0");
            return;
        }
        if (symbol->indirect) SbPuts(&emitter->out, "(*");
        SbPuts(&emitter->out, symbol->cName);
        if (symbol->indirect) SbPutc(&emitter->out, ')');
        return;
    }

    if (node->kind == NodeMember)
    {
        const MemberExpr* member = (const MemberExpr*)node;
        SbPutc(&emitter->out, '(');
        EmitLValue(emitter, member->base_node);
        SbPuts(&emitter->out, ").");
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
    case UnNeg: return "-";
    case UnPos: return "+";
    case UnNot: return "!";
    case UnBitNot: return "~";
    }
    return "";
}

static const char* BinarySpelling(BinaryOp op)
{
    switch (op)
    {
    case BinAdd: return "+";
    case BinSub: return "-";
    case BinMul: return "*";
    case BinDiv: return "/";
    case BinMod: return "%";
    case BinBitAnd: return "&";
    case BinBitOr: return "|";
    case BinBitXor: return "^";
    case BinShl: return "<<";
    case BinShr: return ">>";
    case BinEqEq: return "==";
    case BinNotEq: return "!=";
    case BinLt: return "<";
    case BinLtEq: return "<=";
    case BinGt: return ">";
    case BinGtEq: return ">=";
    case BinLogicAnd: return "&&";
    case BinLogicOr: return "||";
    }
    return "";
}

static const char* AssignSpelling(AssignOp op)
{
    switch (op)
    {
    case AssignSet: return "=";
    case AssignAdd: return "+=";
    case AssignSub: return "-=";
    case AssignMul: return "*=";
    case AssignDiv: return "/=";
    case AssignMod: return "%=";
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
        if (i) SbPuts(&emitter->out, ", ");
        if (type && index < type->fields.count)
        {
            FieldDecl* declaration = (FieldDecl*)VecGet(&type->fields, index);
            SbPutc(&emitter->out, '.');
            SbPuts(&emitter->out, FieldName(emitter, declaration->name));
            SbPuts(&emitter->out, " = ");
        }
        EmitExpr(emitter, field->value);
    }
    SbPuts(&emitter->out, " }");
}

static void EmitCall(CEmitter* emitter, const CallExpr* call)
{
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
        if (i) SbPuts(&emitter->out, ", ");
        const Node* argument = (const Node*)VecGet(&call->args, i);
        const ParamDecl* parameter = function && i < function->params.count
            ? (const ParamDecl*)VecGet(&function->params, i) : NULL;

        if (parameter && ParamIsIndirect(emitter, parameter))
        {
            if (IsLValue(argument))
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
            EmitExpr(emitter, argument);
        }
    }
    SbPutc(&emitter->out, ')');
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
        SbPrintf(&emitter->out, "%llu%s", (unsigned long long)literal->value,
                 literal->isUnsigned ? "u" : "");
        return;
    }
    case NodeFloatLiteral:
        SbPrintf(&emitter->out, "%.9ef", ((const FloatLiteral*)node)->value);
        return;
    case NodeBoolLiteral:
        SbPuts(&emitter->out, ((const BoolLiteral*)node)->value ? "1" : "0");
        return;
    case NodeIdent:
    case NodeMember:
        EmitLValue(emitter, node);
        return;
    case NodeUnary:
    {
        const UnaryExpr* unary = (const UnaryExpr*)node;
        SbPutc(&emitter->out, '(');
        SbPuts(&emitter->out, UnarySpelling(unary->op));
        EmitExpr(emitter, unary->operand);
        SbPutc(&emitter->out, ')');
        return;
    }
    case NodeBinary:
    {
        const BinaryExpr* binary = (const BinaryExpr*)node;
        SbPutc(&emitter->out, '(');
        EmitExpr(emitter, binary->lhs);
        SbPutc(&emitter->out, ' ');
        SbPuts(&emitter->out, BinarySpelling(binary->op));
        SbPutc(&emitter->out, ' ');
        EmitExpr(emitter, binary->rhs);
        SbPutc(&emitter->out, ')');
        return;
    }
    case NodeAssign:
    {
        const AssignExpr* assign = (const AssignExpr*)node;
        SbPutc(&emitter->out, '(');
        EmitLValue(emitter, assign->target);
        SbPutc(&emitter->out, ' ');
        SbPuts(&emitter->out, AssignSpelling(assign->op));
        SbPutc(&emitter->out, ' ');
        EmitExpr(emitter, assign->value);
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
        SbPutc(&emitter->out, '(');
        if (increment->isPrefix) SbPuts(&emitter->out, increment->isDec ? "--" : "++");
        EmitLValue(emitter, increment->operand);
        if (!increment->isPrefix) SbPuts(&emitter->out, increment->isDec ? "--" : "++");
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

static void EmitVarDecl(CEmitter* emitter, const VarDeclStmt* declaration, bool semicolon)
{
    EmitType(emitter, &declaration->type);
    SbPutc(&emitter->out, ' ');
    const char* cName = VarName(emitter, declaration->name);
    SbPuts(&emitter->out, cName);
    SbPuts(&emitter->out, " = ");
    if (declaration->init)
    {
        EmitExpr(emitter, declaration->init);
    }
    else
    {
        SbPutc(&emitter->out, '(');
        SbPuts(&emitter->out, TypeNameC(emitter, declaration->type.name));
        SbPuts(&emitter->out, "){0}");
    }
    if (semicolon) SbPutc(&emitter->out, ';');
    AddSymbol(emitter, declaration->name, declaration->type.name, cName, false);
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
    if (!node) return;

    switch (node->kind)
    {
    case NodeBlock:
    {
        const Block* block = (const Block*)node;
        SbPuts(&emitter->out, "{\n");
        emitter->indent++;
        for (size_t i = 0; i < block->statements.count; ++i)
        {
            EmitStmt(emitter, (const Node*)VecGet(&block->statements, i));
        }
        emitter->indent--;
        Pad(emitter);
        SbPutc(&emitter->out, '}');
        return;
    }
    case NodeReturn:
    {
        const ReturnStmt* statement = (const ReturnStmt*)node;
        Pad(emitter);
        SbPuts(&emitter->out, "return");
        if (statement->value)
        {
            SbPutc(&emitter->out, ' ');
            EmitExpr(emitter, statement->value);
        }
        SbPuts(&emitter->out, ";\n");
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
        if (statement->expr) EmitExpr(emitter, statement->expr);
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
                EmitVarDecl(emitter, (const VarDeclStmt*)statement->init, false);
            else
                EmitExpr(emitter, statement->init);
        }
        SbPuts(&emitter->out, "; ");
        if (statement->condition) EmitExpr(emitter, statement->condition);
        SbPuts(&emitter->out, "; ");
        if (statement->update) EmitExpr(emitter, statement->update);
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
    if (ParamIsIndirect(emitter, param)) SbPutc(&emitter->out, '*');
    SbPutc(&emitter->out, ' ');
    SbPuts(&emitter->out, VarName(emitter, param->name));
}

static void EmitFunctionSignature(CEmitter* emitter, const FunctionDecl* function,
                                  const char* name)
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
            if (i) SbPuts(&emitter->out, ", ");
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
            if (i) SbPuts(&emitter->out, ", ");
            const ParamDecl* param = (const ParamDecl*)VecGet(&function->params, i);
            EmitType(emitter, &param->type);
            if (ParamIsIndirect(emitter, param)) SbPutc(&emitter->out, '*');
        }
    }
    SbPuts(&emitter->out, ") = 0;\n");

    CBackendSymbol* symbol = (CBackendSymbol*)arena_alloc(emitter->arena, sizeof(CBackendSymbol));
    symbol->strataName = function->name;
    symbol->cName = slotName;
    VecPush(&emitter->externs, symbol);
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
        if (type->opaque)
        {
            SbPuts(&emitter->out, "typedef struct ");
            SbPuts(&emitter->out, Encode(emitter, "__strata_handle_tag_", type->name));
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

    for (size_t i = 0; i < emitter->types.count; ++i)
    {
        const StructType* type = &emitter->types.types[i];
        if (type->opaque) continue;
        const char* cName = TypeNameC(emitter, type->name);
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
    SbPutc(&emitter->out, '\n');
}

static void EmitGlobals(CEmitter* emitter)
{
    for (size_t i = 0; i < emitter->mod->globals.count; ++i)
    {
        const GlobalDecl* global = (const GlobalDecl*)VecGet(&emitter->mod->globals, i);
        EmitType(emitter, &global->type);
        SbPutc(&emitter->out, ' ');
        const char* cName = GlobalName(emitter, global->name);
        SbPuts(&emitter->out, cName);
        SbPuts(&emitter->out, " = ");
        if (global->init)
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
    if (emitter->mod->globals.count) SbPutc(&emitter->out, '\n');
}

static void EmitDeclarations(CEmitter* emitter)
{
    for (size_t i = 0; i < emitter->mod->functions.count; ++i)
    {
        const FunctionDecl* function = (const FunctionDecl*)VecGet(&emitter->mod->functions, i);
        if (function->isExtern && emitter->jitMode)
        {
            EmitExternSlot(emitter, function);
        }
        else
        {
            if (function->isExtern) SbPuts(&emitter->out, "extern ");
            EmitFunctionSignature(emitter, function, FunctionName(emitter, function->mangledName));
            SbPuts(&emitter->out, ";\n");
        }

        if (!function->isExtern)
        {
            CBackendSymbol* symbol = (CBackendSymbol*)arena_alloc(emitter->arena, sizeof(CBackendSymbol));
            symbol->strataName = function->mangledName;
            symbol->cName = FunctionName(emitter, function->mangledName);
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
        if (!function->body) continue;

        DisposeMap(&emitter->symbols);
        StrMapInit(&emitter->symbols);
        for (size_t j = 0; j < emitter->mod->globals.count; ++j)
        {
            const GlobalDecl* global = (const GlobalDecl*)VecGet(&emitter->mod->globals, j);
            AddSymbol(emitter, global->name, global->type.name,
                      GlobalName(emitter, global->name), false);
        }
        for (size_t j = 0; j < function->params.count; ++j)
        {
            const ParamDecl* param = (const ParamDecl*)VecGet(&function->params, j);
            AddSymbol(emitter, param->name, param->type.name,
                      VarName(emitter, param->name), ParamIsIndirect(emitter, param));
        }

        EmitFunctionSignature(emitter, function, FunctionName(emitter, function->mangledName));
        SbPuts(&emitter->out, " {\n");
        emitter->indent++;
        const Block* body = (const Block*)function->body;
        for (size_t j = 0; j < body->statements.count; ++j)
        {
            EmitStmt(emitter, (const Node*)VecGet(&body->statements, j));
        }
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

BuiltCModule BuildCModule(const Module* ast, DiagnosticEngine* diag, Arena* arena, bool jitMode)
{
    BuiltCModule result;
    BuiltCModuleInit(&result);
    if (!ast)
    {
        DiagError(diag, SRC_INVALID, "C backend received a null module");
        return result;
    }

    CEmitter emitter;
    memset(&emitter, 0, sizeof(emitter));
    emitter.mod = ast;
    emitter.diag = diag;
    emitter.arena = arena;
    emitter.jitMode = jitMode;
    TypeRegistryInit(&emitter.types);
    TypeRegistryBuild(&emitter.types, ast);
    StrMapInit(&emitter.symbols);
    SbInit(&emitter.out);
    VecInit(&emitter.exports);
    VecInit(&emitter.externs);

    SbPuts(&emitter.out,
        "/* Generated by Strata. */\n"
        "_Static_assert(sizeof(int) == 4, \"Strata requires 32-bit int\");\n\n");
    EmitTypes(&emitter);
    EmitGlobals(&emitter);
    EmitDeclarations(&emitter);
    EmitDefinitions(&emitter);

    result.source = SbFinish(&emitter.out, arena);
    result.exports = emitter.exports;
    result.externs = emitter.externs;

    DisposeMap(&emitter.symbols);
    free(emitter.types.types);
    return result;
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
