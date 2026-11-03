#include "AST/AST.h"

#include <stdlib.h>
#include <string.h>

static void DisposeVec(Vec* vec)
{
    free(vec->items);
    VecInit(vec);
}

void AstReleaseModuleLists(Module* module)
{
    if (!module)
    {
        return;
    }

    DisposeVec(&module->structs);
    DisposeVec(&module->handles);
    DisposeVec(&module->enums);
    DisposeVec(&module->functions);
    DisposeVec(&module->globals);
    DisposeVec(&module->imports);
    DisposeVec(&module->impls);
    DisposeVec(&module->genericInstances);
    DisposeVec(&module->delegates);
}

const EnumDecl* ModuleFindEnum(const Module* mod, const char* name)
{
    if (!mod || !name)
    {
        return NULL;
    }

    for (size_t i = 0; i < mod->enums.count; i++)
    {
        const EnumDecl* ed = (const EnumDecl*)VecGet(&mod->enums, i);

        if (strcmp(ed->name, name) == 0)
        {
            return ed;
        }
    }

    return NULL;
}

const ImplDecl* ModuleFindImpl(const Module* mod, const char* handleName)
{
    if (!mod || !handleName)
    {
        return NULL;
    }

    for (size_t i = 0; i < mod->impls.count; i++)
    {
        const ImplDecl* impl = (const ImplDecl*)VecGet(&mod->impls, i);

        if (strcmp(impl->handleName, handleName) == 0)
        {
            return impl;
        }
    }

    return NULL;
}

/* Kinds whose disposal is more than one call get a function of their own. Kinds that dispose a
   single child stay on one line in AstDispose. */

static void DisposeModule(Node* node)
{
    Module* module = (Module*)node;

#define DISPOSE_ALL(field)                                                                                             \
    for (size_t i = 0; i < module->field.count; ++i)                                                                   \
    AstDispose((Node*)VecGet(&module->field, i))

    DISPOSE_ALL(structs);
    DISPOSE_ALL(handles);
    DISPOSE_ALL(enums);
    DISPOSE_ALL(functions);
    DISPOSE_ALL(globals);
    DISPOSE_ALL(imports);
    DISPOSE_ALL(impls);
    DISPOSE_ALL(genericInstances);
    DISPOSE_ALL(delegates);

#undef DISPOSE_ALL

    AstReleaseModuleLists(module);
}

static void DisposeStruct(Node* node)
{
    StructDecl* decl = (StructDecl*)node;

    for (size_t i = 0; i < decl->fields.count; ++i)
    {
        AstDispose(((FieldDecl*)VecGet(&decl->fields, i))->defaultValue);
    }

    DisposeVec(&decl->fields);
    DisposeVec(&decl->attributes);
}

static void DisposeEnum(Node* node)
{
    EnumDecl* decl = (EnumDecl*)node;

    for (size_t i = 0; i < decl->members.count; ++i)
    {
        AstDispose((Node*)VecGet(&decl->members, i));
    }

    DisposeVec(&decl->members);
}

static void DisposeFunction(Node* node)
{
    FunctionDecl* function = (FunctionDecl*)node;
    AstDispose(function->body);
    DisposeVec(&function->params);
}

static void DisposeBlock(Node* node)
{
    Block* block = (Block*)node;

    for (size_t i = 0; i < block->statements.count; ++i)
    {
        AstDispose((Node*)VecGet(&block->statements, i));
    }

    DisposeVec(&block->statements);
}

static void DisposeIf(Node* node)
{
    IfStmt* statement = (IfStmt*)node;
    AstDispose(statement->condition);
    AstDispose(statement->thenBranch);
    AstDispose(statement->elseBranch);
}

static void DisposeWhile(Node* node)
{
    WhileStmt* statement = (WhileStmt*)node;
    AstDispose(statement->condition);
    AstDispose(statement->body);
}

static void DisposeFor(Node* node)
{
    ForStmt* statement = (ForStmt*)node;
    AstDispose(statement->init);
    AstDispose(statement->condition);
    AstDispose(statement->update);
    AstDispose(statement->body);
}

static void DisposeBinary(Node* node)
{
    BinaryExpr* expression = (BinaryExpr*)node;
    AstDispose(expression->lhs);
    AstDispose(expression->rhs);
}

static void DisposeAssign(Node* node)
{
    AssignExpr* expression = (AssignExpr*)node;
    AstDispose(expression->target);
    AstDispose(expression->value);
}

static void DisposeCall(Node* node)
{
    CallExpr* expression = (CallExpr*)node;

    for (size_t i = 0; i < expression->args.count; ++i)
    {
        AstDispose((Node*)VecGet(&expression->args, i));
    }

    DisposeVec(&expression->args);

    /* Pre-sema member calls keep the base outside args; after the sema
       rewrite it has been moved into args (and calleeBase cleared). */
    AstDispose(expression->calleeBase);
}

static void DisposeStructInit(Node* node)
{
    StructInitExpr* expression = (StructInitExpr*)node;

    for (size_t i = 0; i < expression->fields.count; ++i)
    {
        StructInitField* field = (StructInitField*)VecGet(&expression->fields, i);
        AstDispose(field->value);
    }

    DisposeVec(&expression->fields);
}

static void DisposeIndex(Node* node)
{
    IndexExpr* idx = (IndexExpr*)node;
    AstDispose(idx->base_node);
    AstDispose(idx->index);
}

static void DisposeArrayInit(Node* node)
{
    ArrayInitExpr* ai = (ArrayInitExpr*)node;
    for (size_t i = 0; i < ai->elements.count; i++)
    {
        AstDispose((Node*)VecGet(&ai->elements, i));
    }

    AstDispose(ai->repeatCount);

    DisposeVec(&ai->elements);
}

/* Method FunctionDecls are shared with Module::functions (disposed
   there); only the impl's own membership lists go away here. */
static void DisposeImpl(Node* node)
{
    DisposeVec(&((ImplDecl*)node)->methods);
    DisposeVec(&((ImplDecl*)node)->properties);
}

static void DisposeConditional(Node* node)
{
    AstDispose(((ConditionalExpr*)node)->condition);
    AstDispose(((ConditionalExpr*)node)->thenExpr);
    AstDispose(((ConditionalExpr*)node)->elseExpr);
}

void AstDispose(Node* node)
{
    if (!node)
    {
        return;
    }

    switch (node->kind)
    {
    case NodeModule:
        DisposeModule(node);
        return;
    case NodeStruct:
        DisposeStruct(node);
        return;
    case NodeEnum:
        DisposeEnum(node);
        return;
    case NodeEnumMember:
        AstDispose(((EnumMemberDecl*)node)->valueExpr);
        return;
    case NodeFunction:
        DisposeFunction(node);
        return;
    case NodeBlock:
        DisposeBlock(node);
        return;
    case NodeReturn:
        AstDispose(((ReturnStmt*)node)->value);
        return;
    case NodeIf:
        DisposeIf(node);
        return;
    case NodeWhile:
        DisposeWhile(node);
        return;
    case NodeFor:
        DisposeFor(node);
        return;
    case NodeVarDecl:
        AstDispose(((VarDeclStmt*)node)->init);
        return;
    case NodeExprStmt:
        AstDispose(((ExprStmt*)node)->expr);
        return;
    case NodeUnary:
        AstDispose(((UnaryExpr*)node)->operand);
        return;
    case NodeBinary:
        DisposeBinary(node);
        return;
    case NodeAssign:
        DisposeAssign(node);
        return;
    case NodeCall:
        DisposeCall(node);
        return;
    case NodeMember:
        AstDispose(((MemberExpr*)node)->base_node);
        return;
    case NodeStructInit:
        DisposeStructInit(node);
        return;
    case NodeIncDec:
        AstDispose(((IncDecExpr*)node)->operand);
        return;
    case NodeCast:
        AstDispose(((CastExpr*)node)->operand);
        return;
    case NodeGlobal:
        AstDispose(((GlobalDecl*)node)->init);
        return;
    case NodeIndex:
        DisposeIndex(node);
        return;
    case NodeArrayInit:
        DisposeArrayInit(node);
        return;
    case NodeImport:
    case NodeHandle:
    case NodeParam:
    case NodeBreak:
    case NodeContinue:
    case NodeIntLiteral:
    case NodeFloatLiteral:
    case NodeBoolLiteral:
    case NodeStrLiteral:
    case NodeIdent:
        return;
    case NodeImpl:
        DisposeImpl(node);
        return;
    case NodeNullTest:
        AstDispose(((NullTestExpr*)node)->operand);
        return;
    case NodeDefer:
        AstDispose(((DeferStmt*)node)->stmt);
        return;
    case NodeConditional:
        DisposeConditional(node);
        return;
    }
}
