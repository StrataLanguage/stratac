# `var`: local type inference

`var name = expr;` declares a local whose type is the type of `expr`, worked out at compile time — like
`auto` in C++. After sema fills it in, the declaration is indistinguishable from one written with the
type, so ownership, moves, drops and codegen need no changes.

```c
int get_sum(int a, int b) { return a + b; }

int main()
{
    var some_int = 10;                   // int
    var some_float = 10.0f;              // float
    var some_array = int[] { 10, 20, 30 }; // int[]
    var result = get_sum(10, 20);        // int
    const var limit = 64;                // const int
    for (var i = 0; i < 3; i++) { }      // int
    return 0;
}
```

## Rules

| Form | Result |
| --- | --- |
| `var x = e;` | `x` has the type of `e`, with a top-level `const` dropped (copying a const makes a mutable value) |
| `const var x = e;` | as above, then `const` |
| `for (var i = e; ...)` | allowed, same rule |
| `var x = boxOrOwning;` | moves `e` exactly as `^T x = e;` would — nothing new, the type is just filled in first |
| `var x;` | parse error: `'var' declaration of 'x' needs an initializer to infer its type` |
| `ref var x = p;` | parse error: `'ref var' is not supported; write the type` |
| `var` as a parameter, field, return type, global or cast type | parse error: `'var' is only allowed for local variables with an initializer` |
| init is `void` (a void call) | sema error: `cannot infer the type of 'x': the initializer has no value` |
| init is `null` | sema error: `cannot infer the type of 'x' from 'null'; write the type` |
| init is an untyped braced list (`{1, 2}`, `{ .a = 1 }`) | sema error: `cannot infer the type of 'x' from a braced initializer; write the type, e.g. 'int[] { ... }'` |
| init has no type for any other reason | sema error: `cannot infer the type of 'x' from its initializer` |
| `var` inside an `@meta` function | rejected like `ref` locals are: `a 'var' local` construct error |

Integer literals infer `int` (or `long`/`uint`/`ulong` as the literal already does), float literals
`float`. Nothing new in literal typing.

### Typed array literals

`int[] { 10, 20, 30 }` is not an expression today (only `int[] a = { ... }` is). This plan adds it as a
general primary expression for **dynamic** arrays: `T[] { e0, e1, ... }` and `T[] { v; n }`, producing an
`ArrayInitExpr` with `elementType = T`, whose inferred type is already `T[]` (`InferArrayInitType`).
A fixed-size literal `T[N] { ... }` in expression position is a parse error for now:
`fixed-size array literals are not supported in expressions; write 'T[N] name = { ... }'`.

## Implementation

### 1. Lexer — `src/Lex/Token.h`, `src/Lex/Token.c`
- Add `TokKwVar` to the token enum (next to `TokKwRef`) and `{TokKwVar, "var"}` to the keyword table.
  No existing `.strata` file uses `var` as an identifier.

### 2. AST — `src/AST/AST.h`
- `VarDeclStmt` gets `bool isVar;` — "Set by the parser for `var x = e;`: `type` is empty (`name == NULL`,
  `isConst` set for `const var`) until sema fills it from `e`."

### 3. Parser
- `src/Parse/ParseType.c` `LooksLikeVarDecl`: `case TokKwVar: return true;`.
- `ParserTryParseType`: if `p->cur.kind == TokKwVar`, report
  `'var' is only allowed for local variables with an initializer` and fail. This covers parameters,
  fields, return types, globals and casts in one place.
- `src/Parse/ParseStmt.c` `ParseVarDeclOrExprStmt`: before the existing `const`/`ref` handling, detect
  `var` and `const var` (use `SaveLexerState`/`RestoreLexerState` for the `const` lookahead, like the
  `const ref` code). After `ref`, a `var` gives the `ref var` error. For a `var` decl: read the name, require
  `=` (else the "needs an initializer" error), then `init = ParseExpr(p)` — no type-directed braced parsing.
  Set `isVar = true`, `type = {0}`, `type.isConst` for `const var`, `type.range` = the `var` token's range.
  Finish the statement exactly as the typed path does (`;` handling etc.).
- `ParseFor`: same `var` branch for the loop initializer (`vd->isVar = true`, `init = ParseExpr(p)`).
- `src/Parse/ParseExpr.c` primary expressions: for the type-starting tokens (the primitive keywords already
  routed to `ParseNamedPrimary`, plus `TokKwBool`, `TokKwString`, and `TokIdent`), speculatively parse a type
  with `ParserTryParseType` under a lexer checkpoint. If it parsed, is a dynamic array
  (`TypeNameIsDynamicArray`) and the next token is `{`, return
  `ParseArrayInitBody(p, token, <arena copy of type.elem>)`. If it is a fixed array followed by `{`, give the
  fixed-size error. Otherwise restore the checkpoint and fall through to the existing code unchanged.
  `ParserTryParseType` must not emit diagnostics during this speculation (check: `LooksLikeVarDecl` already
  relies on that) — make sure the `var` diagnostic added above can't fire here (the speculation never
  starts on `TokKwVar`).

### 4. Pre-sema passes must tolerate an empty type
`Monomorphize` and the AST cloner run before sema, so they see `isVar` decls with `type.name == NULL`.
- `src/AST/ASTClone.c` `CloneVarDecl`: only call `TypeNameSubstitute` when `TypeNameValid(&src->type)`;
  otherwise copy the empty type (this is what lets `var b = a;` inside a generic body infer `T`'s
  substitution per instance).
- `src/Sema/Monomorphize.c`: in the `NodeVarDecl` rewrite (`RewriteType(m, &v->type)`), skip `RewriteType`
  when the type is empty. In the meta-body validation (`case NodeVarDecl` near the `isRef` check), reject
  `v->isVar` with `MetaConstructError(m, fn, n, "a 'var' local")` before `IsTypeParamType` is called.
- Grep every other read of a `VarDeclStmt`'s `type` that can run before sema (`grep -rn "NodeVarDecl" src`,
  plus `ModuleLoader`, `Cfg`, `ASTDump`) and guard `type.name == NULL`. `ASTDump` prints `var` for an
  unfilled type.

### 5. Sema — `src/Sema/Resolve/Stmt.c`
- New `static bool InferVarDeclType(Resolver* r, VarDeclStmt* vd, StrMap* scope, const TypeName** initTypeOut)`:
  `ResolveExpr(r, vd->init, scope)`, then `InferType(r, vd->init, scope)`. Emit the errors from the table
  (check `void` via `primitiveType == PrimVoid`, `null` via the `STRATA_NULL_TYPE` name, untyped braces via
  `NodeArrayInit` with no `elementType` / `NodeStructInit` with no `typeName`). On success:
  `bool c = vd->type.isConst; vd->type = *initType; vd->type.isConst = c; vd->type.range = vd->base.range;`
  and return the init type.
  Only infer when `vd->isVar && !TypeNameValid(&vd->type)` — a loop warm-up re-walking the same decl then
  treats it as an ordinary typed decl, which is what we want.
- `WalkVarDeclStmt`: call it right after the `refLocals` shadowing block, before `CheckLocalTypeKnown`. On
  failure, still `ScopeDeclare` the name (with whatever type is there; use `InternTypeName(r, "int")`-style
  fallback only if `ScopeDeclare` cannot take an empty type) so later uses don't cascade into
  "unknown variable" errors, then return.
- The init has now been resolved once. Split `WalkVarDeclInit` so the `ResolveExpr`+`InferType` part can be
  skipped: e.g. `WalkVarDeclInitTyped(r, vd, scope, initType)` holding the assignability/literal-fit/move
  logic, called by `WalkVarDeclInit` (typed decls) and directly with the already-inferred type (var decls).
  Never `ResolveExpr` the same init twice in one walk — it would double-report errors and double-mark moves.
- Check the `for` initializer path (`fs->init->kind == NodeVarDecl`, around line 940) goes through
  `WalkVarDeclStmt`.
- The fixed-array path (`TypeTreeHasFixedArray`) cannot be reached from `var` in this version (no fixed
  literals), but nothing should assume that.

### 6. Codegen
No change expected: `EmitVarDecl` reads `vd->type`, which sema has filled. If codegen ever sees
`isVar` with an empty type, that is a sema bug — add an assert-style guard if cheap.

### 7. Tests — `tests/conformance`
Accept (`accept/var_*.strata`, `// expect: run` plus `// expect-output:` lines, end with `Report();` when using
`Res`/`Box`):
- `var_infers_literals` — int, `10.0f` float, `10l` long, bool, string; print each.
- `var_infers_call_result` — the `get_sum` example.
- `var_typed_array_literal` — `var a = int[] { 10, 20, 30 };` print `a[1]` and `a.length`; also
  `int[] b = int[] { 1, 2 };` and passing `int[] { 4, 5 }` to an `int[]` parameter.
- `var_moves_box` — `var b = Box(1); var c = b;` uses `c`, `Report()` gives `live=0`.
- `var_const` — `const var x = 5;` read it.
- `var_for_loop` — `for (var i = 0; i < 3; i++)` summing.
- `var_generic_body` — a generic function whose body does `var b = a;` with `a: T`, instantiated with two types.
- `var_copy_of_const_is_mutable` — `const int c = 1; var x = c; x = 2;`.

Reject (`reject/var_*.strata`, `// expect: reject "..."` with the message text):
- `var_no_init`, `var_ref`, `var_param`, `var_global`, `var_void_init`, `var_null_init`, `var_braced_init`,
  `var_fixed_array_literal`, `var_const_assign` (`const var x = 1; x = 2;`), `var_type_fixed`
  (`var x = 10; x = "hi";`), `var_use_after_move` (`var b = Box(1); var c = b; return b.id;`).

Every case needs a `// rule:` line.

### 8. Docs
Add a short `var` section to `README.md` next to the other declaration syntax.

## Done when
- `cmake --build build` is clean and `ctest --test-dir build -j8` passes everything (baseline: 422/422),
  including `style_gate` (run `clang-format -i` on touched `.c`/`.h` files) and `type_flag_gate` (outside the
  type layer, use `TypeNameIs*` predicates, never `.isArray`/`.isBox`/`.isOptional`; `ParseExpr.c` is not in
  the type layer).
- The example at the top of this file compiles and runs.
