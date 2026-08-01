# C output and TinyCC JIT backend plan

Status: implemented and verified on Linux x86-64 (2026-07-31).

## Implementation results

The work landed as one commit per checkpoint:

1. `32804ed` vendors TinyCC and records this plan.
2. `b06a8b6` partitions LLVM and TinyCC build modes.
3. `0c32e05` adds the C output backend.
4. `219d61c` moves the public JIT to in-memory TinyCC.
5. `e0a1c88` adds the TinyCC-only static runtime profile.
6. `3c8c386` expands the backend and lifecycle test matrix.
7. `fe90553` adds deterministic large-file JIT benchmarks.

The final hardening checkpoint adds AST/container cleanup, original-source
`#line` mappings, C identifier and struct dependency handling, float remainder
parity, generated-C host compiler tests, and a no-LLVM binary audit. Validation
covers the full LLVM+TinyCC build, the `tcc-static` preset, full and TinyCC-only
shared libraries, frontend-only configuration, ASan/UBSan, standalone generated
C, repeated and parallel JIT lifecycles, and imported modules. Native Windows
and macOS execution remains a platform-CI responsibility; unsupported targets
fail during configuration rather than selecting a mismatched TinyCC target.

The Linux `tcc-static` stripped executable measured 269,032 bytes. Its dynamic
dependencies are only the platform loader, libc, and libm; the audit confirms
there are no LLVM sources, symbols, staging paths, or runtime dependencies and
that `tcc_compile_string` is incorporated into the executable.

A Release run over deterministic 1, 5, and 20 MiB fixtures completed 120 timed
samples (four source shapes, five latency views, two backends, three sizes). At
20 MiB, median file-to-callable latency was:

| Shape | LLVM MCJIT | TinyCC | TinyCC / LLVM |
| --- | ---: | ---: | ---: |
| arithmetic | 49.140 ms | 22.594 ms | 0.460x |
| control flow | 357.744 ms | 32.309 ms | 0.090x |
| structs | 132.300 ms | 23.383 ms | 0.177x |
| overloads | 197.936 ms | 21.610 ms | 0.109x |

These numbers are observational results for LLVM 21.1.7 and the vendored
TinyCC 0.9.28-strata on the implementation host, not performance thresholds.

The benchmark also measures already-loaded execution independently of compile
time. Each `hot(uint state, int rounds)` workload runs 4,096 data-dependent
unsigned rounds, receives the preceding call's result, and is checked against a
native reference. In an 11-sample run pinned to one CPU, median TinyCC / LLVM
execution ratios across the 1/5/20 MiB fixtures were 1.00-1.01x for arithmetic,
1.40-1.41x for branch-heavy control flow, 1.25-1.49x for struct mutation, and
1.18-1.21x for overloaded calls.

## Confirmed baseline

- The current compiler is C11, not the older C++ layout described by the
  supplied `AGENTS.md`. The implementation should follow the checked-in `.c`
  sources and current public C API.
- LLVM currently supplies all three code-generation paths: textual IR,
  object/assembly AOT output, and MCJIT. Parsing, import loading, overload
  resolution, and the AST are already shared before code generation.
- The requested modified tinycc-mob tree has been copied byte-for-byte to
  `third_party/tinycc/`. It is 540 files/about 6 MB, reports upstream revision
  `a338258d309c888bde96b2d1f206299231a54ddf`, and contains the Riblang
  embedded-build `config.h`. The tree includes its LGPL-2.1 `COPYING` and
  `RELICENSING` files.
- The existing suite is green (`ctest --test-dir build --output-on-failure`).
- A local proof compiled the vendored `libtcc.c` as one source, compiled a C
  string with `tcc_compile_string`, relocated it with `TCC_OUTPUT_MEMORY`,
  called generated scalar/struct code, and patched a typed extern slot after
  relocation. This validates the intended no-temporary-file JIT design.
- The proof also confirmed two requirements: TinyCC must have its `tccdefs.h`
  predefinitions embedded or available at runtime, and compiler-generated
  helpers such as `memset`/`memcpy` must be registered before relocation when
  `-nostdlib` is used.

## Chosen architecture

1. Keep LLVM for LLVM IR, native object output, and assembly output.
2. Add a separate AST-to-C emitter exposed as `STRATA_EMIT_C` and through a
   `stratac --emit-c` CLI mode.
3. Move the existing generic `strataJit*` API to the C/TinyCC path. JIT
   compilation will build a NUL-terminated C source buffer, pass it directly
   to libtcc, relocate into executable memory, and return symbols with
   `tcc_get_symbol`. It will never write the generated C or an object to disk.
4. Preserve the public JIT API and its ordering: callers may compile first,
   enumerate externs, register them with `strataJitAddSymbol`, and then call an
   entry point. Do not require all externs before compilation/relocation.
5. Keep AOT C and JIT C emission in one builder with a mode flag, just as the
   LLVM module builder currently varies extern lowering by `jitMode`.
6. Do not use `tcc_run`; “execute in memory” means keeping the relocated
   `TCCState` alive and invoking pointers returned by `strataJitGetFunction`,
   matching the current embedding model.
7. Retain the LLVM JIT implementation as an internal comparison path for
   performance tests, even though the public `strataJit*` API defaults to
   TinyCC. Do not expose a second public JIT API solely for benchmarking.
8. Add a size-oriented `tcc-static` build profile that does not discover,
   compile, link, stage, or load LLVM at all. It contains the shared front end,
   C emitter, and in-memory TinyCC JIT, with Strata and libtcc linked directly
   into the `stratac` executable.

## Build profiles and no-LLVM contract

Partition CMake around explicit `STRATA_ENABLE_LLVM` and
`STRATA_ENABLE_TCC` options instead of treating LLVM as unconditional:

| Profile | LLVM | C/TinyCC | Strata linkage | Intended use |
| --- | --- | --- | --- | --- |
| `full` (default) | IR, AOT, internal benchmark JIT | C output and public JIT | static or shared | Development, comparison, native AOT |
| `tcc-static` | completely absent | C output and public JIT | static | Small redistributable compiler/JIT |

The `tcc-static` preset uses `MinSizeRel`, `STRATA_ENABLE_LLVM=OFF`,
`STRATA_ENABLE_TCC=ON`, and `STRATA_SHARED=OFF`, producing
`build/tcc-static/bin/stratac[.exe]`. “Static” guarantees that the Strata and
TinyCC code is incorporated into that executable and that no LLVM or libtcc
DLL/shared object is needed. A separate `STRATA_STATIC_RUNTIME` option may add
the platform/toolchain flags for a fully static C runtime on MinGW or a
static-libc Linux toolchain; it must fail clearly when unsupported and is not
claimed on macOS.

Implementation rules for the no-LLVM profile:

- Split source lists into common front-end/C-emitter sources, TinyCC sources,
  and LLVM sources. Put `find_package(LLVM)`, LLVM include paths/libraries, DLL
  staging, `LLVMCApi.h`, and every `LLVM*.c` source entirely inside the LLVM
  option. Configuring `tcc-static` must succeed on a machine with no LLVM
  package, headers, library, or `LLVM_C_DIR`.
- Keep one public header/ABI. Add build capability bits (C output, TCC JIT,
  LLVM IR, LLVM AOT) and a small `strataCapabilities()` query. In a no-LLVM
  build, requests for `STRATA_EMIT_LLVM_IR` and `strataCompileToObject` fail
  with an owned, explicit “LLVM backend not built” diagnostic, while
  `strataLLVMVersion()` returns a stable disabled value without referencing
  LLVM. Export the capability query from shared builds and `strata.def`.
- Compile CLI help and behavior from the same capability bits. Add
  `stratac --run <file.strata>` (optional `--entry`, default `main`) so the
  small executable directly exercises the in-memory JIT; initially accept an
  `int(void)` entry and reject unresolved host externs with their names. In the
  no-LLVM profile, `--emit-c`, `--ast`, and `--run` work, while object/assembly
  requests explain that the LLVM profile is required. A bare invocation that
  would normally emit an object must not silently change formats.
- Build the size profile with `-Os`/`/O1`, function/data sections, linker
  dead-code elimination, and optional supported IPO/LTO. Provide a stripped
  copy or size-report target without destroying the normal executable. Record
  text/data/total file size and dynamic dependencies; set a numeric size budget
  only after the first measured cross-platform baseline.
- Keep the full build as the only mode that builds the side-by-side
  LLVM-versus-TinyCC benchmark. The same benchmark executable may run its TCC
  columns in `tcc-static`, but comparison reports require `full`.

Initial TinyCC build support will mirror the known working Riblang matrix:

| Host | Architectures | TinyCC definitions |
| --- | --- | --- |
| Windows | x86-64 | `TCC_TARGET_X86_64`, `TCC_TARGET_PE` |
| Linux | x86-64 | `TCC_TARGET_X86_64` |
| macOS | x86-64, arm64 | matching CPU target plus `TCC_TARGET_MACHO` |

CMake should fail at configure time with a clear message for other hosts,
rather than silently building a mismatched JIT.

## Emitted C contract

Emit a conservative C99/C11-compatible translation unit that needs no system
headers. Normal generated C should compile with TinyCC, GCC, Clang, and MSVC
where the language ABI permits it.

| Strata construct | C representation |
| --- | --- |
| `void`, `bool` | `void`, `_Bool` |
| `int`, `uint` | 32-bit `int`, `unsigned int`, guarded by a generated compile-time size check |
| `float`, `double` | `float`, `double`; float literals carry an `f` suffix |
| `handle H` | forward-declared opaque C struct pointer typedef |
| `struct S` | named C struct with the same ordered fields |
| plain scalar parameter | by value |
| `ref` scalar parameter | pointer, with reads/writes emitted through the pointer |
| any defined-struct parameter | pointer; `const` produces pointer-to-const |
| struct local/return | value type, preserving current Strata behavior |
| uninitialized local/global | explicit zero initialization |

Additional emitter rules:

- Emit all type declarations and function prototypes before definitions so
  forward calls and recursion work. Forward-declare struct names and order
  complete struct bodies by value dependency; diagnose impossible by-value
  cycles.
- Use a centralized, deterministic C-name encoder. Preserve legal,
  non-overloaded public and extern function names for host ABI compatibility;
  encode `$` overload names as standard C identifiers; escape C keywords and
  reserved helper-name collisions. Retain a Strata-name-to-C-symbol map for
  JIT lookup. Diagnose public ABI names that cannot be represented portably.
- Extend semantic resolution to cache each expression’s resolved type on the
  AST (calls already retain `resolvedDecl`). The C backend must consume Sema’s
  decisions rather than duplicate overload/type inference.
- Maintain a scoped emitter symbol table recording type, constness, and
  whether an identifier is already indirect. Use it for member access,
  lvalues, `ref` calls, and struct-by-reference calls.
- Emit struct constructors/braced initializers as C compound literals, with
  designated initializers for named fields and C zero-fill for omitted fields.
  Materialize a temporary when a struct rvalue must be passed by address.
- Preserve expression precedence with explicit parentheses. Emit direct C
  control flow for `if`, `while`, `for`, `break`, and `continue`; C already
  supplies the required short-circuit behavior for `&&`/`||`.
- Emit an explicit zero return at the end of a non-void function when Strata’s
  current LLVM lowering would synthesize one.
- Treat the existing vector types as structural preview types: emit stable
  wrapper declarations so structs containing them can be emitted, but report
  a backend diagnostic for vector construction/arithmetic until the language
  defines those operations and their cross-backend ABI.
- Cover semantic edge cases where C and LLVM differ (notably signed overflow,
  float remainder, NaN comparisons, and bool conversion) with explicit tests
  and helper/cast lowering where required; do not silently inherit undefined C
  behavior if it changes an already-tested Strata result.
- Emit `#line` directives at top-level declarations using `SourceRange.fileId`
  and the existing `SourceManager` array, so TinyCC diagnostics refer to the
  originating `.strata` file, including imports.

### Extern lowering

AOT C output uses ordinary typed `extern` declarations and direct calls so the
generated `.c` can be compiled and linked with a host normally.

JIT C output instead declares one non-static, correctly typed function-pointer
global per extern, for example `__strata_ext_host_add`, and routes calls through
it. No unresolved host symbol is therefore present when TinyCC relocates.
`strataJitAddSymbol` looks up the slot’s address with `tcc_get_symbol` and writes
the supplied host pointer into it. Extern enumeration continues to expose the
original Strata names, and unknown names continue to return failure.

## Implementation sequence

### 1. Finish dependency integration

- Add `third_party/tinycc/STRATA-VENDOR.md` with source path, Snow commits
  `0cedafb9`/`57412f37`, upstream revision, local config delta, update procedure,
  and license/linking notice.
- Add a `strata_tinycc` static CMake target compiling only
  `third_party/tinycc/libtcc.c` with `ONE_SOURCE=1`, `CONFIG_TCC_STATIC=1`, C99,
  target-specific CPU/object-format definitions, and warning suppressions
  scoped to the vendor target.
- Link `m` on Unix where required and propagate the static dependency correctly
  through both static and shared Strata builds. Do not expose TinyCC headers as
  part of Strata’s public include surface.
- Make the embedded library self-contained: generate `tccdefs_.h` from
  `third_party/tinycc/include/tccdefs.h` in the build directory using TinyCC’s
  adjacent-C-string format, put that directory first in the vendor target’s
  private includes, and set `CONFIG_TCC_PREDEFS=1`. This avoids a compiled-in
  absolute source path or runtime dependency on the checkout.
- Enable TinyCC’s compilation semaphore or otherwise serialize its global
  compilation state; add a parallel-JIT smoke test so this choice is verified
  rather than assumed.

### 2. Add the shared C module builder

- Add `src/Codegen/CBackend.h` and `src/Codegen/CBackend.c`.
- Introduce an owned `BuiltCModule` containing generated source, original
  extern names, JIT slot names, and exported-symbol mappings, with explicit
  init/dispose functions.
- Implement `BuildCModule(module, diagnostics, arena, sources, sourceCount,
  jitMode)` and a `GenerateC` wrapper returning the existing `CodegenResult`
  shape for text emission.
- Add the expression type cache in `src/AST/AST.h` and populate it in
  `src/Sema/ResolveOverloads.c`; add focused Sema tests for literals, members,
  casts, constructors, overload calls, globals, and ref arguments.
- Implement type/name/prototype emission first, then globals, expressions,
  statements, initializers, and the AOT/JIT extern split. Route all failures
  through `DiagnosticEngine` with original source ranges.

### 3. Expose C output

- Add `STRATA_EMIT_C` to `StrataEmitKind` in `include/strata/strata.h` without
  renumbering the existing values.
- Make `BuildResult` in `src/Embed.c` explicitly switch across AST, LLVM IR,
  and C output; preserve the current result ownership rules.
- Add `stratac --emit-c`. With this flag, `-o` names the C file and defaults to
  the input basename plus `.c`; do not also emit an object. Reject incompatible
  combinations such as `--emit-c --asm`. Keep default object and `--asm`
  behavior on LLVM.
- Add the `--run`/`--entry` mode described by the no-LLVM contract, using
  `strataJitCompileFile` and `strataJitGetFunction` rather than a separate CLI
  compilation path.
- Add C output usage and the generated aggregate/handle ABI to `README.md` and
  update the repository backend notes in `AGENTS.md` after implementation.

### 4. Implement the in-memory TinyCC engine

- Add `src/Codegen/TccJit.h` and `src/Codegen/TccJit.c`, including only
  `third_party/tinycc/libtcc.h` privately.
- Own a `TCCState` for the complete `StrataJit` lifetime. Install an error
  callback before all other operations and accumulate all TinyCC warnings and
  errors into owned diagnostics text.
- Configure `-nostdlib -nostdinc` and the memory output type before compiling;
  feed the generated buffer to `tcc_compile_string` and never call a file-output
  API.
- Register the small internal runtime allowlist needed by compiler-emitted C
  (`memcpy`, `memmove`, `memset`, plus any target helper demonstrated by the
  parity suite) before `tcc_relocate`. Keep user externs as generated slots,
  not as unresolved TinyCC symbols.
- Relocate once, retain the state, implement exported-function and slot lookup
  through the mappings in `BuiltCModule`, and copy extern-name metadata before
  the parse arena is released.
- Merge front-end/C-emitter diagnostics and TinyCC callback text into
  `strataJitDiagnostics`/`errOut`. Ensure every failure path deletes the state
  and frees source, vectors, maps, and diagnostic buffers exactly once.
- Change `JitFromModule` and the generic public JIT operations in `src/Embed.c`
  to use `TccJit`. Keep `LLVMJit.c/.h` buildable behind an internal backend
  selector used by the benchmark target, so both engines can consume the same
  resolved module in one process without changing the public ABI.

### 5. Add the small `tcc-static` build mode

- Refactor `CMakeLists.txt` into common, TCC, and LLVM source/target sections;
  make LLVM discovery conditional and add generated capability definitions for
  library sources, tests, and CLI code.
- Add `tcc-static` configure/build/test presets with a separate build directory
  and no inherited `LLVM_C_DIR`. Add `STRATA_STATIC_RUNTIME` and size-optimized
  linker settings as independently testable options.
- Add the capability API and no-LLVM stubs for LLVM-specific public operations,
  then make CLI version/help/output selection capability-aware.
- Ensure the executable references the JIT through `--run`, allowing normal
  section garbage collection while retaining exactly the TinyCC memory path
  needed at runtime.
- Add `size-tcc-static` and dependency-inspection targets/scripts that report
  both unstripped and stripped artifacts without modifying build outputs.

### 6. Tests and hardening

- Add `tests/unit/CBackendTests.c` to CMake. Test C text for scalar types,
  literal suffixes, forward prototypes, overload-safe names, handles, nested
  structs, zero initialization, mutable/const struct pointers, scalar `ref`,
  constructors, globals, casts, control flow, and both extern-lowering modes.
- Add TCC error-callback tests and a regression proving compile -> relocate ->
  `strataJitAddSymbol` -> function call works in that order.
- Run the entire existing generic JIT suite unchanged against TinyCC. It
  already covers arithmetic, calls, floats, recursion, large loops,
  break/continue, short circuiting, casts, globals, ref parameters, struct
  layout/mutation/return/construction, opaque handles, host externs, overloads,
  and imported modules.
- Keep LLVM-specific IR and AOT tests intact. Add an emitter parity table only
  where a behavior is intentionally backend-specific. Guard or split those
  test cases so the no-LLVM test executable never includes an LLVM header or
  unresolved LLVM reference.
- Add a CLI integration test that writes `hello.c`, compiles it with a normal C
  compiler, runs it, and observes exit code 25. Also link a generated extern-
  struct sample with its host to verify the documented pointer ABI.
- Add a `tcc-static` CLI test running `stratac --run samples/hello.strata` and
  observing exit code 25, plus negative tests for `--asm`, default object
  output, LLVM IR emission, unresolved externs, and unsupported entry
  signatures.
- Configure the no-LLVM preset in an environment where LLVM discovery would
  fail, then inspect its compile database, link map, symbols, and runtime
  dependencies (`nm`/`objdump`/`ldd` or Windows equivalents). Fail the test if
  any `LLVM`, `LLVM-C`, MCJIT, or LLVM staging path appears. Confirm libtcc is
  incorporated into the executable rather than loaded dynamically.
- Run ASan/UBSan where available over repeated compile/destroy cycles and a
  parallel compile smoke test. Verify stale function pointers are documented
  as invalid after `strataJitDestroy`.

### 7. Large-file JIT performance benchmarks

- Add an opt-in Release-mode benchmark executable at
  `tests/perf/JitCompileBench.c`, built as `strata_jit_bench`. Keep it out of
  the default unit-test/CTest path so normal correctness tests remain fast and
  timing noise never makes CI flaky.
- Have the benchmark generate deterministic `.strata` fixtures under the build
  directory before timing. Cover approximately 1 MiB, 5 MiB, and 20 MiB source
  tiers (with a command-line scale override) and record exact bytes, lines,
  declarations, and functions for every result.
- Generate several source shapes rather than one repeated-token stress case:
  many small forward-calling functions, large control-flow/arithmetic bodies,
  many nested structs plus member operations, overload-heavy call sites, and a
  multi-file import graph. Every fixture must expose an `entry` function with
  a deterministic result that both JITs call once outside the timing window.
- Compare LLVM MCJIT and C/TinyCC with identical source and front-end work.
  Alternate which backend runs first, perform at least one untimed warm-up, run
  enough measured iterations for a stable median, destroy each JIT between
  iterations, and reject a sample if either backend returns the wrong result.
- Report three latency views so the result answers both end-to-end and backend
  questions:

  | Measurement | Start | Stop | Purpose |
  | --- | --- | --- | --- |
  | file-to-callable | before loading the `.strata` file/import graph | after executable-memory relocation | Real `strataJitCompileFile` hot-reload cost |
  | source-to-callable | source already read into memory, before lex/parse/Sema | after executable-memory relocation | Fair in-memory consumption of the same source bytes |
  | AST-to-callable | one already parsed/resolved AST | after executable-memory relocation | Isolates LLVM IR+MCJIT versus C emission+libtcc |

- Instrument the internal pipeline with optional timing output (public calls
  pass `NULL`) and break backend-only latency into LLVM module construction and
  execution-engine creation, versus C source emission, `tcc_compile_string`,
  and `tcc_relocate`. Keep timer calls outside inner AST emission loops.
- For imported fixtures, use file-to-callable timing only; for single-file
  fixtures, include all three views. Read files and generate fixtures outside
  the source-to-callable and AST-to-callable windows. Neither backend may emit
  temporary C, IR, object, or assembly files during a measured JIT run.
- Print a compact console table and optionally write CSV/JSON containing per
  phase/backend/workload: minimum, median, p95, source MiB/s, iteration count,
  and the TinyCC/LLVM latency ratio. Also record compiler versions, build type,
  OS, architecture, and fixture seed so results are reproducible.
- Add CMake targets such as `strata_jit_bench` and `bench-jit`; document a
  canonical invocation in `README.md`, for example
  `cmake --build build --target bench-jit`. Performance numbers are
  observational, not hard pass/fail thresholds; correctness and successful
  result generation are the automated requirements.

## Acceptance criteria

- `stratac --emit-c samples/hello.strata -o <build>/hello.c` produces readable,
  standalone C that a normal host compiler links and whose executable exits 25.
- `strataCompileString/File(..., STRATA_EMIT_C)` returns that same C in memory
  with normal diagnostics and ownership behavior.
- Every existing `jit_*` test passes through `tcc_compile_string` and
  `TCC_OUTPUT_MEMORY`; the JIT path performs no temporary-file I/O.
- Current host registration behavior and aggregate/handle ABI remain intact,
  including registering externs after relocation.
- LLVM IR, object, and assembly output remain green and unchanged by default.
- Static and shared builds succeed on Windows x64 and Linux x64; the macOS
  configurations at least compile and have targeted JIT smoke coverage before
  being advertised as supported.
- The `tcc-static` preset configures and builds with LLVM absent, embeds Strata
  and libtcc into one size-optimized executable, reports its stripped size and
  dependencies, contains no LLVM symbols/dependencies, and runs
  `samples/hello.strata` in memory with exit code 25.
- No-LLVM builds expose accurate capability bits and fail LLVM-only API/CLI
  requests deterministically without missing symbols, crashes, or format
  fallback.
- Failure diagnostics identify the original Strata source/import and no JIT or
  generated-source allocation survives `strataJitDestroy` or an error path.
- The opt-in benchmark produces side-by-side LLVM and TinyCC timing summaries
  for every large fixture, including source-to-callable and AST-to-callable
  medians and their ratio, while verifying both generated entry points return
  the same result.
