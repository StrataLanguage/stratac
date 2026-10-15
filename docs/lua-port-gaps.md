# Proposals: language gaps found porting rna's Lua API (Strafe)

Status (2026-10-15): **4, 6, 1 and 5 implemented** (documented in `engine-ffi.md`); **3, 2 and 7b
held**. Differences from the designs below:

- 4 also covers moving a `T?` out of a global (it used to empty it silently); in Strafe it caught a
  real bug — `DxcLoad` returned `gDxc.failure` by value, so a second call after a failure returned
  `""`.
- 1: `'c'` parses to exactly `(byte)N`, so arithmetic between char literals stays `byte` and wraps
  (`'\\' * 1000`), like `(byte)92 * 1000` always did. `\xNN` is not supported (strings lack it too).
- 5 is implicit at: var init, assignment (local, field, element), struct literal fields and
  positional constructors, by-value call args (not externs), return, array literals, `array_push`.
  A sema-inserted conversion node (`CastBoxToRc`) carries it, so codegen has one emit site. It also
  fixed a pre-existing hole: `rc<T>[] list = { box }` used to be accepted and stored the raw `^T`
  as if it had a count header (never dropped).

Each section is independent; pick any subset.
Examples are from `Strafe/engine/lua` at commit 67972c0. "Today" behaviour was checked with
`build/default/bin/stratac.exe --run` on small repros.

| # | Gap | Proposal | Kind | Size |
|---|---|---|---|---|
| 4 | `string dir = gHost.dir;` silently empties the global | Make it an error, like every other global move | **bug fix** (rule exists, hole in it) | S |
| 6 | `handle` reserved everywhere | Contextual keyword: only `handle Name;` at top level | grammar | S |
| 1 | `(byte)47` for `'/'` | `'/'` is a `byte` constant | new syntax | S |
| 5 | `^T` can't become `rc<T>` | Implicit move-conversion `^T` → `rc<T>` | new conversion | S–M |
| 3 | Global narrowing dies at any call | Per-function global write sets; facts survive calls that can't write the path | flow typing | M |
| 2 | ~100-branch `if` chains over an enum | `switch` with no fallthrough, enum exhaustiveness, LLVM `switch` | new statement | M |
| 7 | `string[4]` rejected | (a) nothing needed for literal tables; (b) drop glue for owning fixed arrays | ownership | S / M |

Suggested order if you want several: 4, 6, 1, 5, 3, 2, 7b.

---

## 4. Reading an owning field of a global moves it out

### Problem

`script.strata` `Script.Create`:

```c
gHost.dir = DirOf(path);
...
string dir = copy(gHost.dir);   // was `string dir = gHost.dir;` — that emptied gHost.dir
```

Strata already forbids moving out of globals:

```c
string gName;
void Use() { string n = gName; }              // error: 'gName' cannot be moved as it is not
                                              //   owned because it is global ...
Item[] gItems;
void UseItem() { string n = gItems[0].name; } // same error
```

but not out of a field of a global *struct*:

```c
struct Host { string dir; };
Host gHost;
void Use() { string dir = gHost.dir; }        // accepted; gHost.dir is now ""
int Len() { return (int)gHost.dir.length; }
int main() { gHost.dir = "abc"; Use(); return Len(); }   // exits 0
```

Inside one function the move is caught ("used after move"), so the silent case is exactly the
cross-function one — the one that hurts.

### Proposal

An **error**, the same one `gName` gets. No new rule: the existing rule ("a global is not owned
by any function, so nothing can move out of it") is applied to every path rooted at a global,
whatever the root's type. The fix-its stay: `copy(gHost.dir)`, `const ref string dir = gHost.dir;`,
or pass it to a borrowing parameter (which already works: read-only params borrow).

Why not copy implicitly: everywhere else in Strata a by-value read of an owning value is a move
and copies are spelled `copy()`. An implicit copy only for globals would make `string a = b;`
mean two different things depending on where `b` lives, and hide allocations in hot paths.

What still works: assigning the field (`gHost.dir = DirOf(path);` drops the old value and
stores the new one; `gHost.dir = "";` empties it), moving a value *into* a global, and every read that borrows (`.length`, indexing, `(cstring)`, passing it
to a read-only parameter).

### Ownership / borrowing

Strictly tightens. A function that wants to take a global's value and leave it empty must say so
by assigning (`string dir = copy(gHost.dir); gHost.dir = "";`) — or we add a `take(path)`
intrinsic later if this shows up often (not proposed now).

### Compiler impact

- `src/Sema/ResolveOverloads.c`: `m_boxGlobals` is filled at ~L10935 only when
  `AliasIsOwning(&gd->type)`; an inline struct global like `LuaHost` is not "owning" by that
  predicate, so `MarkMoved` (~L2042) never sees `gHost` as a global root. Register every global
  whose type *contains* owning parts (struct with owning fields, fixed arrays of them, optionals)
  — or simply every global, since `MarkMoved` is only reached for owning values.
- Check `m_boxGlobals`' other user (~L8260) still means what it says, or split it into
  "owning global" vs "box global".
- Tests: `tests/unit` move tests + a sample line. Expect a few Strafe call sites to need
  `copy()`/`const ref` — which is the point.

---

## 6. `handle` as a contextual keyword

### Problem

`host.strata` `LuaSlot`: the texture-view slot had to be called `view`:

```c
rc<TextureHandle>? view;    // wanted: `handle`
```

`struct S { int handle; };` → "expected a field name". Same for locals and params.

### Proposal

`handle` is a keyword **only** where it starts a declaration: at top level (module scope),
`handle` followed by an identifier and then `;` or `:` (`handle VkBuffer;`,
`handle Derived : Base;`). Everywhere else it is an ordinary identifier.

The only other top-level form that starts with two identifiers is a global of a user type
(`Foo x;`), so the rule is: at top level, `handle Ident` followed by `;` or `:` is always a handle
declaration, and declaring a type named `handle` is an error (it could never be used as a
global's type).

Same treatment could apply to `property` and `fieldoffset` (only meaningful inside `impl` /
`struct` bodies); not proposed unless they collide in practice.

### Ownership / borrowing

None.

### Compiler impact

- `src/Lex/Token.c` / `Token.h`: drop `TokKwHandle` from the keyword table.
- `src/Parse/Parser.c`: top-level loop (~L1962) and the attribute check (~L1391) test
  "identifier with text `handle` followed by an identifier" instead of `TokKwHandle`;
  `ParseHandleDecl` (~L842) consumes the identifier.
- No Sema/codegen change. Rename `view` back to `handle` in Strafe afterwards (optional).

---

## 1. Character literals

### Problem

`script.strata` `DirOf`, `device_api.strata` `KeyCode`:

```c
if (path[i] == (byte)47 || path[i] == (byte)92)    // '/' or '\'
if (lower[i] >= (byte)65 && lower[i] <= (byte)90) { lower[i] = lower[i] + (byte)32; }
if (c >= (byte)97 && c <= (byte)122) { return 0x41 + (int)(c - (byte)97); }
```

(The casts are actually unnecessary today — `path[i] == 47` and `s[1] == 92 ? 47 : s[1]` both
compile, integer literals adapt to `byte` — but the numbers still need a comment to be read.)

### Proposal

`'c'` is a constant of type `byte` whose value is the character's byte:

```c
if (path[i] == '/' || path[i] == '\\')
if (lower[i] >= 'A' && lower[i] <= 'Z') { lower[i] = lower[i] + ('a' - 'A'); }
if (c >= 'a' && c <= 'z') { return 0x41 + (int)(c - 'a'); }
```

- Exactly one byte: a printable ASCII character, or one escape from the string-literal set
  (`\n \r \t \0 \\ \' \" \xNN`). A non-ASCII character (`'é'` is two UTF-8 bytes) or an empty /
  multi-character literal is an error naming the problem.
- It is a constant: usable anywhere an integer literal is — comparisons, arithmetic, array fills
  (`{ ' '; 16 }`), `switch` cases (see 2), and it adapts to wider integer types exactly like an
  integer literal does (`int k = 'a';`).
- `byte`, not a new `char` type: strings are byte strings and `s[i]` is a `byte`; a separate
  `char` would need conversions everywhere and would suggest Unicode semantics Strata doesn't have.

### Ownership / borrowing

None (scalar constant).

### Compiler impact

- `src/Lex/Lexer.c`: `'` is currently "unexpected character" (~L371); add `LexCharLiteral`
  next to `LexStringLiteral` (~L521), sharing its escape decoder. New `TokCharLiteral` in
  `Token.h`/`Token.c`.
- `src/Parse/Parser.c`: primary expression → an `IntLiteral` node with a `byte` type hint (or a
  flag `isChar` for diagnostics/AST dump). Reusing `NodeIntLiteral` means Sema/codegen are
  untouched apart from typing it `byte` instead of the default integer type.
- `src/Codegen/ASTDump.c`: print `'c'`.

Related bug found while checking this (separate from the proposal): `byte b = 300;` compiles
and gives 44 — an out-of-range integer literal is silently truncated instead of rejected.

---

## 5. Turning `^T` into `rc<T>`

### Problem

`gfx/layout.strata` has the constructor

```c
^DynParams Create(rc<StructLayout> layout) { ... return DynParams { .layout = layout, .bytes = { 0; size } }; }
```

but `objects.strata` `FrameParams` needs an `rc<DynParams>` (a `ParamsRef.block` is shared), so it
rebuilds the value by hand, duplicating `Create`'s body:

```c
rc<DynParams> block = DynParams { .layout = layout, .bytes = { 0; layout.size } };
```

Today: `rc<P> r = P.Create(3);` and `^P b = ...; rc<P> r = b;` → "'rc<P>' cannot be initialized
by expression of type '^P'". A struct literal converts; a box doesn't.

### Proposal

An owned `^T` converts implicitly to `rc<T>` (and `^T?` / `^T` to `rc<T>?`) wherever a value of
the target type is expected: initialization, assignment, argument, return, field init.

```c
rc<DynParams> block = DynParams.Create(layout);
gHost.slots[slot].params = ParamsRef { .block = DynParams.Create(layout) };
```

It is a **move**: the source must be something that can be moved (a temporary, an owned local, an
owned field of a local); afterwards it is gone. Exactly the rule used when a `^T` is moved into an
inline `T` field (4966837) — same consistency argument for making it implicit rather than an
`rc(x)` intrinsic.

Not proposed: `rc<T>` → `^T` (the value is shared; you'd need `copy()` of the contents, which
Drop types forbid anyway).

### Ownership / borrowing

- Source is consumed; "used after move" applies to it as usual. From a global, a `ref`, or an
  array element: the existing errors (cannot move out of global / ref; element poisoning).
- Drop: `T`'s `Drop` runs once, when the last rc reference goes — the box move is bitwise, no
  Drop on the old cell.
- A `const ref ^T` or `ref ^T` cannot convert (not owned).

### Compiler impact

- `src/Codegen/LLVMModuleBuilder.c`: an rc cell has a 16-byte count header before the value
  (`RC_HEADER_BYTES`, `AllocBoxCell(..., isRc)`), a `^T` cell does not, so the conversion is:
  `AllocBoxCell(size, isRc=true)`, set count 1, `memcpy` `sizeof(T)` from the box, free the box
  cell (no drop). One alloc + copy + free; only at the conversion point.
  (A zero-cost variant — give every `^T` cell the header — changes every box allocation and the
  `^T` that crosses into C; not worth it for this.)
- `src/Sema/ResolveOverloads.c`: the assignability check that produced the error above gains
  the `^T`→`rc<T>` / `rc<T>?` case and records a move of the source (`MarkMoved`), plus an
  implicit-conversion node or flag on the expression for codegen.
- Tests in `tests/unit/BoxTests.c` (Drop-once, moved-source error, optional target).

---

## 3. Narrowing of a global optional dies at any call

### Problem

`script.strata` `Script.Load`:

```c
if (gHost.gd?)
{
    ResetStates(gHost.gd);     // had to be a helper: inline, the first call in the block
}                              //   (even BlendOpaque()) un-proves gHost.gd
printf("strafe: loaded %s\n", (cstring)self.path);
```

`ResetStates` and `ClearIdle` exist only to bind `gHost.gd` to a `ref` parameter before the body
makes a call. Today *any* call clears every global fact — an extern `printf`, or
`int Pure(int x) { return x + 1; }`:

```c
if (gHost.gd?) { int y = Pure(2); Touch(gHost.gd); }   // error: 'gHost.gd' has not been blessed
```

Meanwhile `ref` locals into globals already survive extern calls (`TrackCallArgMoves`, ~L4768:
`if (!best->isExtern) InvalidateRefLocals(...)`), so the two rules disagree.

### Options considered

**A. Keep facts across extern calls only.** One-line change, matches the ref-local rule, fixes
the `printf` case. Does not fix `ResetStates`/`ClearIdle` (their bodies call Strata functions).
Unsound when C calls back into Strata — and in Strafe it does: `lua_pcall` → `strafe_lua_call`,
`DispatchMessage` → `BedrockWndProc`, Vulkan → `GpuDebugCallback`. (The existing ref-local
exemption has the same hole.)

**B. Scoped binding `if (gHost.gd? gd) { ... }`.** Binds `gd` as a `ref GraphicsDevice`. Reads
nicely, but a `ref` into a global must die at a call for the same reason the fact does, so on its
own it fixes nothing; it would need a runtime "borrowed" flag on the slot (assigning `gHost.gd`
while bound panics), i.e. RefCell semantics plus a hidden field in every optional global.

**C. Global write sets (proposed).** A call clears a global fact only if the callee may write that
path.

### Proposal (C)

Before resolving bodies, compute for every function the set of global *paths* it may write,
transitively:

- Direct writes: assignment to a path rooted at a global (`gHost.gd = none;`), `drop(path)`,
  `array_push/resize/remove/...` on one, passing one by mutable `ref` or by owning value, `++`/`--`.
  Paths are kept at field granularity with indices erased: `gHost.gd`, `gHost.slots[]`,
  `gHost.exit`.
- Calls: union of the callee's set. Overloads/methods resolved by name only at this stage, so a
  call to `Foo` unions every `Foo` (conservative, cheap).
- Externs and delegate (function pointer) calls: may run any `@export` function or any function
  whose address is taken (`addressof(fn)`), so their set is the union of those functions' sets.
  That is what makes this sound under callbacks — and in Strafe it is still tight: `gHost.gd` is
  written only in the body of `main()` (main.strata:104, 118, 208), which no `@export` function
  (`strafe_lua_call`, `BedrockWndProc`, `GpuDebugCallback`, the runtime hooks) reaches, so
  `lua_pcall` keeps the fact.
- Fixed point over the call graph (recursion is fine: start empty, iterate to stable).

Then at a call, a fact on path `F` is cleared iff some written path `W` is a prefix of `F` (or
equal): writing `gHost` or `gHost.gd` kills it, writing `gHost.slots[]` or `gHost.gd.blendState`
does not (setting a field inside `gd` cannot make `gd` empty).

The same sets can replace "refs into globals die at any call": a ref into `gHost.gd` dies only at
calls that may write a prefix of it. That would make B unnecessary.

Result for Strafe: `ResetStates`/`ClearIdle` can be inlined, and `Route` could do
`if (!gHost.gd?) { ... return -1; }` once and keep `gHost.gd` proven for the rest.

### Ownership / borrowing

Narrowing only; moves are unaffected (moving out of a global stays an error — see 4). Facts
from locals are unchanged. Soundness argument: a global path can only change through a write
inside some function's body; every function that can run during the call is in the callee's
transitive set, including C-reachable entry points.

Known limits: Embed.c / JIT hosts that call arbitrary Strata functions by name from C behave
like externs calling back — treat every function the host can look up as an export (or, simpler,
when compiling for embedding, treat externs as "write everything").

### Compiler impact

- `src/Sema/ResolveOverloads.c`: new pre-pass (~200–300 lines) walking all function bodies of
  the module set (the whole program is one compilation unit — Strafe builds from `main.strata`);
  `TrackCallArgMoves` (~L4766) consults the callee's set instead of clearing every global;
  `ClearNullableFacts`/`InvalidateRefLocals` gain a "prefix of" filter (they already take keys).
- Diagnostics: when a fact *is* cleared by a call, say which call and which write
  ("`gHost.gd` may be changed by `Reset()` (writes `gHost.gd` at main.strata:208)") — today's
  "has not been blessed" message doesn't explain why.
- `docs/engine-ffi.md`: rewrite the "for refs into globals — any call" sentence.

---

## 2. `switch`

### Problem

`lua_api.strata` dispatches every Lua → engine call through `Route` → `Global` / `Method` /
`WithDevice` / `Meta`, ~100 lines of

```c
if (fn == Fn.Vec2New) { return VecNew(L, 2u, error); }
if (fn == Fn.Vec3New) { return VecNew(L, 3u, error); }
...
error = "unknown function";
return -1;
```

That is a linear compare chain on the hottest path of the Lua API, and nothing tells you when a
new `Fn` member was never routed.

### Proposal

```c
switch (fn)
{
    case Fn.Vec2New: return VecNew(L, 2u, error);
    case Fn.Vec3New: return VecNew(L, 3u, error);
    case Fn.MatRotationX, Fn.MatRotationY, Fn.MatRotationZ:
    {
        uint axis = (uint)fn - (uint)Fn.MatRotationX;
        return MatRotationAbout(L, axis, error);
    }
    default:
    {
        error = "unknown function";
        return -1;
    }
}
```

Rules:

- Scrutinee: an integer, `byte` (so char literals work as cases), `bool`, or enum value.
  Evaluated once. Strings are not supported in this version (would lower to a compare chain;
  can be added later for `KeyCode`-style tables).
- Case labels: constants — integer/char literals, enum members, (later) constant ranges
  `case Fn.Add ... Fn.Unm:` if you want `Route`'s range tests. Several labels per case with `,`.
  Duplicate labels are an error.
- **No fallthrough, no `break` needed.** Each case body is one statement or one block; control
  leaves the `switch` at its end. A `break` directly in a case body is an error ("cases do not fall
  through; remove 'break'") so neither the C reading (exit switch) nor the loop reading is silently
  chosen. `break`/`continue` inside a nested loop, and `continue` targeting an enclosing loop, work
  as usual.
- Exhaustiveness: switching on a (non-flags) enum without `default` requires every member to be
  covered; the error lists the missing ones. With `default`, no check. Flag enums and integers
  always need `default` to be exhaustive for flow purposes (a switch without one may fall out
  the bottom).
- Flow typing: each case is a branch; the state after the `switch` is the join of every case that
  can complete (plus "no case matched" when not exhaustive). A `switch` whose every case returns
  / `@noreturn`s ends the path like an `if`/`else` that does.

Spelling: `switch`/`case`/`default` is the C-family reading Strata already follows; none of the
three words is used as an identifier in Strafe or the samples (only the string `"default"`).
A `match` with `=>` arms is the alternative if you'd rather not reuse C's words with different
(no-fallthrough) semantics.

### Ownership / borrowing

No owning scrutinee in v1, so nothing is moved by matching. Moves inside cases join exactly like
`if`/`else if` branches: moved in one case and not another → "maybe moved" after. Smart casts
proven before the `switch` hold in every case; facts proven inside a case don't leak out unless
every completing case proves them.

### Compiler impact

- `src/Lex/Token.c/.h`: keywords `switch`, `case`, `default`.
- `src/AST/AST.h/.c`: `NodeSwitch { Node* subject; Vec cases; }` with `{ Vec labels; Node* body;
  bool isDefault; }`; visitor in `AST.c` (~L114 next to `NodeIf`).
- `src/Parse/Parser.c`: statement parser (next to `TokKwDefer` ~L2103).
- `src/Sema/ResolveOverloads.c`: the five `case NodeIf:` sites (~L3321, 3645, 10124, 10349,
  10397) get a `NodeSwitch` twin. Flow uses the existing `FlowCapture`/`FlowApply`/`FlowJoin`
  (~L2394–2444). Constant evaluation of labels, duplicate check, enum exhaustiveness, `break`
  check.
- `src/Codegen/LLVMModuleBuilder.c` (~L8520): `LLVMBuildSwitch` — LLVM picks a jump table or
  binary search. `src/Codegen/ASTDump.c`.
- Roughly the size of `for` + `defer` together.

---

## 7. Fixed-size arrays of owning elements

### Problem

`string[4] names = {...}` → "fixed-size array local 'names' may not own its elements ('string'
is owning); fixed-size arrays have no drop glue" (fields get the same error).

Every Strafe site that hit this is a table of **literals**:

```c
cstring[4] names = { "sphere", "cube", "plane", "torus" };              // objects.strata MakeMesh
cstring[16] names = { "space", "enter", ... };                          // device_api.strata KeyCode
```

and for those `cstring[N]` is the right type: no allocation, no drop. Both remaining frictions are
already gone — `names[i] == lower` compares text, and `"rna.mesh." + names[shape]` concatenates
a `cstring` without the `(string)` cast the code has today.

### Proposal

**7a (no language change):** document that literal tables are `cstring[N]` and that `cstring`
concatenates and compares as text; drop the needless `(string)` casts in Strafe.

**7b (if a runtime-filled table shows up):** give fixed arrays of owning elements drop glue.

- Allowed element types: `string`, `T[]`, `T?`, `rc<T>?`, owning/Drop structs. Elements that must
  hold a value (`^T`, `rc<T>`) need a full initializer, the same rule as `{ {}; n }` fills today.
- Default value: every element empty (`""`, empty array, empty optional) — zero bits, as for
  default-filled dynamic arrays.
- Drop: elements dropped in reverse index order when the array goes away (local scope end,
  containing struct dropped).
- Element assignment drops the old element; moving out of an element poisons it exactly like a
  `T[]` element today; `const ref` / `ref` element locals as today.
- Whole-array copy/assign stays forbidden (already is); `copy(arr)` could copy element-wise later.

### Ownership / borrowing

All existing element rules reused; the only new thing is that a fixed array becomes "owning" for
the purposes of the struct/aggregate owning predicate (so a struct holding `string[4]` is owning,
can't be `copy()`d if a Drop is inside, etc.).

### Compiler impact (7b)

- `src/Sema/ResolveOverloads.c`: remove the two rejections (locals and fields), make the owning
  predicate (`AliasIsOwning` / `TypeNameIsOwning` in `src/AST/AST.h`) see through fixed arrays;
  element-move poisoning (~L9121) is already path-based.
- `src/Codegen/LLVMModuleBuilder.c`: a drop loop for fixed arrays in local scope cleanup and in
  struct drop glue; zero-init (already the case for `= {}`).
- `src/Codegen/TypeMetadata.c` if drop glue there is table-driven.

---

## Follow-ups noticed (not proposals)

- `byte b = 300;` silently truncates to 44 — out-of-range integer literals should be an error.
- Strafe: `(byte)NN` casts and `(string)names[i]` are unnecessary today (see 1 and 7).
