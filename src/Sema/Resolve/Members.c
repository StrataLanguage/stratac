/* Member access, impl resolution, accessor synthesis and struct initializers. */
#include "Sema/Resolve/ResolverInternal.h"

bool IsLengthPseudoMemberBase(const TypeRegistry* reg, const TypeName* t)
{
    if (!t || !t->name)
    {
        return false;
    }

    if (TypeNameHasArrayShape(t))
    {
        return true;
    }

    const char* leaf = TypeRegistryResolveAlias(reg, t->name);
    return TypeIsString(reg, leaf) || TypeIsCString(reg, leaf);
}

// Rewrite a braced list as a positional struct init of `structName`.
static Node* BracedToStructInit(Resolver* r, Node* braced, const char* structName)
{
    ArrayInitExpr* ai = AsNode(ArrayInitExpr, braced);

    StructInitExpr* init = (StructInitExpr*)ArenaAlloc(r->m_arena, sizeof(StructInitExpr));
    memset(init, 0, sizeof(StructInitExpr));
    init->base.kind = NodeStructInit;
    init->base.range = ai->base.range;
    init->typeName = (char*)structName;
    VecInit(&init->fields);

    for (size_t i = 0; i < ai->elements.count; i++)
    {
        StructInitField* field = (StructInitField*)ArenaAlloc(r->m_arena, sizeof(StructInitField));
        field->name = NULL;
        field->value = (Node*)VecGet(&ai->elements, i);
        VecPush(&init->fields, field);
    }

    return (Node*)init;
}

// Give a bare braced node the struct target type (no-op otherwise).
Node* ApplyBracedStructTarget(Resolver* r, Node* node, const TypeName* target)
{
    const TypeName* unwrapped = UnwrapBoxPtr(target);

    if (!unwrapped || !TypeRegistryIsUserType(&r->m_registry, unwrapped->name)
        || TypeRegistryIsOpaque(&r->m_registry, unwrapped->name))
    {
        return node;
    }

    if (node->kind == NodeArrayInit && !((ArrayInitExpr*)node)->elementType)
    {
        return BracedToStructInit(r, node, unwrapped->name);
    }

    if (node->kind == NodeStructInit && !((StructInitExpr*)node)->typeName)
    {
        ((StructInitExpr*)node)->typeName = (char*)unwrapped->name;
    }

    return node;
}

void CheckStructInitFields(Resolver* r, Node* n)
{
    StructInitExpr* init = (StructInitExpr*)n;

    const StructType* structType = TypeRegistryFind(&r->m_registry, init->typeName);

    if (!structType || structType->fields.count == 0 || TypeRegistryIsOpaque(&r->m_registry, init->typeName))
    {
        return;
    }

    size_t fieldCount = structType->fields.count;
    bool* covered = (bool*)ArenaAlloc(r->m_arena, fieldCount * sizeof(bool));
    size_t positionalSeen = 0;

    for (size_t i = 0; i < init->fields.count; i++)
    {
        StructInitField* field = (StructInitField*)VecGet(&init->fields, i);

        size_t idx = (size_t)-1;
        FieldDecl* fd = NULL;

        if (field->name && field->name[0] != '\0')
        {
            int named = TypeRegistryFieldIndex(&r->m_registry, init->typeName, field->name);

            if (named >= 0)
            {
                idx = (size_t)named;
                fd = (FieldDecl*)VecGet((Vec*)&structType->fields, idx);
            }
        }
        else if (positionalSeen < fieldCount)
        {
            idx = positionalSeen++;
            fd = (FieldDecl*)VecGet((Vec*)&structType->fields, idx);
        }
        else
        {
            positionalSeen++; /* too many initializers: reported elsewhere */
        }

        if (field->value)
        {
            /* Type a nested bare-braced value from the field's declared type
               (`{ {} }`); already-typed values pass through unchanged. */
            if (fd && (field->value->kind == NodeArrayInit || field->value->kind == NodeStructInit))
            {
                field->value = ApplyBracedStructTarget(r, field->value, &fd->type);
            }

            if (field->value->kind == NodeStructInit && ((StructInitExpr*)field->value)->typeName)
            {
                bool emptyOptional
                    = fd && TypeNameIsOptional(&fd->type) && ((StructInitExpr*)field->value)->fields.count == 0;

                if (!emptyOptional)
                {
                    CheckStructInitFields(r, field->value);
                }
            }
        }

        if (idx < fieldCount)
        {
            covered[idx] = true;
        }
    }

    for (size_t f = 0; f < fieldCount; f++)
    {
        FieldDecl* fd = (FieldDecl*)VecGet((Vec*)&structType->fields, f);

        /* Every OWNING field (`^T`, owning struct, `string`) must be
           initialized; optionals and dynamic arrays may stay empty (a T? is
           null, a zero-filled T[] is the canonical empty {null, 0} fat
           struct). */
        /* An alias of an optional or an array is judged as the type it names. */
        const TypeName* fieldType = ResolveAliasedType(&r->m_registry, r->m_arena, &fd->type);
        bool mustInit = !TypeNameIsOptional(fieldType) && !TypeNameIsDynamicArray(fieldType)
                        && (AliasIsOwning(r, &fd->type) || TypeRegistryIsOwningStruct(&r->m_registry, fd->type.name));

        if (mustInit && !covered[f])
        {
            /* Suggest the optional spelling of the inner type. A `^string`
               field's optional is spelled `string?`. */
            const TypeName* inner = TypeNameBoxInner(&fd->type);
            const char* optSpelling = inner ? inner->name : fd->type.name;

            DiagErrorFmt(r->m_diag, init->base.range,
                         "owning field '%s' of struct '%s' must be initialized "
                         "(declare it '%s?' if it may be empty)",
                         fd->name, init->typeName, optSpelling);
        }
    }
}

// -- Impl blocks.

/* Method lookup walks base handles: a `handle Player : Entity` sees impl Entity methods. */
FunctionDecl* FindImplMethod(Resolver* r, const char* handleName, const char* methodName)
{
    const char* hn = handleName;
    size_t depth = 0;

    while (hn && depth <= r->m_registry.count)
    {
        for (size_t i = 0; i < r->m_mod->impls.count; i++)
        {
            ImplDecl* impl = (ImplDecl*)VecGet(&r->m_mod->impls, i);

            if (strcmp(impl->handleName, hn) != 0)
            {
                continue;
            }

            for (size_t j = 0; j < impl->methods.count; j++)
            {
                FunctionDecl* fn = (FunctionDecl*)VecGet(&impl->methods, j);

                if (fn->methodName && strcmp(fn->methodName, methodName) == 0)
                {
                    return fn;
                }
            }
        }

        const StructType* t = TypeRegistryFind(&r->m_registry, hn);
        hn = (t && t->opaque && t->extendsFrom) ? t->extendsFrom : NULL;
        depth++;
    }

    return NULL;
}

/* Property lookup on the exact handle (no inheritance). */
static PropertyDecl* FindImplProperty(Resolver* r, const char* handleName, const char* name)
{
    const ImplDecl* impl = ModuleFindImpl(r->m_mod, handleName);

    if (!impl)
    {
        return NULL;
    }

    for (size_t i = 0; i < impl->properties.count; i++)
    {
        PropertyDecl* prop = (PropertyDecl*)VecGet(&impl->properties, i);

        if (strcmp(prop->name, name) == 0)
        {
            return prop;
        }
    }

    return NULL;
}

/* If `n` is a property read on an impl target (after unwrapping `T?`/`^T`),
   returns the property; NULL when the member is not an impl property. */
PropertyDecl* PropertyOnHandle(Resolver* r, const TypeName* baseType, const char* member)
{
    const TypeName* inner = UnwrapBoxPtr(baseType);

    if (!inner || !TypeRegistryIsImplTarget(&r->m_registry, inner->name))
    {
        return NULL;
    }

    return FindImplProperty(r, inner->name, member);
}

/* Resolves `int.max` / `float.min` — a pseudo-property read on a BUILTIN
   scalar type name (aliases and user types are excluded; only the builtin
   spellings carry the C-limit constants). Returns true when the base names
   a scalar pseudo type, meaning the member expression was fully handled:
   a valid `max`/`min` marks the member as a constant; an invalid member is
   diagnosed. Anything else (value bases, non-scalar bases) returns false. */
bool TryResolveScalarPseudoConst(Resolver* r, MemberExpr* m)
{
    if (m->baseNode->kind != NodeIdent)
    {
        return false;
    }

    const char* baseName = ((IdentExpr*)m->baseNode)->name;
    const PrimitiveType primitive = GetPrimitiveType(baseName);

    if (!IsScalarPseudoType(primitive))
    {
        return false;
    }

    if (!ScalarPseudoConst(primitive, m->member, NULL, NULL, NULL))
    {
        DiagErrorFmt(r->m_diag, m->base.range, "type '%s' has no member '%s'", baseName, m->member);
        return true;
    }

    m->isScalarConst = true;
    return true;
}

static void VecPushFront(Vec* v, void* item)
{
    VecPush(v, NULL);
    memmove(v->items + 1, v->items, (v->count - 1) * sizeof(void*));
    v->items[0] = item;
}

/* Resolves `expr.Member(args...)` / `Type.Member(args...)` against impl blocks.
   Returns true when the call was fully handled (diagnostic emitted); false
   after a successful rewrite (fall through to normal overload resolution). */
bool ResolveMemberCall(Resolver* r, CallExpr* c, StrMap* scope)
{
    Node* base = c->calleeBase;

    /* `Type.Method(args)` — static call when the base is a type name. */
    bool isStatic = base->kind == NodeIdent && TypeRegistryIsUserType(&r->m_registry, ((IdentExpr*)base)->name);

    const char* handleName = NULL;

    if (isStatic)
    {
        handleName = ((IdentExpr*)base)->name;
    }
    else
    {
        ResolveExpr(r, base, scope);

        const TypeName* raw = InferType(r, base, scope);

        if (raw && TypeNameIsOptional(raw))
        {
            /* Reading through a `T?` receiver needs a narrowing fact. */
            const char* key = MovableBoxSourceKey(r, base);
        }

        const TypeName* inner = UnwrapBoxPtr(raw);

        if (!inner || !TypeRegistryIsImplTarget(&r->m_registry, inner->name))
        {
            DiagErrorFmt(r->m_diag, c->base.range, "type '%s' has no method '%s'", raw && inner ? inner->name : "?",
                         c->callee);
            return true;
        }

        handleName = inner->name;
    }

    FunctionDecl* method = FindImplMethod(r, handleName, c->callee);

    if (!method)
    {
        if (FindImplProperty(r, handleName, c->callee))
        {
            DiagErrorFmt(r->m_diag, c->base.range, "'%s' is a property of '%s', not a method; access it without ()",
                         c->callee, handleName);
        }
        else
        {
            DiagErrorFmt(r->m_diag, c->base.range, "type '%s' has no method '%s'", handleName, c->callee);
        }

        return true;
    }

    /* Instance call: when the method's first parameter is the receiver slot
       (the impl type the receiver's type extends or equals), prepend the base
       expression as the self argument. Parameterless methods (factories) and
       static calls map arguments 1:1. */
    if (!isStatic && method->params.count > 0)
    {
        const ParamDecl* p0 = (const ParamDecl*)VecGet(&method->params, 0);

        if (TypeRegistryIsImplTarget(&r->m_registry, p0->type.name)
            && (strcmp(p0->type.name, handleName) == 0 || HandleExtendsFrom(&r->m_registry, handleName, p0->type.name)))
        {
            VecPushFront(&c->args, c->calleeBase);
        }
    }

    c->calleeBase = NULL;
    c->isIntrinsicCall = false;
    c->callee = method->name; /* qualified extern symbol, e.g. Camera_GetFOV */

    return false;
}

/* Synthesizes the extern declaration a property accessor refers to. When a
   function with that name already exists (e.g. a flat extern), its signature
   is validated against the accessor shape instead. */
static void SynthesizeAccessor(Module* mod, Arena* arena, const char* symbol, const TypeName* propType,
                               const char* handleName, bool selfByRef, bool isSetter, SourceRange range,
                               DiagnosticEngine* diag)
{
    size_t matchCount = 0;
    FunctionDecl* match = NULL;

    for (size_t i = 0; i < mod->functions.count; i++)
    {
        FunctionDecl* fn = (FunctionDecl*)VecGet(&mod->functions, i);

        if (strcmp(fn->name, symbol) != 0)
        {
            continue;
        }

        matchCount++;
        match = fn;
    }

    if (matchCount > 1)
    {
        /* An accessor must name a unique function: with overloads there is no
           way to tell which one `get`/`set` refers to, and first-match binding
           would silently depend on declaration order. */
        DiagErrorFmt(diag, range,
                     "property %s '%s' is ambiguous: %zu functions share that name; "
                     "property accessors must name a unique function",
                     isSetter ? "setter" : "getter", symbol, matchCount);
        return;
    }

    if (match)
    {
        /* Validate the pre-existing declaration against the accessor shape:
           getter (self) -> propType; setter (self, value propType) -> void.
           A getter may return the value via a plain return OR a trailing
           `return propType` out-param (`void Get(self, return T c)`) — the
           parser folds the out-param's type into fn->returnType and
           NamedParamCount excludes it, so the checks below cover both forms. */
        size_t wantParams = isSetter ? 2 : 1;

        if (NamedParamCount(match) != wantParams)
        {
            DiagErrorFmt(diag, range, "property %s '%s' must take (%s self%s); found %zu parameter(s)",
                         isSetter ? "setter" : "getter", symbol, handleName, isSetter ? ", value" : "",
                         match->params.count);
            return;
        }

        const ParamDecl* p0 = (const ParamDecl*)VecGet(&match->params, 0);

        if (strcmp(p0->type.name, handleName) != 0)
        {
            DiagErrorFmt(diag, range, "property %s '%s' must take '%s' as its first parameter; found '%s'",
                         isSetter ? "setter" : "getter", symbol, handleName, p0->type.name);
            return;
        }

        if (isSetter)
        {
            if (match->returnType.primitiveType != PrimVoid)
            {
                DiagErrorFmt(diag, range, "property setter '%s' must return void; found '%s'", symbol,
                             match->returnType.name);
                return;
            }

            const ParamDecl* p1 = (const ParamDecl*)VecGet(&match->params, 1);

            if (strcmp(p1->type.name, propType->name) != 0)
            {
                DiagErrorFmt(diag, range, "property setter '%s' must take '%s' as its value parameter; found '%s'",
                             symbol, propType->name, p1->type.name);
            }
        }
        else if (strcmp(match->returnType.name, propType->name) != 0)
        {
            DiagErrorFmt(diag, range, "property getter '%s' must return '%s'; found '%s'", symbol, propType->name,
                         match->returnType.name);
        }

        return;
    }

    FunctionDecl* fn = AST_NEW(arena, FunctionDecl);
    fn->base.kind = NodeFunction;
    fn->base.range = range;
    fn->name = ArenaStrdup(arena, symbol);
    fn->mangledName = fn->name;
    fn->isExtern = true;
    fn->fromImpl = true;
    VecInit(&fn->params);

    if (isSetter)
    {
        fn->returnType.name = ArenaStrdup(arena, "void");
    }
    else
    {
        fn->returnType = *propType;
    }

    /* A struct receiver crosses as a pointer, like any extern struct param; a getter only reads it. */
    ParamDecl* self = AST_NEW(arena, ParamDecl);
    self->base.kind = NodeParam;
    self->base.range = range;
    self->mod = selfByRef ? ModRef : ModNone;
    self->type = TypeNameLeaf(ArenaStrdup(arena, handleName));
    self->type.isConst = selfByRef && !isSetter;
    self->name = ArenaStrdup(arena, "self");
    VecPush(&fn->params, self);

    if (isSetter)
    {
        ParamDecl* value = AST_NEW(arena, ParamDecl);
        value->base.kind = NodeParam;
        value->base.range = range;
        value->mod = ModNone;
        value->type = *propType;
        value->name = ArenaStrdup(arena, "value");
        VecPush(&fn->params, value);
    }

    VecPush(&mod->functions, fn);
}

/* Validates impl targets and materializes property accessor externs (before
   the overload/mangling pass, so accessors flow through the normal pipeline). */
void ResolveImpls(Module* mod, DiagnosticEngine* diag, Arena* arena, const TypeRegistry* registry)
{
    for (size_t i = 0; i < mod->impls.count; i++)
    {
        ImplDecl* impl = (ImplDecl*)VecGet(&mod->impls, i);

        if (!TypeRegistryIsImplTarget(registry, impl->handleName))
        {
            DiagErrorFmt(diag, impl->base.range, "impl type '%s' is not a declared struct or handle", impl->handleName);
            continue;
        }

        for (size_t j = 0; j < impl->properties.count; j++)
        {
            PropertyDecl* prop = (PropertyDecl*)VecGet(&impl->properties, j);

            if (!prop->getterSymbol && !prop->setterSymbol)
            {
                DiagErrorFmt(diag, prop->range, "property '%s' must declare at least one of 'get' or 'set'",
                             prop->name);
                continue;
            }

            /* 'void' has no value to get/set; a dynamic `T[]` crosses extern
               as a fat {ptr,len} struct no C thunk can sanely implement for a
               single accessor. Fixed `T[N]` is rejected later by the
               function-level fixed-array rules (synthesized getter/setter). */
            if (prop->returnType.primitiveType == PrimVoid)
            {
                DiagErrorFmt(diag, prop->range, "property '%s' may not have type 'void'", prop->name);
                continue;
            }

            if (TypeNameIsDynamicArray(&prop->returnType))
            {
                DiagErrorFmt(diag, prop->range, "property '%s' may not have a dynamic array type ('%s')", prop->name,
                             prop->returnType.name);
                continue;
            }

            const char* targetLeaf = TypeRegistryResolveAlias(registry, impl->handleName);
            bool selfByRef = IsDefinedStruct(registry, targetLeaf) || IsIncompleteStruct(registry, targetLeaf);

            if (prop->getterSymbol)
            {
                SynthesizeAccessor(mod, arena, prop->getterSymbol, &prop->returnType, impl->handleName, selfByRef,
                                   false, prop->range, diag);
            }

            if (prop->setterSymbol)
            {
                SynthesizeAccessor(mod, arena, prop->setterSymbol, &prop->returnType, impl->handleName, selfByRef, true,
                                   prop->range, diag);
            }
        }
    }
}

static bool ResolveBracedFieldInit(Resolver* r, FieldDecl* fieldDecl, StructInitField* field, StrMap* scope)
{
    ArrayInitExpr* ai = (ArrayInitExpr*)field->value;

    if (TypeNameHasArrayShape(&fieldDecl->type) && fieldDecl->type.length >= 0)
    {
        // Fixed arrays need matching shape; rows lay out row-major.
        ArrayLength total = 0;
        const TypeName* leaf = FixedArrayLeaf(&fieldDecl->type, &total);

        if (!ai->elementType)
        {
            ai->elementType = leaf;
        }

        if (!CheckFixedArrayInitShape(r, &fieldDecl->type, ai, leaf, fieldDecl->name, "field"))
        {
            ai->elements.count = 0;
            return false;
        }

        // Short rows leave holes that zero-fill.
        {
            Vec flat;
            VecInit(&flat);

            for (ArrayLength z = 0; z < total; z++)
            {
                VecPush(&flat, NULL);
            }

            if (ai->repeatCount)
            {
                DiagErrorFmt(r->m_diag, field->value->range,
                             "'{ value; count }' cannot initialize fixed-size array field '%s'; "
                             "list the elements, or fill the field after construction",
                             fieldDecl->name);
                free(flat.items);
                ai->repeatCount = NULL;
                ai->elements.count = 0;
                return false;
            }

            if (!PlaceFixedArrayInit(&flat, 0, &fieldDecl->type, ai))
            {
                DiagErrorFmt(r->m_diag, field->value->range,
                             "too many initializers for fixed-size array field '%s' (%ld max)", fieldDecl->name, total);
            }

            void** oldItems = ai->elements.items;
            ai->elements = flat;
            free(oldItems);
        }

        // Bare-brace leaves are struct inits of the leaf type.
        for (size_t k = 0; k < ai->elements.count; k++)
        {
            Node* elem = (Node*)VecGet(&ai->elements, k);

            if (!elem)
            {
                continue;
            }

            if (elem->kind == NodeArrayInit || elem->kind == NodeStructInit)
            {
                VecSet(&ai->elements, k, ApplyBracedStructTarget(r, elem, leaf));
            }
        }

        for (size_t k = 0; k < ai->elements.count; k++)
        {
            Node* elem = (Node*)VecGet(&ai->elements, k);

            if (!elem)
            {
                continue; // Hole: stays zero.
            }

            const TypeName* elemType = InferType(r, elem, scope);

            if (elemType && !IsAssignableType(r, leaf, elemType) && !IsStrLiteralForCString(r, leaf, elem))
            {
                DiagErrorFmt(r->m_diag, elem->range,
                             "element of type '%s' cannot initialize '%s' element of field '%s'", elemType->name,
                             leaf->name, fieldDecl->name);
            }
        }
    }
    else if (TypeNameIsDynamicArray(&fieldDecl->type))
    {
        // Braced dynamic arrays allocate fresh; fill element type.
        const TypeName* elem = TypeNameArrayElem(&fieldDecl->type);

        if (!ai->elementType)
        {
            ai->elementType = elem;
        }

        for (size_t k = 0; k < ai->elements.count; k++)
        {
            Node* element = (Node*)VecGet(&ai->elements, k);
            const TypeName* elemType = InferType(r, element, scope);

            if (elemType && !IsAssignableType(r, elem, elemType) && !IsStrLiteralForCString(r, elem, element))
            {
                DiagErrorFmt(r->m_diag, element->range,
                             "element of type '%s' cannot initialize '%s' element of field '%s'", elemType->name,
                             elem->name, fieldDecl->name);
            }
        }
    }
    else
    {
        DiagErrorFmt(r->m_diag, field->value->range,
                     "braced initializers are only supported for array fields; "
                     "field '%s' has type '%s'",
                     fieldDecl->name, fieldDecl->type.name);
    }

    return true;
}

static void ResolveStructInitField(Resolver* r, StructInitExpr* structInitExpr, const StructType* structType,
                                   StructInitField* field, size_t* positionalCount, StrMap* scope)
{
    ResolveExpr(r, field->value, scope);

    FieldDecl* fieldDecl = NULL;

    if (field->name && field->name[0] != '\0')
    {
        int idx = structType ? TypeRegistryFieldIndex(&r->m_registry, structInitExpr->typeName, field->name) : -1;

        if (structType && idx < 0)
        {
            DiagErrorFmt(r->m_diag, structInitExpr->base.range, "struct '%s' has no field named '%s'",
                         structInitExpr->typeName, field->name);
        }
        else if (structType)
        {
            fieldDecl = (FieldDecl*)VecGet(&structType->fields, (size_t)idx);
        }
    }
    else
    {
        if (structType && (*positionalCount) >= structType->fields.count)
        {
            DiagErrorFmt(r->m_diag, structInitExpr->base.range, "too many initializers for struct '%s'",
                         structInitExpr->typeName);
        }
        else if (structType)
        {
            fieldDecl = (FieldDecl*)VecGet(&structType->fields, (*positionalCount));
        }

        (*positionalCount)++;
    }

    if (fieldDecl && field->value)
    {
        // Nested braced values take the field's struct shape.
        if (field->value->kind == NodeArrayInit || field->value->kind == NodeStructInit)
        {
            field->value = ApplyBracedStructTarget(r, field->value, &fieldDecl->type);
        }

        if (field->value->kind == NodeArrayInit)
        {
            if (!ResolveBracedFieldInit(r, fieldDecl, field, scope))
            {
                return;
            }
        }
        else
        {
            field->value = field->value;
            const TypeName* fieldValueType = InferType(r, field->value, scope);

            if (fieldValueType && !IsAssignableType(r, &fieldDecl->type, fieldValueType)
                && !IsStrLiteralForCString(r, &fieldDecl->type, field->value))
            {
                DiagErrorFmt(r->m_diag, structInitExpr->base.range,
                             "field '%s' of struct '%s' cannot be initialized by expression of type '%s'",
                             fieldDecl->name, structInitExpr->typeName, fieldValueType->name);
            }

            /* A plain `T` field initialized from a `T?` unwraps it. */
        }

        /* A ^T field moves its source only when that source is itself owning. */
        const TypeName* fieldValueType2 = InferType(r, field->value, scope);

        const char* movedFieldKey
            = (AliasIsOwning(r, &fieldDecl->type) && fieldValueType2 && AliasIsOwning(r, fieldValueType2))
                  ? MovableBoxSourceKey(r, field->value)
                  : NULL;

        if (movedFieldKey)
        {
            MarkMoved(r, movedFieldKey, structInitExpr->base.range);
        }
    }
}

void ResolveStructInitExpr(Resolver* r, Node* n, StrMap* scope)
{
    StructInitExpr* structInitExpr = (StructInitExpr*)n;

    /* A context-free braced literal has no type yet - resolve field values; full checks run later. */
    if (!structInitExpr->typeName)
    {
        for (size_t i = 0; i < structInitExpr->fields.count; i++)
        {
            StructInitField* field = (StructInitField*)VecGet(&structInitExpr->fields, i);

            if (field->value)
            {
                ResolveExpr(r, field->value, scope);
            }
        }

        return;
    }

    if (!TypeRegistryIsUserType(&r->m_registry, structInitExpr->typeName))
    {
        DiagErrorFmt(r->m_diag, structInitExpr->base.range, "'%s' is not a known aggregate type",
                     structInitExpr->typeName);
    }
    else if (TypeRegistryIsTypeAlias(&r->m_registry, structInitExpr->typeName))
    {
        DiagErrorFmt(r->m_diag, structInitExpr->base.range,
                     "'%s' is a type alias and cannot be initialized with a struct literal", structInitExpr->typeName);
    }
    else if (TypeRegistryIsOpaque(&r->m_registry, structInitExpr->typeName))
    {
        DiagErrorFmt(r->m_diag, structInitExpr->base.range, "'%s' is opaque and may not be instantiated",
                     structInitExpr->typeName);
    }

    size_t positionalCount = 0;

    const StructType* structType = TypeRegistryFind(&r->m_registry, structInitExpr->typeName);
    for (size_t i = 0; i < structInitExpr->fields.count; i++)
    {
        StructInitField* field = (StructInitField*)VecGet(&structInitExpr->fields, i);
        ResolveStructInitField(r, structInitExpr, structType, field, &positionalCount, scope);
    }

    /* Non-optional `^T` fields must be initialized (NULL would trip every use); `T?` may stay empty.
       Also types any nested bare-braced field values. */
    CheckStructInitFields(r, n);

    return;
}

void ResolveMemberExpr(Resolver* r, Node* n, StrMap* scope)
{
    MemberExpr* m = (MemberExpr*)n;

    /* Scoped enum constant: `EnumName.Member` reads a constant — the base
       is a type name, not a value, so skip the normal member path. */
    if (TryResolveEnumMember(r, m, scope))
    {
        return;
    }

    /* Builtin scalar pseudo-property: `int.max` / `float.min`. */
    if (TryResolveScalarPseudoConst(r, m))
    {
        return;
    }

    ResolveExprImpl(r, m->baseNode, scope, true);

    const TypeName* rawBaseType = InferType(r, m->baseNode, scope);

    /* Impl property reads are call-like: no lvalue, no move source. */
    m->isImplProperty = PropertyOnHandle(r, rawBaseType, m->member) != NULL;

    /* Reading a possibly-empty optional needs a narrowing fact. */
    if (rawBaseType && TypeNameIsOptional(rawBaseType))
    {
        const char* baseKey = MovableBoxSourceKey(r, m->baseNode);
    }

    const TypeName* baseType = UnwrapBoxPtr(rawBaseType);

    if (baseType && TypeRegistryIsOpaque(&r->m_registry, baseType->name))
    {
        /* Properties and methods work on handles AND on opaque
           (forward-declared) structs; only the "no such member" wording
           differs. */
        PropertyDecl* prop = PropertyOnHandle(r, rawBaseType, m->member);

        if (prop)
        {
            if (!prop->getterSymbol)
            {
                DiagErrorFmt(r->m_diag, m->base.range, "property '%s' is write-only", m->member);
            }
        }
        else if (FindImplMethod(r, baseType->name, m->member))
        {
            DiagErrorFmt(r->m_diag, m->base.range, "method '%s' must be called: use '%s.%s(...)'", m->member,
                         baseType->name, m->member);
        }
        else if (IsIncompleteStruct(&r->m_registry, baseType->name))
        {
            DiagErrorFmt(r->m_diag, m->base.range, "cannot access a member of incomplete type '%s'", baseType->name);
        }
        else
        {
            DiagErrorFmt(r->m_diag, m->base.range, "handle '%s' has no member '%s'", baseType->name, m->member);
        }
    }

    /* `cstring` exposes only `.length` (no `.cap`, no other members). */
    if (baseType && TypeIsCString(&r->m_registry, baseType->name) && strcmp(m->member, "length") != 0)
    {
        DiagErrorFmt(r->m_diag, m->base.range, "cstring has no member '%s' (only '.length')", m->member);
    }

    /* A swizzle names lanes the vector has: `float2` has x and y, no z. */
    if (baseType)
    {
        LaneCount lanes = IsNameSimdVector(TypeRegistryResolveAlias(&r->m_registry, baseType->name));
        size_t len = strlen(m->member);
        bool swizzle = lanes != 0 && len >= 1 && len <= 4;

        for (size_t i = 0; swizzle && i < len; i++)
        {
            swizzle = m->member[i] >= 'w' && m->member[i] <= 'z';
        }

        for (size_t i = 0; swizzle && i < len; i++)
        {
            int lane = m->member[i] == 'w' ? 3 : m->member[i] - 'x';

            if (lane >= lanes)
            {
                DiagErrorFmt(r->m_diag, m->base.range, "'%s' has no lane '%c'", baseType->name, m->member[i]);
                break;
            }
        }
    }

    return;
}
