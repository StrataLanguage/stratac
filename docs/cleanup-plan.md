# Codebase cleanup plan

Scope: everything under `src/`, `include/`, `utilities/`, plus the style tooling. ~40.6k lines of C; three
files hold 61% of it:

| File | Lines | Problem |
|---|---|---|
| `src/Sema/ResolveOverloads.c` | 11,151 | Whole type checker: const eval, moves, borrows, calls, every builtin, every expr/stmt |
| `src/Codegen/LLVMModuleBuilder.c` | 10,239 | Whole IR emitter: strings, arrays, boxes, ownership, equality, vectors, calls, stmts, const folding |
| `src/Parse/Parser.c` | 3,312 | Types, decls, stmts, exprs, literals in one file |
| `src/Sema/MoveCheck.c` | 2,950 | Places, facts, loans, write sets, driver |

Single functions that need breaking up: `ResolveExprImpl` (~1,550 lines), `EmitCall` (~770), `ResolveCall`
(~660), `EmitStmtInner` (~600), `BuilderBuild` (~560), `WalkStmt` (~545), `EmitBinary` (~430), `EmitAssign`
(~400).

## Ground rules

- Behaviour does not change. Every phase ends with `ctest --preset default` green, plus the ASan variants
  (`asan/*` cases, `aot_components_test` ASan) and the ownership fuzzer smoke run.
- One concern per commit. File moves are **pure moves** (bodies byte-identical) in their own commit, so
  `git log --follow` / `git blame -C` keep working. Renames are their own commits. Logic changes never ride
  along with a move or rename.
- Whitespace-only commits go into `.git-blame-ignore-revs`.
- The public C API in `include/strata/` keeps its names (hosts link against it). Script-level builtin names
  (`array_push`, `array_take`, ...) are language surface and are not touched.

## Decisions to confirm before starting

Defaults are what this plan assumes.

1. **Member prefix `m_`.** `Builder`, `Resolver`, `Lexer`, `Parser`, `DiagnosticEngine` and others use
   `m_builder`, `m_diag`; `TypeDesc`, `Vec` use plain camelCase; `Arena` uses snake_case. Default: drop `m_`
   everywhere → plain camelCase (`b->builder`, `r->diag`). ~3,000 mechanical edits.
2. **Comment style.** Today ~1,300 `/* */` vs ~400 `//`, plus a few `/** @brief */` blocks. Default: `//` for
   all comments, no Doxygen tags.
3. **Non-flag enum constants.** Default: keep the existing TitleCase-with-prefix style (`NodeModule`,
   `BinAdd`, `MoveDiagUse`); only bitflag enums use the `PREFIX_NAME` form below.
4. **`TypeDesc` bools → one flags field.** Default: yes (see Phase 6). `TypeName` in the AST stays as bools.

## Phase 0 — tooling baseline

- `.clang-format` / `.clang-tidy` describe C++ (`Standard: c++20`, `m_` private members, PascalCase enum
  constants). Rewrite both for C11 and for the conventions in this plan:
  functions/structs/typedefs `CamelCase`, variables/params/members `camelBack`, global constants and
  bitflag constants `UPPER_CASE`, public `strata*` / `STRATA_*` exempt.
- One `clang-format -i` over the tree (it is not currently formatted to its own config — e.g. AST.h uses
  `typedef struct {` against `BreakBeforeBraces: Allman`). Add the commit to `.git-blame-ignore-revs`.
- Add a `ctest` target `style_gate` that runs `clang-format --dry-run -Werror` over `src/` so drift is caught.
  The tidy naming check is run by hand per phase (too slow for every test run).

## Phase 1 — remove duplicated logic

Each item is a small, testable commit. Found so far:

| Duplicate | Where | Fix |
|---|---|---|
| Constant folding (int arith, float arith, int/float compare, init folding, casts) | `Sema*Fold*` in ResolveOverloads.c:559–996 and `Fold*` / `CastConstVal` in LLVMModuleBuilder.c:9084–9497 | One `src/Sema/ConstFold.{c,h}` over a small `ConstValue` type; both sides call it. Follow-up (separate commit): sema stores the folded value on the node so codegen stops re-folding. |
| `IsArrayMutator` | MoveCheck.c:1278 and ResolveOverloads.c:3294 | **The lists have already drifted**: sema's copy lacks `array_replace` and `array_take`. Replace both with one array-builtin table (name + `ArrayBuiltinFlags`: mutates / grows / removes) in `src/Sema/Builtins.{c,h}`. Add a regression test for borrow inference over `array_take`/`array_replace` first. |
| `IsIdentStart` / `IsIdentCont` | Lexer.c:12 and ResolveOverloads.c:1609 | Export from `Lex/Lexer.h`. |
| `FindEnumDecl` | LLVMModuleBuilder.c:3826 and TypeMetadata.c:240 (plus sema's `FindEnum`, `FindImplDecl`) | `ModuleFindEnum` / `ModuleFindImpl` in `AST/AST.c`. |
| `EnsureTargetsInitialized` | LLVMAot.c:8 and LLVMJit.c:24 | `Codegen/LLVMTargets.{c,h}`. |
| Scope / defer stacks | Cfg.c, LLVMModuleBuilder.c, ResolveOverloads.c | **Dropped:** the shared part is ~75 lines and each pass does something different per defer; a shared module would save ~30. Instead, merge sema's parallel `m_scopeDecls` / `m_deferFrames` into one frame struct. |
| Two move trackers | Path-based in ResolveOverloads.c vs CFG-based `MoveCheck.c` | Investigated. The sema tracker is the only source of three diagnostics (move from a global, from a const ref, from a ref-bound place) plus the "cannot be reassigned ... bound as a ref" check, so it can't be deleted yet. Now: delete its ~250 lines of zero-caller helpers (`PathIsDescendant`, `PathMayAlias`, `ClearAliasedSubtree`, `CopyStrMap`, ...) and the write-only `m_refThroughBox` / `m_indexWriteTarget`. Later (separate change): port the three rules into MoveCheck as new `MoveDiag` kinds, add a test for the untested reassign check first, then delete the rest (~600 lines). |
| `sizeof(a)/sizeof(a[0])` | several | Use the existing `ARRAY_COUNT`. |
| `TypeNameBoxWrap` / `OptionalWrap` / `ArrayWrap` | AST.h | One private `TypeNameWrapInner` helper. |

Also sweep for re-implementations of things `Core/Util.h` and `Codegen/TypeUtil.h` already provide (string
compare loops, manual `Vec` growth, alias resolution) and replace them.

## Phase 2 — split files by feature

Pure moves. Shared declarations go into a private header per directory; the mid-file forward declarations
(46 in LLVMModuleBuilder.c, 31 in ResolveOverloads.c) go away. `CMakeLists.txt` source lists and the
`type_flag_gate` layer list in `tests/typelayer/CheckTypeFlags.cmake` are updated in the same commit.

### `src/Codegen/LLVMModuleBuilder.c` → `src/Codegen/Builder/`

| New file | Contents (current line ranges) |
|---|---|
| `BuilderInternal.h` | `Builder`, `Loop`, `BlockScope`, cross-file prototypes |
| `IrUtil.c` | `I32Ty`/`I64Ty`/`I1Ty`, `IdxConst`, `NewBb`, `EntryAlloca`, `Br`, predicate tables (166–447) |
| `Types.c` | `TypeDescMake`, `ScalarLlvmType`, `VectorLlvmType`, `Resolve`, `Coerce`, defaults/zero values (99–165, 1020–1510) |
| `Runtime.c` | alloc/free/panic/oob helper fns, bounds checks, message globals (1511–1650) |
| `Numeric.c` | `EmitBinary` numeric paths, division/shift checks, mixed-sign compare, float→int saturating, enum cast check |
| `Strings.c` | fat string values, concat, strlen, strdup/streq/cstrlen bodies, text casts, substring (448–670, 5250–5508, 6371, 7744–7815) |
| `Arrays.c` | array struct/ptrs, checked element ptrs, copy/zero/drop loops, array builtins, array init/repeat, fixed arrays (349–390, 1855–2004, 4800–5250, 7554–8012) |
| `Boxes.c` | box cells, deref/unbox, box↔inline moves, optional null stores |
| `Structs.c` | field index/insert/extract, struct init, struct copy/drop fns |
| `Ownership.c` | drops, temporaries, moved sources, dummy stores (1680–2530, 3086–3190) |
| `Equality.c` | `==` helpers (5509–5971) |
| `Copy.c` | `copy()` emission (5972–6330) |
| `Vectors.c` | vector construct/reduce/dot/cross, lane lvalues — merged with the existing `LLVMSimd.c` where they overlap |
| `Expr.c` | `EmitExpr` dispatch, ident, unary, conditional, member, index, lvalues |
| `Call.c` | `EmitCall`, arg passing, C vararg promotion, intrinsics, memory builtins |
| `Assign.c` | `EmitAssign` |
| `Stmt.c` | statements, scopes, symbols, defers |
| `Function.c` | declare/define function, AOT entry point |
| `Module.c` | `BuilderBuild`, `BuildLlvmModule(Ex)`, `BuiltModule` lifecycle, globals |

### `src/Sema/ResolveOverloads.c` → `src/Sema/Resolve/`

| New file | Contents |
|---|---|
| `ResolverInternal.h` | `Resolver`, scopes, prototypes |
| `ConstEval.c` | enum ranges, literal fits, `EvalConstInt`, lossy-conversion checks (66–558) |
| `TypeRules.c` | `IsBoolType`, `IsAssignableType`, `IsVectorAssignableType`, implicit conversions, `NumericRank`, `ConditionalType`, `SameResolvedType` |
| `Borrows.c` | borrow inference, growable array params (2981–3660) |
| `Vectors.c` | SIMD construct/intrinsics and their builtin types (2464–2708, 6811ff) |
| `Strings.c` | substring, concat, cstring literal rules |
| `Arrays.c` | array builtins, fixed-array init shape, array repeat |
| `Memory.c` | memory builtins, pointer operands, `addressof` |
| `Calls.c` | `ResolveCall`, arg checks, varargs, generic externs, delegates |
| `Members.c` | impls, properties, accessors, member calls |
| `Enums.c` | `ResolveEnums`, enum const exprs, enum member lookup |
| `Expr.c` | `ResolveExpr` dispatch |
| `Stmt.c` | `WalkStmt`, loops, defers, fall-through analysis |
| `Decls.c` | components, field defaults, drop methods, extern merging, the `ResolveOverloads` entry point |

The directory and entry point should be renamed to what the pass is (`Sema/Check/`, `CheckModule`) — it
resolves far more than overloads. Do this as its own rename commit after the split.

### Others

- `Parse/Parser.c` → `ParseType.c`, `ParseDecl.c`, `ParseStmt.c`, `ParseExpr.c`, `ParseLiteral.c`
  (int/float literal values, suffix stripping), shared `ParserInternal.h`.
- `Sema/MoveCheck.c` → `Places.c`, `Facts.c` (narrowing), `Loans.c`, `WriteSets.c`, `MoveCheck.c` (driver).
- `Embed.c` (1,225) → `Embed/Compiler.c`, `Embed/Jit.c`, `Embed/Aot.c`, `Embed/Types.c` by public API group.

Target: no `.c` file over ~1,500 lines.

## Phase 3 — break up the giant functions

After the moves, in their new files:

- `ResolveExprImpl`, `EmitExpr`, `WalkStmt`, `EmitStmtInner`: the switch becomes a thin dispatcher calling
  one `Resolve<Kind>` / `Emit<Kind>` function per `NodeKind`, each living in the feature file it belongs to.
- `EmitCall` / `ResolveCall`: split into callee resolution, builtin dispatch (table keyed by builtin name,
  shared with Phase 1's builtin table), argument lowering, result handling.
- `EmitBinary`: numeric / string / enum-flag / vector / comparison paths into their files.
- `BuilderBuild`: declare types → declare functions → globals → bodies → finalize, one function each.

## Phase 4 — named integer types

Replace bare `int`/`unsigned`/`long`/`long long` where the number has a meaning. Candidates from the survey:

| Typedef | Replaces |
|---|---|
| `ConstInt` / `ConstUInt` (`int64_t` / `uint64_t`) | `long long` / `unsigned long long` in const eval and folding |
| `ArrayLength` (`int64_t`, `< 0` = dynamic) | `long length`, `long fixedLength` |
| `LaneCount` (`uint8_t`) | `int simdLanes`, `lanes`, `lane` |
| `BitWidth` (`uint32_t`) | `ScalarBitWidth`, `ConstBitWidth`, `bits`, `width` |
| `FieldIndex` (`uint32_t`) | struct field indices passed to LLVM GEP/insert/extract |
| `ScopeDepth`, `LocalMark` (`size_t`) | loop `bodyMark`, `scopeDepth`, temp marks |
| `bool` | any `int` used as a flag or success result |

Also `GenerateUniqueID(char*, int)` → `size_t`, and `AlignUp(long, long)` → `size_t`. Plain `size_t i` loop
counters stay.

## Phase 5 — bitflags

All flag enums use:

```c
enum ExampleBitFlags
{
    EBF_NONE = (0),
    EBF_TEST1 = (1 << 0),
    EBF_HELLO = (1 << 1),
};
```

Existing ones to convert:

- `TypeDescFlag` (`TD_*`, LLVMModuleBuilder.c:91) → `TypeDescFlags`, add `TDF_NONE`.
- `CommandFlags` (`CF_*`, stratac/main.c:121) → `CommandFlags`, `CF_NONE = (0)` form.
- `STRATA_CAP_*` (strata.h:58) and `STRATA_TYPES_STRUCT_COMPONENT` (strata_types.h:54): public; keep the
  names, add the `_NONE` value, a named enum and the parenthesised form. Values unchanged, so ABI-safe.
- New: `ArrayBuiltinFlags` from Phase 1.

Then look for bool clusters that are really flag sets and convert where the struct is passed around a lot
(Phase 6 is the main one).

## Phase 6 — `TypeDesc` flags field

`TypeDesc` carries 11 bools (`isFloat`, `isUnsigned`, `isVoid`, `isBox`, `isOptional`, `isArray`, `isString`,
`isCString`, `isSimdVector`, `aliasedArray`, `isFixedArray`), and `TypeDescMake` already takes `TD_*` flags only
to unpack them. Replace with `TypeDescFlags flags`, read only through the predicates in
`Codegen/TypeDescFlags.h` (extend them to cover every flag). Update `CheckTypeFlags.cmake` to gate the new
field instead of the three bool names.

## Phase 7 — naming

Mechanical, one commit per kind of rename, driven by clang-tidy `--fix` where it works and a script elsewhere:

- `Core/Util` snake_case API → TitleCase: `arena_alloc` → `ArenaAlloc`, `arena_format` → `ArenaFormat`,
  `arena_strdup` → `ArenaStrdup`, `scratch_arena` → `ScratchArena`, etc. (~200 call sites).
- `Arena.default_chunk` and any other snake_case members → camelCase.
- `m_` member prefix removal (if decision 1 holds).
- Single-letter or unclear struct members found by tidy.
- Static helpers named after their file's old prefix (`SemaFoldConst*`) lose the prefix once they live in a
  module of their own.

## Phase 8 — comments

- Convert to the chosen style (decision 2).
- Remove process/history notes that only made sense during development: "Roadmap phase 3/4/5" (TypeDescFlags.h,
  Cfg.h, MoveCheck.h, LLVMAot.c, CheckTypeFlags.cmake), "forward decl" tags, comments that restate the
  next line.
- Keep comments that explain *why*: ABI layout, ownership invariants, why a check is deferred to codegen.
  One short comment per non-obvious function is the ceiling; field comments stay where the meaning isn't in
  the name (e.g. `simdLanes` for float3).
- Each new file gets a one-line header saying what feature it covers, nothing more.

## Order and risk

| Phase | Risk |
|---|---|
| 0 Tooling + format | low |
| 1 Dedupe | medium (changes logic) |
| 2 File splits | low (pure moves) |
| 3 Giant functions | medium |
| 4 Typedefs | low–medium |
| 5 Bitflags | low |
| 6 TypeDesc flags | medium |
| 7 Naming | low (mechanical) |
| 8 Comments | low |

Phases 0 → 1 → 2 must go in order (dedupe before moving so shared code lands in its final home once).
After Phase 2, phases 3–8 touch different concerns but the same files, so they run one after another, not in
parallel.
