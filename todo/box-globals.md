# Global `^T` variables

Status: implemented (2026-08-01). Sema, C/TinyCC codegen, and the TCC JIT
lifecycle hooks described below have landed; LLVM stays out of scope for v1
as planned. One refinement beyond the original plan: reading a box's value
(not just moving it) now works in general, not only for globals - see
"Read vs. move" below.

## Read vs. move (added after initial landing)

The no-reassignment/no-move rule only forbids handing out the *box itself*
(`^T` for `^T`). It never applied to reading the value a box holds.
That distinction didn't exist before this feature: previously, any bare
`return ident;` where `ident` was box-typed was unconditionally treated as a
move, regardless of what the function actually returned. That was already
latently wrong for locals whenever the inner type was a scalar (e.g.
`^int x = 9; float f() { return x; }` would return the box's raw pointer
bits, not 9) - it just had no way to surface, since nothing exercised it.

The fix, applied uniformly to locals and globals in both backends:
- **Sema** (`ResolveOverloads.c`, `NodeReturn`): a bare box-typed identifier
  in a `return` only counts as a move when the function's declared return
  type is that same `^T`. When the return type is the inner type instead
  (or numerically coercible to it), it's a read, not a move - and reads never
  hit the box-global move restriction. Bit-copying an owning struct's value
  out this way is still unsound, so that case still goes through the move
  path (and is still rejected for a global).
- **Codegen**: a box value is read through its pointer, not treated as a raw
  pointer bit-pattern, wherever it's consumed as a plain value instead of a
  box - arithmetic/logical operands and coercion to a non-box target type.
  LLVM centralizes this in `Coerce`/`DerefBoxValue` (`LLVMModuleBuilder.c`);
  the C backend adds a parallel `EmitScalarValue` helper used by binary/unary
  operand emission (`CBackend.c`). Member-access bases and the specific
  box-to-box move sites (assignment target, owned-param argument, box-typed
  var-decl init, box-typed return) are unaffected - they still need, and
  still get, the raw box pointer.

This is what lets `samples/box_demo.strata`'s `^int i = 9; ... return
a + b + d + i;` compile and run: `i`'s value is read inline in the
expression, never moved.

## Scope for v1

- Target the C/TinyCC backend only (the public `strataJit*` API's actual JIT
  path). LLVM IR/AOT keeps rejecting box globals with a diagnostic, consistent
  with its existing "owning structs are not yet supported by the LLVM backend"
  restriction (`LLVMModuleBuilder.c:292-295`).
- Box globals are bound once at module init and never rebound. `^T g = ...;`
  at module scope is legal; `g = other;` anywhere in a function body is a sema
  error. Field mutation (`g.field = x;`) remains legal — only rebinding the
  global's own box pointer is forbidden.
- A box global's initializer must be either a value of the inner type
  (`^T g = T{...};`, boxed on module init the same way a local box var is
  boxed) or a box-returning call (`^T g = makeT();`). Moving from another
  global (`^T b = a;`) is rejected, so there is no global-init-ordering
  question to reason about.
- Box globals are freed on JIT teardown, not leaked. A synthetic
  `__strata_module_teardown` frees every box global; `TccJitDestroy` calls it
  automatically. AOT callers must call the emitted teardown function
  themselves before process exit (documented, not automatic — there is no
  portable constructor/destructor mechanism across TinyCC/GCC/Clang/MSVC that
  we want to depend on).

## Why this shape

Existing box-local codegen (`CBackend.c:834-928` `EmitVarDecl`, and
`LLVMModuleBuilder.c:1392-1438` `NodeVarDecl`) already does alloc + init +
move + drop for `^T` locals. `EmitIdent`/`EmitLValue`
(`LLVMModuleBuilder.c:578-627`) already resolve identifiers through a single
`m_symbols`/`m_globals` lookup chain that doesn't care whether the storage is
a stack alloca or a module-level global — and `Resolve()`
(`LLVMModuleBuilder.c:301-313`) already gives `^T` a `ptr`-typed storage
slot regardless of whether it's called for a local or (once `EmitGlobals`
uses it for box types) a global. So *reads and writes* of a box global need
no new machinery in either backend — the only real gap is *initialization*,
because global initializers today are compile-time constants
(`LLVMModuleBuilder.c:1699-1730` only handles literal/negated-literal nodes;
`CBackend.c:1464-1496` just emits whatever expression text is given and
relies on the host C compiler accepting it as a static initializer), and
boxing requires a runtime `strata_alloc` call. That forces an init function
that runs after the JIT/host loads the module, before any Strata code
touches its globals — and symmetrically, a teardown function that runs
before the module unloads.

## Sema changes (`src/Sema/ResolveOverloads.c`)

1. Remove the blanket ban at `:853-857` (`@TODO: Fix` / "global cannot have
   box type"). Replace it with real validation, mirroring the local-box-var
   rules at `:589-636`:
   - Error if a box-typed global has no initializer (mirrors `:589-592`).
   - Validate the initializer expression itself. Global inits aren't run
     through `ResolveExpr` at all today (only two `GlobalDecl` touch points
     exist in the whole file: populating the per-function `scope` map at
     `:787-790`, and the box ban at `:851-857` — there's no existing
     structural validation of `global->init` to preserve or worry about
     regressing). Build a `globalScope` `StrMap` once (every global's
     `name -> type.name`, independent of any function's scope) and call
     `ResolveExpr(r, gd->init, &globalScope)` for box-typed globals so calls
     get resolved (`c->resolvedDecl`) and struct-init fields get checked,
     the same way a local's init would be.
   - Infer the init's type (`InferType`) and require it to equal the box's
     inner type (boxing form) or the box type itself (box-returning-call
     form) — same two-way check as `:618`. But additionally reject the case
     `gd->init->kind == NodeIdent && IsBoxTypeName(initType)` specifically
     (move-from-another-global) with its own diagnostic: box globals may only
     be initialized by boxing a value or calling a box-returning function.
   - `m_movedBoxes` tracking doesn't apply here (it's reset per function body
     and this validation runs outside that loop) — the move-from-identifier
     case is rejected outright rather than tracked.
2. Extend the `NodeAssign` handling at `:414-451`: before treating a
   box-typed identifier target as a legal move-assignment, check it against a
   `m_boxGlobalNames` set (built once, listing every box-typed global's
   name). If the target name is in that set, emit "box global 'g' cannot be
   reassigned; only its fields may be mutated" instead of allowing the move.
   `g.field = x` is unaffected — its target is a `NodeMember`, which already
   takes the non-box-move branch at `:420` (`tt == NULL` since
   `a->target->kind != NodeIdent`).

## C backend changes (`src/Codegen/CBackend.c`)

1. `EmitGlobals` (`:1464-1496`): branch on `IsBoxTypeName(global->type.name)`.
   For a box global, emit `<innerC> *<GlobalName> = 0;` (a null pointer slot,
   exactly like a box local's declared-but-not-yet-boxed slot) instead of
   evaluating `global->init` inline — the real init moves to the new function
   below. Still register it via `AddSymbol` as today (`:1489`).
2. Extract the box-init-statement logic already inside `EmitVarDecl`
   (`:838-917`, both the move/box-returning-call branch at `:850-877` and the
   box-a-value branch at `:878-917`, including the `EmitBoxedStructInit` call
   at `:885`) into a small shared helper parameterized on the target lvalue
   name (`cName`) instead of assuming a fresh local declaration. `EmitVarDecl`
   keeps calling it for locals unchanged; a new `EmitModuleInit` calls it once
   per box global, targeting `GlobalName(...)`. Because sema now guarantees a
   box global's init is never a bare identifier, the existing
   "`init->kind == NodeIdent` → null the source" branch (`:861-876`) simply
   never fires for globals — no special-casing needed there.
3. Reuse `EmitDrops` (`:958-995`) for teardown instead of writing a new free
   loop: populate `emitter->boxVars` with one `OwnEntry{cName: GlobalName(...),
   typeName: global->type.name, byRef: false}` per box global, then call
   `EmitDrops(emitter, 0)` inside the new `EmitModuleTeardown`. This also
   gets nested-owning-struct field drops (`DropHelperName`) for free, same as
   locals.
4. Add `EmitModuleInit`/`EmitModuleTeardown`, each emitting a
   `void __strata_module_init(void) { ... }` / `void __strata_module_teardown(void)
   { ... }` function, only when `mod->globals` contains at least one box
   global (skip entirely otherwise — no empty no-op functions for ordinary
   scripts). `__strata_module_init`/`__strata_module_teardown` are literal C
   names, not run through `GlobalName`/`Encode` — they don't need mangling
   and the leading-double-underscore prefix is already reserved (user
   identifiers starting with `_` + uppercase/`_` are rejected by
   `IsPlainIdentifier`, `:143-146`), so there's no collision risk with
   generated `strata__*` names either. Do **not** add them to
   `emitter->exports` — they're looked up by fixed name directly (see below),
   not through the Strata-name export table used by `strataJitGetFunction`.
5. Call both from `BuildCModuleWithSources` (`:1619-1674`) right after
   `EmitDefinitions(&emitter);` (`:1664`).

## TinyCC JIT changes (`src/Codegen/TccJit.c`)

- `TccJitLoad` (`:136-195`): right after `tcc_relocate` succeeds, look up
  `tcc_get_symbol(jit->state, "__strata_module_init")` directly (fixed C
  name, not through the `exports`/`FindSymbol` table) and call it if
  present, before returning success.
- `TccJitDestroy` (`:113-134`): before `tcc_delete(jit->state)`, look up and
  call `__strata_module_teardown` the same way, if present.
- No change needed in `src/Embed.c` — `JitFromModule`/`strataJitDestroy`
  already just call `TccJitLoad`/`TccJitDestroy`, so the hook is fully
  contained inside `TccJit.c` and transparent to the public API and
  `stratac --run`.

## Explicitly out of scope for v1

- LLVM backend support (IR text, object/assembly AOT, and the internal MCJIT
  benchmark path) — keep erroring on box globals there for now.
- Any compiler-specific `__attribute__((constructor))`/linker-section trick
  for AOT auto-init — AOT hosts call `__strata_module_init`/
  `__strata_module_teardown` explicitly; document this in the box-globals
  section once it's added to `README.md`.
- Cross-function/whole-program use-after-move detection for box globals —
  rebinding is forbidden entirely instead, which is what makes skipping this
  safe.

## Suggested landing order

1. Sema (ban removal + init validation + reassignment rejection) with tests
   covering: missing init, move-from-global init rejected, box-returning-call
   init accepted, reassignment rejected, field mutation still accepted.
2. C backend init/teardown emission, with a generated-C golden test.
3. `TccJit.c` auto-call hooks, with a JIT lifecycle test asserting the boxed
   global's heap allocation is actually freed on `TccJitDestroy` (ASan/valgrind
   or an allocation-counting `strata_alloc`/`strata_free` shim, matching the
   existing `TccLifecycleTests.c` style).
4. End-to-end `strataJitCompileString`/`strataJitCompileFile` test exercising
   a box global read/write/field-mutation from multiple functions in one
   script.
