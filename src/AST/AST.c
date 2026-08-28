#include "AST/AST.h"

#include <stdlib.h>

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
    DisposeVec(&module->functions);
    DisposeVec(&module->globals);
    DisposeVec(&module->imports);
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
    {
        Module* module = (Module*)node;

#define DISPOSE_ALL(field)                                                                                             \
    for (size_t i = 0; i < module->field.count; ++i)                                                                   \
    AstDispose((Node*)VecGet(&module->field, i))

        DISPOSE_ALL(structs);
        DISPOSE_ALL(handles);
        DISPOSE_ALL(functions);
        DISPOSE_ALL(globals);
        DISPOSE_ALL(imports);

#undef DISPOSE_ALL

        AstReleaseModuleLists(module);

        return;
    }
    case NodeStruct:
        DisposeVec(&((StructDecl*)node)->fields);
        return;
    case NodeFunction:
    {
        FunctionDecl* function = (FunctionDecl*)node;
        AstDispose(function->body);
        DisposeVec(&function->params);
        return;
    }
    case NodeBlock:
    {
        Block* block = (Block*)node;

        for (size_t i = 0; i < block->statements.count; ++i)
        {
            AstDispose((Node*)VecGet(&block->statements, i));
        }

        DisposeVec(&block->statements);

        return;
    }
    case NodeReturn:
        AstDispose(((ReturnStmt*)node)->value);
        return;
    case NodeIf:
    {
        IfStmt* statement = (IfStmt*)node;
        AstDispose(statement->condition);
        AstDispose(statement->thenBranch);
        AstDispose(statement->elseBranch);

        return;
    }
    case NodeWhile:
    {
        WhileStmt* statement = (WhileStmt*)node;
        AstDispose(statement->condition);
        AstDispose(statement->body);

        return;
    }
    case NodeFor:
    {
        ForStmt* statement = (ForStmt*)node;
        AstDispose(statement->init);
        AstDispose(statement->condition);
        AstDispose(statement->update);
        AstDispose(statement->body);

        return;
    }
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
    {
        BinaryExpr* expression = (BinaryExpr*)node;
        AstDispose(expression->lhs);
        AstDispose(expression->rhs);

        return;
    }
    case NodeAssign:
    {
        AssignExpr* expression = (AssignExpr*)node;
        AstDispose(expression->target);
        AstDispose(expression->value);

        return;
    }
    case NodeCall:
    {
        CallExpr* expression = (CallExpr*)node;

        for (size_t i = 0; i < expression->args.count; ++i)
        {
            AstDispose((Node*)VecGet(&expression->args, i));
        }

        DisposeVec(&expression->args);

        return;
    }
    case NodeMember:
        AstDispose(((MemberExpr*)node)->base_node);
        return;
    case NodeStructInit:
    {
        StructInitExpr* expression = (StructInitExpr*)node;

        for (size_t i = 0; i < expression->fields.count; ++i)
        {
            StructInitField* field = (StructInitField*)VecGet(&expression->fields, i);
            AstDispose(field->value);
        }

        DisposeVec(&expression->fields);

        return;
    }
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
    {
        IndexExpr* idx = (IndexExpr*)node;
        AstDispose(idx->base_node);
        AstDispose(idx->index);
        return;
    }
    case NodeArrayInit:
    {
        ArrayInitExpr* ai = (ArrayInitExpr*)node;
        for (size_t i = 0; i < ai->elements.count; i++)
        {
            AstDispose((Node*)VecGet(&ai->elements, i));
        }
        return;
    }
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
    case NodeNullTest:
        AstDispose(((NullTestExpr*)node)->operand);
        return;
    case NodeDefer:
        AstDispose(((DeferStmt*)node)->stmt);
        return;
    }
}
