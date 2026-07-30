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
| `extern_math.strata` + `hosts/extern_math_host.c` | `extern` host functions, AOT link-and-run | native |
| `engine_api.strata` + `hosts/engine_api_host.c` | opaque engine handles (`extern struct`), AOT link-and-run | native |
| `vectors.strata` | HLSL-style vector types (structural preview) | AST/type check |

### hello.strata

```sh
stratac --emit ir   samples/hello.strata            # in-process LLVM IR
stratac --emit obj  samples/hello.strata -o hello.o # native object
clang hello.o -o hello.exe && ./hello.exe ; echo $? # runs; exit code 25
```

### control_flow.strata

`if`/`else`, `while`, `for`, `break`/`continue`, recursion, and `out`/`inout`
parameters. Both back-ends lower it:

```sh
stratac --no-llvm --emit ir samples/control_flow.strata     # text IR
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
  **recursion** are lowered by both the text and the native (JIT/AOT) back-ends.
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
