# Plan: `extern` functions are file-local unless `@export`

## Goal

A top-level `extern` function declaration is visible only in the file that declares it. Other
files that `import` that file see it only when it is marked `@export`:

```strata
/* a.strata */
extern void free(ptr p);            // a.strata only
@export extern *void malloc(ulong n); // every file that (transitively) imports a.strata

/* b.strata */
import a;

int free(ptr p) { return 10; }      // OK: a's `free` is not visible here, no conflict
void f() { *void m = malloc(8); }   // OK: malloc is @export
```

## Scope

- Applies to: plain top-level extern functions, including `extern<T>` generic externs.
- Does NOT apply to (they stay visible everywhere, as today):
  - extern methods inside `impl H { ... }` (`fromImpl`), which you reach through the type;
    `@export` stays rejected on impl methods.
  - `extern delegate`, `extern struct`, extern globals (they're types/globals, not functions).
- `@private extern` is still allowed and does the same thing as a bare extern. `@private @export`
  together is an error.

## Design

### 1. AST: a separate flag (`src/AST/AST.h`)

Add `bool isExportedExtern;` to `FunctionDecl`, with a comment: an `@export extern` that files
which import this one can see. Do **not** reuse `isExport`: codegen (`Builder/Function.c`),
`Borrows.c`, `Memory.c` and `Embed/Pipeline.c` read `isExport` as "defined function with the C
ABI and the export context". Also print the flag in `Codegen/ASTDump.c` next to `isPrivate`.

### 2. Parser (`src/Parse/ParseDecl.c`, `ParseFunctionWithAttributes` / `NewFunctionDecl`)

- When `isExtern && isExport && !isDelegate`: set `node->isExportedExtern = true` and
  `node->isExport = false`.
- `@export` on `extern delegate`: error "'@export' does not apply to an extern delegate".
- `@private` together with `@export`: error "a function cannot be both '@private' and '@export'".
- The `genericParams.count > 0 && (isExtern || isExport)` check still runs on the original flags.
- Impl methods: the existing `fn->isExport` rejection in `ParseImplDecl` must still catch
  `@export extern` inside an impl, so check `isExportedExtern` there as well.

### 3. Visibility (`src/Sema/Resolve/Calls.c`, `FunctionVisibleTo`)

```c
bool FunctionVisibleTo(const FunctionDecl* fd, uint16_t callerFile)
{
    bool fileLocal = fd->isPrivate || (fd->isExtern && !fd->isExportedExtern && !fd->fromImpl && !fd->isDelegate);
    return !fileLocal || fd->base.range.fileId == callerFile;
}
```

Factor that predicate out as `FunctionIsFileLocal(const FunctionDecl*)` (declare it in the
resolver's internal header next to `FunctionVisibleTo`) so the later steps share it. Check every
other place that decides visibility or looks a function up by name for a call: grep for
`FunctionVisibleTo`, `ModuleHasFunctionNamed`, `addressof` handling in `Memory.c` (taking
`addressof(free)` from another file must fail the same way a call does), generic-extern
instantiation in `Calls.c`, and `DiagHiddenImplCallee` (a hidden extern should just stay
"unknown function", like a plain `@private`, and must not hit the impl/init messages).

Instantiations of an `extern<T>` (`genericOf`) must keep the `base.range.fileId` of the original
declaration, so check that the instantiation copies it.

### 4. Names and overloads (`src/Sema/Resolve/Decls.c`, `MangleFunctions`)

All files merge into one `Module`, so today `extern void free(ptr)` in a.strata and
`int free(ptr)` in b.strata collide ("extern function 'free' cannot be overloaded" / duplicate
signature). New rules, for each name:

- **Two declarations conflict only if one can see the other**: same file, or neither is file-local.
  (A file-local function and a function in another file never conflict.)
- "extern function cannot be overloaded" fires only when the extern shares its name with another
  function that it conflicts with by that rule.
- **An extern is never mangled**: its `mangledName` stays its C name.
- **Any non-extern function that shares its name with an extern anywhere in the module gets
  mangled** (`Mangle(arena, fd)`), even if it's the only Strata function with that name.
  Otherwise b's `free` would get the LLVM/linker symbol `free` and clash with the C `free`. An
  `@export` (C-ABI) function can't be mangled, so an `@export` function that shares its name with
  any extern anywhere is an error: "'@export' function 'free' clashes with extern 'free' declared
  in another file".
- **The same extern in several files** (e.g. both a.strata and c.strata declare
  `extern void free(ptr p);`): allowed. Each is file-local, and they all name one C symbol. If
  their signatures differ (return type, parameter types, variadic-ness, `return` param), report
  "conflicting extern declarations of 'free'" with a note at the other declaration. The
  duplicate-mangled-name check (`byMangled`) must skip pairs of externs that agree, and pairs that
  don't conflict by the visibility rule above.

Overload resolution: a call in a.strata to `free(p)` can see a's extern *and* b's public
`int free(ptr)` (public functions are visible everywhere). When two visible candidates have
identical parameter types and one of them is in the caller's own file, prefer the caller's own
file instead of reporting ambiguity. Look at how ties are broken in `ResolveOverloadCall` /
`ResolveOverloads.h` and add that tiebreak there, at the lowest priority.

### 5. Codegen (`src/Codegen/Builder/Function.c`, `DeclareFunction`)

In the AOT/non-generic branch, an extern must reuse an existing declaration of the same symbol:
`LLVMGetNamedFunction(b->mod, symbol)` first, then `LLVMAddFunction` only if there isn't one
(otherwise LLVM renames the second one to `free.1` and linking breaks). JIT mode already shares
`ExternSlot` by name. `b->funcs` is keyed by `mangledName`, so two externs with the same name
share one `FuncInfo`, which is fine because sema guaranteed they agree.

### 6. Migrate existing code

Every extern that another file uses now needs `@export`. Find them by building and running the
whole test suite after steps 1–5, then add `@export` where a cross-file use breaks. Start with:

- `std/memory.strata` (`malloc`, `realloc`, `free`, `memcpy`), `std/io.strata`, `std/rc.strata`,
  and any other `std/*.strata` extern a sample or test calls from outside that file.
- `tests/conformance/prelude.strata` (check how it's combined with cases: if it's a separate
  imported file, its externs need `@export`).
- Import-resolver test fixtures in `tests/unit/*.c` (strings like `out->text = "extern ..."`)
  whose externs are called from the importing `main` text.
- `samples/**`. Leave `samples/repl/repl.strata` and `samples/repl/repl_tui.strata` alone except
  for adding `@export` to externs; they have uncommitted user work.

Don't add `@export` to an extern that only its own file uses.

### 7. Tests (`tests/unit/ImportTests.c` or `ExternTests.c`, with an import resolver like
`ResolverPrivateMethods` in `PrivateTests.c`)

1. The user's example: lib has `extern void free(ptr p);`, main imports lib and defines
   `int free(ptr p) { return 10; }` plus `int entry()` calling `free(...)` → compiles, and JIT
   run returns 10.
2. Calling a lib's non-exported extern from main → "unknown function".
3. `@export extern` in lib, called from main → compiles (IR emit is enough; use an extern the JIT
   can resolve, e.g. `strlen`, if you JIT it).
4. lib calls its own extern `free` while main defines `int free(ptr)` → lib's call goes to the
   extern (check IR contains a call to `@free` and main's free is mangled).
5. Same extern in two files with the same signature → OK; with different signatures → "conflicting
   extern declarations".
6. `@export extern` in lib plus `int free(ptr)` in main → error (exported extern conflicts).
7. Parser: `@export extern void f();` sets `isExportedExtern`, not `isExport`; `@private @export`
   errors; `@export extern delegate` errors; `@export extern` inside `impl` errors.
8. `addressof` of another file's non-exported extern fails.

### 8. Docs

Add a short section to `README.md` where externs/imports/`@private` are described (or
`docs/host-types.md` if that's where `@private` is documented) explaining the rule and
`@export extern`.

## Done when

- The full test suite (`ctest --preset default` or the project's usual test command) passes,
  including the new tests, and `samples/` still compiles.
- No regressions in the `strata_tests` unit binary.
