// Textual dump of the AST.
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
    case UnAddrOf:
        return "&";
    case UnDeref:
        return "*";
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

// `<T, U>` after a declaration's name; nothing for a non-generic declaration.
static void DumpTypeParams(const Vec* params, Sb* out)
{
    if (params->count == 0)
    {
        return;
    }

    SbPutc(out, '<');

    for (size_t i = 0; i < params->count; i++)
    {
        if (i > 0)
        {
            SbPuts(out, ", ");
        }

        SbPuts(out, (const char*)VecGet(params, i));
    }

    SbPutc(out, '>');
}

static void DumpModule(Node* n, int indent, Sb* out)
{
    Module* module = AS_NODE(Module, n);
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
    for (size_t i = 0; i < module->enums.count; i++)
    {
        Node* e = (Node*)VecGet(&module->enums, i);
        Dump(e, indent + 2, out);
    }
    for (size_t i = 0; i < module->globals.count; i++)
    {
        Node* g = (Node*)VecGet(&module->globals, i);
        Dump(g, indent + 2, out);
    }
    for (size_t i = 0; i < module->functions.count; i++)
    {
        Node* func = (Node*)VecGet(&module->functions, i);

        // Impl methods and synthesized property accessors print inside
        // their impl blocks instead.
        if (((FunctionDecl*)func)->fromImpl)
        {
            continue;
        }

        Dump(func, indent + 2, out);
    }
    for (size_t i = 0; i < module->impls.count; i++)
    {
        Node* impl = (Node*)VecGet(&module->impls, i);
        Dump(impl, indent + 2, out);
    }
    return;
}

static void DumpImpl(Node* n, int indent, Sb* out)
{
    ImplDecl* impl = AS_NODE(ImplDecl, n);
    SbPrintf(out, "impl %s", impl->handleName);
    DumpTypeParams(&impl->genericParams, out);
    SbPuts(out, " {\n");

    for (size_t i = 0; i < impl->methods.count; i++)
    {
        Dump((Node*)VecGet(&impl->methods, i), indent + 4, out);
    }

    for (size_t i = 0; i < impl->properties.count; i++)
    {
        PropertyDecl* prop = (PropertyDecl*)VecGet(&impl->properties, i);
        Pad(indent + 4, out);
        SbPrintf(out, "property %s %s {", prop->returnType.name, prop->name);

        if (prop->getterSymbol)
        {
            SbPrintf(out, " get = %s;", prop->getterSymbol);
        }

        if (prop->setterSymbol)
        {
            SbPrintf(out, " set = %s;", prop->setterSymbol);
        }

        SbPuts(out, " }\n");
    }

    Pad(indent + 2, out);
    SbPuts(out, "}\n");
    return;
}

static void DumpImport(Node* n, Sb* out)
{
    ImportDecl* imp = AS_NODE(ImportDecl, n);
    SbPrintf(out, "import %s;\n", imp->importPath);
    return;
}

static void DumpHandle(Node* n, Sb* out)
{
    HandleDecl* handleDecl = AS_NODE(HandleDecl, n);
    SbPrintf(out, "handle %s  ; opaque\n", handleDecl->name);
    return;
}

static void DumpEnum(Node* n, int indent, Sb* out)
{
    EnumDecl* enumDecl = AS_NODE(EnumDecl, n);
    SbPrintf(out, "enum %s", enumDecl->name);

    if (enumDecl->underlyingType)
    {
        SbPrintf(out, " : %s", enumDecl->underlyingType);
    }

    SbPuts(out, " {\n");
    for (size_t i = 0; i < enumDecl->members.count; i++)
    {
        EnumMemberDecl* member = (EnumMemberDecl*)VecGet(&enumDecl->members, i);
        Pad(indent + 4, out);

        if (member->hasExplicitValue)
        {
            SbPrintf(out, "%s = ", member->name);
            Dump(member->valueExpr, 0, out);
        }
        else
        {
            SbPrintf(out, "%s\n", member->name);
        }
    }
    Pad(indent + 2, out);
    SbPuts(out, "}\n");
    return;
}

static void DumpStruct(Node* n, int indent, Sb* out)
{
    StructDecl* structDecl = AS_NODE(StructDecl, n);
    if (structDecl->isTypeAlias)
    {
        SbPrintf(out, "struct %s = %s;\n", structDecl->name, structDecl->underlyingType);
        return;
    }
    if (structDecl->incomplete)
    {
        SbPrintf(out, "struct %s;  ; forward\n", structDecl->name);
        return;
    }
    SbPrintf(out, "%sstruct %s", structDecl->isExtern ? "extern " : "", structDecl->name);
    DumpTypeParams(&structDecl->genericParams, out);
    SbPuts(out, " {\n");
    for (size_t i = 0; i < structDecl->fields.count; i++)
    {
        FieldDecl* field = (FieldDecl*)VecGet(&structDecl->fields, i);
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

static void DumpFunction(Node* n, int indent, Sb* out)
{
    FunctionDecl* functionDecl = AS_NODE(FunctionDecl, n);
    if (functionDecl->isPrivate)
    {
        SbPuts(out, "@private\n");
        Pad(indent, out);
    }
    SbPuts(out, "fn ");

    if (functionDecl->isExtern)
    {
        SbPuts(out, "extern ");
    }

    SbPrintf(out, "%s %s", functionDecl->returnType.name,
             functionDecl->methodName ? functionDecl->methodName : functionDecl->name);
    DumpTypeParams(&functionDecl->genericParams, out);
    SbPutc(out, '(');

    for (size_t i = 0; i < functionDecl->params.count; i++)
    {
        if (i > 0)
        {
            SbPuts(out, ", ");
        }

        ParamDecl* param = (ParamDecl*)VecGet(&functionDecl->params, i);

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

        if (param->isReturn)
        {
            SbPuts(out, "return ");
        }
        else
        {
            if (param->type.isConst)
            {
                SbPuts(out, "const ");
            }

            SbPuts(out, ParamModSpelling(param->mod));

            if (param->mod != ModNone)
            {
                SbPutc(out, ' ');
            }
        }

        SbPrintf(out, "%s %s", param->type.name, param->name);
    }

    if (functionDecl->isCVararg)
    {
        SbPuts(out, ", ...");
    }

    SbPutc(out, ')');

    if (!functionDecl->body)
    {
        SbPuts(out, ";\n");

        return;
    }

    SbPutc(out, '\n');
    Dump(functionDecl->body, indent + 2, out);

    return;
}

static void DumpBlock(Node* n, int indent, Sb* out)
{
    Block* block = AS_NODE(Block, n);
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

static void DumpReturn(Node* n, Sb* out)
{
    ReturnStmt* returnStmt = AS_NODE(ReturnStmt, n);
    SbPuts(out, "return");

    if (returnStmt->value)
    {
        SbPutc(out, ' ');

        Dump(returnStmt->value, 0, out);
    }
    else
    {
        SbPutc(out, '\n');
    }

    return;
}

static void DumpIf(Node* n, int indent, Sb* out)
{
    IfStmt* i = AS_NODE(IfStmt, n);

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

static void DumpWhile(Node* n, int indent, Sb* out)
{
    WhileStmt* w = AS_NODE(WhileStmt, n);
    SbPuts(out, "while ");
    Dump(w->condition, 0, out);
    SbPutc(out, '\n');
    Dump(w->body, indent + 2, out);

    return;
}

static void DumpFor(Node* n, int indent, Sb* out)
{
    ForStmt* forStmt = AS_NODE(ForStmt, n);
    SbPuts(out, "for (");

    if (forStmt->init)
    {
        Dump(forStmt->init, 0, out);
    }
    else
    {
        SbPuts(out, "; ");
    }

    if (forStmt->condition)
    {
        Dump(forStmt->condition, 0, out);
    }

    SbPuts(out, "; ");

    if (forStmt->update)
    {
        Dump(forStmt->update, 0, out);
    }

    SbPuts(out, ")\n");
    Dump(forStmt->body, indent + 2, out);

    return;
}

static void DumpVarDecl(Node* n, Sb* out)
{
    VarDeclStmt* varDecl = AS_NODE(VarDeclStmt, n);
    SbPrintf(out, "%s %s", varDecl->type.name, varDecl->name);

    if (varDecl->init)
    {
        SbPuts(out, " = ");
        Dump(varDecl->init, 0, out);
    }
    else
    {
        SbPutc(out, '\n');
    }

    return;
}

static void DumpExprStmt(Node* n, Sb* out)
{
    ExprStmt* expr = AS_NODE(ExprStmt, n);

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

static void DumpBreak(Sb* out)
{
    SbPuts(out, "break\n");
    return;
}

static void DumpContinue(Sb* out)
{
    SbPuts(out, "continue\n");
    return;
}

static void DumpIntLiteral(Node* n, Sb* out)
{
    IntLiteral* lit = AS_NODE(IntLiteral, n);
    SbPrintf(out, "%llu", (unsigned long long)lit->value);

    if (lit->isUnsigned)
    {
        SbPutc(out, 'u');
    }

    SbPutc(out, '\n');

    return;
}

static void DumpFloatLiteral(Node* n, Sb* out)
{
    SbPrintf(out, "%g\n", AS_NODE(FloatLiteral, n)->value);
    return;
}

static void DumpBoolLiteral(Node* n, Sb* out)
{
    SbPuts(out, AS_NODE(BoolLiteral, n)->value ? "true\n" : "false\n");
    return;
}

static void DumpStrLiteral(Node* n, Sb* out)
{
    SbPrintf(out, "\"%s\"\n", AS_NODE(StrLiteral, n)->value);
    return;
}

static void DumpIdent(Node* n, Sb* out)
{
    SbPrintf(out, "%s\n", AS_NODE(IdentExpr, n)->name);
    return;
}

static void DumpUnary(Node* n, Sb* out)
{
    UnaryExpr* unaryExpr = AS_NODE(UnaryExpr, n);
    SbPrintf(out, "(%s ", UnaryOpSpelling(unaryExpr->op));
    Dump(unaryExpr->operand, 0, out);

    return;
}

static void DumpBinary(Node* n, Sb* out)
{
    BinaryExpr* binaryExpr = AS_NODE(BinaryExpr, n);
    SbPrintf(out, "(%s ", BinaryOpSpelling(binaryExpr->op));
    Dump(binaryExpr->lhs, 0, out);
    Pad(0, out);
    Dump(binaryExpr->rhs, 0, out);

    return;
}

static void DumpAssign(Node* n, Sb* out)
{
    AssignExpr* assignExpr = AS_NODE(AssignExpr, n);
    SbPrintf(out, "(%s ", AssignOpSpelling(assignExpr->op));
    Dump(assignExpr->target, 0, out);
    Dump(assignExpr->value, 0, out);

    return;
}

static void DumpCall(Node* n, Sb* out)
{
    CallExpr* callExpr = AS_NODE(CallExpr, n);

    if (callExpr->calleeBase)
    {
        // Pre-sema member call: `base.Member(args...)`.
        SbPrintf(out, "(callmethod %s ", callExpr->callee);
        Dump(callExpr->calleeBase, 0, out);
    }
    else
    {
        SbPrintf(out, "(call %s", callExpr->callee);
    }

    for (size_t i = 0; i < callExpr->args.count; i++)
    {
        Node* arg = (Node*)VecGet(&callExpr->args, i);
        SbPutc(out, ' ');
        Dump(arg, 0, out);
    }

    return;
}

static void DumpMember(Node* n, Sb* out)
{
    MemberExpr* memberExpr = AS_NODE(MemberExpr, n);
    SbPrintf(out, "(. %s ", memberExpr->member);
    Dump(memberExpr->baseNode, 0, out);

    return;
}

static void DumpStructInit(Node* n, Sb* out)
{
    StructInitExpr* si = AS_NODE(StructInitExpr, n);
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

static void DumpIncDec(Node* n, Sb* out)
{
    IncDecExpr* inc = AS_NODE(IncDecExpr, n);
    SbPrintf(out, "(%c%c%s ", inc->isDec ? '-' : '+', inc->isDec ? '-' : '+', inc->isPrefix ? "" : "p");
    Dump(inc->operand, 0, out);
    SbPutc(out, ')');

    return;
}

static void DumpCast(Node* n, Sb* out)
{
    CastExpr* cast = AS_NODE(CastExpr, n);
    SbPrintf(out, "(cast %s ", cast->type.name);
    Dump(cast->operand, 0, out);
    SbPutc(out, ')');
    return;
}

static void DumpGlobal(Node* n, Sb* out)
{
    GlobalDecl* gd = AS_NODE(GlobalDecl, n);
    SbPrintf(out, "global %s %s", gd->type.name, gd->name);

    if (gd->init)
    {
        SbPuts(out, " = ");
        Dump(gd->init, 0, out);
    }

    SbPuts(out, " ;\n");

    return;
}

static void DumpIndex(Node* n, Sb* out)
{
    IndexExpr* idx = AS_NODE(IndexExpr, n);
    SbPuts(out, "([] ");
    Dump(idx->baseNode, 0, out);
    SbPutc(out, ' ');
    Dump(idx->index, 0, out);
    SbPutc(out, ')');
    return;
}

static void DumpArrayInit(Node* n, Sb* out)
{
    ArrayInitExpr* ai = AS_NODE(ArrayInitExpr, n);
    SbPrintf(out, "(array-init %s", ai->elementType ? ai->elementType->name : "");

    for (size_t i = 0; i < ai->elements.count; i++)
    {
        SbPutc(out, ' ');
        Dump((Node*)VecGet(&ai->elements, i), 0, out);
    }

    SbPutc(out, ')');
    return;
}

static void DumpConditional(Node* n, Sb* out)
{
    ConditionalExpr* ce = AS_NODE(ConditionalExpr, n);
    SbPuts(out, "(? ");
    Dump(ce->condition, 0, out);
    SbPutc(out, ' ');
    Dump(ce->thenExpr, 0, out);
    SbPutc(out, ' ');
    Dump(ce->elseExpr, 0, out);
    SbPutc(out, ')');
    return;
}

static void DumpNullTest(Node* n, Sb* out)
{
    NullTestExpr* nt = AS_NODE(NullTestExpr, n);
    SbPuts(out, "(nulltest ");
    Dump(nt->operand, 0, out);
    SbPutc(out, '?');
    SbPutc(out, ')');
    return;
}

static void DumpDefer(Node* n, Sb* out)
{
    DeferStmt* d = AS_NODE(DeferStmt, n);
    SbPuts(out, "defer ");
    Dump(d->stmt, 0, out);
    return;
}

static void DumpParam(Sb* out)
{
    SbPuts(out, "(param)\n");
    return;
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
        DumpModule(n, indent, out);
        return;
    case NodeImpl:
        DumpImpl(n, indent, out);
        return;
    case NodeImport:
        DumpImport(n, out);
        return;
    case NodeHandle:
        DumpHandle(n, out);
        return;
    case NodeEnum:
        DumpEnum(n, indent, out);
        return;
    case NodeEnumMember:
        return;
    case NodeStruct:
        DumpStruct(n, indent, out);
        return;
    case NodeFunction:
        DumpFunction(n, indent, out);
        return;
    case NodeBlock:
        DumpBlock(n, indent, out);
        return;
    case NodeReturn:
        DumpReturn(n, out);
        return;
    case NodeIf:
        DumpIf(n, indent, out);
        return;
    case NodeWhile:
        DumpWhile(n, indent, out);
        return;
    case NodeFor:
        DumpFor(n, indent, out);
        return;
    case NodeVarDecl:
        DumpVarDecl(n, out);
        return;
    case NodeExprStmt:
        DumpExprStmt(n, out);
        return;
    case NodeBreak:
        DumpBreak(out);
        return;
    case NodeContinue:
        DumpContinue(out);
        return;
    case NodeIntLiteral:
        DumpIntLiteral(n, out);
        return;
    case NodeFloatLiteral:
        DumpFloatLiteral(n, out);
        return;
    case NodeBoolLiteral:
        DumpBoolLiteral(n, out);
        return;
    case NodeStrLiteral:
        DumpStrLiteral(n, out);
        return;
    case NodeIdent:
        DumpIdent(n, out);
        return;
    case NodeUnary:
        DumpUnary(n, out);
        return;
    case NodeBinary:
        DumpBinary(n, out);
        return;
    case NodeAssign:
        DumpAssign(n, out);
        return;
    case NodeCall:
        DumpCall(n, out);
        return;
    case NodeMember:
        DumpMember(n, out);
        return;
    case NodeStructInit:
        DumpStructInit(n, out);
        return;
    case NodeIncDec:
        DumpIncDec(n, out);
        return;
    case NodeCast:
        DumpCast(n, out);
        return;
    case NodeGlobal:
        DumpGlobal(n, out);
        return;
    case NodeIndex:
        DumpIndex(n, out);
        return;
    case NodeArrayInit:
        DumpArrayInit(n, out);
        return;
    case NodeConditional:
        DumpConditional(n, out);
        return;
    case NodeNullTest:
        DumpNullTest(n, out);
        return;
    case NodeDefer:
        DumpDefer(n, out);
        return;
    case NodeParam:
        DumpParam(out);
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
