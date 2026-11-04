// Deep copies of AST subtrees with type parameters substituted (see ASTClone.h).

#include "AST/ASTClone.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The argument that replaces the type parameter `name`, or NULL when `name` is not a parameter.
static const TypeName* SubstArg(const TypeSubst* s, const char* name)
{
    if (!s || !s->params || !name)
    {
        return NULL;
    }

    for (size_t i = 0; i < s->params->count; i++)
    {
        if (strcmp((const char*)VecGet(s->params, i), name) == 0)
        {
            return &s->args[i];
        }
    }

    return NULL;
}

// Whether `name` is the parameter pack's type name (`TArgs`) or its rest parameter (`args`) in `s`.
static bool IsPackTypeName(const TypeSubst* s, const char* name)
{
    return s && s->packType && name && strcmp(s->packType, name) == 0;
}

static bool IsPackValueName(const TypeSubst* s, const char* name)
{
    return s && s->packValue && name && strcmp(s->packValue, name) == 0;
}

// The name of element `index` of the pack value `value`: `args[0]`. The spelling is the one diagnostics show.
static Node* PackElementIdent(Arena* arena, SourceRange range, const char* value, size_t index)
{
    IdentExpr* e = AST_NEW(arena, IdentExpr);
    e->base.kind = NodeIdent;
    e->base.range = range;
    e->name = ArenaFormat(arena, "%s[%zu]", value, index);
    e->useKind = UseUnknown;

    return (Node*)e;
}

// The substitution a nested ordinary loop sees: a `break` or `continue` in it belongs to that loop, not to a pack
// loop around it. Returns `s` itself when nothing changes.
static const TypeSubst* OrdinaryLoopSubst(const TypeSubst* s, TypeSubst* storage)
{
    if (!s || !s->inPackLoopBody)
    {
        return s;
    }

    *storage = *s;
    storage->inPackLoopBody = false;

    return storage;
}

// The canonical spelling of an array type: its non-array leaf, then every dimension in source order.
// Naming one wrapper at a time would reverse the dimensions of `int[2][6]`.
static char* ArraySpelling(Arena* arena, const TypeName* array)
{
    const TypeName* leaf = array;

    while (TypeNameIsArray(leaf))
    {
        leaf = TypeNameArrayElem(leaf);
    }

    Sb sb;
    SbInit(&sb);
    SbPuts(&sb, leaf->name);

    for (const TypeName* dim = array; TypeNameIsArray(dim); dim = TypeNameArrayElem(dim))
    {
        if (dim->lengthName)
        {
            SbPrintf(&sb, "[%s]", dim->lengthName);
        }
        else if (dim->length < 0)
        {
            SbPuts(&sb, "[]");
        }
        else
        {
            SbPrintf(&sb, "[%ld]", dim->length);
        }
    }

    return SbFinish(&sb, arena);
}

// A new arena array of the type arguments, each substituted. A `Name...` argument that names the pack is replaced
// by the pack's types, so the count can change: `*outCount` is the count of the new array.
static TypeName* SubstTypeArgs(Arena* arena, const TypeName* args, size_t count, const TypeSubst* s, size_t* outCount)
{
    size_t total = 0;

    for (size_t i = 0; i < count; i++)
    {
        total += (args[i].isPackExpansion && IsPackTypeName(s, args[i].name)) ? s->packCount : 1;
    }

    *outCount = total;

    if (total == 0)
    {
        return NULL;
    }

    TypeName* copy = (TypeName*)ArenaAlloc(arena, total * sizeof(TypeName));
    size_t out = 0;

    for (size_t i = 0; i < count; i++)
    {
        if (args[i].isPackExpansion && IsPackTypeName(s, args[i].name))
        {
            for (size_t j = 0; j < s->packCount; j++)
            {
                copy[out++] = s->packArgs[j];
            }
        }
        else if (args[i].isPackExpansion)
        {
            // `T...` where T is not the pack: copied as written, so Monomorphize reports it by its own name.
            copy[out++] = args[i];
        }
        else
        {
            copy[out++] = TypeNameSubstitute(arena, &args[i], s);
        }
    }

    return copy;
}

TypeName TypeNameSubstitute(Arena* arena, const TypeName* t, const TypeSubst* s)
{
    // `number<T>`: the base stays, and the canonical name is rebuilt from the substituted arguments.
    if (t->typeArgCount > 0)
    {
        TypeName result = *t;
        result.typeArgs = SubstTypeArgs(arena, t->typeArgs, t->typeArgCount, s, &result.typeArgCount);
        result.name = TypeNameGenericSpelling(arena, t->genericBase, result.typeArgs, result.typeArgCount);
        result.primitiveType = GetPrimitiveType(result.name);

        return result;
    }

    // `T[]` and `T[N]`, rebuilt around the substituted element. A `[constName]` length is kept for sema.
    if (TypeNameIsArray(t))
    {
        TypeName elem = TypeNameSubstitute(arena, TypeNameArrayElem(t), s);
        bool isFixed = t->lengthName || t->length >= 0;

        TypeName result = isFixed ? TypeNameFixedArrayWrap(arena, elem, t->length) : TypeNameArrayWrap(arena, elem);
        result.lengthName = t->lengthName;
        result.name = ArraySpelling(arena, &result);
        result.isConst = t->isConst;
        result.range = t->range;

        return result;
    }

    // `^T`, `T?` and `*T`, rebuilt around the substituted inner type. A wrapper with no inner type is copied.
    if ((TypeNameIsBox(t) || TypeNameIsOptional(t) || TypeNameIsPointer(t)) && t->inner)
    {
        TypeName inner = TypeNameSubstitute(arena, t->inner, s);
        TypeName result;

        if (TypeNameIsBox(t))
        {
            result = TypeNameBoxWrap(arena, inner);
        }
        else if (TypeNameIsOptional(t))
        {
            result = TypeNameOptionalWrap(arena, inner);
        }
        else
        {
            result = TypeNamePointerWrap(arena, inner);
        }

        result.isConst = t->isConst;
        result.range = t->range;

        return result;
    }

    // A parameter leaf becomes a copy of its argument. It keeps the use's range, and const if either has it.
    const TypeName* arg = SubstArg(s, t->name);

    if (arg)
    {
        TypeName result = *arg;
        result.isConst = arg->isConst || t->isConst;
        result.primitiveType = GetPrimitiveType(result.name);
        result.range = t->range;

        return result;
    }

    // The pack itself is only ever spliced (`TArgs...`) or used as the rest parameter's element type.
    if (IsPackTypeName(s, t->name) && s->diag)
    {
        DiagErrorFmt(s->diag, t->range, "parameter pack '%s' must be expanded: write '%s...'", t->name, t->name);
    }

    return *t;
}

// Clones every node of `src` into `dst`, a fresh Vec.
static void CloneNodeList(Arena* arena, Vec* dst, const Vec* src, const TypeSubst* s)
{
    VecInit(dst);

    for (size_t i = 0; i < src->count; i++)
    {
        VecPush(dst, AstCloneSubst(arena, (const Node*)VecGet(src, i), s));
    }
}

// Copies a list of arena strings into a fresh Vec. The strings themselves are shared.
static void CloneNameList(Vec* dst, const Vec* src)
{
    VecInit(dst);

    for (size_t i = 0; i < src->count; i++)
    {
        VecPush(dst, VecGet(src, i));
    }
}

// The argument list of a call. `args...` naming the pack splices `args[0]` … `args[k-1]` into it.
static void CloneCallArgs(Arena* arena, Vec* dst, const Vec* src, const TypeSubst* s)
{
    VecInit(dst);

    for (size_t i = 0; i < src->count; i++)
    {
        const Node* arg = (const Node*)VecGet(src, i);

        if (arg->kind == NodePackExpand && IsPackValueName(s, ((const PackExpandExpr*)arg)->name))
        {
            for (size_t j = 0; j < s->packCount; j++)
            {
                VecPush(dst, PackElementIdent(arena, arg->range, s->packValue, j));
            }

            continue;
        }

        VecPush(dst, AstCloneSubst(arena, arg, s));
    }
}

// The parameter of a copied function that the pack rest parameter `src` becomes: `args[index]`, of the pack's
// type at `index`, with the rest parameter's modifier and constness.
static Node* PackParam(Arena* arena, const ParamDecl* src, const TypeSubst* s, size_t index)
{
    ParamDecl* copy = AST_NEW(arena, ParamDecl);
    *copy = *src;

    copy->name = ArenaFormat(arena, "%s[%zu]", src->name, index);
    copy->type = s->packArgs[index];
    copy->type.isConst = src->type.isConst || s->packArgs[index].isConst;
    copy->type.range = src->type.range;
    copy->isVarargRest = false;
    copy->isReturn = false;

    // Set by sema.
    copy->growsArray = false;
    copy->inferredBorrow = false;
    copy->borrowCandidate = false;

    return (Node*)copy;
}

// The parameters of a copied function. The pack rest parameter becomes one parameter per pack type; `*outExpanded`
// is set when that happened, so the copy is no longer variadic.
static void CloneParams(Arena* arena, Vec* dst, const Vec* src, const TypeSubst* s, bool* outExpanded)
{
    VecInit(dst);

    for (size_t i = 0; i < src->count; i++)
    {
        const ParamDecl* param = (const ParamDecl*)VecGet(src, i);

        if (param->isVarargRest && IsPackValueName(s, param->name))
        {
            for (size_t j = 0; j < s->packCount; j++)
            {
                VecPush(dst, PackParam(arena, param, s, j));
            }

            *outExpanded = true;
            continue;
        }

        VecPush(dst, AstCloneSubst(arena, (const Node*)param, s));
    }
}

// A node with no children and no sema-set state: a plain copy.
static Node* CloneLeaf(Arena* arena, const Node* n, size_t size)
{
    return (Node*)ArenaDup(arena, n, size);
}

static FieldDecl* CloneField(Arena* arena, const FieldDecl* src, const TypeSubst* s)
{
    FieldDecl* copy = AST_NEW(arena, FieldDecl);
    *copy = *src;

    copy->type = TypeNameSubstitute(arena, &src->type, s);
    copy->defaultValue = AstCloneSubst(arena, src->defaultValue, s);

    // Folded by sema.
    copy->folded = (FieldDefaultValue){0};

    return copy;
}

static PropertyDecl* CloneProperty(Arena* arena, const PropertyDecl* src, const TypeSubst* s)
{
    PropertyDecl* copy = AST_NEW(arena, PropertyDecl);
    *copy = *src;

    copy->returnType = TypeNameSubstitute(arena, &src->returnType, s);

    return copy;
}

static Node* CloneStruct(Arena* arena, const StructDecl* src, const TypeSubst* s)
{
    StructDecl* copy = AST_NEW(arena, StructDecl);
    *copy = *src;

    VecInit(&copy->fields);

    for (size_t i = 0; i < src->fields.count; i++)
    {
        VecPush(&copy->fields, CloneField(arena, (const FieldDecl*)VecGet(&src->fields, i), s));
    }

    VecInit(&copy->attributes);

    for (size_t i = 0; i < src->attributes.count; i++)
    {
        VecPush(&copy->attributes, ArenaDup(arena, VecGet(&src->attributes, i), sizeof(Attribute)));
    }

    CloneNameList(&copy->genericParams, &src->genericParams);

    // Bound by sema: the struct's drop function.
    copy->dropFunction = NULL;

    return (Node*)copy;
}

static Node* CloneEnumMember(Arena* arena, const EnumMemberDecl* src, const TypeSubst* s)
{
    EnumMemberDecl* copy = AST_NEW(arena, EnumMemberDecl);
    *copy = *src;

    copy->valueExpr = AstCloneSubst(arena, src->valueExpr, s);

    // Folded by sema.
    copy->value = 0;
    copy->isNegative = false;

    return (Node*)copy;
}

static Node* CloneEnum(Arena* arena, const EnumDecl* src, const TypeSubst* s)
{
    EnumDecl* copy = AST_NEW(arena, EnumDecl);
    *copy = *src;

    CloneNodeList(arena, &copy->members, &src->members, s);

    return (Node*)copy;
}

static Node* CloneImpl(Arena* arena, const ImplDecl* src, const TypeSubst* s)
{
    ImplDecl* copy = AST_NEW(arena, ImplDecl);
    *copy = *src;

    CloneNodeList(arena, &copy->methods, &src->methods, s);

    VecInit(&copy->properties);

    for (size_t i = 0; i < src->properties.count; i++)
    {
        VecPush(&copy->properties, CloneProperty(arena, (const PropertyDecl*)VecGet(&src->properties, i), s));
    }

    CloneNameList(&copy->genericParams, &src->genericParams);

    return (Node*)copy;
}

static Node* CloneFunction(Arena* arena, const FunctionDecl* src, const TypeSubst* s)
{
    FunctionDecl* copy = AST_NEW(arena, FunctionDecl);
    *copy = *src;

    copy->returnType = TypeNameSubstitute(arena, &src->returnType, s);

    bool expanded = false;
    CloneParams(arena, &copy->params, &src->params, s, &expanded);

    // A pack rest parameter became fixed parameters: the copy takes exactly those.
    if (expanded)
    {
        copy->isVariadic = false;
    }

    copy->body = AstCloneSubst(arena, src->body, s);
    CloneNameList(&copy->genericParams, &src->genericParams);

    // Set by sema when an `extern<T>` is instantiated.
    copy->genericOf = NULL;
    copy->typeArg = NULL;

    return (Node*)copy;
}

static Node* CloneParam(Arena* arena, const ParamDecl* src, const TypeSubst* s)
{
    ParamDecl* copy = AST_NEW(arena, ParamDecl);
    *copy = *src;

    copy->type = TypeNameSubstitute(arena, &src->type, s);

    // Set by sema.
    copy->growsArray = false;
    copy->inferredBorrow = false;
    copy->borrowCandidate = false;

    return (Node*)copy;
}

static Node* CloneBlock(Arena* arena, const Block* src, const TypeSubst* s)
{
    Block* copy = AST_NEW(arena, Block);
    *copy = *src;

    CloneNodeList(arena, &copy->statements, &src->statements, s);

    return (Node*)copy;
}

static Node* CloneReturn(Arena* arena, const ReturnStmt* src, const TypeSubst* s)
{
    ReturnStmt* copy = AST_NEW(arena, ReturnStmt);
    *copy = *src;

    copy->value = AstCloneSubst(arena, src->value, s);

    return (Node*)copy;
}

static Node* CloneIf(Arena* arena, const IfStmt* src, const TypeSubst* s)
{
    IfStmt* copy = AST_NEW(arena, IfStmt);
    *copy = *src;

    copy->condition = AstCloneSubst(arena, src->condition, s);
    copy->thenBranch = AstCloneSubst(arena, src->thenBranch, s);
    copy->elseBranch = AstCloneSubst(arena, src->elseBranch, s);

    return (Node*)copy;
}

static Node* CloneWhile(Arena* arena, const WhileStmt* src, const TypeSubst* s)
{
    TypeSubst storage;
    WhileStmt* copy = AST_NEW(arena, WhileStmt);
    *copy = *src;

    copy->condition = AstCloneSubst(arena, src->condition, s);
    copy->body = AstCloneSubst(arena, src->body, OrdinaryLoopSubst(s, &storage));

    return (Node*)copy;
}

// `for (T i = <literal>; i < args.length; i++)`: the shape a pack loop must have.
static bool IsPackLoopCondition(const Node* cond, const char* packValue)
{
    if (!cond || cond->kind != NodeBinary)
    {
        return false;
    }

    const BinaryExpr* e = (const BinaryExpr*)cond;

    if (e->op != BinLt || !e->rhs || e->rhs->kind != NodeMember)
    {
        return false;
    }

    const MemberExpr* len = (const MemberExpr*)e->rhs;

    return len->baseNode && len->baseNode->kind == NodeIdent
           && strcmp(((const IdentExpr*)len->baseNode)->name, packValue) == 0 && strcmp(len->member, "length") == 0;
}

static bool IsVarIdent(const Node* n, const char* var)
{
    return n && n->kind == NodeIdent && strcmp(((const IdentExpr*)n)->name, var) == 0;
}

static bool IsIntLiteralOne(const Node* n)
{
    return n && n->kind == NodeIntLiteral && ((const IntLiteral*)n)->value == 1;
}

// `i++`, `++i`, `i += 1` or `i = i + 1` on the loop variable `var`.
static bool IsPackLoopUpdate(const Node* u, const char* var)
{
    if (!u)
    {
        return false;
    }

    if (u->kind == NodeIncDec)
    {
        const IncDecExpr* e = (const IncDecExpr*)u;

        return !e->isDec && IsVarIdent(e->operand, var);
    }

    if (u->kind != NodeAssign)
    {
        return false;
    }

    const AssignExpr* e = (const AssignExpr*)u;

    if (!IsVarIdent(e->target, var))
    {
        return false;
    }

    if (e->op == AssignAdd)
    {
        return IsIntLiteralOne(e->value);
    }

    if (e->op == AssignSet && e->value && e->value->kind == NodeBinary)
    {
        const BinaryExpr* sum = (const BinaryExpr*)e->value;

        return sum->op == BinAdd && IsVarIdent(sum->lhs, var) && IsIntLiteralOne(sum->rhs);
    }

    return false;
}

// A `for` over a parameter pack is unrolled: a block holding, for each element, a block that declares the loop
// variable as a constant and holds one copy of the body. The body is cloned once per element with the loop
// variable bound, so `args[i]` becomes `args[<element>]`. An empty block when the loop has the wrong shape.
static Node* ClonePackLoop(Arena* arena, const ForStmt* src, const TypeSubst* s)
{
    Block* out = AST_NEW(arena, Block);
    out->base.kind = NodeBlock;
    out->base.range = src->base.range;
    VecInit(&out->statements);

    bool ok = src->init && src->init->kind == NodeVarDecl && ((const VarDeclStmt*)src->init)->init
              && ((const VarDeclStmt*)src->init)->init->kind == NodeIntLiteral;

    const VarDeclStmt* decl = ok ? (const VarDeclStmt*)src->init : NULL;
    const char* var = ok ? decl->name : NULL;

    ok = ok && IsVarIdent(((const BinaryExpr*)src->condition)->lhs, var) && IsPackLoopUpdate(src->update, var);

    if (!ok)
    {
        if (s->diag)
        {
            DiagErrorFmt(s->diag, src->base.range,
                         "a 'for' loop over a parameter pack must be written 'for (T i = 0; i < %s.length; i++)'",
                         s->packValue);
        }

        return (Node*)out;
    }

    size_t start = (size_t)((const IntLiteral*)decl->init)->value;

    for (size_t j = start; j < s->packCount; j++)
    {
        PackLoop loop = {var, j, s->loop};
        TypeSubst inner = *s;
        inner.loop = &loop;
        inner.inPackLoopBody = true;

        VarDeclStmt* iterDecl = AST_NEW(arena, VarDeclStmt);
        *iterDecl = *decl;

        if (decl->type.name)
        {
            iterDecl->type = TypeNameSubstitute(arena, &decl->type, s);
        }

        iterDecl->type.isConst = true;
        iterDecl->isRef = false;

        IntLiteral* value = AST_NEW(arena, IntLiteral);
        value->base.kind = NodeIntLiteral;
        value->base.range = decl->init->range;
        value->value = j;
        iterDecl->init = (Node*)value;

        Block* iter = AST_NEW(arena, Block);
        iter->base.kind = NodeBlock;
        iter->base.range = src->base.range;
        VecInit(&iter->statements);
        VecPush(&iter->statements, iterDecl);
        VecPush(&iter->statements, AstCloneSubst(arena, src->body, &inner));

        VecPush(&out->statements, iter);
    }

    return (Node*)out;
}

static Node* CloneFor(Arena* arena, const ForStmt* src, const TypeSubst* s)
{
    if (s && s->packValue && src->condition && IsPackLoopCondition(src->condition, s->packValue))
    {
        return ClonePackLoop(arena, src, s);
    }

    TypeSubst storage;
    const TypeSubst* bodySubst = OrdinaryLoopSubst(s, &storage);
    ForStmt* copy = AST_NEW(arena, ForStmt);
    *copy = *src;

    copy->init = AstCloneSubst(arena, src->init, s);
    copy->condition = AstCloneSubst(arena, src->condition, s);
    copy->update = AstCloneSubst(arena, src->update, s);
    copy->body = AstCloneSubst(arena, src->body, bodySubst);

    return (Node*)copy;
}

// `break` and `continue` directly inside a pack loop body: the loop is unrolled, so there is no loop to leave.
static Node* CloneJump(Arena* arena, const Node* n, size_t size, const char* keyword, const TypeSubst* s)
{
    if (s && s->inPackLoopBody && s->diag)
    {
        DiagErrorFmt(s->diag, n->range, "'%s' is not supported inside a 'for' loop over a parameter pack", keyword);
    }

    return CloneLeaf(arena, n, size);
}

static Node* CloneVarDecl(Arena* arena, const VarDeclStmt* src, const TypeSubst* s)
{
    VarDeclStmt* copy = AST_NEW(arena, VarDeclStmt);
    *copy = *src;

    // A `var` local has no type until sema infers one, so there is nothing to substitute yet.
    if (TypeNameValid(&src->type))
    {
        copy->type = TypeNameSubstitute(arena, &src->type, s);
    }

    copy->init = AstCloneSubst(arena, src->init, s);

    // Set by sema.
    copy->refThroughBox = false;

    return (Node*)copy;
}

static Node* CloneExprStmt(Arena* arena, const ExprStmt* src, const TypeSubst* s)
{
    ExprStmt* copy = AST_NEW(arena, ExprStmt);
    *copy = *src;

    copy->expr = AstCloneSubst(arena, src->expr, s);

    return (Node*)copy;
}

static Node* CloneUnary(Arena* arena, const UnaryExpr* src, const TypeSubst* s)
{
    UnaryExpr* copy = AST_NEW(arena, UnaryExpr);
    *copy = *src;

    copy->operand = AstCloneSubst(arena, src->operand, s);

    // Set by sema.
    copy->pointee = NULL;

    return (Node*)copy;
}

static Node* CloneBinary(Arena* arena, const BinaryExpr* src, const TypeSubst* s)
{
    BinaryExpr* copy = AST_NEW(arena, BinaryExpr);
    *copy = *src;

    copy->lhs = AstCloneSubst(arena, src->lhs, s);
    copy->rhs = AstCloneSubst(arena, src->rhs, s);

    return (Node*)copy;
}

static Node* CloneAssign(Arena* arena, const AssignExpr* src, const TypeSubst* s)
{
    AssignExpr* copy = AST_NEW(arena, AssignExpr);
    *copy = *src;

    copy->target = AstCloneSubst(arena, src->target, s);
    copy->value = AstCloneSubst(arena, src->value, s);

    // Set by sema.
    copy->isImplPropertyWrite = false;

    return (Node*)copy;
}

static Node* CloneCall(Arena* arena, const CallExpr* src, const TypeSubst* s)
{
    CallExpr* copy = AST_NEW(arena, CallExpr);
    *copy = *src;

    copy->typeArgs = SubstTypeArgs(arena, src->typeArgs, src->typeArgCount, s, &copy->typeArgCount);
    copy->typeArg = copy->typeArgCount == 1 ? &copy->typeArgs[0] : NULL;

    CloneCallArgs(arena, &copy->args, &src->args, s);
    copy->calleeBase = AstCloneSubst(arena, src->calleeBase, s);

    // Set by sema.
    copy->resolvedDecl = NULL;
    copy->addressOfFunction = NULL;
    copy->delegateCallee = NULL;
    copy->delegateDecl = NULL;

    return (Node*)copy;
}

static Node* CloneMember(Arena* arena, const MemberExpr* src, const TypeSubst* s)
{
    // `args.length` of the parameter pack: the number of its elements.
    if (src->baseNode && src->baseNode->kind == NodeIdent && IsPackValueName(s, ((const IdentExpr*)src->baseNode)->name)
        && strcmp(src->member, "length") == 0)
    {
        IntLiteral* count = AST_NEW(arena, IntLiteral);
        count->base.kind = NodeIntLiteral;
        count->base.range = src->base.range;
        count->value = s->packCount;
        count->fromPackLength = true;

        return (Node*)count;
    }

    MemberExpr* copy = AST_NEW(arena, MemberExpr);
    *copy = *src;

    copy->baseNode = AstCloneSubst(arena, src->baseNode, s);

    // Set by the move checker and by sema.
    copy->useKind = UseUnknown;
    copy->isImplProperty = false;
    copy->isEnumConst = false;
    copy->enumValue = 0;
    copy->enumTypeName = NULL;
    copy->isScalarConst = false;

    return (Node*)copy;
}

static Node* CloneStructInit(Arena* arena, const StructInitExpr* src, const TypeSubst* s)
{
    StructInitExpr* copy = AST_NEW(arena, StructInitExpr);
    *copy = *src;

    if (src->genericType)
    {
        copy->genericType = TypeNameWrapInner(arena, TypeNameSubstitute(arena, src->genericType, s));
        copy->typeName = copy->genericType->name;
    }
    else
    {
        // `T { ... }` naming a parameter, or a return-type initializer spelled with one.
        const TypeName* arg = SubstArg(s, src->typeName);

        if (arg)
        {
            copy->typeName = arg->name;
        }
    }

    VecInit(&copy->fields);

    for (size_t i = 0; i < src->fields.count; i++)
    {
        const StructInitField* field = (const StructInitField*)VecGet(&src->fields, i);
        StructInitField* fieldCopy = AST_NEW(arena, StructInitField);

        fieldCopy->name = field->name;
        fieldCopy->value = AstCloneSubst(arena, field->value, s);
        VecPush(&copy->fields, fieldCopy);
    }

    return (Node*)copy;
}

static Node* CloneIncDec(Arena* arena, const IncDecExpr* src, const TypeSubst* s)
{
    IncDecExpr* copy = AST_NEW(arena, IncDecExpr);
    *copy = *src;

    copy->operand = AstCloneSubst(arena, src->operand, s);

    return (Node*)copy;
}

static Node* CloneCast(Arena* arena, const CastExpr* src, const TypeSubst* s)
{
    CastExpr* copy = AST_NEW(arena, CastExpr);
    *copy = *src;

    copy->type = TypeNameSubstitute(arena, &src->type, s);
    copy->operand = AstCloneSubst(arena, src->operand, s);

    // Set by sema.
    copy->textKind = CastTextNone;

    return (Node*)copy;
}

static Node* CloneGlobal(Arena* arena, const GlobalDecl* src, const TypeSubst* s)
{
    GlobalDecl* copy = AST_NEW(arena, GlobalDecl);
    *copy = *src;

    copy->type = TypeNameSubstitute(arena, &src->type, s);
    copy->init = AstCloneSubst(arena, src->init, s);

    return (Node*)copy;
}

// `args[N]` of the pack value: element N, from a literal or from the variable of an enclosing pack loop.
static Node* ClonePackIndex(Arena* arena, const IndexExpr* src, const TypeSubst* s)
{
    const char* value = ((const IdentExpr*)src->baseNode)->name;
    const Node* index = src->index;

    if (index && index->kind == NodeIntLiteral)
    {
        // An element past the end is not reported here: the clone runs before an `if` on `args.length` is folded,
        // so the branch that guards it may be dropped. It stays `args[N]`, which sema reports if the branch is kept.
        size_t element = (size_t)((const IntLiteral*)index)->value;

        return PackElementIdent(arena, src->base.range, value, element);
    }

    if (index && index->kind == NodeIdent)
    {
        const char* var = ((const IdentExpr*)index)->name;

        for (const PackLoop* loop = s->loop; loop; loop = loop->outer)
        {
            if (strcmp(loop->var, var) == 0)
            {
                return PackElementIdent(arena, src->base.range, value, loop->index);
            }
        }
    }

    if (s->diag)
    {
        DiagErrorFmt(s->diag, src->base.range,
                     "a parameter pack can only be indexed by an integer literal or by the variable of a 'for' loop "
                     "over '%s.length'",
                     value);
    }

    return PackElementIdent(arena, src->base.range, value, 0);
}

static Node* CloneIndex(Arena* arena, const IndexExpr* src, const TypeSubst* s)
{
    if (src->baseNode && src->baseNode->kind == NodeIdent
        && IsPackValueName(s, ((const IdentExpr*)src->baseNode)->name))
    {
        return ClonePackIndex(arena, src, s);
    }

    IndexExpr* copy = AST_NEW(arena, IndexExpr);
    *copy = *src;

    copy->baseNode = AstCloneSubst(arena, src->baseNode, s);
    copy->index = AstCloneSubst(arena, src->index, s);

    // Set by the move checker.
    copy->useKind = UseUnknown;

    return (Node*)copy;
}

static Node* CloneArrayInit(Arena* arena, const ArrayInitExpr* src, const TypeSubst* s)
{
    ArrayInitExpr* copy = AST_NEW(arena, ArrayInitExpr);
    *copy = *src;

    if (src->elementType)
    {
        copy->elementType = TypeNameWrapInner(arena, TypeNameSubstitute(arena, src->elementType, s));
    }

    CloneNodeList(arena, &copy->elements, &src->elements, s);
    copy->repeatCount = AstCloneSubst(arena, src->repeatCount, s);

    return (Node*)copy;
}

static Node* CloneNullTest(Arena* arena, const NullTestExpr* src, const TypeSubst* s)
{
    NullTestExpr* copy = AST_NEW(arena, NullTestExpr);
    *copy = *src;

    copy->operand = AstCloneSubst(arena, src->operand, s);

    return (Node*)copy;
}

static Node* CloneDefer(Arena* arena, const DeferStmt* src, const TypeSubst* s)
{
    DeferStmt* copy = AST_NEW(arena, DeferStmt);
    *copy = *src;

    copy->stmt = AstCloneSubst(arena, src->stmt, s);

    return (Node*)copy;
}

static Node* CloneConditional(Arena* arena, const ConditionalExpr* src, const TypeSubst* s)
{
    ConditionalExpr* copy = AST_NEW(arena, ConditionalExpr);
    *copy = *src;

    copy->condition = AstCloneSubst(arena, src->condition, s);
    copy->thenExpr = AstCloneSubst(arena, src->thenExpr, s);
    copy->elseExpr = AstCloneSubst(arena, src->elseExpr, s);

    // Set by sema.
    copy->resultType = NULL;

    return (Node*)copy;
}

// A meta call's type arguments are substituted like any type (`@droppable(T)` inside a template).
static Node* CloneMetaCall(Arena* arena, const MetaCallExpr* src, const TypeSubst* s)
{
    MetaCallExpr* copy = AST_NEW(arena, MetaCallExpr);
    *copy = *src;

    VecInit(&copy->args);

    for (size_t i = 0; i < src->args.count; i++)
    {
        const MetaArg* arg = (const MetaArg*)VecGet(&src->args, i);
        MetaArg* argCopy = AST_NEW(arena, MetaArg);

        argCopy->type = arg->type ? TypeNameWrapInner(arena, TypeNameSubstitute(arena, arg->type, s)) : NULL;
        argCopy->expr = AstCloneSubst(arena, arg->expr, s);
        VecPush(&copy->args, argCopy);
    }

    return (Node*)copy;
}

// Expression identifiers that name a parameter (`T.max`, `sizeof(T)`) become the argument's name.
static Node* CloneIdent(Arena* arena, const IdentExpr* src, const TypeSubst* s)
{
    IdentExpr* copy = AST_NEW(arena, IdentExpr);
    *copy = *src;

    // A pack is only used through `args...`, `args.length` and `args[i]`, which are handled by their parents.
    if ((IsPackValueName(s, src->name) || IsPackTypeName(s, src->name)) && s->diag)
    {
        DiagErrorFmt(s->diag, src->base.range,
                     "parameter pack '%s' can only be used as 'args...', 'args.length' or 'args[i]'", src->name);
    }

    const TypeName* arg = SubstArg(s, src->name);

    if (arg)
    {
        copy->name = arg->name;
    }

    // Set by the move checker.
    copy->useKind = UseUnknown;

    return (Node*)copy;
}

Node* AstCloneSubst(Arena* arena, const Node* n, const TypeSubst* s)
{
    if (!n)
    {
        return NULL;
    }

    switch (n->kind)
    {
    case NodeModule:
        // Not cloned: a module is never part of a template. Reported below.
        break;
    case NodeImport:
        return CloneLeaf(arena, n, sizeof(ImportDecl));
    case NodeStruct:
        return CloneStruct(arena, (const StructDecl*)n, s);
    case NodeHandle:
        return CloneLeaf(arena, n, sizeof(HandleDecl));
    case NodeEnum:
        return CloneEnum(arena, (const EnumDecl*)n, s);
    case NodeEnumMember:
        return CloneEnumMember(arena, (const EnumMemberDecl*)n, s);
    case NodeImpl:
        return CloneImpl(arena, (const ImplDecl*)n, s);
    case NodeFunction:
        return CloneFunction(arena, (const FunctionDecl*)n, s);
    case NodeParam:
        return CloneParam(arena, (const ParamDecl*)n, s);
    case NodeBlock:
        return CloneBlock(arena, (const Block*)n, s);
    case NodeReturn:
        return CloneReturn(arena, (const ReturnStmt*)n, s);
    case NodeIf:
        return CloneIf(arena, (const IfStmt*)n, s);
    case NodeWhile:
        return CloneWhile(arena, (const WhileStmt*)n, s);
    case NodeFor:
        return CloneFor(arena, (const ForStmt*)n, s);
    case NodeVarDecl:
        return CloneVarDecl(arena, (const VarDeclStmt*)n, s);
    case NodeExprStmt:
        return CloneExprStmt(arena, (const ExprStmt*)n, s);
    case NodeBreak:
        return CloneJump(arena, n, sizeof(BreakStmt), "break", s);
    case NodeContinue:
        return CloneJump(arena, n, sizeof(ContinueStmt), "continue", s);
    case NodeIntLiteral:
        return CloneLeaf(arena, n, sizeof(IntLiteral));
    case NodeFloatLiteral:
        return CloneLeaf(arena, n, sizeof(FloatLiteral));
    case NodeBoolLiteral:
        return CloneLeaf(arena, n, sizeof(BoolLiteral));
    case NodeStrLiteral:
        return CloneLeaf(arena, n, sizeof(StrLiteral));
    case NodeIdent:
        return CloneIdent(arena, (const IdentExpr*)n, s);
    case NodeUnary:
        return CloneUnary(arena, (const UnaryExpr*)n, s);
    case NodeBinary:
        return CloneBinary(arena, (const BinaryExpr*)n, s);
    case NodeAssign:
        return CloneAssign(arena, (const AssignExpr*)n, s);
    case NodeCall:
        return CloneCall(arena, (const CallExpr*)n, s);
    case NodeMember:
        return CloneMember(arena, (const MemberExpr*)n, s);
    case NodeStructInit:
        return CloneStructInit(arena, (const StructInitExpr*)n, s);
    case NodeIncDec:
        return CloneIncDec(arena, (const IncDecExpr*)n, s);
    case NodeCast:
        return CloneCast(arena, (const CastExpr*)n, s);
    case NodeGlobal:
        return CloneGlobal(arena, (const GlobalDecl*)n, s);
    case NodeIndex:
        return CloneIndex(arena, (const IndexExpr*)n, s);
    case NodeArrayInit:
        return CloneArrayInit(arena, (const ArrayInitExpr*)n, s);
    case NodeNullTest:
        return CloneNullTest(arena, (const NullTestExpr*)n, s);
    case NodeDefer:
        return CloneDefer(arena, (const DeferStmt*)n, s);
    case NodeConditional:
        return CloneConditional(arena, (const ConditionalExpr*)n, s);
    case NodeMetaCall:
        return CloneMetaCall(arena, (const MetaCallExpr*)n, s);
    case NodePackExpand:
        // Only a call argument can splice a pack, and CloneCallArgs does that. Any other `name...` is copied
        // as written, and Monomorphize reports it.
        return CloneLeaf(arena, n, sizeof(PackExpandExpr));
    }

    fprintf(stderr, "AstCloneSubst: node kind %d has no clone\n", (int)n->kind);
    abort();
}
