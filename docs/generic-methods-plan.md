# Generic methods plan

## Status

Phases 1-5 and the Afterwards step are implemented: parsing, the declared-type lookup, method templates and their
instances, inference for methods and free functions, the conformance cases (`accept/generic_method_*`,
`accept/generic_function_inferred*`, `reject/generic_method_*`), unit tests in `tests/unit/GenericTests.c`, the
README section, and `std/rc.strata` (generic `init` and `create_ref` enabled). Deviations from the plan as written:

- **Float literal type.** `FloatLiteral` had no suffix flag, so the parser now sets `isSingle` for an `f` suffix.
  An unsuffixed float literal is `double` for inference, as the plan says.
- **Quiet `init` inference.** For `H(args)` and `p.init(args)` (and `obj.init(args)` inside a body), an `init`
  template whose type arguments cannot be inferred is not an error: the call is left alone, and sema picks a plain
  `init` if one matches. Without this, `Pt()` would fail whenever `Pt` had a generic `init<U, V>` beside a plain
  one. Every other method or function call with an uninferable type argument is reported as the plan says.
- **Methods with explicit type arguments that are not generic** (`h.plain<int>()`) are an error:
  "'plain' is not a generic method, so it takes no type arguments". The plan did not list this case.
- **Unknown receiver for `init`.** `init` is in the generic-name set, so an untyped `x.init(...)` is an error
  whenever some generic `init` exists in the module. Constructors `H(...)` never reach this check.
- **Declared types.** `MonoTypeOf` types the forms listed in Phase 2 and nothing else. A method call's result is
  unknown, so `var x = h.convert<float>();` leaves `x` untyped, and a later generic-method call on `x` needs a
  declared type. Binary expressions are unknown too, so `h.set(1 + 2)` needs explicit type arguments.
- **On-demand instances.** A generic type use that is reached before its declaration is rewritten (a field or a
  function's return type) is rewritten when the lookup needs it (`MonoCanonical`), so its instance and methods exist.
- **`@has_function`.** It also finds a plain struct's generic methods, through the method templates. The plan did
  not mention it, but without it `@initable(T)` would miss a generic-only method.
- **`make_ref` in `std/rc.strata`** is unchanged. It calls `res.create_ref()` with no arguments and then
  `res.ptr.init(args...)`, so with a `T` whose `init` needs arguments it would fail when instantiated. It is a
  template, so it is only checked when called, and nothing in the repository calls it.
- Not verified: an AddressSanitizer build (not configured here). The disposal rules were checked by reading
  `AstDispose` and `DisposeModule`: a method template goes to `genericTemplates` only, an instance to
  `mod->functions` only, and a skipped `init` instance is disposed directly.

Methods inside an `impl` may have type parameters of their own, including a parameter pack, and calls to them can
give the type arguments or have them inferred from the arguments. This is what `std/rc.strata` needs:

```c
impl rc<T>
{
    void init(ref rc<T> self) { self.ptr = null; self.refcnt = null; }

    void init<TArgs...>(ref rc<T> self, TArgs... args)   // a generic constructor
    {
        self.create_ref(args...);                         // TArgs inferred from args[0], args[1], ...
    }

    @private
    void create_ref<TArgs...>(ref rc<T> self, TArgs... args)
    {
        *T obj_ptr = ...;
        obj_ptr.init(args...);                            // T's own (plain) init, placed through the pointer
    }
}

struct Box2<T> { T value; }
impl Box2<T>
{
    U convert<U>(const ref Box2<T> self) { return (U)self.value; }
    void set_all<U>(ref Box2<T> self, U value) { self.value = (T)value; }
}

int main()
{
    rc<Foo> a = rc<Foo>(1, 2.5f);           // generic init: TArgs = int, float
    Box2<int> b = Box2<int> { .value = 3 };
    float f = b.convert<float>();           // explicit: U appears only in the return type
    b.set_all(2.0);                         // inferred: U = double
    return 0;
}
```

## Approach

Everything stays in **Monomorphize**, before sema, like the rest of generics: each generic method gets one ordinary
method per distinct type argument list, added to the impl it came from. Sema, the move checker and codegen see plain
methods.

The hard part is that a method call can only be matched to its impl once the receiver's type is known, and inferring
type arguments needs the arguments' types — and Monomorphize runs before type checking. So Monomorphize gets a small
**declared-type lookup** (Phase 2): it tracks the declared types of parameters, locals and struct fields while it
walks a body, and can type the simple expressions that matter here (names, fields, literals, casts, constructor
calls). Anything it can't type is left alone unless it is the receiver of, or an argument that must be inferred for,
a generic method — then it is an error asking for a declared type or explicit type arguments. Code that doesn't use
generic methods is unaffected.

The rejected alternative is instantiating inside sema on demand. That would type everything exactly, but sema's
passes are whole-module phases (type registry, impl binding, init wrappers, mangling) that would all have to accept
new structs and functions mid-way. It is a much larger change; the lookup here covers the cases above and can be
replaced later.

Free generic functions get inference from the same machinery (Phase 5), removing today's
"needs explicit type arguments" error wherever the arguments can be typed.

## Semantics

- A method template is any method in an `impl` (generic or not) with `<...>` after its name. The last parameter may
  be a pack (`docs/parameter-packs-plan.md`). In a generic impl, the method's parameter names must differ from the
  impl's ("type parameter 'T' of method 'm' shadows the impl's type parameter 'T'"). The impl's own parameters are
  substituted when the impl is instantiated; the method's stay until the method is.
- Not allowed (diagnosed): generic `drop`, generic properties, generic extern methods, `@export` methods.
- **Naming.** An instance of method `m` on handle `H` with arguments `A..` is a method with `methodName`
  `m<A..>` (canonical spelling, as for free functions) and `name` = `mangledName` = `H_m<A..>`. The exception is
  `init`: its instances keep `methodName` `init` and `name` `H_init`, so they join `init`'s overload set and
  `BindInitMethods` builds a constructor wrapper and a `__place_H` for each one like any other `init` overload.
- **Overloads.** A handle may have at most one template per method name ("generic method 'm' cannot be
  overloaded"). Apart from `init`, a template may not share its name with a plain method of the same handle ("generic
  method 'm' cannot share its name with another method of 'H'"). `init` may have plain overloads beside its template:
  an `init` instance whose parameter types equal a plain `init` overload's is not made (the plain one is used), so
  `rc<Foo>()` with an empty pack doesn't clash with the plain `init(ref rc<T> self)`.
- **Calls.** For `recv.m(args)` / `recv.m<A..>(args)` / `H.m<A..>(args)` (static) where `m` names a template on
  the receiver's handle: type arguments are the explicit ones, or else inferred; the instance is made; the call's
  `callee` becomes the instance's `methodName` (`m<A..>`) and its type arguments are cleared. Sema then resolves it
  as a call to an ordinary method. Calls through `^H`, `H?` and `*H` receivers work the same (the handle is the
  innermost type), as do `ref` parameters and fields.
- **Constructors.** `H(args)` / `H<..>(args)` (after the struct-call rewrite), and `p.init(args)` with `p` a `*H`,
  where `H`'s impl has an `init` template: infer the arguments from `args` (skipping `self`), make the instance, and
  leave the call alone (sema's overload resolution picks the wrapper).
- **Inference** (v1): unify each parameter type of the template (after `self`, when the call passes a receiver)
  with the argument's type. A parameter whose type is exactly a type parameter `U` (with any `ref` / `const`) binds
  `U`; the pack rest parameter `TArgs... args` binds the pack to the remaining arguments' types, one each. Any other
  parameter that mentions a type parameter (`U[]`, `^U`, `pair<U,int>`) does not infer in v1. Errors:
  - an argument the lookup can't type, where its type is needed: "can't infer type argument 'U' of 'm': the type of
    this argument isn't known before type checking; write 'x.m<...>(...)'";
  - a type parameter no argument binds (e.g. `U` only in the return type): "can't infer type argument 'U' of 'm':
    write 'x.m<...>(...)'";
  - two bindings that differ: "type argument 'U' of 'm' is inferred as both 'int' and 'float'".
  The bound type is the argument's type with top-level `const` dropped.
- **Unknown receiver.** If the receiver can't be typed and *some* handle has a template named `m`: "can't tell the
  type of the receiver of 'm' before type checking; give it a declared type". If no template is named `m` anywhere,
  the call is left for sema exactly as today.

## Phase 1 — parsing (`src/Parse/`)

- `ParseDecl.c:1237`: replace "type parameters go on the impl, not on method" with acceptance; keep the method's
  `genericParams` and `genericPack`. Reject on extern methods, `@export`, `drop`, and properties as listed above.
- `ParseExpr.c:540` (the `.member(` postfix call): after the member name, a `<` starts a speculative type-argument
  list exactly as in `ParseNamedPrimary` (save the lexer state, `ParseTypeArgsAfter(..., allowEmpty = true)`, keep
  it only when `(` follows, else roll back), so `a.b < c` is still a comparison. Fill `typeArgs`, `typeArgCount`,
  `typeArg`, `hasTypeArgList`.
- Unit tests in `tests/unit/GenericTests.c`: generic method declarations (with and without a pack, in generic and
  plain impls), the rejects, `x.m<int>(1)`, `T.m<int,float>()`, `x.m<>()`, and that `a.b < c`, `a.b < c && d > e`
  and `f(a.b < c, d > e)` still parse as comparisons.

## Phase 2 — declared-type lookup (`src/Sema/Monomorphize.c`, or a new `src/Sema/MonoTypes.c`)

A scope stack on `Mono`: `Vec scopes` of `StrMap` (name → `const TypeName*`).

- `RewriteNode(NodeFunction)`: push a scope with each parameter (types already rewritten), pop after the body.
  `NodeBlock`: push / pop. `NodeVarDecl`: after rewriting its type and init, bind the name to its declared type, or
  for `var x = e` to the lookup's type of `e` if it has one. `NodeFor`: its own scope around init / body.
  Unrolled pack-loop `const` locals are ordinary var decls by then.
- `static bool MonoTypeOf(Mono* m, const Node* e, TypeName* out)`:
  - `IdentExpr`: innermost binding; a name that is a struct (plain or instance) is a type, not a value (static call
    receiver — return it via a separate `MonoStaticHandle` helper);
  - `MemberExpr`: base typed, unwrapped through `^` / `?` / `*`, then the field's declared type from the struct decl
    (`mod->structs`, which holds instances as they are made);
  - literals: `IntLiteral` → `int` / `uint` / `long` / `ulong` from `isUnsigned` / `isLong`; `FloatLiteral` →
    `float` or `double` per its suffix flag (check the struct); `StrLiteral` → `string`; `BoolLiteral` → `bool`;
  - `CastExpr` → its target type; `UnaryExpr` `-` / `!` / `~` → the operand's type (`!` → `bool`);
  - `CallExpr` constructing a struct (`H(...)`, callee names a struct) → `H`; a call to a free function with exactly
    one non-generic declaration of that name → its return type;
  - `StructInitExpr` → its type;
  - anything else → false.
- Unit-test the lookup through Monomorphize behaviour (Phase 4 cases); no separate API.

## Phase 3 — method templates and instantiation (`src/Sema/Monomorphize.c`)

- `Mono` gains `StrMap methodTemplates` (key `H.m` → `FunctionDecl*`) and `StrMap methodInstances` (key `H.m<A..>`
  → `FunctionDecl*`), plus a `StrMap genericMethodNames` (`m` → non-NULL) for the unknown-receiver check.
- **Collect.** For each plain impl at the start, and for each impl instance as `InstantiateStruct` makes it: move
  methods with `genericParams` out of `impl->methods` and out of `mod->functions` (or don't push them, for instances)
  into `methodTemplates` and `mod->genericTemplates` (templates inside a template impl are freed with it; make sure
  every node is disposed exactly once — ASan). Run the overload / shadowing / rejected-kind checks here. A template
  method in a generic impl *template* is never registered itself — only its copies inside impl instances are.
- **InstantiateMethod(m, impl, tmpl, args, count)**: like `InstantiateFunction` (fixed params + pack `TypeSubst`),
  then set `methodName` / `name` / `mangledName` per Semantics, `fromImpl` stays set, clear `genericParams` /
  `genericPack`, push onto `impl->methods`, `mod->functions` and `pending`. Count `TakeInstanceSlot` /
  `WithinLimits` like other instances. For `init`, first compare the instance's parameter types with each plain
  `init` of the impl and skip (dispose the clone) on a match.
- **Calls** (`RewriteCall`, the `calleeBase` branch, after rewriting the base and arguments): find the handle
  (static `H.m`, or `MonoTypeOf(base)` unwrapped), look up `H.m`; when found, take or infer the arguments, check the
  count (at least the fixed count with a pack, exactly the count without), instantiate, rename the callee. The
  unknown-receiver error applies only when `genericMethodNames` has `m`.
- **Constructors**: in `RewriteCall` after `RewriteGenericStructCall` / for a callee naming a plain struct, and for
  `p.init(...)` on a `*H` receiver: when `H.init` is a template, infer from the arguments and instantiate (call left
  unchanged).
- The rewrite of an instance's body may request more instances (generic methods calling each other); the work list
  already handles that, and the depth / count limits stop runaway recursion.

## Phase 4 — tests and docs

Conformance cases (`tests/conformance/README.md` for headers; `Report()` + `END live=0` when owning types appear):

accept (`expect: run` with `expect-output` lines):
- `generic_method_explicit.strata`: `b.convert<float>()`, static `H.make<int>()`, `x.m<>()` with an empty pack.
- `generic_method_inferred.strata`: inference from locals, params, fields, literals, casts; through `ref`, `^H`
  and `*H` receivers.
- `generic_method_in_generic_impl.strata`: `Box2<T>` above, with two struct instances and two method instances each.
- `generic_method_pack.strata`: a pack method forwarding `args...` to another pack method on `self`.
- `generic_method_init.strata`: a generic `init` beside a plain one; `H(1, 2.5f)`, `H()` (plain), and
  `p.init(1, 2.5f)` through a pointer; an `rc`-like holder whose generic init calls a private generic method that
  places a `T` through a pointer (the shape of `std/rc.strata`).
- `generic_method_owning.strata`: a generic method taking `^Res` / `string` by value; `END live=0`.

reject (`expect: reject "..."`), one per diagnostic in Semantics plus: generic `drop`, generic property, generic
extern method, wrong explicit count, a method type parameter shadowing the impl's.

Docs: a "Generic methods" subsection under "Generics" in `README.md`; update the generics-plan "Out" list (generic
methods are no longer out); a Status section at the top of this file listing deviations.

## Phase 5 — inference for free generic functions

In `RewriteCall`, a call to a free function template without `hasTypeArgList` infers its arguments with the same
unification instead of failing; the existing "needs explicit type arguments" message becomes the "can't infer" one
when inference fails. Accept cases: `make_number(3)`, `print_all(1, 2.0f, "x")` with a pack, an inferred call inside
another generic function. Reject: an uninferable call. Existing reject tests that expect the old message are updated.

## Afterwards

Fix `std/rc.strata` (its `__refcnt` methods also take `ref __refcountimpl self`, which should be `__refcnt`) and
check that it compiles in a small program.

`ctest --test-dir build` must stay fully green; run `clang-format -i` on every touched file.
