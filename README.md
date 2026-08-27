# Strata

A small, statically typed language for embedding in game engines. The compiler
is a single C11 library with no external dependencies; it JIT-compiles scripts
in memory (with LLVM ORCv2) or emits native objects and assembly
AOT for linking together with your project.

```c
extern int printf(string fmt, ...);

struct Vec3 { float x; float y; float z; };

float length_sq(Vec3 v)
{
    return v.x * v.x + v.y * v.y + v.z * v.z;
}

int main()
{
    Vec3 v = { .x = 1.0, .y = 2.0, .z = 2.0 };
    printf("%f\n", length_sq(v));   // 9.0
    return 0;
}
```

## Highlights

- **Statically typed** — structs, handles, fixed and dynamic arrays, overloads,
  full type inference and const-checking at compile time
- **Ownership built in** — `^T` boxes are move-only and auto-freed; `T?`
  optionals with flow-typed emptiness checks; no GC, no refcounting
- **Host-friendly ABI** — extern functions, varargs (`extern int printf(string
  fmt, ...);`), extern structs that mirror host C layouts field-for-field
- **LLVM codegen** — in-memory ORCv2 JIT, native object/assembly emission,
  and textual IR output
- **Embeddable** — one public header (`include/strata/strata.h`), virtual
  import graphs via a host-installed resolver, host-defined allocators

## Building

Requires CMake 3.20+, Ninja, and a C11 compiler. The default preset links an
x64 `LLVM-C.dll` (see `LLVM_C_DIR` in `CMakePresets.json`):

```sh
cmake --preset default
cmake --build --preset default
ctest --preset default
```

Binaries land in `build/default/bin/`.

## CLI

```sh
stratac samples/hello.strata -o hello.o      # emit native object (AOT)
stratac --run script.strata                  # JIT and run `main`
stratac --run --entry update script.strata   # ... or any int(void) function
stratac --asm script.strata -o script.s      # native assembly
stratac --emit-ir script.strata              # LLVM IR text
```

## Embedding

```c
#include "strata.h"

StrataCompiler* c = strataCompilerCreate();
StrataJit* jit = strataJitCompileFile(c, "scripts/game.strata", &err);
if (!jit) { /* err holds diagnostics */ }

strataJitAddSymbol(jit, "host_spawn", (void*)host_spawn);  // bind externs

int (*update)(float) = (int (*)(float))strataJitGetFunction(jit, "update");
int state = update(dt);

strataJitDestroy(jit);
strataCompilerDestroy(c);
```

A host can also take over `import` resolution entirely
(`strataSetImportResolver`) and compile a fully in-memory module graph with no
disk access.

## Repo layout

- `include/strata/strata.h` — the only public header
- `src/` — compiler sources (`Core`, `Lex`, `Parse`, `Sema`, `Codegen`, `Import`)
- `samples/` — example scripts and host drivers
- `tests/unit/` — test suite (runs via `ctest`)
- `AGENTS.md` — detailed architecture and contribution notes
