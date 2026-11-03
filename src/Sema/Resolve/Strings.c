/* String builtins: substring resolution and string literal and concatenation rules. */
#include "Sema/Resolve/ResolverInternal.h"

/* Result type of `substring(s, start, len)` — always string, or NULL if not
   a substring call. An optional `string?` source unwraps here; the resolve
   half still demands the narrowing fact via CheckOptionalDeref. */
const TypeName* SubstringBuiltinType(Resolver* r, CallExpr* c, StrMap* scope)
{
    if (!c->callee || strcmp(c->callee, "substring") != 0)
    {
        return NULL;
    }
    if (c->args.count != 3)
    {
        return NULL;
    }
    Node* arg0 = (Node*)VecGet(&c->args, 0);
    const TypeName* argType = InferType(r, arg0, scope);

    if (argType && TypeNameIsOptional(argType))
    {
        argType = argType->inner;
    }

    if (!argType || !TypeIsString(&r->registry, argType->name))
    {
        return NULL;
    }
    return InternTypeName(r, "string");
}

static bool SubstringIndexTypeOk(Resolver* r, const TypeName* t)
{
    if (!t || !t->name)
    {
        return false;
    }

    const char* resolved = TypeRegistryResolveAlias(&r->registry, t->name);

    return IsNameNumeric(resolved) && !IsNameFloatType(resolved) && !IsBoolType(&r->registry, t);
}

bool ResolveSubstringBuiltin(Resolver* r, CallExpr* c, StrMap* scope)
{
    if (!c->callee || strcmp(c->callee, "substring") != 0)
    {
        return false;
    }

    if (c->args.count != 3)
    {
        DiagErrorFmt(r->diag, c->base.range, "'substring' expects 3 arguments (string, start, len), got %zu",
                     c->args.count);
        return true;
    }

    Node* arg0 = (Node*)VecGet(&c->args, 0);
    const TypeName* argType = InferType(r, arg0, scope);
    const TypeName* unwrapped = argType && TypeNameIsOptional(argType) ? argType->inner : argType;

    if (!unwrapped || !TypeIsString(&r->registry, unwrapped->name))
    {
        DiagErrorFmt(r->diag, arg0->range, "'substring' expects a string argument, not '%s'",
                     argType ? argType->name : "");
        return true;
    }

    /* Reading through a `string?` needs the narrowing fact, like any
       other optional read. */

    const char* indexNames[2] = {"start", "len"};

    for (int i = 1; i <= 2; i++)
    {
        Node* arg = (Node*)VecGet(&c->args, (size_t)i);
        const TypeName* indexType = InferType(r, arg, scope);

        if (!SubstringIndexTypeOk(r, indexType))
        {
            DiagErrorFmt(r->diag, arg->range, "'substring' %s must be an integer, not '%s'", indexNames[i - 1],
                         indexType ? indexType->name : "");
            return true;
        }
    }

    c->isIntrinsicCall = true;
    return true;
}

/* `a + b` where both sides are a string, cstring or string literal: a new owned string. */
bool IsStringConcat(Resolver* r, BinaryOp op, const TypeName* lt, const TypeName* rt)
{
    if (op != BinAdd || !lt || !rt || TypeNameIsOptional(lt) || TypeNameIsOptional(rt))
    {
        return false;
    }

    bool lStr = TypeIsString(&r->registry, lt->name) || TypeIsCString(&r->registry, lt->name);
    bool rStr = TypeIsString(&r->registry, rt->name) || TypeIsCString(&r->registry, rt->name);

    return lStr && rStr;
}

/* A string literal contextually coerces to `cstring` (a `.rodata` pointer).
   A `string`-typed value never converts to `cstring` implicitly. */
bool IsStrLiteralForCString(const Resolver* r, const TypeName* targetType, const Node* valueNode)
{
    return targetType && valueNode && valueNode->kind == NodeStrLiteral
           && TypeIsCString(&r->registry, targetType->name);
}
