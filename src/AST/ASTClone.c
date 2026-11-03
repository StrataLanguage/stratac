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

// A new arena array of `count` type arguments, each substituted.
static TypeName* SubstTypeArgs(Arena* arena, const TypeName* args, size_t count, const TypeSubst* s)
{
    if (count == 0)
    {
        return NULL;
    }

    TypeName* copy = (TypeName*)ArenaAlloc(arena, count * sizeof(TypeName));

    for (size_t i = 0; i < count; i++)
    {
        copy[i] = TypeNameSubstitute(arena, &args[i], s);
    }

    return copy;
}

TypeName TypeNameSubstitute(Arena* arena, const TypeName* t, const TypeSubst* s)
{
    // `number<T>`: the base stays, and the canonical name is rebuilt from the substituted arguments.
    if (t->typeArgCount > 0)
    {
        TypeName result = *t;
        result.typeArgs = SubstTypeArgs(arena, t->typeArgs, t->typeArgCount, s);
        result.name = TypeNameGenericSpelling(arena, t->genericBase, result.typeArgs, t->typeArgCount);
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
    CloneNodeList(arena, &copy->params, &src->params, s);
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
    WhileStmt* copy = AST_NEW(arena, WhileStmt);
    *copy = *src;

    copy->condition = AstCloneSubst(arena, src->condition, s);
    copy->body = AstCloneSubst(arena, src->body, s);

    return (Node*)copy;
}

static Node* CloneFor(Arena* arena, const ForStmt* src, const TypeSubst* s)
{
    ForStmt* copy = AST_NEW(arena, ForStmt);
    *copy = *src;

    copy->init = AstCloneSubst(arena, src->init, s);
    copy->condition = AstCloneSubst(arena, src->condition, s);
    copy->update = AstCloneSubst(arena, src->update, s);
    copy->body = AstCloneSubst(arena, src->body, s);

    return (Node*)copy;
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

    copy->typeArgs = SubstTypeArgs(arena, src->typeArgs, src->typeArgCount, s);
    copy->typeArg = copy->typeArgCount == 1 ? &copy->typeArgs[0] : NULL;

    CloneNodeList(arena, &copy->args, &src->args, s);
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

static Node* CloneIndex(Arena* arena, const IndexExpr* src, const TypeSubst* s)
{
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
        return CloneLeaf(arena, n, sizeof(BreakStmt));
    case NodeContinue:
        return CloneLeaf(arena, n, sizeof(ContinueStmt));
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
    }

    fprintf(stderr, "AstCloneSubst: node kind %d has no clone\n", (int)n->kind);
    abort();
}
