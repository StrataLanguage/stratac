# Strata

**Strata** is a small, fast scripting/programming language designed to be
embedded in game engines and compiled to **native code via LLVM**. Its syntax is
inspired by HLSL: C-style, simple, with no pointers or references — parameters
are passed by value, optionally with `in` / `out` / `inout` modifiers.

> Status: early bootstrap. The front-end (lexer, parser, AST, diagnostics) and
> two code-generation back-ends are in place; see **What works** below.

## What's here

- A recursive-descent **lexer + parser** producing a typed AST, with a
  source-range-aware diagnostic engine and error recovery.
- A **text back-end** (`llvm-ir-text`) that emits valid LLVM IR; the IR has been
  verified by assembling it to an object file with `clang`.
- An **in-process LLVM back-end** (`llvm-c`) that builds the IR through the
  linked **LLVM C API** (`LLVM-C.dll`) — the path that produces native code.
- A **C embedding API** (`include/strata/strata.h`) for host applications.
- A CLI driver, **`stratac`**.

## What works (bootstrap subset)

- Scalar types: `void bool int uint half float double`, plus HLSL-style vector
  types (`float4`, `int3`, ...).
- Functions with `in` / `out` / `inout` parameters.
- Statements: `return`, `if`/`else`, `while`, `break`, `continue`, `var` decls,
  expression statements, blocks.
- Expressions: literals, identifiers, calls, unary/binary operators with C
  precedence, assignment (`= += -= *= /= %=`), member access (parsed).
- Comments: `//` and nestable `/* */`.

### Known limitations (intentional, for follow-up)

- The **LLVM back-end** lowers a focused subset (params, returns, locals,
  arithmetic `+ - *`, calls). Control flow and `out` mutation are handled by the
  text back-end and are next on the list for the LLVM back-end.
- `&&`/`||` are non-short-circuit; `half` constants are not fully lowered.
- No semantic analysis / type checking yet — types are mapped at code-gen time.

## Building

Requirements: **CMake ≥ 3.20**, **Ninja**, a **C++20** compiler. This repository
was bootstrapped with **MinGW GCC 16** (`g++`) on Windows.

LLVM linkage is optional but enabled by default. Point `LLVM_C_DIR` at an x64
LLVM distribution that provides `lib/LLVM-C.{lib,dll}` (the official
`LLVM-<ver>-win64.exe` installer works). MinGW links the LLVM **C** ABI; the
back-end uses a curated set of `llvm-c` declarations
(`include/strata/Codegen/LLVMCApi.h`) rather than the full header graph.

```sh
# Configure + build (MinGW + Ninja, with LLVM)
cmake --preset default
cmake --build --preset default

# Build without LLVM (text IR only)
cmake --preset no-llvm
cmake --build --preset no-llvm
```

### Running the tests

```sh
ctest --preset default        # via CTest
build/default/bin/strata_tests # directly
```

### Using the compiler

```sh
# Emit LLVM IR (uses the in-process LLVM back-end by default)
stratac --emit ir samples/hello.strata

# Emit text IR without LLVM linkage, write to a file
stratac --no-llvm --emit ir samples/hello.strata -o hello.ll

# Pretty-print the AST
stratac --emit ast samples/hello.strata
```

The emitted IR can be assembled to native code by an external LLVM toolchain:

```sh
clang -c hello.ll -o hello.o
```

## Embedding

Link against the `strata` library and use the C API:

```c
#include "strata/strata.h"

StrataCompiler* c = strataCompilerCreate();
StrataResult r = strataCompileString(c, "int add(int a, int b){ return a+b; }",
                                     "math", STRATA_EMIT_LLVM_IR);
if (r.ok) { /* r.output is the LLVM IR */ }
strataResultFree(&r);
strataCompilerDestroy(c);
```

## Project layout

```
include/strata/   public headers (Core, Lex, AST, Parse, Codegen, strata.h)
src/Core          SourceManager, Diagnostics
src/Lex           Token kinds, Lexer
src/Parse         recursive-descent Parser
src/Codegen       AST dump, back-end interface, text + LLVM-C back-ends
src/Embed.cpp     implementation of the C embedding API
src/stratac       the stratac CLI driver
tests/unit        unit + end-to-end tests (self-contained test framework)
samples/          example Strata programs
third_party/      (reserved for vendored deps)
```

## A note on the toolchain on this machine

The pre-existing LLVM at `C:\llvm` is an **ARM64** build that cannot run on this
**x86_64** host. A matching **x64 LLVM 22.1.0** was installed to
`C:\Users\andre\llvm-x64` (its `clang` runs, and `LLVM-C.dll`/`LLVM-C.lib` are
used for linking). The preset defaults reflect this.
