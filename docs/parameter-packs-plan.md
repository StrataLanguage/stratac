# Parameter packs plan

## Status

Phases 1-4 are implemented: parsing, expansion in the clone, folding and instantiation in Monomorphize, the unit
tests, the conformance cases and the README section. Deviations from the plan as written:

- An out-of-range literal index (`args[2]` of a 2-element pack) is not reported when cloned. The clone runs before
  an `if` on `args.length` is folded, so a guard such as `if (args.length > 2) { args[2] }` must not fail. The
  element stays `args[2]`, and sema reports it as an unknown variable if the code is kept. The message is weaker
  than the planned "pack index N is out of range" text.
- Other pack misuse inside an `if` that folds away is still reported, since those errors are raised by the clone
  too. Only the literal index is deferred.
- An empty `<>` is accepted only on the call path. A struct literal `Name<> { ... }` falls back to a comparison
  and is a parse error, as before.
- A pack used as a type argument with an empty expansion (`pair<TArgs...>` with no elements) keeps its base name,
  so RewriteType reports the arity mismatch of the generic struct instead of treating it as a plain name.
- The `CollectFunctionTemplates` pack check from the plan is not needed: a rest parameter is the only way a pack
  becomes a value, and the parser already keeps the pack last.
- The `args__0` fallback was not needed; `args[0]` is accepted as a parameter name throughout.

The last type parameter of a generic function may be a **parameter pack**, written `Name...`. It binds any number
of type arguments (including none). A rest parameter typed with the pack binds one value per pack type.

```c
void forward<T, TArgs...>(TArgs... args)
{
    T.consume(args...);              // expands to T.consume(args[0], args[1], ...)
}

void each<TArgs...>(TArgs... args)
{
    for (uint i = 0; i < args.length; i++)   // unrolled: one copy of the body per pack element
    {
        print_one(args[i]);
    }
}

void print_all<T, Rest...>(T first, Rest... rest)
{
    print_one(first);
    if (rest.length > 0)             // folds: the dead branch is never instantiated
    {
        print_all<Rest...>(rest...);
    }
}

int main()
{
    each<int, float, string>(1, 2.5f, "three");
    forward<Sink>();                 // empty pack
    each<>();                        // empty pack, no fixed parameters
    print_all<int, float>(1, 2.0f);
    return 0;
}
```

Like the rest of generics this is done entirely in **Monomorphize**, before sema: an instance of a pack function
is an ordinary function with a fixed parameter list. Sema and codegen are unchanged.

## Semantics

Instantiating `f<A1..An, P1..Pk>` where `f` has `n` fixed type parameters and a pack `TArgs`:

- `TArgs` binds `P1..Pk` (`k >= 0`). The call must give at least `n` type arguments. Type arguments are still
  explicit (no inference), as for every generic function, except that a call writing only the fixed type
  arguments (`make_ref<T>(a, b)`) infers the pack from the arguments.
- The canonical name is the usual flat spelling of all arguments: `f<int,float,string>`, `f<>` for an empty one.
- The rest parameter `TArgs... args` (with optional `ref` / `const`) becomes `k` ordinary parameters named
  `args[0]` … `args[k-1]`, of types `P1..Pk`, each with the rest parameter's modifier and constness. The instance
  is not variadic. (The `args[i]` spelling makes diagnostics read like the source; if anything downstream chokes on
  `[` in a parameter name, use `args__0` instead and note it here.)
- In the body:
  - `args...` as a call argument (free calls, method calls `x.m(args...)`, `T.m(args...)`, struct constructor
    calls) splices `args[0], …, args[k-1]` into the argument list at that position.
  - `TArgs...` as a type argument (`g<TArgs...>`, `g<int, TArgs...>`, `pair<TArgs...>`) splices `P1..Pk`.
  - `args.length` is the integer literal `k`.
  - `args[N]` with an integer literal `N` is the parameter `args[N]`; `N >= k` is an error.
  - `args[i]` where `i` is the variable of an enclosing **pack loop** (below) is `args[<current iteration>]`.
  - Any other use of `args` or `TArgs` is an error.
- **Pack loop**: a `for` whose condition is `<ident> < P.length`, `P` being the pack value. It must have the shape
  `for (<type or var> i = <int literal>; i < P.length; <i++ | ++i | i += 1 | i = i + 1>) body`. It is unrolled:
  the `for` is replaced by a block holding, for each `j` from the start literal up to `k - 1`, a block
  `{ const <type> i = j; <body with P[i] → P[j]> }`. `break` / `continue` directly inside a pack loop body (not
  inside a nested ordinary loop) are errors in v1. Pack loops may nest.
- **Pack-length folding**: an `if` or `?:` whose condition mentions a pack `.length` literal is folded like a meta
  condition, so only the branch taken is kept and instantiated. This gives recursive packs their base case.

## Out of scope (diagnosed)

- Packs on structs, unions and impls: "parameter packs are only supported on generic functions".
- A pack that is not the last type parameter: "a parameter pack must be the last type parameter".
- Pattern expansions: `f(args)...`, `^TArgs...`, `{args...}`. Only a bare `name...` expands.
- `TArgs.length`, indexing a type pack, packs in `@meta` functions, inferring pack arguments.

## Phase 1 — AST and parsing

AST (`src/AST/AST.h`, `AST.c`, `src/Codegen/ASTDump.c`):

- `FunctionDecl::genericPack` (`bool`): the last name in `genericParams` is a pack.
- `TypeName::isPackExpansion` (`bool`): a type argument written `Name...`.
- `CallExpr::hasTypeArgList` (`bool`): the call wrote `<...>`, even an empty `<>`. Set wherever `typeArgs` is set.
- New `NodePackExpand` / `PackExpandExpr { Node base; char* name; }`: `name...` in a call argument list. Add it to
  `AstDispose`, `ASTDump`, `AstCloneSubst` (which aborts on an unknown kind), and every exhaustive `switch` over
  `NodeKind` the compiler warns about (Monomorphize `RewriteNode`, sema, move checker, CFG, codegen — sema and later
  never see it, so those cases can be unreachable/error, but they must compile without `-Wswitch` warnings).

Parser:

- `ParseTypeParamList` (`ParseDecl.c`) accepts `Name...` for a parameter and reports it through a new
  `bool* outPack` out-parameter. Not last → "a parameter pack must be the last type parameter". Struct / union /
  impl callers reject a pack with "parameter packs are only supported on generic functions".
- `ParseTypeArgList` (`ParseType.c`): after each argument, a `TokDotDotDot` sets `isPackExpansion` (only on a bare
  leaf name; otherwise fail the parse quietly, as the list is parsed speculatively). Add a `bool allowEmpty`
  parameter: the call path in `ParseNamedPrimary` (`ParseExpr.c`) passes true so `f<>()` parses; type positions pass
  false. `TypeNameGenericSpelling` spells an expansion as `Name...`.
- `ParseCallArgs` and the method-call argument loop in `ParseExpr.c` (both push to `call->args`): an argument that
  is a bare identifier followed by `...` becomes a `PackExpandExpr`.
- Unit tests in `tests/unit/GenericTests.c`: `<T, U...>` and `<U...>` on functions, the pack-position and
  struct/impl errors, `g<int, U...>(a...)`, `f<>()`, and that `a < b` comparisons and existing variadic forms
  (`int... rest`, extern `...`) still parse.

## Phase 2 — expansion in the clone (`src/AST/ASTClone.c`, `ASTClone.h`)

Extend `TypeSubst` (zeroed fields mean "no pack", so struct / impl / existing callers are unchanged):

```c
typedef struct PackLoop
{
    const char* var;   // the loop variable
    size_t index;      // the iteration being cloned
    const struct PackLoop* outer;
} PackLoop;

typedef struct
{
    const Vec* params;          // fixed type parameter names (the pack name is NOT in here)
    const TypeName* args;
    const char* packType;       // "TArgs", NULL when the template has no pack
    const char* packValue;      // "args": the rest parameter typed with the pack, NULL if none
    const TypeName* packArgs;   // the pack's types
    size_t packCount;
    DiagnosticEngine* diag;     // pack misuse is reported here
    const PackLoop* loop;       // innermost pack loop being unrolled
    bool inPackLoopBody;        // directly inside a pack loop body (break/continue are errors)
} TypeSubst;
```

- `SubstTypeArgs` returns the new count and splices `packArgs` for an `isPackExpansion` arg naming `packType`.
  Update its callers (`TypeNameSubstitute`, `CloneCall`, struct init, …) to use the returned count and rebuild the
  canonical name.
- `TypeNameSubstitute`: a leaf naming `packType` that is not being spliced → error "parameter pack 'TArgs' must be
  expanded: write 'TArgs...'".
- `CloneFunction`: the rest parameter whose array element type is the leaf `packType` becomes `packCount`
  `ParamDecl`s as described above; clear `isVariadic` on the copy.
- `CloneNodeList` for call arguments: a `PackExpandExpr` naming `packValue` splices `IdentExpr`s `args[0..k-1]`.
  Other `PackExpandExpr`s are copied as they are (Monomorphize reports them).
- `CloneMember`: `packValue.length` → `IntLiteral` with value `k` and a new `IntLiteral::fromPackLength = true`.
- `CloneIndex`: base `IdentExpr` naming `packValue` → `IdentExpr` `args[j]`, `j` from an int literal index or from
  the `PackLoop` whose `var` the index names; anything else → "a parameter pack can only be indexed by an integer
  literal or by the variable of a 'for' loop over 'args.length'"; out of range → "pack index N is out of range:
  'args' has K element(s)".
- `CloneIdent`: naming `packValue` or `packType` (not handled by a parent above) → "parameter pack 'args' can only
  be used as 'args...', 'args.length' or 'args[i]'".
- `CloneFor`: a pack loop (condition `<ident> < packValue.length`) is unrolled as in Semantics, checking the init
  and update shapes ("a 'for' loop over a parameter pack must be written 'for (T i = 0; i < args.length; i++)'").
  Each body copy is cloned with a `TypeSubst` copy whose `loop` points to a stack `PackLoop` and
  `inPackLoopBody = true`. Ordinary `for` / `while` bodies are cloned with `inPackLoopBody = false`.
- `CloneBreak` / `CloneContinue`: with `inPackLoopBody` → "'break' is not supported inside a 'for' loop over a
  parameter pack" (same for `continue`).
- `CloneIntLiteral` copies `fromPackLength`.

## Phase 3 — Monomorphize (`src/Sema/Monomorphize.c`)

- `CollectFunctionTemplates`: for a template with `genericPack`, check there is at most one rest parameter whose
  element type is the pack (only the last parameter can be a rest parameter, so this is just "is the last param
  that rest parameter"). A pack rest parameter that is also `return` is impossible; nothing else to check.
- `RewriteCall`: a call to a template now needs `hasTypeArgList` (not `typeArgCount > 0`) — keep the existing
  message. Count check: exactly `genericParams.count` without a pack, at least `genericParams.count - 1` with one
  ("generic function 'f' takes at least N type argument(s), but M were given").
- `InstantiateFunction`: with a pack, build the `TypeSubst` with `params` = the fixed names (a temporary `Vec`
  without the last name, or a `count`-limited view), `args` = the first `n` arguments, and the pack fields from the
  rest; `diag = m->diag`; `packValue` = the name of the pack rest parameter if any. Clear `genericPack` on the
  instance.
- `RewriteType`: a `TypeName` with `isPackExpansion` that survives → "'X...' expands a parameter pack, but 'X' is
  not one". `RewriteNode` for `NodePackExpand` → the same message for values. These cover packs used outside a pack
  function.
- `RewriteIf` / `RewriteConditional`: also fold when the condition contains an `IntLiteral` with `fromPackLength`
  (a small walker over unary / binary / conditional / cast / int literal). Confirm `FoldCondition` folds plain
  literal comparisons such as `0 > 0`.

## Phase 4 — tests and docs

Conformance cases (`tests/conformance/README.md` has the header format; end with `Report()` when using `Res`):

accept (`expect: run`, with `expect-output` lines checking real values):
- `generic_pack_example.strata`: the two forms from the top of this file (`T.method(args...)` on a struct with a
  static-style method, and the unrolled `for` calling an overloaded function per element type).
- `generic_pack_empty.strata`: `f<>()`, `g<int>()` with an empty pack, `args.length == 0`.
- `generic_pack_forward.strata`: `outer<TArgs...>(args...)` forwarding to `inner<TArgs...>(args...)`, and
  `g<int, TArgs...>(1, args...)` with a prefix.
- `generic_pack_recursive.strata`: `print_all` with the folded `if (rest.length > 0)` base case.
- `generic_pack_index.strata`: `args[0]`, `args[1]` with literal indices; nested pack loops.
- `generic_pack_ref_const.strata`: `ref TArgs... args` mutating the caller's values, `const TArgs...`.
- `generic_pack_owning.strata`: a pack holding `^Res` / `string` values moved into the function, `END live=0`.

reject (`expect: reject "..."`), one per diagnostic: pack not last, pack on struct, pack on impl, too few type
arguments, bare `args` use, `TArgs` used as a plain type, out-of-range literal index, non-literal non-loop index,
bad pack-loop shape, `break` in a pack loop, `x...` where `x` is not a pack, `T...` type arg where `T` is not a pack.

Docs: a "Parameter packs" subsection under "Generics" in `README.md` (syntax, the expansion forms, the v1 limits),
and a Status section at the top of this file listing deviations.

`ctest --test-dir build` must stay green apart from the pre-existing `style_gate` failure on
`src/Sema/Resolve/Members.c` (an uncommitted user edit; leave it alone). Run `clang-format -i` on every file this
work touches so it adds no new style failures.
