# AGENTS.md — how to work in this repo

Commands and conventions for any agent (or human) editing the Strata compiler.

## Language: C11

The entire compiler is **C11**. Allman brace style, `PascalCase` for types and
functions, `camelCase` for locals.

## Build / test

```sh
cmake --preset default          # configure
cmake --build --preset default  # build
build/default/bin/strata_tests  # run tests directly (or: ctest --preset default)
```

Binaries land in `build/default/bin/` (`stratac`, `strata_tests`, `jit_demo`,
`engine_demo`). `LLVM-C.dll` is copied next to them automatically.

`STRATA_SHARED=ON` (the default) builds `strata.dll`; `-DSTRATA_SHARED=OFF`
builds `libstrata.a`. The test executable compiles all sources directly (with
`STRATA_STATIC`), so it works in both modes.

Validate the emitted object after changes to the back-end:

```sh
stratac samples/hello.strata -o build/default/hello.o
clang build/default/hello.o -o build/default/hello.exe && ./build/default/hello.exe ; echo $?
# exit code 25
```

## Architecture

```
include/strata/strata.h        Public C API (the only public header)
src/                           All internal headers + sources
  Core/                        Arena, Str, Vec, StrMap, Diagnostics, SourceManager
  Lex/                         Token kinds, Lexer
  AST/AST.h                    Node hierarchy (tagged structs, struct embedding)
  Parse/Parser.c               Recursive-descent parser
  Sema/ResolveOverloads.c      Overload resolution, type inference, const-checking
  Codegen/
    TypeRegistry.h/.c          Struct/handle type table
    TypeUtil.h/.c              TypeName → LLVM type mapping (MapType)
    LLVMCApi.h                 Curated forward-decls of the LLVM C API
    LLVMModuleBuilder.h/.c     AST → LLVM IR (shared by AOT, JIT, IR text)
    LLVMAot.h/.c               Object/assembly emission (TargetMachine)
    LLVMJit.h/.c               ORCv2 (LLJIT) execution engine
    LLVMCBackend.c             GenerateLlvmIr (IR text via LLVMPrintModuleToString)
    ASTDump.c                  Pretty-print the AST
  Import/ModuleLoader.c        `import` directive resolution + module merging
  Embed.c                      Implements strata.h (strataCompile*, strataJit*)
  stratac/main.c               CLI driver (uses only the public API)
tests/unit/                    Test framework (Test.h) + all tests
samples/                       Example .strata files + host drivers
```

### Memory management

All AST nodes and strings are arena-allocated (`src/Core/Util.h`). The arena
is a bump allocator — everything is freed at once when compilation finishes.
No `free()` calls, no RAII, no ownership tracking.

Key types in `Core/Util.h`:
- `Arena` — bump allocator (`arena_alloc`, `arena_strdup`, `arena_format`)
- `Str` — non-owning string view (`{data, len}`)
- `Vec` — dynamic array of `void*` (`VecPush`, `VecGet`, `.count`)
- `StrMap` — open-addressing hash map `char* → void*` (`StrMapPut`, `StrMapGet`)
- `Sb` — string builder (`SbPrintf`, `SbFinish`)

### AST design

Nodes are plain C structs with a `Node base` first member (struct embedding).
The `AST_NEW(arena, Type)` macro zero-initializes via `memset`. Cast between
node types with `(Type*)node` or the `AsNode(Type, node)` macro. Dispatch on
`node->kind` (the `NodeKind` enum).

`TypeName` is the authority for type shape: `isArray`/`length`/`elem` (arrays;
dynamic `T[]` has length < 0, fixed `T[N]` >= 1) and `isBox`/`inner` (boxes).
Query shape with the `TypeNameIs*`/`TypeNameArrayElem`/`TypeNameBoxInner`
accessors in `src/AST/AST.h` — never parse `TypeName.name`. The `name` string
is derived canonical spelling, used only for display, mangling and map keys;
`TypeNameParse(arena, spelling)` rebuilds a tree from a spelling at true
boundaries (the only code that understands brackets/carets).

### Pipeline

```
Source → Lexer → Parser → Module (AST) → ResolveOverloads (sema) → LLVMModuleBuilder → LLVM IR
                                                                              ↓
                                                                    ┌─────────────┐
                                                                    │ AOT: .o/.s  │
                                                                    │ JIT: LLJIT  │
                                                                    │ IR: text    │
                                                                    └─────────────┘
```

`ModuleLoader` (used by `stratac` and `strataCompileFile`) handles `import`
directives: it parses each imported file, resolves relative paths, merges
structs/handles/functions/globals/imports into a single `Module`.

### Custom import resolver

A host can take over `import` resolution by installing a callback on the
compiler before compiling:

```c
typedef struct {
    const char* text;    /* module source (BORROWED until the compile ends) */
    size_t      length;
    const char* name;    /* canonical name for diagnostics + cycle dedup (copied) */
} StrataResolvedModule;

typedef int (*StrataImportResolverFn)(void* userData,
                                      const char* importerName,
                                      const char* importPath,
                                      StrataResolvedModule* out);
/* return 1 = resolved (fill `out`), 0 = not found (hard error, no FS fallback) */

void strataSetImportResolver(StrataCompiler* c, StrataImportResolverFn fn, void* userData);
```

When set, the resolver is authoritative for EVERY `import X;` (the main
entry file/source is still supplied by the caller as usual). `importPath` is
the path as written (no `.strata` appended); `importerName` is the canonical
name of the module doing the import (for relative-style resolution). Returning
0 is a hard error — there is no filesystem fallback, so a host that wants
hybrid disk+virtual resolution implements the disk lookup itself.

This also lifts the "imports not supported from a string" restriction:
`strataCompileString`/`strataJitCompileString` accept imports when a resolver
is installed, letting a host compile a fully virtual module graph with no disk
access. `ModuleLoaderLoad` (disk main) and `ModuleLoaderLoadSource` (in-memory
main) are the internal entry points.


## Language features

### Types

- Scalars: `void bool int uint long ulong byte sbyte short ushort float double`
- Structs: `struct Vec3 { float x; float y; float z; };` — value types,
  passed by reference by default (pointer at the ABI level)
- Extern structs: `extern struct Header { fieldoffset(8) long size; byte[16] name; };`
  — mirrors a **host-defined** C struct type. The body must match the host
  layout byte-for-byte; `fieldoffset(N)` pins a field to byte N (extern
  structs only), fields without an offset flow at the next naturally aligned
  offset (C rules). Any struct with an explicit offset is emitted *packed*
  with explicit pad members, so `sizeof`/offsets match C exactly (natural
  extern structs rely on the backend's C-compatible padding). Duplicate
  extern-struct declarations must be field-identical (hard error otherwise).
  Pointer members are spelled `^T`: instances Strata creates (as `^Struct`)
  are dropped automatically; bare struct params stay borrows — the host never
  frees a Strata box and treats `^T` members as opaque pointers. See
  `samples/extern_layout.strata` + `samples/hosts/extern_layout_host.c`.
- Handles: `handle Entity;` — opaque, pointer-sized, passed by value.
- Handle inheritance: `handle Player extends Entity;` — `Player` is
  passable anywhere `Entity` is expected (checked by `HandleExtendsFrom`)
- Boxes: `^T` — an owning, heap-allocated, move-only handle to a `T`
  (e.g. `^Vec3 v = Vec3 { ... };`). `^` always takes the next type and
  binds tighter than `[]`: `^Foo[]` is an array of boxed `Foo`. There is
  no spelling for a box whose inner type is an array. In the compiler a
  box is a structural `TypeName` flag (`isBox`/`inner`); canonical type
  spellings look like `^Foo` / `^Foo[]`. `^T` is NON-NULL by contract:
  every `^T` local must be initialized, and every `^T` struct field must
  appear in the struct literal (compile error otherwise). Dynamic `T[]`
  fields are the exemption: an omitted field zero-fills to the canonical
  empty `{null, 0}` array (there is no `T[]?` spelling — a box never
  wraps an array).
- Optionals: `T?` — the maybe-empty form of a box (`Weapon? w;`). Same
  runtime representation as `^T` (pointer slot; null = empty; identical
  ABI and drop glue), purely a sema-level distinction (`TypeName`
  `isOptional` flag). Uninitialized optional locals are legal (empty).
  Reading through a `T?` — a member/index reach, a call argument,
  return, assignment, or initializer that unwraps it to `T`, an array
  element, a bare extern `...` slot — requires a narrowing fact (a
  "blessing": `'x' has not been blessed; test it first: if (x?) { ... }`,
  the mirror image of the "poisoned" move state) from `if (path?)`,
  definite reassignment, or while/for condition narrowing.
  Array-element facts are index-precise: an index spelling is pinned to a
  canonical fully-parenthesized form — a literal (`arr[0]`), a local
  variable (`arr[i]`), `.length` of a local array, arithmetic over those
  (`arr[i + 1]`, `arr[arr.length - 1]`), or a nested index
  (`foo[other[bar]]`, `grid[i][j]`); every local and every array the
  spelling mentions (as a variable, length base, or index source) is
  tracked, and the fact dies on any mutation of them (assignment,
  `++`/`--`, element writes, or passing them as a non-const `ref`) or any
  length change of a `.length` base (push/resize/pop/rebind). Any other
  index expression (calls, comparisons, globals, member bases) is never
  provable (hint suggests moving/copying the element into a local).
  `array_resize`/`array_pop` drop the receiver's element facts;
  `array_push` preserves them; any real call drops global-rooted facts — facts use the
  same dotted-path machinery as move poisoning (`m_nonEmptyPaths`) with
  intersection-merge at if/else joins and full invalidation at loop exits.
  An initialized declaration also establishes the fact
  (`Weapon? w = Weapon {}` proves `w`) — unless the initializer is a
  maybe-empty `T?` that is not itself blessed (`Weapon? b = a;` proves `b`
  only when `a` is proven). The else branch of `if (path?)`
  carries a definitely-EMPTY fact (`m_emptyPaths`, scoped to that block):
  reads there get a sharper "is definitely empty" error, and assigning in
  the else is what makes the lazy-init idiom join to a non-empty fact.
  Testing `a.b.c?` also requires every optional ancestor proven. Every
  `=` into a `T?` rebinds the whole slot (drop old + take new) and drops
  the old blessing: the rebinding `=` re-establishes it only when the new
  value is provably non-empty (a non-optional source, or an optional path
  already blessed — `cur = cur.next` alone does NOT re-bless `cur`), and
  moving out of a `T?` leaves the source definitely empty, never poisoned
  (so `if (a?)` after `W? b = a;` is legal and false). Compound
  assignment into optionals is rejected. Recursive owning structs use
  optionals for self-references (`struct Node { int v; Node? next; };`).
- Fixed-size arrays: `byte[16] name;` — C-ABI inline storage (`[16 x i8]`
  in LLVM, `unsigned char name[16]` in C). **Struct fields only** (not
  locals, params, returns, globals, or dynamic-array elements); elements
  must be non-owning scalars/handles/structs (no `^T`/`string`/`T[]`
  elements — fixed arrays have no drop glue). Dimensions read like C,
  outermost first (`int[2][6]` is 2×`int[6]`, mirroring `int x[2][6]`;
  nesting is spelled inside-out relative to C). Braced struct-init values
  mirror the type's shape — MANDATORY nested rows for multidimensional
  fields (`S { .m = {{1,2},{3,4}} }`, one brace level per dimension; rows
  may be short and missing rows/elements zero), flat leaf lists for
  single-dimension fields (`S { .m = {1,2,3} }`); the wrong shape is a
  compile error. Indexing is bounds-checked against the compile-time
  length, and `.length` is a compile-time constant. Whole fixed-array assignment
  (`s.a = s.b`) is rejected — assign elements. No auto-conversion to the
  fat `T[]` pointer yet.

### Parameters

- Scalars: pass by value by default
- `ref int x` — pass by reference (mutable, caller sees changes)
- `const int x` — by value, read-only (const-checked)
- `const ref int x` — by reference, read-only
- Structs: always by reference; `const` makes them read-only
- `int... rest` — typed rest param (only allowed last): trailing call args are
  collected into a **stack-allocated** `{ptr, len}` and exposed as a real
  `int[]` (use `.length`, `[i]`, loops as usual). `ref`/`const` are allowed:
  `ref int... rest` borrows the stack array (mutable, non-owning, elements not
  moved); `const int... rest` is a read-only view. Default (no mod) is owned:
  owning elements are moved in and dropped at return, but the stack buffer is
  never freed. The callee's ABI is a pointer to the fat struct either way.
- `extern int printf(string fmt, ...);` — bare `...` is **extern-only** and
  call-only: the host provides the body as a real C vararg function. No
  `va_list`/`va_start` appears in user or generated code. Trailing args must be
  scalar/`string`/handle/`^T`; the LLVM backend applies C default argument
  promotions (`float`→`double`, small ints→`int`) explicitly. Variadic string
  params cross as `const char*`.

### Operators

- `++` / `--` prefix and postfix (on any lvalue)
- `&&` / `||` short-circuit (basic blocks + PHI, not bitwise)
- C-style casts: `(int)x`, `(float)y`, `(Player)e` (scalar and handle casts)
- Compound assignment: `+= -= *= /= %=`

### Other

- Global variables: `int g_count = 0;` at module scope (LLVM globals,
  literal constant initializers only)
- Overloads: functions may share a name with different param types;
  mangled as `name$type$type` (non-overloaded keep the base name)
- Inferred struct init: `return { .x = 1, .y = 2 };` infers the return type
- `const` on any param or local triggers const-checking in the sema

## extern and the host boundary

- AOT: `extern` lowers to a body-less `declare`; the host links it.
  Owning globals (`^T` / `T[]` with runtime initializers) self-initialize:
  `__strata_module_init` is registered in `llvm.global_ctors`, which lowers
  to `.init_array` (ELF) / `.CRT$XCU` (COFF), so it runs before the host's
  `main` with no host-side call. (`__strata_module_teardown` still exists as
  an export the host MAY call; nothing runs it automatically.)
- JIT: each `extern` call goes through a writable global pointer slot
  `__strata_ext_<name>`. `strataJitAddSymbol` writes the host address.
- Structs cross the boundary as pointers (`ptr`); handles are already
  pointer-sized and pass by value. `extern struct` declarations let Strata
  code name and read/write the host's own struct layouts (see Types above);
  AOT hosts must also provide `strata_alloc`/`strata_free` (and
  `strata_panic`) whenever the Strata code allocates boxes.
- Box/optional ABI at `extern`: a `T?` or `^T` param crosses as ONE pointer
  by value (`T*` in the host prototype; NULL = empty for `T?`) — not as a
  pointer to the caller's slot. A plain value arg is boxed into a temp cell
  before the call. A `T?`/`^T` return is likewise one pointer; ownership
  transfers to the caller (the host hands back memory Strata will free).
  The LLVM backend implements this.
- Array-decay at `extern`: a **by-value** (`mod == ModNone`),
  **non-owning-element** dynamic array param (`int[]`, `uint[]`, `handle[]`,
  `Foo[]`, …) decays to a pointer to its first element — the inner buffer
  pointer of the fat `{ptr,len}` struct — matching a C `T*` parameter. The
  caller passes the element count separately as `arr.length`. `ref`/`const`
  array params and owning-element arrays (`^T[]`, `string[]`) keep crossing as
  the fat `{ptr,len}` struct so the host can read `ptr`/`len` or replace the
  array. This is what lets Strata call C APIs that take `T*` + a length (e.g.
  LLVM-C's array-taking functions).

## Adding a language feature (typical path)

1. **Tokens**: `src/Lex/Token.h` (enum) + `src/Lex/Token.c` (keyword table)
2. **Lexer**: `src/Lex/Lexer.c` (punctuation/keyword recognition)
3. **AST**: `src/AST/AST.h` (add node struct + NodeKind value)
4. **Parser**: `src/Parse/Parser.c` (grammar rule)
5. **AST dump**: `src/Codegen/ASTDump.c` (handle new node kind)
6. **Sema**: `src/Sema/ResolveOverloads.c` (type inference, const checks,
   overload matching — anything type-related)
7. **Codegen**: `src/Codegen/LLVMModuleBuilder.c` (IR emission)
8. **Type mapping**: `src/Codegen/TypeUtil.c` (`MapType`) if it's a new type
9. **Tests**: `tests/unit/` (framework is `tests/unit/Test.h`), then add the
   file to `STRATA_TEST_SOURCES` in `CMakeLists.txt`

## LLVM C API

`src/Codegen/LLVMCApi.h` is a curated forward-declaration of the LLVM C API.
When adding LLVM functions, declare them there and **always use the
`...InContext` variants** so types/BBs belong to the module's context.

All `alloca` instructions go through `EntryAlloca` which inserts them in the
function's entry block (before the terminator) — never inside loop bodies.

## CTest wiring

A single test target `strata_tests` compiles all strata sources + test files.
`STRATA_SAMPLE_DIR` is defined so `SampleTests.c` can read `samples/`.
New test files must be added to `STRATA_TEST_SOURCES` in `CMakeLists.txt`.

## DLL / MSVC integration

```sh
cmake --preset default -DSTRATA_SHARED=ON
cmake --build --preset default
# → strata.dll (produced as strata.dll, not libstrata.dll)
```

`gen_msvc_lib.bat` generates an MSVC-compatible `strata.lib` import library
from `strata.def`. From a VS Developer Command Prompt:
```
gen_msvc_lib.bat
```

Ship: `strata.dll`, `strata.lib`, `strata.h`, `LLVM-C.dll`.
