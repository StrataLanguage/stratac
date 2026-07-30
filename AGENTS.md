# AGENTS.md — how to work in this repo

Commands and conventions for any agent (or human) editing the Strata compiler.

## Toolchain (this machine)

- Host is **x86_64** Windows. C++ compiler: **MinGW GCC 16** (`g++`, on PATH).
- Generators: **CMake 4.2** + **Ninja**.
- **Do not** use the LLVM at `C:\llvm` — it is **ARM64** and will not run here.
  The working x64 LLVM is at `C:\Users\andre\llvm-x64` (provides `clang`,
  `LLVM-C.dll`, `LLVM-C.lib`). `CMakePresets.json` already points `LLVM_C_DIR`
  there.

## Build / test

```sh
cmake --preset default          # configure (MinGW + Ninja, LLVM ON)
cmake --build --preset default  # build
ctest --preset default          # run tests
```

Binaries land in `build/default/bin/` (`stratac.exe`, `strata_tests.exe`).
`LLVM-C.dll` is copied next to them automatically so they run from the build
tree.

Run tests directly for full output: `build/default/bin/strata_tests.exe`.

Validate emitted IR with the real LLVM after changes to the back-end:

```sh
stratac --emit ir samples/hello.strata -o build/default/hello.ll
C:/Users/andre/llvm-x64/bin/clang.exe -c build/default/hello.ll -o build/default/hello.o
```

`clang -c` must exit 0 (a `-Woverride-module` warning about the target triple is
fine; the IR carries no triple by design).

## Running Strata code (execution)

Two paths, both via LLVM:

- **JIT (in-process):** `strataJitCompileString/CompileFile` → `strataJitGetFunction`
  returns native function pointers. Implemented in `src/Codegen/LLVMJit.*` (MCJIT
  via the ExecutionEngine C API). ORC v2 (`LLVMOrc*`) is also exported by
  LLVM-C.dll for future hot-reload.
- **AOT (disk):** `stratac --emit obj|asm`, implemented in `src/Codegen/LLVMAot.*`
  via `LLVMTargetMachineEmitToFile`. Emits the host x64 ABI; the object links
  like any COFF object.

Both share one IR builder (`src/Codegen/LLVMModuleBuilder.*`) returning a live,
owned `BuiltModule`. **Always call the X86 target initializers before creating a
TargetMachine or ExecutionEngine** — `LLVMInitializeAll*` / `LLVMInitializeNativeTarget`
are NOT exported by this LLVM-C.dll, but the X86-specific entry points are. The
`ensureX86Initialized()` helpers in LLVMAot.cpp / LLVMJit.cpp do this once each.

`extern` functions (host-provided) are lowered two ways, depending on path:
- AOT: a body-less `declare`; the host satisfies it at link time.
- JIT: each `extern` call is an indirect call through a writable global pointer
  slot `__strata_ext_<name>`. `strataJitAddSymbol` resolves the slot via
  `LLVMGetGlobalValueAddress` and writes the host pointer. (MCJIT's
  `LLVMAddGlobalMapping` is not consulted by this build, so we don't rely on it.)
  The builder only creates slots in `jitMode` (true for the JIT, false for
  AOT/IR).

**User-defined types** (`src/Codegen/TypeRegistry.h` is shared by all codegen paths):
- `struct Name { ... }` -> `%struct.Name = type { ... }` (a defined value type; a
  body-less `struct` is an error). `handle Name;` -> a distinct opaque type
  lowered as `ptr` (engine objects; member access is a compile error).
- Member access uses `LLVMBuildGEP2` on an alloca (`emitLValue` in
  LLVMModuleBuilder.cpp); positional construction (`Vec3(a,b,c)`) uses
  `insertvalue`. **Struct parameters are always passed by reference**: any
  struct parameter must declare `in`/`out`/`inout` (enforced by
  `resolveOverloads`), lowered to `ptr`; the callee's symbol for it *is* the
  incoming pointer, and call sites pass the argument's address. An `extern` may
  not return a struct by value. Locals/returns are still by value; copy
  explicitly (`Vec3 c = v;`) for a mutable local.
- Aggregate-ABI caveat: structs are value types *within* Strata but cross the
  host boundary by pointer. Any struct parameter must declare
  `in`/`out`/`inout` (enforced by `resolveOverloads`); it lowers to `ptr`. An
  `extern` may not return a struct by value. Handles (`handle Name;`) are
  pointer-sized and passed by value, so they need no modifier. See README
  "A note on aggregates across the host/JIT boundary".

The JIT is demonstrated in `tests/unit/JitTests.cpp` (it actually calls JIT'd
functions and asserts results).

## Conventions

- **C++20**, `namespace strata`. No compiler extensions. No new comments beyond
  the existing doc style unless asked.
- Headers use `#pragma once`. Internal helper headers live next to their sources
  (e.g. `src/Codegen/TypeUtil.h`) and are included relatively.
- AST nodes derive from `Node` (kind enum + `SourceRange`); children are owned by
  `std::unique_ptr`. Use `asNode<T>`/`static_cast<T*>` by `NodeKind`.
- The front-end has **no semantic analysis** yet. Type mapping happens in
  `src/Codegen/TypeUtil.h` (`detail::mapType`) and is shared by all codegen paths —
  change type representation there.
- The LLVM back-end uses a **curated forward-declaration** of the LLVM C API in
  `include/strata/Codegen/LLVMCApi.h`. When adding LLVM functions, declare them
  there with signatures matching `llvm-c/Core.h`, and **always use the
  `...InContext` variants** so types/BBs belong to the module's context (the
  context-less constructors bind to LLVM's global context and fail verification
  with "Function context does not match Module context").

## Adding a language feature (typical path)

1. Tokens/keywords: `include/strata/Lex/Token.h` + `src/Lex/Token.cpp`.
2. Lexing: `src/Lex/Lexer.cpp`.
3. Grammar + AST: `include/strata/AST/AST.h` (add a node) + `src/Parse/Parser.cpp`.
4. AST dump: `src/Codegen/ASTDump.cpp`.
5. Semantic analysis: `src/Sema/ResolveOverloads.cpp` runs between parse and
   codegen (in `stratac` and `Embed.cpp`). It infers argument types, resolves
   overloads, and sets `FunctionDecl::mangledName` + `CallExpr::callee`
   (mangled) + `CallExpr::resolvedDecl`. Anything type-related belongs here.
6. Codegen: `src/Codegen/LLVMModuleBuilder.cpp` (note: LLVMModuleBuilder feeds
   the IR printer, AOT emitter, and JIT). It emits functions/calls by
   `mangledName`.
7. Tests: `tests/unit/` (framework is `include/strata/Test.hpp`), then re-run
   `ctest --preset default` and the `clang -c` IR check above.

Overloads: two functions may share a name with different parameter types.
`resolveOverloads` mangles overloaded symbols as `name$type$type` (single
functions keep the base name, so host-callable entries like `main`/`entry` must
not be overloaded). Extern functions cannot be overloaded.

Statement lowering (in `emitStmt`): `out`/`inout` parameters are
lowered to pointers -- the callee's symbol for such a param *is* the incoming
pointer, and call sites pass the argument's address (`&var`, via `emitLValue`).
Control flow (`if`/`else`, `while`, `for`, `break`, `continue`) uses basic
blocks + `br`/`cond br`; a loop stack tracks break/continue targets (continue
targets the `for` update block, not the condition).

## CTest wiring

A single test target `strata_tests` runs all unit tests. `STRATA_SAMPLE_DIR` is
defined so `SampleTests.cpp` can read `samples/hello.strata`. New test files must
be added to the `strata_tests` source list in `CMakeLists.txt`.
