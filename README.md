# Strata

**Strata** is a small, fast scripting/programming language designed to be
embedded in game engines and compiled to **native code via LLVM**. Its syntax is
inspired by HLSL: C-style, simple, with no pointers or references — parameters
are passed by value, optionally with `in` / `out` / `inout` modifiers.

> Status: early bootstrap. The front-end (lexer, parser, AST, diagnostics) and
> LLVM code-generation back-end are in place; see **What works** below.

## What's here

- A recursive-descent **lexer + parser** producing a typed AST, with a
  source-range-aware diagnostic engine and error recovery.
- An **in-process LLVM back-end** (`llvm-c`) that builds the IR through the
  linked **LLVM C API** (`LLVM-C.dll`) — the path that produces native code
  (IR text, object files, assembly, and JIT execution).
- A **C embedding API** (`include/strata/strata.h`) for host applications.
- A CLI driver, **`stratac`**.

## What works (bootstrap subset)

- Scalar types: `void bool int uint half float double`, plus HLSL-style vector
  types (`float4`, `int3`, ...).
- Functions with `in` / `out` / `inout` parameters.
- Statements: `return`, `if`/`else`, `while`, `for`, `break`, `continue`, `var` decls,
  expression statements, blocks.
- Expressions: literals, identifiers, calls, unary/binary operators with C
  precedence, assignment (`= += -= *= /= %=`), member access (parsed).
- **Overloads**: functions may share a name when their parameter types differ;
  each call resolves to the best-matching overload by argument type (exact match
  first, then numeric conversions).
- Comments: `//` and nestable `/* */`.

### Known limitations (intentional, for follow-up)

- The **LLVM back-end** lowers the full statement surface (control flow,
  recursion, `out`/`inout`, structs, overloads). Struct parameters cross the
  host boundary by reference (`in`/`out`/`inout`). What remains is vector
  construction/per-component arithmetic and a host *calling into* a Strata entry
  point that takes/returns a struct by value (see below).
- `&&`/`||` are non-short-circuit; `half` constants are not fully lowered.
- Semantic analysis is currently overload resolution + argument-type inference
  only; broader type checking is still ahead.

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
```

### Running the tests

```sh
ctest --preset default        # via CTest
build/default/bin/strata_tests # directly
```

### Using the compiler

```sh
# Emit LLVM IR (uses the in-process LLVM back-end)
stratac --emit ir samples/hello.strata

# Write IR to a file
stratac --emit ir samples/hello.strata -o hello.ll

# Pretty-print the AST
stratac --emit ast samples/hello.strata
```

The emitted IR can be assembled to native code by an external LLVM toolchain:

```sh
clang -c hello.ll -o hello.o
```

### Compile and run in one step (Windows)

`run.bat` AOT-compiles a `.strata` program to a native executable and runs it:

```bat
run.bat samples\hello.strata                              :: standalone (needs int main())
run.bat samples\engine_api.strata samples\hosts\engine_api_host.c   :: with a host driver
```

Standalone mode links the `.strata` object alone (it must define `int main()`).
Pass a host `.c` file as the second argument to link a driver that provides
`main` and any `extern` functions the script calls. Tools can be overridden with
the `STRATAC` and `CLANG` environment variables.

## Running Strata code

There are two ways to run Strata, both via LLVM:

### 1. JIT — compile in-process, call function pointers (the game-engine path)

Compile a module to native code in-process and resolve entry points as raw
function pointers. No external toolchain or process spawn is involved; this is
how a host engine runs scripts at native speed at load time.

```c
#include "strata/strata.h"

StrataCompiler* c = strataCompilerCreate();
const char* err = NULL;
StrataJit* jit = strataJitCompileString(
    c, "int add(int a, int b){ return a + b; }", "math", &err);
strataFree((char*)err);

int (*add)(int, int) = (int(*)(int,int)) strataJitGetFunction(jit, "add");
int r = add(2, 3);  // r == 5   -- JIT-compiled native code

strataJitDestroy(jit);
strataCompilerDestroy(c);
```

The suite verifies this end-to-end: it JITs `add`, `answer()` (which calls
`sq(7)`), and `twice(float)`, then asserts the native results.

### 2. AOT — emit native object / assembly to disk (the shipping / cache path)

`stratac` lowers the IR to a relocatable object (`.o`) or assembly (`.s`) in
process via LLVM's `TargetMachine`:

```sh
stratac --emit obj run.strata -o run.o    # native object
stratac --emit asm run.strata -o run.s    # native assembly
```

The emitted object uses the host's x64 ABI and links like any other:

```sh
# host.c references int add(int,int); produced by stratac --emit obj
clang host.c run.o -o run.exe && ./run.exe
```

A real example compiles `int add(int a,int b){ return a+b; }` to:

```asm
add:
    leal (%rcx,%rdx), %eax   ; Windows x64: args in RCX/RDX, result in EAX
    retq
```

### Calling into the engine: `extern` functions

Strata declares host-provided functions with `extern`. These have no body in
Strata; the engine provides the implementation. This is engine-agnostic: the
host just supplies native function pointers (JIT) or symbols (AOT).

```strata
extern int   engine_get_hp(int entity);
extern void  engine_set_position(int entity, float x, float y, float z);
extern float engine_distance(int a, int b);

int danger_level(int self, int foe) {
    return engine_get_hp(foe) + (int)engine_distance(self, foe); // calls into the host
}
```

**JIT** — after compiling, register each extern name with a native pointer
(before resolving any Strata function, which triggers compilation). You can also
enumerate the names the script declared:

```c
StrataJit* jit = strataJitCompileString(c, src, "ai", &err);
for (size_t i = 0; i < strataJitGetExternSymbolCount(jit); ++i) {
    const char* name = strataJitGetExternSymbolName(jit, i);
    strataJitAddSymbol(jit, name, engine_lookup(name)); // bind host function pointer
}
int (*f)(int,int) = (int(*)(int,int)) strataJitGetFunction(jit, "danger_level");
```

Internally the JIT lowers each `extern` call to an indirect call through a
writable per-extern pointer slot (`__strata_ext_<name>`) that `strataJitAddSymbol`
fills. This sidesteps MCJIT symbol resolution, which isn't exposed by this
`LLVM-C.dll`.

**AOT** — `extern` becomes an ordinary undefined symbol in the object file; the
host provides it at link time:

```sh
stratac --emit obj ai.strata -o ai.o
clang host.c ai.o -o ai        # host.c defines engine_get_hp, engine_set_position, ...
```

### What the JIT / AOT paths lower today

The native paths (JIT and AOT) share one IR builder and lower: scalar int/float
functions, parameters (including **`out`/`inout`**, lowered to pointers),
returns, locals, arithmetic (with int/float promotion), **control flow**
(`if`/`else`, `while`, `for`, `break`, `continue`), recursion, calls between
Strata functions, `extern` calls into the host (structs cross the boundary by
pointer), **user-defined structs** (member access, positional construction,
by-value use within Strata), **opaque engine handle types**, and **type-based
overloads**.

The remaining gaps are narrower: vector construction/per-component arithmetic,
and passing a struct *by value* across the host<->JIT boundary (see below).

## User-defined types

### Structs (value types)

```strata
struct Vec3 { float x; float y; float z; };
struct Body { int id; Vec3 pos; };

float energy(Body b) {
    return b.pos.x * b.pos.x + b.pos.y * b.pos.y + b.pos.z * b.pos.z + b.id;
}

float entry() {
    Body body;
    body.id  = 5;
    body.pos = Vec3(3.0, 4.0, 0.0);   // positional constructor
    return energy(body);              // passed by value, Strata -> Strata
}
```

Structs are value types with no pointers: declare fields of any type (including
other structs), read/write members (`v.x`, `b.pos.y`), and construct positionally
(`Vec3(1, 2, 3)`). Locals are values; **function parameters are always passed by
reference** -- a struct parameter must declare `in`/`out`/`inout`, and is lowered
to a pointer. `in` is a read-only reference; if you need a mutable local copy,
make one explicitly. This keeps the calling convention uniform with `extern`
(and future modules).

```strata
struct Vec3 { float x; float y; float z; };

float dot(in Vec3 a, in Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
void  scale(inout Vec3 v, float s) {
    Vec3 c = v;            // explicit local copy
    v.x = c.x * s; v.y = c.y * s; v.z = c.z * s;
}
```

Initialization options:

```strata
Vec3 a;                  // zero-initialized by default -> {0.0, 0.0, 0.0}
Vec3 b = Vec3(1, 2, 3);  // positional constructor
Vec3 c = b;              // copy from another value
a.x = 5.0;               // then set fields individually
```

Every variable declaration without an initializer is zero-initialized (scalars
to `0`, floats to `0.0`, structs and vectors to all-zero fields).

### Opaque engine handles

```strata
handle Entity;                       // opaque, pointer-sized; engine owns the layout
extern Entity spawn();               // engine returns a handle
extern int  id_of(Entity e);         // Strata passes the handle to the engine
extern void despawn(Entity e);
```

`handle Name;` declares a distinct opaque type. Strata code can hold handles,
pass them to `extern` functions, get them back, and compare them -- but not
access fields (member access on a handle is a compile error). A handle is a
single pointer-sized value, so it crosses the host<->JIT boundary cleanly (like a
scalar) and needs no `in`/`out`/`inout` modifier. This is the recommended way to
expose engine objects to scripts, and the shape that will cross future script
modules unchanged. (`struct` is always a defined value type with a body; a
body-less `struct` is an error -- use `handle`.)

### A note on aggregates across the host/JIT boundary

Passing a struct *by value* between host code and a JIT'd function depends on
the platform's aggregate ABI, which the JIT can't always be retargeted to on this
box (`LLVMSetTarget` isn't exported by `LLVM-C.dll`, and LLVM's `windows-msvc`
codegen and the MinGW host disagree on small aggregates).

Strata sidesteps the problem entirely: **struct parameters are always passed by
reference** (a `ptr`), whether the function is `extern` or not, so a struct never
crosses any boundary by value. An `extern` may also not *return* a struct by
value (use an `out` parameter). The host implements the natural pointer
signature:

```strata
extern float length_sq(in Vec3 v);                          // host: float(const Vec3*)
extern void  scale_into(in Vec3 src, float s, out Vec3 dst); // host: void(const Vec3*, float, Vec3*)
```

(Handles declared with `handle Name;` are already pointer-sized, so they cross
the boundary as-is.) Within Strata, structs are value types whose parameters are
passed by reference. The remaining caveat is only for a host
*calling into* a Strata entry point that takes/returns a struct by value --
prefer scalar/handle entry points there.



## Embedding (compile to IR / AST)

For inspecting IR or the AST rather than executing, use the non-JIT API:

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

## Samples

The `samples/` directory has a worked example for each feature, including host
drivers you can build and run:

```sh
# hello: runs as a standalone program (exit code 25)
stratac --emit obj samples/hello.strata -o hello.o
clang hello.o -o hello.exe && ./hello.exe ; echo $?

# structs: native IR + object
stratac --emit ir  samples/structs.strata
stratac --emit obj samples/structs.strata -o structs.o

# script calls host externs, then opaque engine handles (AOT link + run)
stratac --emit obj samples/extern_math.strata -o extern_math.o
clang samples/hosts/extern_math_host.c extern_math.o -o extern_math.exe && ./extern_math.exe

stratac --emit obj samples/engine_api.strata -o engine_api.o
clang samples/hosts/engine_api_host.c engine_api.o -o engine_api.exe && ./engine_api.exe
```

See `samples/README.md` for the full list and which back-end each uses.

## Project layout

```
include/strata/   public headers (Core, Lex, AST, Parse, Codegen, strata.h)
src/Core          SourceManager, Diagnostics
src/Lex           Token kinds, Lexer
src/Parse         recursive-descent Parser
src/Codegen       AST dump, back-end interface, LLVM-C back-end,
                  shared IR builder, AOT object emitter, and JIT engine
src/Embed.cpp     implementation of the C embedding API
src/stratac       the stratac CLI driver
tests/unit        unit + end-to-end tests (self-contained test framework)
samples/          example Strata programs + host drivers (see samples/README.md)
third_party/      (reserved for vendored deps)
```

## A note on the toolchain on this machine

The pre-existing LLVM at `C:\llvm` is an **ARM64** build that cannot run on this
**x86_64** host. A matching **x64 LLVM 22.1.0** was installed to
`C:\Users\andre\llvm-x64` (its `clang` runs, and `LLVM-C.dll`/`LLVM-C.lib` are
used for linking). The preset defaults reflect this.
