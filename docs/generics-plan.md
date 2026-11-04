# Generics plan

Generic structs, generic `impl` blocks and generic functions, compiled by monomorphization: every distinct
set of type arguments produces its own concrete struct / function, and every `impl` of a generic struct is
specialized along with it.

```c
struct number<T>
{
    T value;
}

impl number<T>
{
    T get_value(const ref number<T> self) { return self.value; }
    void set_value(ref number<T> self, T value) { self.value = value; }
}

number<T> make_number<T>(T value)
{
    number<T> num;
    num.set_value(value);
    return num;
}

int main()
{
    number<int> a = make_number<int>(3);
    number<float> b = number<float> { .value = 1.5 };
    return a.get_value();
}
```

(`set_value` takes `ref`, not `const ref`: writing through a `const ref self` is already an error,
"'self' is immutable".)

## Status

Phases 1-4 are implemented: parsing, substitution cloning, the Monomorphize pass, and the tests, sample and
README section. Deviations from the plan as written:

- Canonical names are also capped at 1024 bytes, in addition to the nesting depth (32) and instance count
  (4096). A longer spelling is reported with the same "is too deep" error as a nesting overflow.
- Properties and extern methods in a generic `impl` are rejected ("property 'P' in a generic impl is not
  supported", "extern method 'M' in a generic impl is not supported").
- AOT is verified on macOS arm64 only.

## Approach

A new pass, **Monomorphize**, runs on the parsed `Module` *before* any semantic analysis. It clones each
generic template once per distinct argument list, substituting the type parameters, and rewrites every use
(`number<int>`) into a plain named type whose name is the canonical spelling `number<int>`. Generic
templates are then removed from the module's decl lists. Sema and codegen never see a type parameter or a
template: they see ordinary structs named `number<int>`, ordinary impls on `number<int>`, ordinary functions
named `make_number<int>`. No change to sema type rules, moves, borrows or codegen is expected.

The existing `extern<T>` generic-extern mechanism (`FunctionDecl::typeParam`, `Module::genericInstances`,
`Sema/Resolve/Calls.c`) is separate and stays as it is. Calls that name a generic *extern* are left for sema
exactly as today.

### Canonical names

`base<arg1,arg2>` using each argument's `TypeName::name`, comma-separated, **no spaces**:
`number<int>`, `pair<int,float>`, `number<^Foo>`, `number<int[]>`, `number<number<int>>`.
This string is the struct name, the `TypeName::name` of every use, the impl `handleName`, and the prefix of
specialized method names (`number<int>_get_value`). Specialized free functions are named
`make_number<int>` (both `name` and `mangledName`). LLVM accepts these characters in symbol names; the
conformance suite (AOT compile + C link) confirms it. If a platform rejects them, sanitize only the
emitted symbol in `ExportedSymbolName`, not the AST names.

## v1 scope

In:
- Type parameters on `struct` / `union`, on `impl`, and on free functions; any number of parameters.
- Type arguments anywhere a type is written (locals, params, returns, fields, globals, casts, `^`/`?`/`*`/
  `[]`/`[N]` wrappers inside and outside the arguments), nested (`number<number<int>>`, which needs `>>`
  split into two `>`).
- Explicit type arguments on calls: `make_number<int>(3)`, `pair_of<int,float>(1, 2.0)`.
- Struct literals: `number<int> { .value = 3 }`.
- Every `impl` of a generic struct is specialized with each instance of the struct (eagerly, all methods).
- Type parameters used as values where a type name is allowed as an identifier (e.g. `sizeof(T)`,
  `T.max`): an `IdentExpr` naming a parameter is replaced by the argument's name.
- Generic code in an imported module used from another module (the pass runs on the merged module).

Out (diagnosed, not silently wrong):
- Inferring a type argument that no argument's declared type fixes (`make_none()` with `T` only in the
  result): error "can't infer type argument 'T' of 'make_none': write 'make_none<...>(...)'". Inference from
  the arguments is in docs/generic-methods-plan.md (Phase 5).
- Overloading a generic function name (two templates with one name, or a template and a plain function with
  one name).
- `extern struct`, `@component`, struct aliases (`struct Foo<T> = ...`) and `handle`s with type parameters.
- Constraints / bounds on type parameters. A body that doesn't type-check for some argument is reported by
  sema at the template's source location when that instance is created.

## AST changes (`src/AST/AST.h`, `src/AST/AST.c`)

- `TypeName`: `char* genericBase; struct TypeName* typeArgs; size_t typeArgCount;` — set only for a use with
  type arguments (`number<int>`); `name` is then the canonical spelling. After Monomorphize no `TypeName`
  has `typeArgCount > 0`.
- `StructDecl`: `Vec genericParams;` (`char*`). Non-empty = template.
- `ImplDecl`: `Vec genericParams;` (`char*`), the names bound by `impl number<T>`. `handleName` stays the
  base (`number`).
- `FunctionDecl`: `Vec genericParams;` (`char*`). Distinct from the existing `typeParam` of `extern<T>`.
- `CallExpr`: `TypeName* typeArgs; size_t typeArgCount;` alongside the existing `typeArg`. The parser sets
  `typeArg = &typeArgs[0]` when there is exactly one, so the generic-extern path is unchanged.
- `StructInitExpr`: `TypeName* genericType;` — the full `number<T>` type when the literal has type
  arguments; `typeName` is its canonical spelling.
- `Module`: `Vec genericTemplates;` (`Node*`) — templates moved out of the decl lists by Monomorphize, kept so
  `AstDispose` frees them. Dispose them in `DisposeModule`; an impl's methods live in its `methods` only once
  removed from `Module::functions`, so make sure each node is freed exactly once (ASan runs in ctest).
- `AstDispose` frees the new `Vec`s.
- `ASTDump` prints type parameters on decls.

## Phase 1 — parsing (`src/Parse/`)

1. **Type arguments in types** (`ParseType.c`). After an identifier base name, `<` starts a type-argument
   list: one or more types separated by `,`, closed by `>`. A `>>` token closes two lists: consume half of it
   (replace the current token with a `TokGt` whose range starts one character later) — check how the lexer
   and `LexerCheckpoint` represent the current token before choosing how. Fill `genericBase`, `typeArgs`,
   `typeArgCount`, and set `name` to the canonical spelling. Postfix wrappers (`?`, `[]`, `[N]`) and prefix
   `^`/`*` apply to the whole `number<int>` exactly as they do to a plain name.
   Type parsing is also used speculatively (`LooksLikeVarDecl`, casts, `ParseNamedPrimary`): when the list
   doesn't close, the parse must fail **without emitting diagnostics** so the caller's rollback leaves
   `a < b` parsing as a comparison. Check how existing speculation avoids diagnostics.
2. **Type parameter lists on declarations** (`ParseDecl.c`): a shared helper parsing `<Ident, Ident, ...>`
   into a `Vec` of `char*`, rejecting duplicates and builtin type names.
   - `struct Name<T> { ... }` / `union Name<T> { ... }` → `StructDecl::genericParams`. Reject on `extern
     struct`, aliases, and forward declarations.
   - `impl Name<T> { ... }` → `ImplDecl::genericParams`.
   - `R name<T>(params) { ... }` — in `ParseSignatureHead`/`ParseFunctionWithAttributes`, a `<` right after
     the name → `FunctionDecl::genericParams`. Reject on `extern` and `@export` functions. Inside an impl,
     reject with "type parameters go on the impl, not on method 'x'".
3. **Calls and literals** (`ParseExpr.c`, `ParseNamedPrimary`): parse a comma-separated type list after
   `Name<`, fill `typeArgs`/`typeArgCount` (and `typeArg` when one). If `{` follows the `>`, build a
   `StructInitExpr` with `genericType` set and `typeName` = canonical name. Otherwise roll back as today.
4. Unit tests in `tests/unit/ParserTests.c` (or a new `tests/unit/GenericTests.c`, added to
   `CMakeLists.txt` next to the other unit test files): each declaration form, nested `>>`, wrappers around
   and inside arguments, multi-argument calls, generic struct literals, and that `a < b`, `a < b && c > d`,
   `f(a < b, c > d)` still parse as comparisons.

At the end of phase 1 the tree builds and all existing tests still pass; generic code parses but isn't
compiled yet.

## Phase 2 — clone with substitution (`src/AST/ASTClone.c`, `src/AST/ASTClone.h`)

```c
typedef struct
{
    const Vec* params;        // char*: type parameter names
    const TypeName* args;     // one per param
} TypeSubst;

TypeName TypeNameSubstitute(Arena* arena, const TypeName* t, const TypeSubst* s);
Node* AstCloneSubst(Arena* arena, const Node* n, const TypeSubst* s);
```

- `TypeNameSubstitute` rebuilds a type tree: a leaf naming a parameter becomes a copy of that argument
  (keeping the use's `range`, OR-ing in its `isConst`); wrappers are rebuilt with the `TypeName*Wrap`
  helpers so `name` strings are recomputed (`^T` → `^int`); a generic use (`typeArgCount > 0`) has its
  arguments substituted and its canonical `name` rebuilt. Fixed-array `length` / `lengthName` and
  `isVector` / `primitiveType` carry over (recompute `primitiveType` for a substituted leaf with
  `GetPrimitiveType`).
- `AstCloneSubst` deep-copies every node kind in `NodeKind` (statements, expressions, `FunctionDecl` with its
  params, `StructDecl` with its fields and field default expressions, `ImplDecl` with methods and
  properties), substituting every `TypeName` it holds (var decls, params, return types, casts, globals,
  fields, call `typeArgs`, `StructInitExpr::genericType` + `typeName`, property return types) and replacing
  an `IdentExpr` whose name is a parameter with the argument's `name`. Every `Vec` is freshly allocated
  (the clone is disposed independently of the template). Sema-only fields are zero in the copy.
- `src/AST/ASTClone.c` is added to the library sources in `CMakeLists.txt`.
- Unit tests: clone a parsed generic function and struct, substitute `T → int` and `T → ^Foo`, check the
  types and names in the clone and that the template is unchanged; dispose both under ASan.

## Phase 3 — the Monomorphize pass (`src/Sema/Monomorphize.c`, `src/Sema/Monomorphize.h`)

`void Monomorphize(Module* mod, DiagnosticEngine* diag, Arena* arena);` called first thing in
`ResolveOverloads` (`src/Sema/Resolve/Decls.c`), so the JIT, AOT, `stratac` and unit tests all get it.
If it reports errors, `ResolveOverloads` returns without running the rest.

1. **Collect templates.** Structs with `genericParams` (by name), impls with `genericParams` (by
   `handleName`; several impls per struct are allowed), functions with `genericParams` (by name). Move them
   all to `Module::genericTemplates`, removing them from `structs` / `impls` / `functions` (an impl's methods
   are also listed in `functions`: remove those too). Diagnose: an impl with type parameters on a
   non-generic struct, an impl of a generic struct without parameters, parameter-count mismatch between
   `impl` and `struct`, two templates with one name, a template and a non-generic decl sharing a name.
2. **Rewrite concrete code.** Walk every remaining decl — struct fields (and default expressions),
   function signatures and bodies, impl methods and property types, globals, delegates. For each
   `TypeName` with type arguments (rewriting inner arguments first, bottom-up): look up the template by
   `genericBase`, check the argument count, request the instance by canonical name, then turn the node into
   a leaf (`TypeNameLeaf(canonical)`, keeping `range` and `isConst`). For each `CallExpr` whose callee is a
   generic function template: require type arguments, request `callee<args>`, set `callee` to that name and
   clear `typeArg`/`typeArgs`. For a `StructInitExpr` with `genericType`: request it and set `typeName`.
   Diagnose: a generic struct named without arguments, arguments on a non-generic type, a generic function
   called without arguments, wrong argument count, and a `TypeName` naming a type parameter that escaped
   (shouldn't happen; assert-level error).
   Calls with type arguments to a name that is *not* a generic template are left untouched for sema (generic
   externs).
3. **Instantiate on request** (a worklist; each canonical name is created once, looked up in a `StrMap`):
   - Struct `number<int>`: `AstCloneSubst` the template with its params → args; set `name` to the canonical
     name, clear `genericParams`, append to `mod->structs`. Then, for every impl template of `number`, clone
     it with *that impl's* params → the same args, set `handleName` to the canonical name, rename each method
     `name = mangledName = "<canonical>_<methodName>"`, append the impl to `mod->impls` and its methods to
     `mod->functions`.
   - Function `make_number<int>`: clone, set `name`/`mangledName` to the canonical name, clear
     `genericParams`, append to `mod->functions`.
   - New decls are pushed onto the worklist and rewritten by step 2 (which may request more instances).
4. **Limits.** Stop with an error when an instance's type-argument nesting is deeper than 32 or more than
   4096 instances exist ("generic instantiation of 'x' is too deep (recursive generic type?)"), so
   `struct list<T> { list<list<T>>? next; }` terminates.
5. Diagnostics raised later by sema inside an instance point at the template's source, which is the clone's
   `range`. Good enough for v1.

## Phase 4 — tests, samples, docs

- Conformance cases (`tests/conformance/accept/generic_*.strata`, `reject/generic_*.strata`; see
  `tests/conformance/README.md` for the header format): the example above; two instances of one struct;
  multiple type parameters; nested generics (`number<number<int>>`); generic struct holding an owning type
  (`number<string>`, `number<^Res>`, checking `END live=0` via `Report()`); generic function calling another
  generic function; impl method calling another impl method; generic `drop` method on a generic struct;
  array/optional/box wrappers inside and outside arguments; generic code from an imported module if the
  harness allows it. Reject cases for each diagnostic in phases 1 and 3, plus a body that doesn't type-check
  for one argument (`T + 1` with `T = string`).
- `samples/generics.strata` with the example.
- A "Generics" section in `README.md` or `docs/` describing the syntax and the v1 limits.
- `ctest` fully green, including the ASan cases.

## Follow-ups (not in this work)

- Inferring type arguments from argument types: done by a declared-type lookup in Monomorphize, not sema-time
  instantiation (docs/generic-methods-plan.md).
- Constraints, generic aliases, generic `@component`s.
- Lazy method instantiation (only methods that are called).
