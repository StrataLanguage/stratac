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
| `control_flow.strata` | `if`/`else`, `while`, `break`/`continue`, recursion, `out` params | text (IR inspection) |
| `structs.strata` | structs, member access, positional construction, nested structs, by-value | native |
| `overloads.strata` | type-based overload resolution (int / float / struct) | native |
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

The native back-end doesn't lower control flow yet, so inspect it as text IR
(or the AST):

```sh
stratac --no-llvm --emit ir samples/control_flow.strata
stratac --emit ast samples/control_flow.strata
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

- **Control flow** (`if`/`else`, `while`, `for`, `break`, `continue`) is lowered
  by the text IR back-end only; the native (JIT/AOT) back-end lowers straight-
  line code and is gaining control flow next. Samples that need it are tagged
  "text" above.
- **`out`/`inout` parameters** are parsed (see `control_flow.strata`); full
  by-reference lowering is in progress.
- **Vector types** (`float4`, `int3`, ...) are recognized by the type system and
  lower to LLVM vectors, but construction/per-component arithmetic aren't
  lowered yet (`vectors.strata` is a structural preview).
- **Aggregates across the host boundary**: structs are value types *within*
  Strata. Crossing the host<->JIT boundary with a struct by value is
  ABI-sensitive on Windows; use scalars or opaque handles there. See the main
  README's "A note on aggregates across the host/JIT boundary".
