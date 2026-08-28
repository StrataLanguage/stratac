# Self-Hosting Strata Compiler — MVP Plan

Goal: a `stratac.strata` program that lexes → parses → sema-checks → emits a
runnable object file via LLVM-C, replacing the C `stratac`. It is compiled by
the existing C `stratac`, then run via a C host that wires LLVM-C + stdio +
`strata_alloc`/`strata_free`/`strata_panic` (the existing mechanism). No
chicken-and-egg: the C compiler keeps working during the bootstrap.

## Locked design decisions

- **LLVM refs → `handle`** (host-defined opaque types): `handle LLVMRef;` for
  every `LLVM*Ref`. `handle` is non-owning, passes by value, has no drop glue —
  the correct mapping (and matches "handle is for host-defined types"). This
  replaces the `^Any`-as-generic-pointer idea: `^Any` is a box (owning) and
  would leak / risk drop-glue on foreign pointers.
- **Array-of-refs / array-of-type params → `handle[]`** (or `int[]`/`uint[]`
  as appropriate), declared directly in the `extern` signature.
- **`extern` array-decay** (the one hard blocker, implemented in Phase 0):
  a `T[]` argument passed to an `extern` function passes the pointer to its
  first element (the inner `.ptr` of the fat `{ptr,len}` struct), not the fat
  struct. The element count is passed separately as `arr.length`.
- Enums → `const int` groups.
- Hash map / string helpers → hand-rolled in Strata.
- File I/O → `extern` stdio (or the host import resolver).
- AST → typed-array + integer-index "sea of nodes" (separate `IntLit[]`,
  `BinaryExpr[]` arrays; children stored as int indices). No downcasts needed.

## Phase 0 — `extern` array-decay (the only compiler change)

Location: `src/Codegen/LLVMModuleBuilder.c`, the call-argument loop
(`EmitCall`, ~lines 3122–3278), inside the `fd && fd->isExtern` branch.

**Scoped rule (as implemented & tested):**
- A **by-value** (`mod == ModNone`), **non-owning-element** dynamic array
  param (`int[]`, `uint[]`, `handle[]`, `Foo[]`, …) **decays** to a pointer to
  its first element — i.e. the inner buffer pointer of the fat `{ptr,len}`
  struct — matching a C `T*` parameter. The caller passes the element count
  separately as `arr.length`.
- `ref`/`const` array params, and **owning-element** arrays (`^T[]`, `string[]`)
  keep crossing as the fat `{ptr,len}` struct, so cooperative hosts can read
  `ptr`/`len` or replace the array. (This preserves the existing host-interop
  tests for box/string/ref arrays.)

Implementation:
```c
if (fd && fd->isExtern && k < fd->params.count)
{
    const ParamDecl* pd = (ParamDecl*)VecGet(&fd->params, k);
    if (pd->mod == ModNone)
    {
        const TypeName* pty = &pd->type;
        const TypeName* elem = TypeNameArrayElem(pty);
        if (TypeNameIsDynamicArray(pty) && !(elem && TypeNameIsOwning(elem)))
        {
            LLVMValueRef arrAddr = ArgAddress(b, argNode);
            LLVMValueRef dataSlot = ArrayDataPtr(b, arrAddr);
            args[k] = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, dataSlot, "arrptr");
            continue;
        }
    }
}
```

**ABI change note:** previously *every* `T[]` extern param crossed as the fat
struct. By-value non-owning arrays now cross as a raw `T*`. This is exactly
what LLVM-C's array-taking functions need (`LLVMBuildCall2(... LLVMValueRef*
args, unsigned n, ...)`, `LLVMStructSetBody`, `LLVMAddIncoming`, `LLVMFunctionType`,
`LLVMConstArray`/`ConstVector`, `LLVMBuildGEP2`/`ConstGEP2`). The existing
`ExternTests` that used by-value `int[]` were updated to the new contract
(raw pointer + explicit `arr.length`). This removes the need for any C shim;
all LLVM bindings stay 100% in Strata.

**Binder recipe for LLVM array params:** declare the param as `handle[]` (or
`int[]` etc.) and call with `arr, arr.length`:
```strata
extern LLVMRef LLVMBuildCall2(LLVMRef b, LLVMRef fnTy, LLVMRef fn, LLVMRef[] args, uint n, string name);
...
LLVMRef[] a = {}; array_push(a, x); array_push(a, y);
LLVMRef r = LLVMBuildCall2(b, fnTy, fn, a, a.length, "");
```
(`.length` is `long`; pass it to the `unsigned`/`uint` count — widths line up
for normal counts.)

**Sema prerequisite:** parser + sema already accept `T[]`/`handle[]` as an
`extern` param type (no change needed).

## Phases 1–7 (the port itself)

1. **LLVM-C bindings + runtime glue** — `handle LLVMRef;` + `extern` decls for
   the ~180 functions in `LLVMCApi.h` (handle→`handle`, `const char*`→`string`,
   scalars→`int`/`uint`/`double`, array params→`handle[]`); `extern` stdio;
   host-provided alloc/free/panic; a growable scratch buffer (`handle[]` via
   `array_push`) for arg lists.
2. **Lexer** (~670 LOC): keyword table, punctuation, number/string/ident,
   source ranges.
3. **Parser + AST arena** (~2500 LOC): recursive descent into typed arrays +
   index refs; `import`/module merge.
4. **Sema** (~3331 LOC — biggest risk): overload resolution, type inference,
   const/optional/move blessing.
5. **Codegen to LLVM IR** (~3860 LOC — other big risk): `MapType`, function/
   struct emission, all `LLVMBuild*` via `handle`/`handle[]` + decay.
6. **AOT emission + driver** (~580 LOC): `LLVMTargetMachineEmitToFile`, file
   read, diagnostics via `printf`/`puts`.
7. **Self-compile verification**: C `stratac` compiles `stratac.strata` → object
   → link LLVM-C → run; confirm it compiles a sample to an object that links
   and runs (exit 25, per AGENTS.md).

## Known gotchas

- **Hash-map resize/ownership**: a `string[]` key array relocates (not drops)
  elements on `array_push`/resize — verify the backend relocates owning
  elements rather than dropping them; or store interned-string raw pointers as
  `handle` keys (non-owning → resize always safe), or use a fixed-capacity
  table. Recommend the `handle`-key or fixed-capacity approach.
- **`.length` type**: pass `arr.length` (integer) as the `unsigned n` param;
  the backend bitcasts int→unsigned fine.
- **Volume, not feasibility, is the real cost**: ~16.5k LOC re-expressed in
  Strata + a ~30-line compiler change. No new keywords/features beyond the
  decay rule.
