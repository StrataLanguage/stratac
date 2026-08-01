#include "Sema/ResolveOverloads.h"
#include "Codegen/TypeRegistry.h"

#include <limits.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    Module* m_mod;
    DiagnosticEngine* m_diag;
    TypeRegistry m_registry;
    Arena* m_arena;
    StrMap m_constVars;
    StrMap m_movedBoxes;
    const char* m_currentReturnType;
} Resolver;

static char* Mangle(Arena* arena, const FunctionDecl* f)
{
    Sb sb;
    SbInit(&sb);
    SbPuts(&sb, f->name);

    for (size_t i = 0; i < f->params.count; i++)
    {
        ParamDecl* p = (ParamDecl*)VecGet(&f->params, i);
        SbPutc(&sb, '$');
        SbPuts(&sb, p->type.name);
    }

    return SbFinish(&sb, arena);
}

static bool IsDefinedStruct(const TypeRegistry* reg, const char* name)
{
    return TypeRegistryIsUserType(reg, name) && !TypeRegistryIsOpaque(reg, name);
}

static bool IsIncompleteStruct(const TypeRegistry* reg, const char* name)
{
    const StructType* t = TypeRegistryFind(reg, name);
    return t && t->opaque && t->incomplete;
}

/* Move-state tracking for box locals (value 1 = moved, 2 = re-live). */
static bool IsBoxMoved(const Resolver* r, const char* name)
{
    return StrMapGet(&r->m_movedBoxes, name) == (void*)1;
}

static void MarkBoxMoved(Resolver* r, const char* name)
{
    StrMapPut(&r->m_movedBoxes, name, (void*)1);
}

static void MarkBoxLive(Resolver* r, const char* name)
{
    StrMapPut(&r->m_movedBoxes, name, (void*)2);
}

static int CountByName(const Module* mod, const char* name)
{
    int count = 0;

    for (size_t i = 0; i < mod->functions.count; i++)
    {
        FunctionDecl* functionDecl = (FunctionDecl*)VecGet(&mod->functions, i);

        if (strcmp(functionDecl->name, name) == 0)
        {
            count++;
        }
    }

    return count;
}

static const char* InferType(Resolver* r, Node* n, StrMap* scope);

static void WalkBlock(Resolver* r, Block* b, StrMap* scope);
static void WalkStmt(Resolver* r, Node* n, StrMap* scope);
static void ResolveExpr(Resolver* r, Node* n, StrMap* scope);

static void CheckConstAssign(Resolver* r, Node* target, SourceRange range)
{
    Node* base = target;
    while (base->kind == NodeMember)
    {
        base = ((MemberExpr*)base)->base_node;
    }

    if (base->kind == NodeIdent)
    {
        const char* name = ((IdentExpr*)base)->name;
        if (StrMapGet(&r->m_constVars, name))
        {
            DiagErrorFmt(r->m_diag, range, "'%s' is immutable", name);
        }
    }
}

static void ResolveCall(Resolver* r, CallExpr* c, StrMap* scope)
{
    if (TypeRegistryIsUserType(&r->m_registry, c->callee))
    {
        return;
    }

    bool found = false;

    for (size_t i = 0; i < r->m_mod->functions.count; i++)
    {
        FunctionDecl* functionDecl = (FunctionDecl*)VecGet(&r->m_mod->functions, i);

        if (strcmp(functionDecl->name, c->callee) == 0)
        {
            found = true;
            break;
        }
    }

    if (!found)
    {
        DiagErrorFmt(r->m_diag, c->base.range, "unknown function '%s'", c->callee);
        return;
    }

    FunctionDecl* best = NULL;

    int bestScore = INT_MAX;
    bool ambiguous = false;

    for (size_t i = 0; i < r->m_mod->functions.count; i++)
    {
        FunctionDecl* functionDecl = (FunctionDecl*)VecGet(&r->m_mod->functions, i);
        
        if (strcmp(functionDecl->name, c->callee) != 0)
        {
            continue;
        }

        if (functionDecl->params.count != c->args.count)
        {
            continue;
        }

        int score = 0;
        
        bool viable = true;

        for (size_t j = 0; j < c->args.count; j++)
        {
            Node* arg = (Node*)VecGet(&c->args, j);
            const char* argType = InferType(r, arg, scope);
            ParamDecl* param = (ParamDecl*)VecGet(&functionDecl->params, j);

            if (argType[0] == '\0')
            {
                continue;
            }

            if (strcmp(argType, param->type.name) == 0)
            {
            }
            else if (IsNumeric(argType) && IsNumeric(param->type.name))
            {
                score += 1;
            }
            else if (HandleExtendsFrom(&r->m_registry, argType, param->type.name))
            {
                score += 1;
            }
            else
            {
                viable = false;

                break;
            }
        }

        if (!viable)
        {
            continue;
        }

        if (score < bestScore)
        {
            bestScore = score;
            best = functionDecl;
            ambiguous = false;
        }
        else if (score == bestScore && best)
        {
            ambiguous = true;
        }
    }

    if (!best)
    {
        DiagErrorFmt(r->m_diag, c->base.range, "no matching overload for '%s' with %zu argument(s)",
                     c->callee, c->args.count);
        return;
    }

    if (ambiguous)
    {
        DiagErrorFmt(r->m_diag, c->base.range, "ambiguous call to overload '%s'", c->callee);
    }

    c->callee = best->mangledName;
    c->resolvedDecl = best;
}

static const char* InferType(Resolver* r, Node* n, StrMap* scope)
{
    if (!n)
    {
        return "";
    }

    switch (n->kind)
    {
    case NodeIntLiteral:
        return ((IntLiteral*)n)->isUnsigned ? "uint" : "int";
    case NodeFloatLiteral:
        return "float";
    case NodeBoolLiteral:
        return "bool";
    case NodeIdent:
    {
        const char* t = (const char*)StrMapGet(scope, ((IdentExpr*)n)->name);

        return t ? t : "";
    }
    case NodeUnary:
    {
        UnaryExpr* u = (UnaryExpr*)n;

        return u->op == UnNot ? "bool" : InferType(r, u->operand, scope);
    }
    case NodeBinary:
    {
        BinaryExpr* b = (BinaryExpr*)n;

        switch (b->op)
        {
        case BinEqEq:
        case BinNotEq:
        case BinLt:
        case BinLtEq:
        case BinGt:
        case BinGtEq:
        case BinLogicAnd:
        case BinLogicOr:
            return "bool";
        default:
        {
            const char* lt = InferType(r, b->lhs, scope);
            const char* rt = InferType(r, b->rhs, scope);

            if (strcmp(lt, "double") == 0 || strcmp(rt, "double") == 0)
            {
                return "double";
            }

            if (strcmp(lt, "float") == 0 || strcmp(rt, "float") == 0)
            {
                return "float";
            }

            if (strcmp(lt, "ulong") == 0 || strcmp(rt, "ulong") == 0)
            {
                return "ulong";
            }

            if (strcmp(lt, "long") == 0 || strcmp(rt, "long") == 0)
            {
                return "long";
            }

            if (strcmp(lt, "uint") == 0 || strcmp(rt, "uint") == 0)
            {
                return "uint";
            }

            return "int";
        }
        }
    }
    case NodeAssign:
        return InferType(r, ((AssignExpr*)n)->target, scope);
    case NodeIncDec:
        return InferType(r, ((IncDecExpr*)n)->operand, scope);
    case NodeCast:
        return ((CastExpr*)n)->type.name;
    case NodeMember:
    {
        MemberExpr* m = (MemberExpr*)n;

        const char* baseName = InferType(r, m->base_node, scope);

        char boxInner[128];
        if (IsBoxTypeName(baseName) && BoxInnerTypeName(baseName, boxInner, sizeof boxInner))
        {
            baseName = boxInner;
        }

        if (TypeRegistryIsOpaque(&r->m_registry, baseName))
        {
            if (IsIncompleteStruct(&r->m_registry, baseName))
            {
                DiagErrorFmt(r->m_diag, m->base.range, "cannot access member '%s' of incomplete type '%s'", m->member, baseName);
            }
            else
            {
                DiagErrorFmt(r->m_diag, m->base.range, "cannot access member '%s' of opaque handle '%s'", m->member, baseName);
            }

            return "";
        }

        const StructType* structType = TypeRegistryFind(&r->m_registry, baseName);
        if (!structType)
        {
            return "";
        }

        int idx = TypeRegistryFieldIndex(&r->m_registry, baseName, m->member);
        if (idx < 0)
        {
            return "";
        }

        FieldDecl* fieldDecl = (FieldDecl*)VecGet((Vec*)&structType->fields, (size_t)idx);

        return fieldDecl->type.name;
    }
    case NodeCall:
    {
        CallExpr* c = (CallExpr*)n;

        if (c->resolvedDecl)
        {
            return c->resolvedDecl->returnType.name;
        }

        if (TypeRegistryIsUserType(&r->m_registry, c->callee))
        {
            return c->callee;
        }

        return "";
    }
    case NodeStructInit:
        return ((StructInitExpr*)n)->typeName;
    default:
        return "";
    }
}

static void ResolveExpr(Resolver* r, Node* n, StrMap* scope)
{
    if (!n)
    {
        return;
    }

    switch (n->kind)
    {
    case NodeIntLiteral:
    case NodeFloatLiteral:
    case NodeBoolLiteral:
        return;
    case NodeIdent:
    {
        IdentExpr* ident = (IdentExpr*)n;
        const char* varType = (const char*)StrMapGet(scope, ident->name);

        if (!varType)
        {
            DiagErrorFmt(r->m_diag, ident->base.range, "unknown variable '%s'", ident->name);
        }
        else if (IsBoxTypeName(varType) && IsBoxMoved(r, ident->name))
        {
            DiagErrorFmt(r->m_diag, ident->base.range, "use of moved box '%s'", ident->name);
        }

        return;
    }
    case NodeUnary:
        ResolveExpr(r, ((UnaryExpr*)n)->operand, scope);
        return;
    case NodeBinary:
    {
        BinaryExpr* b = (BinaryExpr*)n;

        ResolveExpr(r, b->lhs, scope);
        ResolveExpr(r, b->rhs, scope);

        return;
    }
    case NodeAssign:
    {
        AssignExpr* a = (AssignExpr*)n;

        CheckConstAssign(r, a->target, a->base.range);

        const char* tt = (a->target->kind == NodeIdent) ? InferType(r, a->target, scope) : NULL;
        bool boxMove = tt && IsBoxTypeName(tt);

        if (boxMove)
        {
            /* Box move: the value must be a box of the same type. Reading the
               value validates it is not itself moved; then ownership moves. */
            const char* vt = InferType(r, a->value, scope);

            if (vt[0] != '\0' && strcmp(vt, tt) != 0)
            {
                DiagErrorFmt(r->m_diag, a->base.range, "cannot assign '%s' to box variable '%s'",
                             vt, ((IdentExpr*)a->target)->name);
            }

            ResolveExpr(r, a->value, scope);

            MarkBoxLive(r, ((IdentExpr*)a->target)->name);

            if (a->value->kind == NodeIdent)
            {
                MarkBoxMoved(r, ((IdentExpr*)a->value)->name);
            }
        }
        else
        {
            ResolveExpr(r, a->target, scope);
            ResolveExpr(r, a->value, scope);
        }

        return;
    }
    case NodeIncDec:
    {
        IncDecExpr* inc = (IncDecExpr*)n;
        CheckConstAssign(r, inc->operand, inc->base.range);
        ResolveExpr(r, inc->operand, scope);
        return;
    }
    case NodeCast:
    {
        CastExpr* cast = (CastExpr*)n;
        ResolveExpr(r, cast->operand, scope);

        const char* src = InferType(r, cast->operand, scope);
        const char* dst = cast->type.name;

        bool scalarPair = src && dst && IsScalarTypeName(src) && IsScalarTypeName(dst);
        bool handlePair = src && dst
            && IsHandleType(&r->m_registry, src) && IsHandleType(&r->m_registry, dst)
            && (HandleExtendsFrom(&r->m_registry, dst, src)
                || HandleExtendsFrom(&r->m_registry, src, dst));

        if (src && src[0] != '\0' && dst && dst[0] != '\0' && !scalarPair && !handlePair)
        {
            DiagErrorFmt(r->m_diag, cast->base.range, "invalid cast from '%s' to '%s'", src, dst);
        }

        return;
    }
    case NodeMember:
    {
        MemberExpr* m = (MemberExpr*)n;
        ResolveExpr(r, m->base_node, scope);
        
        const char* baseName = InferType(r, m->base_node, scope);

        char boxInner[128];
        if (IsBoxTypeName(baseName) && BoxInnerTypeName(baseName, boxInner, sizeof boxInner))
        {
            baseName = boxInner;
        }

        if (TypeRegistryIsOpaque(&r->m_registry, baseName))
        {
            if (IsIncompleteStruct(&r->m_registry, baseName))
            {
                DiagErrorFmt(r->m_diag, m->base.range, "cannot access a member of incomplete type '%s'", baseName);
            }
            else
            {
                DiagError(r->m_diag, m->base.range, "cannot access a member of an opaque handle");
            }
        }

        return;
    }
    case NodeCall:
    {
        CallExpr* c = (CallExpr*)n;
        
        for (size_t i = 0; i < c->args.count; i++)
        {
            ResolveExpr(r, (Node*)VecGet(&c->args, i), scope);
        }
        
        ResolveCall(r, c, scope);

        return;
    }
    case NodeStructInit:
    {
        StructInitExpr* structInitExpr = (StructInitExpr*)n;

        if (!TypeRegistryIsUserType(&r->m_registry, structInitExpr->typeName))
        {
            DiagErrorFmt(r->m_diag, structInitExpr->base.range, "'%s' is not a known aggregate type", structInitExpr->typeName);
        }
        else if (TypeRegistryIsOpaque(&r->m_registry, structInitExpr->typeName))
        {
            DiagError(r->m_diag, structInitExpr->base.range, "cannot braced-initialize handles");
        }

        size_t positionalCount = 0;

        const StructType* structType = TypeRegistryFind(&r->m_registry, structInitExpr->typeName);

        for (size_t i = 0; i < structInitExpr->fields.count; i++)
        {
            StructInitField* field = (StructInitField*)VecGet(&structInitExpr->fields, i);
            ResolveExpr(r, field->value, scope);

            if (field->name && field->name[0] != '\0')
            {
                if (structType && TypeRegistryFieldIndex(&r->m_registry, structInitExpr->typeName, field->name) < 0)
                {
                    DiagErrorFmt(r->m_diag, structInitExpr->base.range, "struct '%s' has no field named '%s'", structInitExpr->typeName, field->name);
                }
            }
            else
            {
                if (structType && positionalCount >= structType->fields.count)
                {
                    DiagErrorFmt(r->m_diag, structInitExpr->base.range, "too many initializers for struct '%s'", structInitExpr->typeName);
                }

                positionalCount++;
            }
        }

        return;
    }
    default:
        return;
    }
}

static void WalkStmt(Resolver* r, Node* n, StrMap* scope)
{
    if (!n)
    {
        return;
    }

    switch (n->kind)
    {
    case NodeBlock:
        WalkBlock(r, (Block*)n, scope);
        return;

    case NodeVarDecl:
    {
        VarDeclStmt* vd = (VarDeclStmt*)n;

        if (IsIncompleteStruct(&r->m_registry, vd->type.name))
        {
            DiagErrorFmt(r->m_diag, vd->base.range, "variable '%s' has incomplete type '%s'", vd->name, vd->type.name);
        }

        if (IsBoxTypeName(vd->type.name) && !vd->init)
        {
            DiagErrorFmt(r->m_diag, vd->base.range, "box variable '%s' must be initialized", vd->name);
        }

        if (vd->type.isConst && !vd->init)
        {
            DiagErrorFmt(r->m_diag, vd->base.range, "const variable '%s' must be initialized", vd->name);
        }

        if (vd->init)
        {
            ResolveExpr(r, vd->init, scope);
            const char* initType = InferType(r, vd->init, scope);

            bool ok;

            if (IsBoxTypeName(vd->type.name))
            {
                char boxInner[128];
                BoxInnerTypeName(vd->type.name, boxInner, sizeof boxInner);
                /* Either box a value of the inner type, or move from another box<T>. */
                ok = strcmp(initType, boxInner) == 0 || strcmp(initType, vd->type.name) == 0;
            }
            else
            {
                ok = strcmp(initType, vd->type.name) == 0
                    || (IsNumeric(initType) && IsNumeric(vd->type.name))
                    || HandleExtendsFrom(&r->m_registry, initType, vd->type.name);
            }

            if (initType[0] != '\0' && !ok)
            {
                DiagErrorFmt(r->m_diag, vd->base.range, "'%s' cannot be initialized by expression of type '%s'", vd->type.name, initType);
            }

            /* A box<T> initialized from another box<T> moves the source. */
            if (IsBoxTypeName(vd->type.name) && IsBoxTypeName(initType) && vd->init->kind == NodeIdent)
            {
                MarkBoxMoved(r, ((IdentExpr*)vd->init)->name);
            }
        }

        StrMapPut(scope, vd->name, (void*)vd->type.name);

        return;
    }
    case NodeExprStmt:
        ResolveExpr(r, ((ExprStmt*)n)->expr, scope);
        return;
    case NodeReturn:
    {
        ReturnStmt* rs = (ReturnStmt*)n;
        if (rs->value)
        {
            if (r->m_currentReturnType && strcmp(r->m_currentReturnType, "void") == 0)
            {
                DiagErrorFmt(r->m_diag, rs->base.range, "void function cannot return a value");
            }

            ResolveExpr(r, rs->value, scope);

            const char* typeName = InferType(r, rs->value, scope);

            if (typeName[0] != '\0' && strcmp(typeName, "void") == 0)
            {
                DiagErrorFmt(r->m_diag, rs->base.range, "cannot return a value of type 'void'");
            }

            /* Returning a box moves it out. */
            if (IsBoxTypeName(typeName) && rs->value->kind == NodeIdent)
            {
                MarkBoxMoved(r, ((IdentExpr*)rs->value)->name);
            }
        }

        return;
    }
    case NodeIf:
    {
        IfStmt* i = (IfStmt*)n;
        ResolveExpr(r, i->condition, scope);
        WalkStmt(r, i->thenBranch, scope);

        if (i->elseBranch)
        {
            WalkStmt(r, i->elseBranch, scope);
        }

        return;
    }
    case NodeWhile:
    {
        WhileStmt* w = (WhileStmt*)n;
        ResolveExpr(r, w->condition, scope);
        WalkStmt(r, w->body, scope);

        return;
    }
    case NodeFor:
    {
        ForStmt* fs = (ForStmt*)n;

        if (fs->init)
        {
            if (fs->init->kind == NodeVarDecl)
            {
                WalkStmt(r, fs->init, scope);
            }
            else
            {
                ResolveExpr(r, fs->init, scope);
            }
        }

        if (fs->condition)
        {
            ResolveExpr(r, fs->condition, scope);
        }

        if (fs->update)
        {
            ResolveExpr(r, fs->update, scope);
        }
        
        WalkStmt(r, fs->body, scope);

        return;
    }
    default:
        return;
    }
}

static void WalkBlock(Resolver* r, Block* b, StrMap* scope)
{
    for (size_t i = 0; i < b->statements.count; i++)
    {
        WalkStmt(r, (Node*)VecGet(&b->statements, i), scope);
    }
}

void ResolveOverloads(Module* mod, DiagnosticEngine* diag, Arena* arena)
{
    Resolver r = {0};
    r.m_mod = mod;
    r.m_diag = diag;
    r.m_arena = arena;

    TypeRegistryInit(&r.m_registry);
    TypeRegistryBuild(&r.m_registry, mod);

    StrMapInit(&r.m_constVars);
    StrMapInit(&r.m_movedBoxes);

    StrMap byMangled;
    StrMapInit(&byMangled);

    for (size_t i = 0; i < mod->functions.count; i++)
    {
        FunctionDecl* functionDecl = (FunctionDecl*)VecGet(&mod->functions, i);

        bool overloaded = CountByName(mod, functionDecl->name) > 1;

        if (overloaded && functionDecl->isExtern)
        {
            DiagErrorFmt(diag, functionDecl->base.range, "extern function '%s' cannot be overloaded", functionDecl->name);
        }

        functionDecl->mangledName = overloaded ? Mangle(arena, functionDecl) : functionDecl->name;

        if (StrMapGet(&byMangled, functionDecl->mangledName))
        {
            DiagErrorFmt(diag, functionDecl->base.range, "duplicate function signature for '%s'", functionDecl->name);
        }

        StrMapPut(&byMangled, functionDecl->mangledName, functionDecl);
    }

    for (size_t i = 0; i < mod->functions.count; i++)
    {
        FunctionDecl* functionDecl = (FunctionDecl*)VecGet(&mod->functions, i);

        if (!functionDecl->body)
        {
            continue;
        }

        StrMap scope;
        StrMapInit(&scope);

        for (size_t j = 0; j < mod->globals.count; j++)
        {
            GlobalDecl* gd = (GlobalDecl*)VecGet(&mod->globals, j);
            StrMapPut(&scope, gd->name, (void*)gd->type.name);

            if (gd->type.isConst)
            {
                StrMapPut(&r.m_constVars, gd->name, (void*)1);
            }
        }

        if (r.m_constVars.cap > 0)
        {
            memset(r.m_constVars.keys, 0, r.m_constVars.cap * sizeof(const char*));
            r.m_constVars.count = 0;
        }

        if (r.m_movedBoxes.cap > 0)
        {
            memset(r.m_movedBoxes.keys, 0, r.m_movedBoxes.cap * sizeof(const char*));
            r.m_movedBoxes.count = 0;
        }

        for (size_t j = 0; j < functionDecl->params.count; j++)
        {
            ParamDecl* p = (ParamDecl*)VecGet(&functionDecl->params, j);
            StrMapPut(&scope, p->name, (void*)p->type.name);

            if (p->type.isConst)
            {
                StrMapPut(&r.m_constVars, p->name, (void*)1);
            }
        }

        r.m_currentReturnType = functionDecl->returnType.name;

        WalkBlock(&r, (Block*)functionDecl->body, &scope);
        r.m_currentReturnType = NULL;

        StrMapFree(&scope);
    }

    for (size_t i = 0; i < mod->structs.count; i++)
    {
        StructDecl* sd = (StructDecl*)VecGet(&mod->structs, i);

        if (sd->incomplete)
        {
            continue;
        }

        for (size_t j = 0; j < sd->fields.count; j++)
        {
            FieldDecl* field = (FieldDecl*)VecGet(&sd->fields, j);

            if (IsBoxTypeName(field->type.name))
            {
                DiagErrorFmt(diag, field->type.range, "box fields are not supported yet (field '%s')", field->name);
            }

            if (IsIncompleteStruct(&r.m_registry, field->type.name))
            {
                DiagErrorFmt(diag, field->type.range, "field '%s' has incomplete type '%s'", field->name, field->type.name);
            }
        }
    }

    for (size_t i = 0; i < mod->globals.count; i++)
    {
        GlobalDecl* gd = (GlobalDecl*)VecGet(&mod->globals, i);

        if (IsBoxTypeName(gd->type.name))
        {
            DiagErrorFmt(diag, gd->base.range, "global '%s' cannot have box type", gd->name);
        }
    }

    for (size_t i = 0; i < mod->functions.count; i++)
    {
        FunctionDecl* functionDecl = (FunctionDecl*)VecGet(&mod->functions, i);

        for (size_t j = 0; j < functionDecl->params.count; j++)
        {
            ParamDecl* p = (ParamDecl*)VecGet(&functionDecl->params, j);

            if (IsBoxTypeName(p->type.name))
            {
                DiagErrorFmt(diag, p->base.range, "box parameters are not supported yet ('%s')", p->name);
            }

            if (IsDefinedStruct(&r.m_registry, p->type.name)
                && p->type.isConst
                && p->mod == ModRef)
            {
                DiagErrorFmt(diag, p->base.range, "'ref' parameter cannot be 'const'");
            }
        }

        if (functionDecl->isExtern && IsDefinedStruct(&r.m_registry, functionDecl->returnType.name))
        {
            DiagError(diag, functionDecl->base.range, "extern function cannot return a struct type by value");
        }

        if (IsIncompleteStruct(&r.m_registry, functionDecl->returnType.name))
        {
            DiagErrorFmt(diag, functionDecl->base.range, "function cannot return incomplete type '%s'", functionDecl->returnType.name);
        }
    }

    StrMapFree(&r.m_constVars);
    StrMapFree(&r.m_movedBoxes);
    StrMapFree(&byMangled);
    TypeRegistryFree(&r.m_registry);
}
