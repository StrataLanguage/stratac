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
    LLVMJit.h/.c               MCJIT execution engine
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

### Pipeline

```
Source → Lexer → Parser → Module (AST) → ResolveOverloads (sema) → LLVMModuleBuilder → LLVM IR
                                                                              ↓
                                                                    ┌─────────────┐
                                                                    │ AOT: .o/.s  │
                                                                    │ JIT: MCJIT  │
                                                                    │ IR: text    │
                                                                    └─────────────┘
```

`ModuleLoader` (used by `stratac` and `strataCompileFile`) handles `import`
directives: it parses each imported file, resolves relative paths, merges
structs/handles/functions/globals/imports into a single `Module`.

## Language features

### Types

- Scalars: `void bool int uint long ulong byte sbyte short ushort float double`
- Structs: `struct Vec3 { float x; float y; float z; };` — value types,
  passed by reference by default (pointer at the ABI level)
- Handles: `handle Entity;` — opaque, pointer-sized, passed by value.
- Handle inheritance: `handle Player extends Entity;` — `Player` is
  passable anywhere `Entity` is expected (checked by `HandleExtendsFrom`)

### Parameters

- Scalars: pass by value by default
- `ref int x` — pass by reference (mutable, caller sees changes)
- `const int x` — by value, read-only (const-checked)
- `const ref int x` — by reference, read-only
- Structs: always by reference; `const` makes them read-only

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
- JIT: each `extern` call goes through a writable global pointer slot
  `__strata_ext_<name>`. `strataJitAddSymbol` writes the host address.
- Structs cross the boundary as pointers (`ptr`); handles are already
  pointer-sized and pass by value.

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
