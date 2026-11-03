// Raw memory intrinsics and the copy/drop builtins.
#include "Sema/Resolve/ResolverInternal.h"

// -- copy() and drop()

// Result type of `copy(arg)`, or NULL if not a copy call.
const TypeName* CopyBuiltinType(Resolver* r, CallExpr* c, StrMap* scope)
{
    if (!c->callee || strcmp(c->callee, "copy") != 0)
    {
        return NULL;
    }
    if (c->args.count != 1)
    {
        return NULL;
    }
    Node* arg0 = (Node*)VecGet(&c->args, 0);
    return InferType(r, arg0, scope);
}

bool ResolveCopyBuiltin(Resolver* r, CallExpr* c, StrMap* scope)
{
    if (!c->callee || strcmp(c->callee, "copy") != 0)
    {
        return false;
    }

    if (c->args.count != 1)
    {
        DiagErrorFmt(r->diag, c->base.range, "'copy' expects 1 argument, got %zu", c->args.count);
        return true;
    }

    Node* arg0 = (Node*)VecGet(&c->args, 0);
    const TypeName* argType = InferType(r, arg0, scope);

    if (!argType || !AliasIsOwningValue(r, argType))
    {
        DiagErrorFmt(r->diag, arg0->range, "'copy' expects an owning type (string, ^T, T[]) — not '%s'",
                     argType ? argType->name : "");
        return true;
    }

    {
        const TypeName* pointee = TypeNameBoxInner(argType) ? TypeNameBoxInner(argType) : argType;

        if (IsIncompleteStruct(&r->registry, pointee->name))
        {
            DiagErrorFmt(r->diag, arg0->range, "cannot copy incomplete type '%s'; its contents are unknown",
                         pointee->name);
            return true;
        }
    }

    if (TypeNameHasDrop(&r->registry, argType))
    {
        DiagErrorFmt(r->diag, arg0->range,
                     "cannot copy '%s': it holds a value with a Drop method, which would then be dropped twice",
                     argType->name);
        return true;
    }

    c->isIntrinsicCall = true;
    return true;
}

// Result type of `drop(arg)` - always void (NULL TypeName).
const TypeName* DropBuiltinType(Resolver* r, CallExpr* c, StrMap* scope)
{
    (void)r;
    (void)c;
    (void)scope;
    return NULL;
}

bool ResolveDropBuiltin(Resolver* r, CallExpr* c, StrMap* scope)
{
    if (!c->callee || strcmp(c->callee, "drop") != 0)
    {
        return false;
    }

    if (c->args.count != 1)
    {
        DiagErrorFmt(r->diag, c->base.range, "'drop' expects 1 argument, got %zu", c->args.count);
        return true;
    }

    Node* arg0 = (Node*)VecGet(&c->args, 0);
    const TypeName* argType = InferType(r, arg0, scope);

    if (!argType || !AliasIsOwningValue(r, argType))
    {
        DiagErrorFmt(r->diag, arg0->range, "'drop' expects an owning type (string, ^T, T[]) — not '%s'",
                     argType ? argType->name : "");
        return true;
    }

    // Dropping frees the value: a `const` binding must stay intact.
    const char* constName = ConstRootName(r, arg0, scope);

    if (constName)
    {
        DiagErrorFmt(r->diag, arg0->range, "cannot drop '%s'; it is immutable", constName);
        return true;
    }

    c->isIntrinsicCall = true;

    // Move the source — the box is invalidated after drop(). An optional
    // source is left definitely EMPTY (legal to test later), never poisoned,
    // like any other move out of a T?.
    const char* movedKey = MovableBoxSourceKey(r, arg0);
    if (movedKey)
    {
        if (TypeNameIsOptional(argType))
        {
            EmptyOptionalSource(r, movedKey);
        }
        else
        {
            MarkMoved(r, movedKey, arg0->range);
        }
    }

    return true;
}

// -- Memory intrinsics: raw `ptr` access for FFI.

// `sizeof<T>()`, `alignof<T>()`, `load<T>(p)`, `store<T>(p, v)`: intrinsics that take a type argument.
bool IsTypedMemoryIntrinsic(Resolver* r, const CallExpr* c)
{
    static const char* const names[] = {"sizeof", "alignof", "load", "store"};

    for (size_t i = 0; i < ARRAY_COUNT(names); i++)
    {
        if (strcmp(c->callee, names[i]) == 0)
        {
            return !ModuleHasFunctionNamed(r, c->callee, c->base.range.fileId);
        }
    }

    return false;
}

// The only function named `name` visible from `fileId`; NULL (with *ambiguous set) when overloaded.
FunctionDecl* FindSingleFunction(Resolver* r, const char* name, uint16_t fileId, bool* ambiguous)
{
    FunctionDecl* found = NULL;
    *ambiguous = false;

    for (size_t i = 0; i < r->mod->functions.count; i++)
    {
        FunctionDecl* f = (FunctionDecl*)VecGet(&r->mod->functions, i);

        if (strcmp(f->name, name) != 0 || !FunctionVisibleTo(f, fileId))
        {
            continue;
        }

        if (found)
        {
            *ambiguous = true;
            return NULL;
        }

        found = f;
    }

    return found;
}

// `addressof(f)` names a function, not a variable: a bare identifier that is no local/global.
bool IsAddressOfFunctionArg(Resolver* r, const CallExpr* c, StrMap* scope)
{
    if (!c->callee || strcmp(c->callee, "addressof") != 0 || c->args.count != 1)
    {
        return false;
    }

    const Node* arg = (const Node*)VecGet((Vec*)&c->args, 0);

    if (arg->kind != NodeIdent || StrMapGet(scope, ((const IdentExpr*)arg)->name))
    {
        return false;
    }

    return ModuleHasFunctionNamed(r, ((const IdentExpr*)arg)->name, c->base.range.fileId);
}

// A `T` that load/store/sizeof may move as raw bytes: no owning parts.
static bool CheckPlainDataTypeArg(Resolver* r, const CallExpr* c, const TypeName* t)
{
    if (!t || !t->name)
    {
        DiagErrorFmt(r->diag, c->base.range, "'%s' needs a type argument (e.g. '%s<int>(...)')", c->callee, c->callee);
        return false;
    }

    if (AliasIsOwningValue(r, t)
        || TypeRegistryIsOwningStruct(&r->registry, TypeRegistryResolveAlias(&r->registry, t->name)))
    {
        DiagErrorFmt(r->diag, c->base.range, "'%s' cannot move owning type '%s' as raw memory", c->callee, t->name);
        return false;
    }

    if (TypeRegistryIsOpaque(&r->registry, t->name) && !IsHandleType(&r->registry, t->name))
    {
        DiagErrorFmt(r->diag, c->base.range, "'%s' cannot use incomplete type '%s'", c->callee, t->name);
        return false;
    }

    if (GetPrimitiveType(t->name) == PrimVoid)
    {
        DiagErrorFmt(r->diag, c->base.range, "'%s' cannot use 'void'", c->callee);
        return false;
    }

    return true;
}

// A pointer operand: any handle (they all convert to `ptr`).
static bool CheckPtrOperand(Resolver* r, const CallExpr* c, Node* arg, StrMap* scope)
{
    const TypeName* t = InferType(r, arg, scope);

    if (!t || !IsHandleType(&r->registry, t->name))
    {
        DiagErrorFmt(r->diag, arg->range, "'%s' expects a 'ptr' (or other handle), not '%s'", c->callee,
                     t ? t->name : "");
        return false;
    }

    return true;
}

const TypeName* MemoryBuiltinType(Resolver* r, CallExpr* c, StrMap* scope)
{
    (void)scope;

    if (!c->callee)
    {
        return NULL;
    }

    if (strcmp(c->callee, "addressof") == 0)
    {
        return InternTypeName(r, STRATA_PTR_TYPE);
    }

    if (strcmp(c->callee, "sizeof") == 0 || strcmp(c->callee, "alignof") == 0)
    {
        return InternTypeName(r, "ulong");
    }

    if (strcmp(c->callee, "load") == 0)
    {
        return c->typeArg;
    }

    return NULL; // store: void
}

static bool ResolveAddressOfBuiltin(Resolver* r, CallExpr* c, StrMap* scope)
{
    if (c->args.count != 1 || c->typeArg)
    {
        DiagErrorFmt(r->diag, c->base.range, "'addressof' expects 1 argument, got %zu", c->args.count);
        return true;
    }

    Node* arg0 = (Node*)VecGet(&c->args, 0);

    if (IsAddressOfFunctionArg(r, c, scope))
    {
        bool ambiguous = false;
        const char* fnName = ((IdentExpr*)arg0)->name;
        FunctionDecl* fn = FindSingleFunction(r, fnName, c->base.range.fileId, &ambiguous);

        if (!fn)
        {
            DiagErrorFmt(r->diag, arg0->range, "cannot take the address of overloaded function '%s'", fnName);
            return true;
        }

        if ((!fn->isExtern && !fn->isExport) || fn->typeParam)
        {
            DiagErrorFmt(r->diag, arg0->range,
                         "cannot take the address of '%s': only extern and '@export' functions have a C ABI", fnName);
            return true;
        }

        c->addressOfFunction = fn;
        c->isIntrinsicCall = true;
        return true;
    }

    while (arg0->kind == NodeCast)
    {
        arg0 = ((CastExpr*)arg0)->operand;
    }

    if (!IsLValueNode(arg0))
    {
        DiagError(r->diag, arg0->range, "'addressof' expects a variable, field, element or function");
        return true;
    }

    if (arg0->kind == NodeMember
        && IsFatPseudoProperty(r, InferType(r, ((MemberExpr*)arg0)->baseNode, scope), ((MemberExpr*)arg0)->member))
    {
        DiagErrorFmt(r->diag, arg0->range, "cannot take the address of '.%s'", ((MemberExpr*)arg0)->member);
        return true;
    }

    c->isIntrinsicCall = true;
    return true;
}

static bool ResolveSizeofBuiltin(Resolver* r, CallExpr* c, const char* name)
{
    if (c->args.count != 0)
    {
        DiagErrorFmt(r->diag, c->base.range, "'%s' takes no arguments: write '%s<T>()'", name, name);
        return true;
    }

    if (c->typeArg && TypeNameIsDynamicArray(c->typeArg))
    {
        DiagErrorFmt(r->diag, c->base.range, "'%s' of a dynamic array is not a fixed size", name);
        return true;
    }

    if (!c->typeArg)
    {
        CheckPlainDataTypeArg(r, c, NULL);
        return true;
    }

    const TypeName* t = c->typeArg;
    bool known = t->primitiveType != PrimNone || TypeNameHasArrayShape(t) || TypeNameIsBox(t) || TypeNameIsOptional(t)
                 || TypeRegistryIsUserType(&r->registry, t->name);

    if (t->primitiveType == PrimVoid || !known)
    {
        DiagErrorFmt(r->diag, c->base.range, "unknown type '%s'", c->typeArg->name);
        return true;
    }

    if (!TypeNameIsBox(t) && !TypeNameIsOptional(t) && IsIncompleteStruct(&r->registry, t->name))
    {
        DiagErrorFmt(r->diag, c->base.range, "cannot take the %s of incomplete type '%s'; its size is unknown", name,
                     t->name);
        return true;
    }

    c->isIntrinsicCall = true;
    return true;
}

static bool ResolveLoadBuiltin(Resolver* r, CallExpr* c, StrMap* scope)
{
    if (c->args.count != 1)
    {
        DiagErrorFmt(r->diag, c->base.range, "'load' expects 1 argument, got %zu", c->args.count);
        return true;
    }

    if (!CheckPlainDataTypeArg(r, c, c->typeArg) || !CheckPtrOperand(r, c, (Node*)VecGet(&c->args, 0), scope))
    {
        return true;
    }

    c->isIntrinsicCall = true;
    return true;
}

static bool ResolveStoreBuiltin(Resolver* r, CallExpr* c, StrMap* scope)
{
    if (c->args.count != 2)
    {
        DiagErrorFmt(r->diag, c->base.range, "'store' expects 2 arguments (ptr, value), got %zu", c->args.count);
        return true;
    }

    Node* value = (Node*)VecGet(&c->args, 1);
    const TypeName* valueType = InferType(r, value, scope);
    const TypeName* t = c->typeArg ? c->typeArg : valueType;

    if (!CheckPlainDataTypeArg(r, c, t) || !CheckPtrOperand(r, c, (Node*)VecGet(&c->args, 0), scope))
    {
        return true;
    }

    if (c->typeArg && valueType && !IsAssignableType(r, c->typeArg, valueType))
    {
        DiagErrorFmt(r->diag, value->range, "cannot store '%s' as '%s'", valueType->name, c->typeArg->name);
        return true;
    }

    if (!c->typeArg)
    {
        c->typeArg = (TypeName*)ArenaDup(r->arena, valueType, sizeof(TypeName));
    }

    c->isIntrinsicCall = true;
    return true;
}

// addressof(x) -> ptr        the address of a variable, field or element (a box: its cell), or of an
// extern / `@export` function
// sizeof<T>() / alignof<T>() -> ulong
// load<T>(p) -> T            reads a T at p
// store(p, v) / store<T>(p, v) writes v (as T) at p
bool ResolveMemoryBuiltin(Resolver* r, CallExpr* c, StrMap* scope)
{
    // These names are not reserved: a module's own function of the same name wins.
    if (!c->callee || ModuleHasFunctionNamed(r, c->callee, c->base.range.fileId))
    {
        return false;
    }

    const char* name = c->callee;

    if (strcmp(name, "addressof") == 0)
    {
        return ResolveAddressOfBuiltin(r, c, scope);
    }

    if (strcmp(name, "sizeof") == 0 || strcmp(name, "alignof") == 0)
    {
        return ResolveSizeofBuiltin(r, c, name);
    }

    if (strcmp(name, "load") == 0)
    {
        return ResolveLoadBuiltin(r, c, scope);
    }

    if (strcmp(name, "store") == 0)
    {
        return ResolveStoreBuiltin(r, c, scope);
    }

    return false;
}
