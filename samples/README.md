# Strata samples

Example programs that exercise the language feature by feature, with notes on
which back-end each one uses. Build the compiler first
(`cmake --preset default && cmake --build --preset default`); the driver is at
`build/default/bin/stratac`, and a native assembler/linker at
`C:/Users/andre/llvm-x64/bin/clang` (or any x64 `clang`/`gcc`).

## Samples

| File | Demonstrates | Back-end |
|------|--------------|----------|
| `hello.strata` | functions, arithmetic, calls | text **and** native |
| `control_flow.strata` | `if`/`else`, `while`, `for`, `break`/`continue`, recursion, `out`/`inout` params | text **and** native |
| `structs.strata` | structs, member access, positional construction, nested structs, by-value | native |
| `overloads.strata` | type-based overload resolution (int / float / struct) | native |
| `extern_struct.strata` + `hosts/extern_struct_host.c` | structs cross the host boundary by pointer (`in`/`out`) | native |
| `jit_demo.strata` + `hosts/jit_demo_host.c` | **JIT embedding**: load a file, bind externs, compile, call in-process | native (JIT) |
| `engine_demo.strata` + `hosts/engine_demo_host.c` | **dual mode**: same script, JIT (develop) or AOT-linked (ship) | native (both) |
| `extern_math.strata` + `hosts/extern_math_host.c` | `extern` host functions, AOT link-and-run | native |
| `engine_api.strata` + `hosts/engine_api_host.c` | opaque engine handles (`handle`), AOT link-and-run | native |
| `vectors.strata` | HLSL-style vector types (structural preview) | AST/type check |

### hello.strata

```sh
stratac --emit ir   samples/hello.strata            # in-process LLVM IR
stratac --emit obj  samples/hello.strata -o hello.o # native object
clang hello.o -o hello.exe && ./hello.exe ; echo $? # runs; exit code 25
```

### control_flow.strata

`if`/`else`, `while`, `for`, `break`/`continue`, recursion, and `out`/`inout`
parameters. The LLVM back-end lowers it:

```sh
stratac --emit ir samples/control_flow.strata     # LLVM IR
stratac --emit obj samples/control_flow.strata -o control_flow.o
clang control_flow.o -o control_flow.exe && ./control_flow.exe ; echo $?   # 255
```

### structs.strata

Structs are value types used inside Strata; the host reads results through a
scalar entry point (`entry`).

```sh
stratac --emit ir  samples/structs.strata
stratac --emit obj samples/structs.strata -o structs.o
```

### overloads.strata

Several `length` functions share a name; each call resolves to the overload
whose parameter types best match the arguments:

```sh
stratac --emit ir  samples/overloads.strata     # see length$int / length$float / length$Vec3
stratac --emit obj samples/overloads.strata -o overloads.o
clang overloads.o -o overloads.exe && ./overloads.exe ; echo $?   # 15
```

### extern_struct.strata -- structs cross the host boundary by pointer

On `extern` functions, a struct parameter declares `in`/`out`/`inout` and is
lowered to a pointer (by-value struct passing is ABI-fragile). The host
implements the matching pointer signature.

```sh
stratac --emit obj samples/extern_struct.strata -o extern_struct.o
clang samples/hosts/extern_struct_host.c extern_struct.o -o extern_struct.exe
./extern_struct.exe        # entry() = 125
```

### jit_demo -- load, compile, and run in-process (JIT embedding)

A host program that uses the Strata C API to load a `.strata` file, bind the
`extern` functions it declares, JIT-compile it to native code, and call the
entry point through a function pointer -- no on-disk object, no link step.

```sh
cmake --build --preset default --target jit_demo
build\default\bin\jit_demo.exe                              # loads samples/jit_demo.strata
build\default\bin\jit_demo.exe path\to\your.strata          # or any .strata file
```

Output:
```
== Strata JIT demo ==
loading samples/jit_demo.strata
script declared 4 extern symbol(s); binding...
  bound entity_create    -> 0x...
  bound entity_get       -> 0x...
  bound entity_set       -> 0x...
  bound entity_destroy   -> 0x...
run(7) = 55  (expected fibonacci(10) = 55)
```

### engine_demo -- same script, two shipping strategies

One script and one host file (gated by `#ifdef USE_JIT`) demonstrate the
dual-mode workflow a game engine can use:

- **AOT (ship)**: `stratac --emit obj` pre-compiles the script to an object;
  `clang` links it with the host. No Strata library at runtime.
- **JIT (develop)**: the host loads the `.strata` at runtime via the JIT API,
  compiles it in-process, and calls through a function pointer.

```sh
cmake --build --preset default --target engine_demo   # builds the JIT variant
samples\run_engine_demo.bat                           # builds AOT + runs both
```

Output:
```
==================== AOT mode (pre-compiled, linked) ====================
[AOT] script pre-compiled and linked
chase(attacker, target, 2) = 26

==================== JIT mode (loaded at runtime) =======================
[JIT] loading samples/engine_demo.strata
chase(attacker, target, 2) = 26
```

### extern_math.strata -- script calls the host

```sh
stratac --emit obj samples/extern_math.strata -o extern_math.o
clang samples/hosts/extern_math_host.c extern_math.o -o extern_math.exe
./extern_math.exe        # lucky_number(42) = 99
```

### engine_api.strata -- opaque engine handles

```sh
stratac --emit obj samples/engine_api.strata -o engine_api.o
clang samples/hosts/engine_api_host.c engine_api.o -o engine_api.exe
./engine_api.exe         # run() = 15
```

## Status notes

- **Control flow** (`if`/`else`, `while`, `for`, `break`, `continue`) and
  **recursion** are lowered by the native (JIT/AOT) back-end.
- **`out`/`inout` parameters** are lowered to pointers: the callee writes through
  them, and the caller's variable is updated. They cross the host<->JIT boundary
  as pointers (the host passes `&var`).
- **Vector types** (`float4`, `int3`, ...) are recognized by the type system and
  lower to LLVM vectors, but construction/per-component arithmetic aren't
  lowered yet (`vectors.strata` is a structural preview).
- **Aggregates across the host boundary**: structs are value types *within*
  Strata. Across the Strata->host boundary they always go by pointer -- an
  `extern` struct parameter must declare `in`/`out`/`inout` (see
  `extern_struct.strata`); opaque handles are pointer-sized already. Only a host
  calling *into* a Strata entry point that takes/returns a struct by value is
  ABI-sensitive -- prefer scalar/handle entry points. See the main README's
  "A note on aggregates across the host/JIT boundary".
