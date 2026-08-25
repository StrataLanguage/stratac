#include "AST/AST.h"
#include "Codegen/CodegenBackend.h"
#include "Codegen/TypeRegistry.h"

static const char* UnaryOpSpelling(UnaryOp op)
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

    return "?";
}

static const char* BinaryOpSpelling(BinaryOp op)
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

    return "?";
}

static const char* AssignOpSpelling(AssignOp op)
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

static void Dump(Node* n, int indent, Sb* out);

static void Pad(int indent, Sb* out)
{
    for (int i = 0; i < indent; ++i)
    {
        SbPutc(out, ' ');
    }
}

static void Dump(Node* n, int indent, Sb* out)
{
    if (!n)
    {
        SbPuts(out, "(null)");

        return;
    }

    Pad(indent, out);

    switch (n->kind)
    {
    case NodeModule:
    {
        Module* module = AsNode(Module, n);
        SbPrintf(out, "module %s\n", module->name);
        for (size_t i = 0; i < module->imports.count; i++)
        {
            Node* imp = (Node*)VecGet(&module->imports, i);
            Dump(imp, indent + 2, out);
        }
        for (size_t i = 0; i < module->structs.count; i++)
        {
            Node* s = (Node*)VecGet(&module->structs, i);
            Dump(s, indent + 2, out);
        }
        for (size_t i = 0; i < module->handles.count; i++)
        {
            Node* h = (Node*)VecGet(&module->handles, i);
            Dump(h, indent + 2, out);
        }
        for (size_t i = 0; i < module->globals.count; i++)
        {
            Node* g = (Node*)VecGet(&module->globals, i);
            Dump(g, indent + 2, out);
        }
        for (size_t i = 0; i < module->functions.count; i++)
        {
            Node* func = (Node*)VecGet(&module->functions, i);
            Dump(func, indent + 2, out);
        }
        return;
    }

    case NodeImport:
    {
        ImportDecl* imp = AsNode(ImportDecl, n);
        SbPrintf(out, "import %s;\n", imp->importPath);
        return;
    }

    case NodeHandle:
    {
        HandleDecl* handle_decl = AsNode(HandleDecl, n);
        SbPrintf(out, "handle %s  ; opaque\n", handle_decl->name);
        return;
    }

    case NodeStruct:
    {
        StructDecl* struct_decl = AsNode(StructDecl, n);
        if (struct_decl->incomplete)
        {
            SbPrintf(out, "struct %s;  ; forward\n", struct_decl->name);
            return;
        }
        SbPrintf(out, "%sstruct %s {\n", struct_decl->isExtern ? "extern " : "", struct_decl->name);
        for (size_t i = 0; i < struct_decl->fields.count; i++)
        {
            FieldDecl* field = (FieldDecl*)VecGet(&struct_decl->fields, i);
            Pad(indent + 4, out);
            if (field->offset >= 0)
            {
                SbPrintf(out, "fieldoffset(%ld) ", field->offset);
            }
            SbPrintf(out, "%s %s\n", field->type.name, field->name);
        }
        Pad(indent + 2, out);
        SbPuts(out, "}\n");
        return;
    }

    case NodeFunction:
    {
        FunctionDecl* function_decl = AsNode(FunctionDecl, n);
        SbPuts(out, "fn ");
        
        if (function_decl->isExtern)
        {
            SbPuts(out, "extern ");
        }
        
        SbPrintf(out, "%s %s(", function_decl->returnType.name, function_decl->name);

        for (size_t i = 0; i < function_decl->params.count; i++)
        {
            if (i > 0)
            {
                SbPuts(out, ", ");
            }
            
            ParamDecl* param = (ParamDecl*)VecGet(&function_decl->params, i);

            if (param->isVarargRest)
            {
                const TypeName* elem = TypeNameArrayElem(&param->type);

                if (elem)
                {
                    SbPrintf(out, "%s... %s", elem->name, param->name);
                }
                else
                {
                    SbPrintf(out, "%s... %s", param->type.name, param->name);
                }

                continue;
            }

            if (param->type.isConst)
            {
                SbPuts(out, "const ");
            }

            SbPuts(out, ParamModSpelling(param->mod));

            if (param->mod != ModNone)
            {
                SbPutc(out, ' ');
            }

            SbPrintf(out, "%s %s", param->type.name, param->name);
        }

        if (function_decl->isCVararg)
        {
            SbPuts(out, ", ...");
        }

        SbPutc(out, ')');

        if (!function_decl->body)
        {
            SbPuts(out, ";\n");

            return;
        }

        SbPutc(out, '\n');
        Dump(function_decl->body, indent + 2, out);
        
        return;
    }

    case NodeBlock:
    {
        Block* block = AsNode(Block, n);
        SbPuts(out, "{\n");

        for (size_t i = 0; i < block->statements.count; i++)
        {
            Node* stmt = (Node*)VecGet(&block->statements, i);
            Dump(stmt, indent + 2, out);
        }

        Pad(indent, out);
        SbPuts(out, "}\n");
        
        return;
    }

    case NodeReturn:
    {
        ReturnStmt* return_stmt = AsNode(ReturnStmt, n);
        SbPuts(out, "return");

        if (return_stmt->value)
        {
            SbPutc(out, ' ');

            Dump(return_stmt->value, 0, out);
        }
        else
        {
            SbPutc(out, '\n');
        }

        return;
    }

    case NodeIf:
    {
        IfStmt* i = AsNode(IfStmt, n);
        
        SbPuts(out, "if ");
        Dump(i->condition, 0, out);
        SbPutc(out, '\n');
        Dump(i->thenBranch, indent + 2, out);

        if (i->elseBranch)
        {
            Pad(indent, out);
            SbPuts(out, "else\n");
            Dump(i->elseBranch, indent + 2, out);
        }

        return;
    }

    case NodeWhile:
    {
        WhileStmt* w = AsNode(WhileStmt, n);
        SbPuts(out, "while ");
        Dump(w->condition, 0, out);
        SbPutc(out, '\n');
        Dump(w->body, indent + 2, out);

        return;
    }

    case NodeFor:
    {
        ForStmt* for_stmt = AsNode(ForStmt, n);
        SbPuts(out, "for (");

        if (for_stmt->init)
        {
            Dump(for_stmt->init, 0, out);
        }
        else
        {
            SbPuts(out, "; ");
        }

        if (for_stmt->condition)
        {
            Dump(for_stmt->condition, 0, out);
        }

        SbPuts(out, "; ");
        
        if (for_stmt->update)
        {
            Dump(for_stmt->update, 0, out);
        }
        
        SbPuts(out, ")\n");
        Dump(for_stmt->body, indent + 2, out);
        
        return;
    }

    case NodeVarDecl:
    {
        VarDeclStmt* var_decl = AsNode(VarDeclStmt, n);
        SbPrintf(out, "%s %s", var_decl->type.name, var_decl->name);

        if (var_decl->init)
        {
            SbPuts(out, " = ");
            Dump(var_decl->init, 0, out);
        }
        else
        {
            SbPutc(out, '\n');
        }

        return;
    }

    case NodeExprStmt:
    {
        ExprStmt* expr = AsNode(ExprStmt, n);

        if (expr->expr)
        {
            Dump(expr->expr, 0, out);
        }
        else
        {
            SbPuts(out, ";\n");
        }

        return;
    }

    case NodeBreak:
        SbPuts(out, "break\n");
        return;
    case NodeContinue:
        SbPuts(out, "continue\n");
        return;

    case NodeIntLiteral:
    {
        IntLiteral* lit = AsNode(IntLiteral, n);
        SbPrintf(out, "%llu", (unsigned long long)lit->value);
        
        if (lit->isUnsigned)
        {
            SbPutc(out, 'u');
        }
        
        SbPutc(out, '\n');

        return;
    }

    case NodeFloatLiteral:
        SbPrintf(out, "%g\n", AsNode(FloatLiteral, n)->value);
        return;

    case NodeBoolLiteral:
        SbPuts(out, AsNode(BoolLiteral, n)->value ? "true\n" : "false\n");
        return;

    case NodeStrLiteral:
        SbPrintf(out, "\"%s\"\n", AsNode(StrLiteral, n)->value);
        return;

    case NodeIdent:
        SbPrintf(out, "%s\n", AsNode(IdentExpr, n)->name);
        return;

    case NodeUnary:
    {
        UnaryExpr* unary_expr = AsNode(UnaryExpr, n);
        SbPrintf(out, "(%s ", UnaryOpSpelling(unary_expr->op));
        Dump(unary_expr->operand, 0, out);

        return;
    }

    case NodeBinary:
    {
        BinaryExpr* binary_expr = AsNode(BinaryExpr, n);
        SbPrintf(out, "(%s ", BinaryOpSpelling(binary_expr->op));
        Dump(binary_expr->lhs, 0, out);
        Pad(0, out);
        Dump(binary_expr->rhs, 0, out);

        return;
    }

    case NodeAssign:
    {
        AssignExpr* assign_expr = AsNode(AssignExpr, n);
        SbPrintf(out, "(%s ", AssignOpSpelling(assign_expr->op));
        Dump(assign_expr->target, 0, out);
        Dump(assign_expr->value, 0, out);

        return;
    }

    case NodeCall:
    {
        CallExpr* call_expr = AsNode(CallExpr, n);
        SbPrintf(out, "(call %s", call_expr->callee);

        for (size_t i = 0; i < call_expr->args.count; i++)
        {
            Node* arg = (Node*)VecGet(&call_expr->args, i);
            SbPutc(out, ' ');
            Dump(arg, 0, out);
        }
        
        return;
    }

    case NodeMember:
    {
        MemberExpr* member_expr = AsNode(MemberExpr, n);
        SbPrintf(out, "(. %s ", member_expr->member);
        Dump(member_expr->base_node, 0, out);

        return;
    }

    case NodeStructInit:
    {
        StructInitExpr* si = AsNode(StructInitExpr, n);
        SbPrintf(out, "(struct-init %s", si->typeName);

        for (size_t i = 0; i < si->fields.count; i++)
        {
            StructInitField* field = (StructInitField*)VecGet(&si->fields, i);
            SbPutc(out, ' ');

            if (field->name != NULL && field->name[0] != '\0')
            {
                SbPrintf(out, "(.%s ", field->name);
                Dump(field->value, 0, out);
                SbPutc(out, ')');
            }
            else
            {
                Dump(field->value, 0, out);
            }
        }

        return;
    }

    case NodeIncDec:
    {
        IncDecExpr* inc = AsNode(IncDecExpr, n);
        SbPrintf(out, "(%c%c%s ",
            inc->isDec ? '-' : '+',
            inc->isDec ? '-' : '+',
            inc->isPrefix ? "" : "p");
        Dump(inc->operand, 0, out);
        SbPutc(out, ')');

        return;
    }

    case NodeCast:
    {
        CastExpr* cast = AsNode(CastExpr, n);
        SbPrintf(out, "(cast %s ", cast->type.name);
        Dump(cast->operand, 0, out);
        SbPutc(out, ')');
        return;
    }

    case NodeGlobal:
    {
        GlobalDecl* gd = AsNode(GlobalDecl, n);
        SbPrintf(out, "global %s %s", gd->type.name, gd->name);
        
        if (gd->init)
        {
            SbPuts(out, " = ");
            Dump(gd->init, 0, out);
        }

        SbPuts(out, " ;\n");
        
        return;
    }

    case NodeIndex:
    {
        IndexExpr* idx = AsNode(IndexExpr, n);
        SbPuts(out, "([] ");
        Dump(idx->base_node, 0, out);
        SbPutc(out, ' ');
        Dump(idx->index, 0, out);
        SbPutc(out, ')');
        return;
    }

    case NodeArrayInit:
    {
        ArrayInitExpr* ai = AsNode(ArrayInitExpr, n);
        SbPrintf(out, "(array-init %s", ai->elementType ? ai->elementType->name : "");

        for (size_t i = 0; i < ai->elements.count; i++)
        {
            SbPutc(out, ' ');
            Dump((Node*)VecGet(&ai->elements, i), 0, out);
        }

        SbPutc(out, ')');
        return;
    }

    case NodeParam:
        SbPuts(out, "(param)\n");
        return;
    }

    SbPuts(out, "(unknown node)\n");
}

char* DumpAst(const Module* mod, Arena* arena)
{
    Sb out;
    SbInit(&out);
    Dump((Node*)mod, 0, &out);

    return SbFinish(&out, arena);
}
