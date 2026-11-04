// Fixed-array initializers and array builtin resolution.
#include "Sema/Resolve/ResolverInternal.h"

// True if any fixed-size array dimension (`T[N]`) appears in the type tree (even inside a box).
bool TypeTreeHasFixedArray(const TypeName* t)
{
    for (; t; t = (TypeNameIsBox(t) || TypeNameIsOptional(t)) ? t->inner : (TypeNameHasArrayShape(t) ? t->elem : NULL))
    {
        if (TypeNameHasArrayShape(t) && t->length >= 0)
        {
            return true;
        }
    }

    return false;
}

// Returns the innermost non-fixed node and the product of leading fixed dimensions (1 if none).
const TypeName* FixedArrayLeaf(const TypeName* t, ArrayLength* totalCount)
{
    ArrayLength count = 1;

    while (t && TypeNameHasArrayShape(t) && t->length >= 0)
    {
        count *= t->length;
        t = t->elem;
    }

    *totalCount = count;
    return t;
}

// True when `member` on `base` is one of the fat pseudo-properties: dynamic
// arrays and strings carry `.length` and `.cap`; fixed arrays expose only a
// compile-time `.length`. `cstring` exposes `.length` (strlen) and `.ptr` (its bytes), no `.cap`.
// All are read-only views of runtime state.
bool IsFatPseudoProperty(const Resolver* r, const TypeName* base, const char* member)
{
    bool isData = strcmp(member, "data") == 0;
    bool isPtr = strcmp(member, "ptr") == 0;

    if (strcmp(member, "length") != 0 && strcmp(member, "cap") != 0 && !isData && !isPtr)
    {
        return false;
    }

    base = UnwrapBoxPtr(base);

    if (!base)
    {
        return false;
    }

    // `.data`: the element buffer of a dynamic array or string, as a `ptr` (for FFI).
    if (isData)
    {
        return TypeNameIsDynamicArray(base) || TypeIsString(&r->registry, base->name);
    }

    // `.ptr`: a cstring's NUL-terminated bytes, as a `ptr`; a string's element buffer, as a `*byte`.
    if (isPtr)
    {
        return TypeIsCString(&r->registry, base->name) || TypeIsString(&r->registry, base->name);
    }

    if (TypeNameHasArrayShape(base) && base->length >= 0)
    {
        return strcmp(member, "length") == 0;
    }

    if (TypeIsCString(&r->registry, base->name))
    {
        return strcmp(member, "length") == 0;
    }

    return TypeNameIsDynamicArray(base) || TypeIsString(&r->registry, base->name);
}

// Fill `flat` with fixed-array init leaves in row-major order.
bool PlaceFixedArrayInit(Vec* flat, size_t base, const TypeName* t, ArrayInitExpr* ai)
{
    const TypeName* rowType = t->elem;
    ArrayLength innerTotal = 0;

    FixedArrayLeaf(rowType, &innerTotal);

    size_t slot = 0;

    for (size_t k = 0; k < ai->elements.count; k++)
    {
        Node* elem = (Node*)VecGet(&ai->elements, k);

        if (slot >= (size_t)t->length)
        {
            return false;
        }

        if (rowType && TypeNameHasArrayShape(rowType) && rowType->length >= 0)
        {
            if (!PlaceFixedArrayInit(flat, base + slot * (size_t)innerTotal, rowType, (ArrayInitExpr*)elem))
            {
                return false;
            }
        }
        else
        {
            flat->items[base + slot] = elem;
        }

        slot++;
    }

    return true;
}

// Verifies a fixed-array initializer's SHAPE: nested rows for multidimensional fields, a flat list for
// single-dimension. Returns false (with a diagnostic) on mismatch. `kind` is "field" or "local" for messages.
bool CheckFixedArrayInitShape(Resolver* r, const TypeName* t, ArrayInitExpr* ai, const TypeName* leaf, const char* name,
                              const char* kind)
{
    bool multidim = t->elem && TypeNameHasArrayShape(t->elem) && t->elem->length >= 0;
    bool leafIsStruct
        = leaf && TypeRegistryIsUserType(&r->registry, leaf->name) && !TypeRegistryIsOpaque(&r->registry, leaf->name);

    for (size_t k = 0; k < ai->elements.count; k++)
    {
        Node* elem = (Node*)VecGet(&ai->elements, k);

        if (multidim && elem->kind != NodeArrayInit)
        {
            DiagErrorFmt(r->diag, elem->range,
                         "multidimensional fixed-size array %s '%s' requires nested rows "
                         "('{ { ... }, { ... } }'), not a flat element list",
                         kind, name);
            return false;
        }

        if (!multidim && elem->kind == NodeArrayInit && !leafIsStruct)
        {
            DiagErrorFmt(r->diag, elem->range,
                         "fixed-size array %s '%s' has a single dimension - write its elements as a flat list", kind,
                         name);
            return false;
        }

        if (multidim && !CheckFixedArrayInitShape(r, t->elem, (ArrayInitExpr*)elem, leaf, name, kind))
        {
            return false;
        }
    }

    return true;
}

// True when any element of a fixed-array initializer is itself a braced
// row (`NodeArrayInit`); NULL holes are skipped.
bool HasNestedArrayInit(const ArrayInitExpr* ai)
{
    for (size_t k = 0; k < ai->elements.count; k++)
    {
        Node* elem = (Node*)VecGet(&ai->elements, k);

        if (elem && elem->kind == NodeArrayInit)
        {
            return true;
        }
    }

    return false;
}

// A fixed array passed where an extern takes a `ptr` decays to its first element, as in C:
// `snprintf(buffer, ...)` with `byte[32] buffer`. The argument is wrapped in that `(ptr)` cast.
void DecayFixedArgsToPtr(Resolver* r, CallExpr* c, StrMap* scope)
{
    if (!c->callee || c->calleeBase)
    {
        return;
    }

    bool ambiguous = false;
    FunctionDecl* fd = FindSingleFunction(r, c->callee, c->base.range.fileId, &ambiguous);

    if (!fd || !fd->isExtern || fd->isDelegate)
    {
        return;
    }

    for (size_t i = 0; i < c->args.count && i < fd->params.count; i++)
    {
        const ParamDecl* param = (const ParamDecl*)VecGet(&fd->params, i);
        const char* paramType = TypeRegistryResolveAlias(&r->registry, param->type.name);

        if (param->mod != ModNone || !paramType || strcmp(paramType, STRATA_PTR_TYPE) != 0)
        {
            continue;
        }

        Node* arg = (Node*)VecGet(&c->args, i);
        const TypeName* argType = InferType(r, arg, scope);

        if (!argType || !TypeNameIsFixedArray(argType))
        {
            continue;
        }

        CastExpr* cast = AST_NEW(r->arena, CastExpr);
        cast->base.kind = NodeCast;
        cast->base.range = arg->range;
        cast->type = *InternTypeName(r, STRATA_PTR_TYPE);
        cast->operand = arg;
        cast->textKind = CastFixedToPtr;
        VecSet(&c->args, i, cast);
    }
}

// Result type of array_push/pop/resize/remove, or NULL if not one.
const TypeName* ArrayBuiltinType(Resolver* r, CallExpr* c, StrMap* scope)
{
    if (!c->callee)
    {
        return NULL;
    }

    bool isPush = strcmp(c->callee, "array_push") == 0;
    bool isResize = strcmp(c->callee, "array_resize") == 0 || (ArrayBuiltinFlagsOf(c->callee) & ABF_REMOVES)
                    || strcmp(c->callee, "array_reserve") == 0;
    bool isPop = strcmp(c->callee, "array_pop") == 0;
    bool isReplace = strcmp(c->callee, "array_replace") == 0;
    bool isTake = strcmp(c->callee, "array_take") == 0;

    if (!isPush && !isResize && !isPop && !isReplace && !isTake)
    {
        return NULL;
    }

    Node* arg0 = c->args.count > 0 ? (Node*)VecGet(&c->args, 0) : NULL;
    const TypeName* arrType = arg0 ? InferType(r, arg0, scope) : NULL;

    // arg0 must be an array for this to be a (valid) builtin call.
    const TypeName* elem = arrType ? TypeNameArrayElem(arrType) : NULL;

    if (!elem)
    {
        return NULL;
    }

    // Pop, remove, replace and take hand the element to the caller.
    if (isPop || isReplace || isTake || (ArrayBuiltinFlagsOf(c->callee) & ABF_REMOVES))
    {
        return elem;
    }

    return InternTypeName(r, isPush ? "uint" : "void");
}

// An EMPTY braced value for a `T?` target is the canonical empty optional
// (zero-fill), not a T value missing its owning fields.
bool IsEmptyBracedForOptional(const Node* n, const TypeName* target)
{
    bool empty = (n->kind == NodeArrayInit && ((const ArrayInitExpr*)n)->elements.count == 0)
                 || (n->kind == NodeStructInit && ((const StructInitExpr*)n)->fields.count == 0);

    return empty && target && TypeNameIsOptional(target);
}

// True for `arr[i]` reads (casts unwrapped); the array keeps ownership.
bool IsArrayElementBorrow(Node* n)
{
    while (n && n->kind == NodeCast)
    {
        n = ((CastExpr*)n)->operand;
    }

    return n && n->kind == NodeIndex;
}

static void CheckArrayIndexArg(Resolver* r, CallExpr* c, StrMap* scope, const char* what)
{
    Node* arg1 = (Node*)VecGet(&c->args, 1);

    const TypeName* sizeType = InferType(r, arg1, scope);

    if (sizeType && (!IsNumeric(sizeType->primitiveType) || sizeType->primitiveType == PrimBool))
    {
        DiagErrorFmt(r->diag, arg1->range, "'%s' %s must be an integer, not '%s'", c->callee, what, sizeType->name);
    }
}

static void CheckArrayValueArg(Resolver* r, CallExpr* c, StrMap* scope, const TypeName* arrType,
                               const TypeName* unwrapped, bool isReplace)
{
    size_t valueIdx = isReplace ? 2 : 1;
    Node* arg1 = (Node*)VecGet(&c->args, valueIdx);

    const TypeName* elemType = TypeNameArrayElem(unwrapped);

    // A bare `{...}` value takes the element's shape here — the literal
    // would otherwise never be typed: no field checks run, and codegen
    // zero-fills it (a null `^T` owning field crashes at first deref).
    bool bracedValue = (arg1->kind == NodeArrayInit && !((ArrayInitExpr*)arg1)->elementType)
                       || (arg1->kind == NodeStructInit && !((StructInitExpr*)arg1)->typeName);

    Node* typedArg = ApplyBracedStructTarget(r, arg1, elemType);

    if (typedArg != arg1)
    {
        VecSet(&c->args, valueIdx, typedArg);
    }

    if (bracedValue && typedArg->kind == NodeStructInit && ((StructInitExpr*)typedArg)->typeName
        && !IsEmptyBracedForOptional(typedArg, elemType))
    {
        CheckStructInitFields(r, typedArg);
    }

    if (!bracedValue && elemType)
    {
        VecSet(&c->args, valueIdx, typedArg);
    }

    const TypeName* valueType = InferType(r, typedArg, scope);

    if (valueType && elemType && !IsAssignableType(r, elemType, valueType)
        && !IsStrLiteralForCString(r, elemType, typedArg))
    {
        // A proven `T?` pushed into `^T[]` moves (source emptied).
        const TypeName* vi = TypeNameBoxInner(valueType);
        const TypeName* ei = TypeNameBoxInner(elemType);

        bool narrowedOptIntoBox
            = TypeNameIsOptional(valueType) && TypeNameIsBox(elemType) && vi && ei && strcmp(vi->name, ei->name) == 0;

        if (!narrowedOptIntoBox)
        {
            DiagErrorFmt(r->diag, arg1->range, "cannot %s a value of type '%s' into '%s' (element type '%s')",
                         isReplace ? "store" : "push", valueType->name, arrType->name, elemType->name);
        }
    }

    // Pushing a borrow out of an array element would duplicate ownership; move it to a local first.
    if (valueType && AliasIsOwningValue(r, valueType) && IsArrayElementBorrow(arg1))
    {
        DiagErrorFmt(r->diag, arg1->range,
                     "cannot push '%s' read from an array element - it would be owned by two arrays; "
                     "move it into a variable first, or push a copy() of it",
                     valueType->name);
    }

    // Deref check before the move tracking below clears the pushed optional's fact.
    CheckCallArgOptionalDerefs(r, c, scope);

    // Push moves owning values in (boxes/strings/arrays AND owning
    // structs — a `Rec` value pushed into `Rec[]` transfers ownership);
    // optional sources end up empty.
    if (valueType && AliasIsOwningValue(r, valueType))
    {
        const char* movedKey = MovableBoxSourceKey(r, arg1);

        if (movedKey)
        {
            if (TypeNameIsOptional(valueType))
            {
                MoveOptionalSource(r, movedKey, c->base.range);
            }
            else
            {
                MarkMoved(r, movedKey, c->base.range);
            }
        }
    }
}

static void ResolveArrayPush(Resolver* r, CallExpr* c, StrMap* scope, const TypeName* arrType,
                             const TypeName* unwrapped)
{
    CheckArrayValueArg(r, c, scope, arrType, unwrapped, false);
}

static void ResolveArrayReplace(Resolver* r, CallExpr* c, StrMap* scope, const TypeName* arrType,
                                const TypeName* unwrapped)
{
    CheckArrayIndexArg(r, c, scope, "index");
    CheckArrayValueArg(r, c, scope, arrType, unwrapped, true);
}

static void ResolveArrayResize(Resolver* r, CallExpr* c, StrMap* scope, const TypeName* unwrapped)
{
    // Growing fills the new slots with zeroes, so the elements must have a valid zero value.
    if (strcmp(c->callee, "array_resize") == 0)
    {
        const TypeName* elemType = TypeNameArrayElem(unwrapped);

        const char* initHeld = elemType ? InlineInitType(r, elemType, 0) : NULL;

        if (initHeld)
        {
            DiagErrorFmt(r->diag, c->base.range,
                         "'array_resize' fills new elements from their field defaults, which never runs '%s's init; "
                         "push constructed values instead",
                         initHeld);
        }
        else if (elemType && !ZeroIsValid(r, elemType, 0))
        {
            DiagErrorFmt(r->diag, c->base.range,
                         "'array_resize' fills new elements with their zero value, which is not a valid '%s' (a box "
                         "has no zero value; make the element type optional)",
                         elemType->name);
        }
    }

    CheckArrayIndexArg(r, c, scope, "size");
}

static void ResolveArrayReserve(Resolver* r, CallExpr* c, StrMap* scope)
{
    CheckArrayIndexArg(r, c, scope, "capacity");
}

static void ResolveArrayRemove(Resolver* r, CallExpr* c, StrMap* scope)
{
    CheckArrayIndexArg(r, c, scope, "index");
}

static void ResolveArrayTake(Resolver* r, CallExpr* c, StrMap* scope, const TypeName* unwrapped)
{
    const TypeName* elemType = TypeNameArrayElem(unwrapped);

    // Only a string, an array or an optional has a value that means "nothing here".
    if (elemType
        && !(TypeIsString(&r->registry, elemType->name) || TypeNameIsDynamicArray(elemType)
             || TypeNameIsOptional(elemType)))
    {
        DiagErrorFmt(r->diag, c->base.range,
                     "'array_take' leaves an empty value in the slot, and '%s' has none; use array_remove, "
                     "array_swap_remove or array_replace",
                     elemType->name);
    }

    CheckArrayIndexArg(r, c, scope, "index");
}

// Array builtins (push/pop/resize); marks the call pseudo. True if handled.
bool ResolveArrayBuiltin(Resolver* r, CallExpr* c, StrMap* scope)
{
    if (!c->callee)
    {
        return false;
    }

    bool isPush = strcmp(c->callee, "array_push") == 0;
    // `array_reserve(a, n)` grows the capacity like a resize would, without touching the elements.
    bool isResize = strcmp(c->callee, "array_resize") == 0 || strcmp(c->callee, "array_reserve") == 0;
    bool isPop = strcmp(c->callee, "array_pop") == 0;
    bool isRemove = (ArrayBuiltinFlagsOf(c->callee) & ABF_REMOVES) != 0;
    // `array_replace(a, i, v)` stores `v` in slot i and hands the old element to the caller.
    bool isReplace = strcmp(c->callee, "array_replace") == 0;
    // `array_take(a, i)` moves element i out and leaves its empty value behind; the array keeps its length.
    bool isTake = strcmp(c->callee, "array_take") == 0;

    if (!isPush && !isResize && !isPop && !isRemove && !isReplace && !isTake)
    {
        return false;
    }

    size_t wantArgs = isPop ? 1 : isReplace ? 3 : 2;

    if (c->args.count != wantArgs)
    {
        DiagErrorFmt(r->diag, c->base.range, "'%s' expects %zu argument(s) but got %zu", c->callee, wantArgs,
                     c->args.count);
        return true;
    }

    Node* arg0 = (Node*)VecGet(&c->args, 0);

    const TypeName* arrType = InferType(r, arg0, scope);

    // Unwrap `T[]?` receivers for the element type.
    const TypeName* unwrapped = arrType && TypeNameIsOptional(arrType) ? arrType->inner : arrType;

    if (!TypeNameIsDynamicArray(unwrapped))
    {
        DiagErrorFmt(r->diag, arg0->range, "'%s' expects an array argument, not '%s'", c->callee,
                     arrType ? arrType->name : "");
        return true;
    }

    // The array is mutated, so it must be an lvalue.
    if (arg0->kind != NodeIdent && arg0->kind != NodeMember && arg0->kind != NodeIndex)
    {
        DiagErrorFmt(r->diag, arg0->range, "'%s' array argument must be an lvalue", c->callee);
        return true;
    }

    // Push/pop/resize mutate the array in place.
    const char* constName = ConstRootName(r, arg0, scope);

    if (constName)
    {
        DiagErrorFmt(r->diag, arg0->range, "'%s' cannot modify '%s'; it is immutable", c->callee, constName);
        return true;
    }

    // A `ref T[]` param may be a borrowed stack view (fixed-array slice):
    // growing/shrinking can reallocate, which would free the caller's storage.
    if ((isPush || isResize) && arg0->kind == NodeIdent && StrMapGet(&r->refArrayParams, ((IdentExpr*)arg0)->name)
        && !IsGrowableArrayParam(r, ((IdentExpr*)arg0)->name))
    {
        DiagErrorFmt(r->diag, arg0->range,
                     "'%s' cannot grow '%s' (a ref array param may borrow the caller's stack storage); "
                     "copy it into a local array first",
                     c->callee, ((IdentExpr*)arg0)->name);
        return true;
    }

    // array_pop needs nothing beyond the receiver checks above.
    if (isPush)
    {
        ResolveArrayPush(r, c, scope, arrType, unwrapped);
    }
    else if (strcmp(c->callee, "array_resize") == 0)
    {
        ResolveArrayResize(r, c, scope, unwrapped);
    }
    else if (strcmp(c->callee, "array_reserve") == 0)
    {
        ResolveArrayReserve(r, c, scope);
    }
    else if (isRemove)
    {
        ResolveArrayRemove(r, c, scope);
    }
    else if (isReplace)
    {
        ResolveArrayReplace(r, c, scope, arrType, unwrapped);
    }
    else if (isTake)
    {
        ResolveArrayTake(r, c, scope, unwrapped);
    }

    c->isIntrinsicCall = true;

    return true;
}

// True when some visible overload of `name` takes a `ref T[]` param whose
// element matches the fixed array's — the arg can cross as a borrowed
// stack-fat view instead of being rejected wholesale.
bool OverloadTakesFixedView(Resolver* r, const char* name, const TypeName* fixedType, int fileId)
{
    const TypeName* argElem = TypeNameArrayElem(fixedType);

    if (!argElem)
    {
        return false;
    }

    for (size_t i = 0; i < r->mod->functions.count; i++)
    {
        const FunctionDecl* f = (const FunctionDecl*)VecGet(&r->mod->functions, i);

        if (strcmp(f->name, name) != 0 || !FunctionVisibleTo(f, fileId))
        {
            continue;
        }

        for (size_t j = 0; j < f->params.count; j++)
        {
            const ParamDecl* p = (const ParamDecl*)VecGet(&f->params, j);

            if (p->isVarargRest || p->mod != ModRef || !TypeNameIsDynamicArray(&p->type))
            {
                continue;
            }

            const TypeName* paramElem = TypeNameArrayElem(&p->type);

            if (paramElem && strcmp(paramElem->name, argElem->name) == 0)
            {
                return true;
            }
        }
    }

    return false;
}

// Stamps a braced literal and its nested rows with element types, top-down.
// Call args are resolved BEFORE the overload is known (so the generic
// inheritance walk saw a NULL elementType); the chosen param supplies the
// types here.
void StampArrayInitElemTypes(ArrayInitExpr* ai, const TypeName* elem)
{
    if (!elem || ai->elementType)
    {
        return;
    }

    ai->elementType = elem;

    if (!TypeNameIsArray(elem))
    {
        return;
    }

    const TypeName* inner = TypeNameArrayElem(elem);

    for (size_t i = 0; i < ai->elements.count; i++)
    {
        Node* child = (Node*)VecGet(&ai->elements, i);

        if (child && child->kind == NodeArrayInit && !((ArrayInitExpr*)child)->elementType)
        {
            StampArrayInitElemTypes((ArrayInitExpr*)child, inner);
        }
    }
}

// True for `{}`: an empty braced value, which default-constructs.
static bool IsEmptyBraced(const Node* n)
{
    return (n->kind == NodeArrayInit && ((const ArrayInitExpr*)n)->elements.count == 0
            && !((const ArrayInitExpr*)n)->repeatCount)
           || (n->kind == NodeStructInit && ((const StructInitExpr*)n)->fields.count == 0);
}

// `{ value; count }` for a dynamic array: the count is any integer; the value is evaluated once and
// each element is a copy of it (a `{}` value instead default-constructs each element). The value is
// read, not moved, so a variable used as the value stays usable.
void ResolveArrayRepeat(Resolver* r, ArrayInitExpr* ai, StrMap* scope)
{
    ResolveExpr(r, ai->repeatCount, scope);

    const TypeName* countType = InferType(r, ai->repeatCount, scope);

    if (countType
        && (!IsNumeric(countType->primitiveType) || countType->primitiveType == PrimBool
            || countType->primitiveType == PrimFloat || countType->primitiveType == PrimDouble))
    {
        DiagErrorFmt(r->diag, ai->repeatCount->range, "the count in '{ value; count }' must be an integer, not '%s'",
                     countType->name);
    }

    Node* value = (Node*)VecGet(&ai->elements, 0);
    const TypeName* elem = ai->elementType;

    // `{ {}; count }` zero-fills each element: the default value, built in place.
    if (IsEmptyBraced(value))
    {
        const char* initHeld = elem ? InlineInitType(r, elem, 0) : NULL;

        if (initHeld)
        {
            DiagErrorFmt(r->diag, value->range,
                         "'{ {}; count }' fills each element from its field defaults, which never runs '%s's init; "
                         "push constructed values instead",
                         initHeld);
        }
        else if (elem && !ZeroIsValid(r, elem, 0))
        {
            DiagErrorFmt(r->diag, value->range,
                         "'{ {}; count }' fills each element with its zero value, which is not a valid '%s' (a "
                         "'^T' in it must point at something); give a value to copy instead",
                         elem->name);
        }
        return;
    }

    if (elem && (value->kind == NodeArrayInit || value->kind == NodeStructInit))
    {
        value = ApplyBracedStructTarget(r, value, elem);
        VecSet(&ai->elements, 0, value);
    }

    ResolveExpr(r, value, scope);

    if (!elem)
    {
        return;
    }

    const TypeName* valueType = InferType(r, value, scope);

    if (valueType && !IsAssignableType(r, elem, valueType) && !IsStrLiteralForCString(r, elem, value)
        && !IsEmptyBraced(value))
    {
        DiagErrorFmt(r->diag, value->range, "a '%s' cannot fill an array of '%s'", valueType->name, elem->name);
    }

    if (!IsEmptyBraced(value) && AliasIsOwningValue(r, elem) && TypeNameHasDrop(&r->registry, elem))
    {
        DiagErrorFmt(r->diag, value->range,
                     "'%s' has a drop method, so it cannot be copied into each element; write '{ {}; count }' "
                     "to default-construct them instead",
                     elem->name);
    }
}

void ResolveIndexExpr(Resolver* r, Node* n, StrMap* scope)
{
    IndexExpr* ix = (IndexExpr*)n;

    ResolveExprImpl(r, ix->baseNode, scope);
    ResolveExprImpl(r, ix->index, scope);

    const TypeName* baseType = InferType(r, ix->baseNode, scope);

    // `p[i]` on a typed pointer reads and writes the pointee, so it takes the pointee rules `*p` does.
    if (TypeNameIsPointer(baseType) && baseType->inner)
    {
        const TypeName* target = baseType->inner;

        if (target->primitiveType == PrimVoid)
        {
            DiagError(r->diag, ix->base.range, "cannot index a pointer to 'void'; cast it to a typed pointer first");
        }
        else if (TypeRegistryIsOpaque(&r->registry, target->name))
        {
            DiagErrorFmt(r->diag, ix->base.range, "cannot index a pointer to '%s' (no known layout)", target->name);
        }
        else if (!TypeClassify(&r->registry, r->arena, target).trivial)
        {
            DiagErrorFmt(r->diag, ix->base.range,
                         "cannot index a pointer to an owning type: '%s' owns memory, and a pointer neither moves "
                         "nor drops what it points at",
                         target->name);
        }
    }

    return;
}

void ResolveArrayInitExpr(Resolver* r, Node* n, StrMap* scope)
{
    ArrayInitExpr* ai = (ArrayInitExpr*)n;

    if (ai->repeatCount)
    {
        ResolveArrayRepeat(r, ai, scope);
        return;
    }

    for (size_t i = 0; i < ai->elements.count; i++)
    {
        Node* elem = (Node*)VecGet(&ai->elements, i);

        // A flattened multidim fixed-array init carries NULL holes
        // (short rows / missing rows zero-fill); nothing to resolve.
        if (!elem)
        {
            continue;
        }

        // Nested rows inherit the inner element type.
        if (ai->elementType && TypeNameHasArrayShape(ai->elementType) && elem->kind == NodeArrayInit
            && !((ArrayInitExpr*)elem)->elementType)
        {
            const TypeName* innerElem = TypeNameArrayElem(ai->elementType);

            if (innerElem)
            {
                ((ArrayInitExpr*)elem)->elementType = innerElem;
            }
        }

        ResolveExpr(r, elem, scope);

        if (ai->elementType)
        {
            VecSet(&ai->elements, i, elem);
        }

        // Owning elements move out of their sources (owning-struct
        // elements included: `{ Rec{...}, arr2[0] }` takes arr2[0]).
        if (ai->elementType && AliasIsOwningValue(r, ai->elementType))
        {
            const char* movedKey = MovableBoxSourceKey(r, elem);

            if (movedKey)
            {
                MarkMoved(r, movedKey, elem->range);
            }
        }

        // Plain elements unwrap `T?` sources.
    }

    return;
}
