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
| UserDrop | Has `Drop` itself or in a field; no `copy()` | a struct whose `impl` has `Drop` |
| ZeroValid | All-zero bits are a valid value | everything but `^T` and structs holding one |

Type aliases are resolved before any property is read.

## Rules

| ID | Rule | Accept | Reject |
| --- | --- | --- | --- |
| O1 | A by-value use of an Owning place moves it. A moved place may not be used until it is assigned again. `x = x` evaluates the right side first and leaves `x` live. | `move_*` | `move_*` |
| O2 | A struct field may be moved out of an owned local, a by-value param or a `ref`; the struct is then partly moved and the field must be refilled before the borrow ends. A `Drop` type is moved whole or not at all. | `partial_*` | `partial_*` |
| O3 | A value is never moved out of a `const ref`, a global or a `T[]` element. | `opt_*` | `move_*`, `global_*` |
| O4 | `T?` narrows to `T` where flow typing proves it set; narrowing facts die when the place may have been written. | `narrow_*`, `opt_*` | `narrow_*`, `opt_*` |
| O5 | A `ref` or `const ref` is a loan on a place. Writing, moving, dropping or reallocating an overlapping place kills it, and a killed ref may not be used. | - | `ref_*` |
| O6 | A mutable argument may not overlap another argument of the same call. | - | `eval_*`, `ref_*` |
| O7 | Evaluation is left to right. A loan taken by an earlier operand lasts to the end of the expression. Temporaries are dropped at statement end. | `eval_*` | `eval_*` |
| O8 | `Drop` runs exactly once per owned value: at scope end (reverse declaration order, after `defer`), on overwrite, on `drop()`, and after the owner's own `Drop` for fields. | `drop_*`, `defer_*` | `drop_*`, `defer_*` |
| O9 | `copy()` deep-copies an Owning value and is rejected for UserDrop types. | `copy_*` | `copy_*` |
| O10 | A global is initialized in declaration order and destroyed in reverse order. Owning globals are never moved out of. | `global_*` | `global_*` |
| O11 | Default fills and `array_resize` need a ZeroValid element type. Every initializer must set a `^T`. | `init_*` | `init_*` |
| O12 | An alias of a type behaves as the type it names for every rule above. | `alias_*` | `alias_*` |
| O13 | Lossy numeric conversions need a cast; literals that fit convert. Arithmetic traps instead of invoking undefined behaviour. | `arith_*`, `literal_*` | `arith_*`, `lossy_*`, `literal_*` |
| O14 | Enum values are compared and combined only within their enum, and runtime casts are range-checked. | `enum_*` | `enum_*` |
| O15 | Types that cross the host boundary have a fixed layout; a struct passes to an `extern` by pointer. | `layout_*`, `union_*`, `vec_*` | `extern_*`, `layout_*`, `union_*`, `vec_*` |
| O16 | `rc<T>` is out of scope and being removed. | - | `rc_*` |

## Open points

- O5 and O6 have no accept case yet: the checker rewrite (roadmap phase 4) is where loans become
  explicit.
- O1 to O4 are checked on strings of paths today; the CFG checker replaces that.
- The value-optional layout (Foundations, "Value optionals") will change O4's runtime drop flag from
  null to a tag for inline layouts.
