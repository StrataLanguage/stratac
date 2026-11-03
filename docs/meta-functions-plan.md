# Meta functions plan

Meta functions answer questions about types at compile time, while generics are resolved. They are called
with an `@` prefix, and every call folds to a constant before semantic analysis.

```c
@meta
bool droppable(type T)
{
    // Whether T's impl defines a method `drop`.
    return @has_function(T, drop);
}

void free_object<T>(*T ptr_value)
{
    if (@droppable(T))
    {
        ptr_value.drop();
    }
}
```

## Status

Phases 1-4 are implemented. The conformance cases are `tests/conformance/{accept,reject}/meta_*` and
`ptr_method_*`. Where the implementation differs from phases 1-2 as first written:

- Any constant condition that contains a meta call prunes, including int comparisons and arithmetic, not
  only the bool forms listed below.
- A pruned branch that is not a block is wrapped in a `Block`.
- Negative results are emitted as `-literal`.
- Locals and parameters are range-checked on store.
- A `ulong` value above `INT64_MAX` is out of range for the evaluator.
- An uncalled meta function is checked for constructs only, not for types.
- Meta calls also work in global initializers, field defaults and enum values.

Phase 3 (method calls through pointers): `p.m(args)` on a `*S` wraps the receiver in an `UnDeref` built by
`DerefReceiver` (`src/Sema/Resolve/Members.c`), checked by `ResolveAddressUnary`, so codegen needs no
special case and a null receiver panics as `*p` does. Pointer-to-pointer and pointer-to-builtin receivers
get the usual "has no method" error, and a pointer to an opaque handle or an owning type is rejected by the
deref rule. Not done: `const *S` does not stop a `ref` method from mutating the pointee; `*p = v` and
`Bump(*p)` have the same hole today.

## Semantics

- **Separate namespace.** `@meta` functions live in `Module::metaFunctions`, not `Module::functions`. A plain
  function `hello` and a meta function `@hello` can coexist. `hello(...)` never finds the meta function and
  `@hello(...)` never finds the plain one. Meta functions are never emitted by codegen.
- **Compile-time only.** Every `@name(args)` call is evaluated by the Monomorphize pass
  (`src/Sema/Monomorphize.c`) and replaced with a `BoolLiteral` or `IntLiteral`. Inside a generic template
  it's evaluated per instance, after `T` has been substituted. Sema and codegen never see a meta call.
- **Dead branches are dropped.** If an `if` condition contains a meta call and folds to a constant, the
  `if` is replaced by the branch it takes (or an empty block) before that branch is rewritten. So
  `ptr_value.drop()` is never type-checked for a `T` without `drop`. The same goes for `cond ? a : b`. An
  `if (false)` with no meta call in it stays as it is today.
- **`type`.** A contextual type name, not a keyword: a parameter written `type T` in a `@meta` function binds
  a bare type. Writing `type` anywhere else is an error: "'type' is only allowed for parameters of @meta
  functions". A struct, alias, enum or handle named `type` is an error: "'type' is reserved".

### Built-in meta functions

| Call | Result |
|---|---|
| `@has_function(T, name)` | `true` if the struct or handle `T` names has an impl method whose `methodName` is `name` (in any impl, including impl methods with `extern`). For a generic instance (`number<int>`) look at the impl templates of `number`, because instances may not have been created yet. Wrapped types (`^T`, `T?`, `*T`, arrays) and builtin types give `false`. |
| `@is_same(A, B)` | `true` if the two types have the same canonical `name` after substitution, ignoring `const`. Strong aliases are distinct (`@is_same(myint, int)` is `false`). |

Built-in names can't be redefined: a `@meta` function named `has_function` or `is_same` is an error.

### User meta functions

- Declared with the `@meta` attribute on a function with a body. Not `extern`, `@export`, generic,
  variadic, or in an `impl`.
- Parameters: `type` or a bool/integer type. Return type: `bool` or an integer type.
- Body subset that can be evaluated: blocks, `return`, `if`/`else`, bool/integer locals (`VarDecl` with an
  initializer, plain and compound assignment to locals), and expressions made of bool/int literals, locals,
  parameters, unary `!` `-` `~`, binary arithmetic/bitwise/comparison/`&&`/`||` (short-circuit),
  `?:`, and meta calls. Anything else (loops, plain calls, member access, strings, ...) is an error naming
  the construct: "'while' can't be evaluated at compile time in meta function '@f'".
- Integer arithmetic uses 64-bit values. Division or modulo by zero is an error. Results are range-checked
  against the return type.
- Meta functions can call other meta functions. Evaluation depth is capped at 64: "meta function '@f'
  recurses too deeply".
- A meta function that reaches its end without returning is an error.

### Call syntax and arguments

`@name(arg, ...)` is a primary expression: `@` followed by an identifier and `(`. Each argument is parsed
speculatively as a type first. If that succeeds and the next token is `,` or `)`, the argument is stored as a
`TypeName`; otherwise it's parsed as an expression. When evaluating:
- A `type` parameter needs a type argument. A type argument whose leaf name is a bound type parameter of the
  enclosing meta function is substituted.
- A bool/int parameter needs a value. A type argument that is a plain leaf naming a local or parameter of the
  enclosing meta function is read as that variable (`@other(flag)` parses `flag` as a type).
- `@has_function`'s second argument must be a plain identifier (it's parsed as a leaf type; use its name).

Diagnostics: unknown meta function ("unknown meta function '@x'"), wrong argument count, a type passed where
a value is expected and the reverse, `@` with no identifier and `(` after it.

## AST changes

- `NodeMetaCall`: `MetaCallExpr { Node base; char* name; Vec args; }` with `MetaArg { TypeName* type;
  Node* expr; }` (exactly one is set). `AstDispose` frees the args `Vec` and disposes `expr`s.
- `FunctionDecl::isMeta`.
- `Module::metaFunctions` (`FunctionDecl*`). Disposed in `DisposeModule`, released in `AstReleaseModuleLists`,
  initialized wherever `genericTemplates` is, and merged across imports wherever the other decl lists are
  (check `src/Import/ModuleLoader.c`).
- `src/AST/ASTClone.c` clones `NodeMetaCall`, substituting type arguments with `TypeNameSubstitute`.
- `src/Codegen/ASTDump.c` prints meta calls and `@meta` on functions.

## Phase 1 — parsing

- `@meta` in `ApplyFunctionAttributes` (`src/Parse/ParseDecl.c`) sets `isMeta`. Top-level parsing puts a meta
  function in `metaFunctions` instead of `functions`. Diagnose the forbidden combinations listed above at
  parse time where possible (extern, export, generic, impl method, variadic).
- `@name(args)` in `ParsePrimary`/`ParseNamedPrimary` (`src/Parse/ParseExpr.c`). Check that statement parsing
  doesn't treat a leading `@` as an attribute inside function bodies (`@x(T);` as an expression statement can
  be an error later, but `if (@x(T))` and `return @x(T);` must parse).
- Unit tests in `tests/unit/MetaTests.c` (new, registered in `CMakeLists.txt` next to `GenericTests.c`):
  meta declarations, calls with type and value arguments, nested types (`@is_same(number<int>, T[])`),
  `hello` and `@hello` in one module, clone with substitution of a meta call, and the parse errors.

## Phase 2 — evaluation in Monomorphize

- Collect `metaFunctions` into a `StrMap` and check their signatures (parameters, return type, name not a
  builtin, no duplicates). Check that `type` appears only in meta parameters, and that no type is named `type`.
- In the rewrite walk, every expression slot that can hold a meta call is rewritten through a `Node**`, so
  the `MetaCallExpr` can be swapped for a literal. Dispose the replaced node. Evaluate meta calls before
  rewriting the rest of the expression.
- `IfStmt` / `ConditionalExpr`: rewrite the condition first. If it contained a meta call and is now a
  constant (bool literals with `!`, `&&`, `||`, `==`, `!=`), replace the node with the taken branch (an empty
  `Block` when there's no `else`), dispose the other branch, and only then rewrite what's left.
- The evaluator is its own small interpreter over the meta function's body, with an environment of bound
  types and 64-bit values. It doesn't clone the body.
- Meta functions' own bodies are never rewritten or instantiated as code.

## Phase 3 — method calls through pointers

`p.m(args)` where `p` is `*S` (or `const *S` / nullable pointer types as they exist) and `S` has an impl
method `m` calls `m` with `*p` as `self`, null-checked the same way field access through a pointer is (see
`tests/conformance/accept/ptr_null_member_panics.strata`). Today it fails with "no matching overload for
'Foo_bump' with 1 argument(s)". The fix is in sema (`src/Sema/Resolve/Members.c` / `Calls.c`, where
`calleeBase` is resolved against impls), lowering the receiver to a deref (`UnDeref`) so codegen needs no
special case. Both `ref` and `const ref` self work; by-value self copies, under the usual copy rules.

## Phase 4 — tests and docs

- Conformance cases `tests/conformance/accept/meta_*.strata` and `reject/meta_*.strata`: the example above
  with one `T` that has `drop` and one that doesn't (the call is dropped, so a type without `drop` compiles);
  `@is_same` with builtins, aliases, wrappers and generic instances; `hello` and `@hello` side by side; a meta
  function calling a meta function; int-returning meta functions; meta calls in non-generic code; `?:`
  pruning; `@has_function` on a generic instance whose impl is a template. Reject cases for each diagnostic,
  and for a non-pruned call to a missing method (`if (@droppable(T) || true)` keeps the branch).
- A "Meta functions" section in `README.md`. A "Status" section here.

## Out of scope

- `type` as a value outside meta parameters (type locals, returning a type, `type` fields).
- Loops in meta functions.
- Meta calls in type positions (`int[@size(T)]`) and in global initializers.
- More built-ins (`@size_of`, `@is_pointer`, `@has_field`, ...). They slot into the same table.
