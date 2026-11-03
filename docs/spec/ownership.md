# Strata ownership and borrowing (draft)

Roadmap phase 2 of `Strata Language Foundations`. This file states the rules; the conformance suite
(`tests/conformance`) pins them. Each rule lists the cases that exercise it as filename globs, and
`tests/spec/CheckSpec.cmake` fails when a glob matches nothing. A `-` means no case of that kind
exists yet, which is a gap to fill. It supersedes the Ownership section of `docs/engine-ffi.md` as
that section is absorbed.

Where a rule is not enforced yet the reject case carries `// xfail:` in the suite. The rule stands
anyway; the case documents what the compiler still gets wrong.

## Type properties

Computed once per resolved type by `TypeClassify` (`src/Codegen/TypeUtil.c`). No rule tests a
spelling or a representation flag directly.

| Property | Meaning | Examples |
| --- | --- | --- |
| Trivial | Bitwise copy, no drop glue | scalars, enums, `float4`, `handle`, plain structs |
| Owning | A by-value use moves; has drop glue | `string`, `T[]`, `^T`, `T?`, owning structs |
| UserDrop | Has `drop` itself or in a field; no `copy()` | a struct whose `impl` has `drop` |
| ZeroValid | All-zero bits are a valid value | everything but `^T` and structs holding one |

Type aliases are resolved before any property is read.

## Rules

| ID | Rule | Accept | Reject |
| --- | --- | --- | --- |
| O1 | A by-value use of an Owning place moves it. A moved place may not be used until it is assigned again. `x = x` evaluates the right side first and leaves `x` live. | `move_*` | `move_*` |
| O2 | A struct field may be moved out of an owned local, a by-value param or a `ref`; the struct is then partly moved and the field must be refilled before the borrow ends. A `drop` type is moved whole or not at all, and so is a field that holds a `drop` value in its own storage (by value, at any depth, not behind `^T`, `T?` or `T[]`). | `partial_*`, `drop_inline_*` | `partial_*`, `drop_inline_*` |
| O3 | A value is never moved out of a `const ref`, a global or a `T[]` element. | `opt_*` | `move_*`, `global_*` |
| O4 | `T?` narrows to `T` where flow typing proves it set; narrowing facts die when the place may have been written. | `narrow_*`, `opt_*` | `narrow_*`, `opt_*` |
| O5 | A `ref` or `const ref` is a loan on a place. Writing, moving, dropping or reallocating an overlapping place kills it, and a killed ref may not be used. | `ref_*` | `ref_*` |
| O6 | A mutable argument may not overlap another argument of the same call. | `args_*` | `eval_*`, `ref_*` |
| O7 | Evaluation is left to right. A loan taken by an earlier operand lasts to the end of the expression. Temporaries are dropped at statement end. | `eval_*` | `eval_*` |
| O8 | `drop` runs exactly once per owned value: at scope end (reverse declaration order, after `defer`), on overwrite, on `drop()`, and after the owner's own `drop` for fields. A local may hold an Owning struct by value, `drop` types included. Assigning `self` inside its own `drop` drops the old fields, not the old value. | `drop_*`, `defer_*` | `drop_*`, `defer_*` |
| O9 | `copy()` deep-copies an Owning value and is rejected for UserDrop types. | `copy_*` | `copy_*` |
| O10 | A global is initialized in declaration order and destroyed in reverse order. Owning globals are never moved out of. | `global_*` | `global_*` |
| O11 | Default fills and `array_resize` need a ZeroValid element type. Every initializer must set a `^T`. | `init_*` | `init_*` |
| O12 | An alias of a type behaves as the type it names for every rule above. | `alias_*` | `alias_*` |
| O13 | Lossy numeric conversions need a cast; literals that fit convert. Arithmetic traps instead of invoking undefined behaviour. | `arith_*`, `literal_*` | `arith_*`, `lossy_*`, `literal_*` |
| O14 | Enum values are compared and combined only within their enum, and runtime casts are range-checked. | `enum_*` | `enum_*` |
| O15 | Types that cross the host boundary have a fixed layout; a struct passes to an `extern` by pointer. | `layout_*`, `union_*`, `vec_*` | `extern_*`, `layout_*`, `union_*`, `vec_*` |
| O16 | `void init(ref T self, ...)` constructs a `T` and may be overloaded. `T(args)` runs the matching overload on a value that starts from its field defaults; a bare `T v;` runs the one without arguments. A type with init has no positional field constructor and no non-optional box field, and `init` is never called on a live value. Field defaults never run init, so a struct, default fill or `array_resize` that would leave an init type by value at its defaults is rejected. Braced `T { ... }` literals stay field-wise. | `init_*` | `init_*` |

## Where each rule is enforced

The CFG checker (`src/Sema/MoveCheck/MoveCheck.c`, over the graph from `src/Sema/Cfg.c`) is the only place that reports
use after move, partial moves, narrowing, loans and `ref` validity. The rest are sema checks in
`src/Sema/Resolve/` or run-time checks in the LLVM backend.

| Rules | Enforced by |
| --- | --- |
| Rule O1, O2 | CFG checker: per-place move state, join at branches and loops. A part taken out of a `ref` parameter must be back before return; a `drop` method may take its own value apart, except a field holding a `drop` value by value. |
| Rule O3 | CFG checker: an element of an array is never moved out (E0508). `array_remove`, `array_swap_remove`, `array_replace` and `array_take` return the element; `array_take(a, i)` moves out a `string`, array or optional element and leaves its empty value in the slot. A field of a value with its own `drop` method is never moved out. |
| Rule O4 | CFG checker: narrowing facts are must-facts, killed by writes, moves, `ref` arguments, array mutators, calls that write the global (transitive write sets) and by `drop` calls (`drop()`, overwrite, scope end). |
| Rule O5, O6, O7 | CFG checker: loans within one expression, callee write sets for borrowed globals, `ref` locals die when their target changes. |
| Rule O8 | Backend: a `drop` struct keeps its declared layout. A by-value local whose type holds a `drop` value inline has a drop flag on the stack, cleared when the value moves out and checked at scope exit and overwrite; the JIT's optimising pipeline folds it away where the move is unconditional (AOT objects are not optimised yet, so there it stays a load and a branch). Overwrite stores the new value before dropping the old, so a re-entrant `drop` is safe; globals are destroyed in reverse order, including the fields of struct globals; temporaries tested with `x?` or lent to a `ref` are dropped at statement end. |
| Rule O10 | CFG checker module pass: a global's initializer may not read or change a later global, directly or through the functions it calls. |
| Rule O11 | Sema: zero-fill, `array_resize`, global initialization and bare owning struct locals ask `TypeClassify(...).zeroValid`. |
| Rule O12 | Sema and backend resolve an alias before the check, and drop glue follows aliased fields. |
| Rule O13 | Sema: literals must fit; implicit numeric conversions must lose nothing; constant `/ 0`, `% 0`, wide shifts, `MIN / -1` and out-of-range float casts are errors. Backend: the same cases panic at run time, float-to-int casts saturate, mixed signed/unsigned comparisons compare by value. |
| Rule O14 | Sema: no unary minus, no comparison across enums, bitwise operators only on `@flags enum`. Backend: a cast from an integer that is not a member panics. |
| Rule O15 | Sema and backend: no `ref` to a field at a misaligned `fieldoffset`; such fields are read and written unaligned. |
| Rule O16 | Sema: each `init` overload gets a synthesized `T __init_T(args) { T self; self.init(args); return self; }`, and `T(args)` and `T v;` become calls to it, so moves, borrows and drops follow the ordinary call rules. |

The checker also hands its verdict to the code generator. Every identifier, member and index expression that names
a place carries a `UseKind` (`AST.h`): `UseMove` empties the place; `UseCopy` and `UseRead` leave it alone. `MovedSource` in `src/Codegen/Builder/Ownership.c` is the only thing that
decides whether a consuming use empties its source, and it asks the annotation; codegen no longer guesses from the
shape of the expression. A box handed to a bare place is a copy when its pointee is plain data and a move when the
pointee owns something.

## Open points

- `rc<T>` is gone, which closes the holes that only it opened (narrowing and `ref` validity through another
  handle, Foundations hole 4).
- Casting a box through an opaque marker and back is allowed because the round trip is intended
  (`box_cast_via_opaque_marker_round_trips`). Retyping a box through the marker (hole 17) needs the checker
  to know what the opaque box carries.
- The value-optional layout (Foundations, "Value optionals") will change O4's runtime drop flag from
  null to a tag for inline layouts.
- Roadmap phase 5: codegen follows the use annotations for moves, and every `accept/` case plus twelve AOT samples
  also run under AddressSanitizer (see `tests/conformance/README.md`).
  drop glue, temporaries and argument passing stay in codegen: a temporary is parked in a zeroed slot and dropped at
  the end of its statement, so its drop is decided by the slot at run time, not by a static fact the checker could
  annotate. The `temp_*` and `arg_owned_*` cases pin them under AddressSanitizer and the live-count check.

- Roadmap phase 7: `tests/fuzz/fuzz.py` generates random programs over `^Res` boxes, optionals, arrays, struct fields,
  branches, loops and early returns, compiles them under AddressSanitizer and fails any that the checker accepts but
  leaks, double drops, reads freed memory or panics. 2,000 programs (1,916 accepted) ran clean; a 150-program smoke
  run is `fuzz/ownership_smoke`. Not yet generated: strings, `drop` methods that mutate, `ref` aliases, `rc`.
