# Strata

A small, statically typed language for embedding in game engines. The compiler
is a single C11 library with no external dependencies; it JIT-compiles scripts
in memory (with LLVM ORCv2) or emits native objects and assembly
AOT for linking together with your project.

```c
extern int printf(string fmt, ...);

// Built-in SIMD vector types: float3 / float4 are 128-bit vectors with
// per-lane arithmetic, splatting (float4(s)) and swizzles (.xyz, .zyx, ...).
float4 add(ref float4 a, ref float4 b)
{
    return a + b;                 // {11, 22, 33, 44}
}

struct Particle
{
    float4 pos;                   // SIMD field
    Particle? next;              // optional link: maybe-empty, non-null box
};

int main()
{
    // Boxed / owned heap allocation (^T): auto-freed at scope end.
    ^float4 acc = float4(1.0, 2.0, 3.0, 4.0);

    // Dynamic array; elements created via array_push / literals.
    float4[] points = { float4(10.0, 20.0, 30.0, 40.0) };

    // ref = mutable borrow; no ownership transfer, no copy.
    for (uint i = 0; i < points.length; i++)
    {
        acc = add(acc, points[i]);
    }

    // Optionals (T?): a maybe-empty pointer slot. Reading THROUGH one
    // requires a "blessing" — proof it is non-empty.
    ^Particle head = Particle { .pos = acc };
    Particle? cur = head;
    while (cur?)                  // null check: blessing narrows `cur` to non-empty
    {
        printf("pos = %f %f %f %f\n",
               cur.pos.x, cur.pos.y, cur.pos.z, cur.pos.w);
        cur = cur.next;           // advance; old binding becomes empty again
    }

    // `defer` schedules a statement to run at the end of the enclosing block
    defer printf("[defer] acc = %f %f %f %f\n", acc.x, acc.y, acc.z, acc.w);
    defer printf("[defer] cleaning up\n");

    return (int)(acc.x + acc.y);  // 11 + 22 = 33
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

- `include/strata/strata.h` — public header
- `src/` — compiler sources
- `samples/` — example scripts and host drivers
- `tests/unit/` — test suite (runs via `ctest`)
