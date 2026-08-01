# Strata

A small, fast scripting/programming language designed to be embedded in game
engines. Strata can JIT through its vendored TinyCC backend or emit native
objects and assembly through LLVM.

The public embedding JIT compiles generated C directly from memory and keeps
the relocated code in memory; it does not create temporary source or object
files. LLVM remains available for textual IR, AOT output, and development
comparisons.

## Build profiles

The default profile builds both backends:

```sh
cmake --preset default
cmake --build --preset default
ctest --preset default
```

The size-oriented profile builds `stratac` with the C/TinyCC path and no LLVM
discovery, source, linkage, or runtime dependency:

```sh
cmake --preset tcc-static
cmake --build --preset tcc-static
ctest --preset tcc-static
cmake --build build/tcc-static --target stratac-size-report
```

`stratac-size-report` makes and strips a separate `stratac-stripped` copy,
then reports section/file sizes and dynamic platform dependencies without
altering the normal executable. Strata and libtcc are incorporated statically;
the platform C runtime may still be dynamic.

## C output and in-memory execution

```sh
stratac --emit-c script.strata -o script.c
stratac --run script.strata
stratac --run --entry update script.strata
```

`--run` currently accepts a defined `int(void)` entry (`main` by default) and
returns its result as the `stratac` process exit status. Standalone execution
rejects scripts with unresolved host externs; embedding applications can bind
those after compilation with `strataJitAddSymbol`.

The vendored TinyCC source and its LGPL-2.1 licensing/provenance are documented
in `third_party/tinycc/STRATA-VENDOR.md`.

## LLVM versus TinyCC JIT benchmark

The full build provides an opt-in benchmark that generates deterministic large
`.strata` fixtures in the build directory and compares both in-memory engines:

```sh
cmake --build build --target bench-jit
build/bin/strata_jit_bench --sizes 1,5,20 --iterations 3 \
    --csv build/jit-benchmark.csv
build/bin/strata_jit_bench --runtime-only --sizes 1,5,20 --iterations 7 \
    --csv build/jit-runtime.csv
```

It reports file-to-callable, source-to-callable, AST-to-callable, backend-build,
relocation, and post-load execution medians, p95 latency, throughput, and the
TinyCC/LLVM ratio. Hot execution uses a dynamic unsigned workload specialized
for each source shape, chains results between calls, runs 4,096 data-dependent
rounds per call, and validates both JITs against a native reference before
timing. `--runtime-only` skips the compile-latency passes. The benchmark is
observational and is not part of CTest.
