# Systems features (branch `engine-ffi`)

What was added to Strata so that a game engine (Strafe: Vulkan, Win32, Lua, Dear ImGui) can be
written in it with almost no C glue. Each feature has a sample run by `ctest`:
`samples/ffi_pointers.strata`, `samples/ownership.strata`, `samples/arrays_strings.strata`.

## Native interop

| Feature | Example |
|---|---|
| Raw pointers | `ptr p = null;` — every handle converts to `ptr`; `(ulong)p`, `(ptr)addr`, `(cstring)p` |
| Memory intrinsics | `addressof(x)`, `addressof(fn)`, `sizeof<T>()`, `alignof<T>()`, `load<T>(p)`, `store(p, v)` |
| Buffer of an array or string | `values.data`, `name.data` |
| Functions C can call | `@export uint Callback(ptr data) { ... }` — plain C ABI, no hidden context; the body sees the module's newest context |
| C function pointers | `extern delegate VkResult PFN_vkX(VkDevice d);` then `PFN_vkX f = (PFN_vkX)p; f(device);` (a null one panics) |
| C unions | `extern union VkClearValue { VkClearColorValue color; ... };` |
| Flag enums | `VkImageUsageFlags.SAMPLED \| VkImageUsageFlags.STORAGE`, `&`, `~` |
| Integer literal suffixes | `1l`, `1ul` |
| Program entry | `int main()` in an AOT build becomes the C `main` |
| Runtime hooks in Strata | `@export ptr strata_alloc(ulong size) { ... }` (also `strata_free`, `strata_panic`, `strata_oob`) |
| Import paths | `stratac -I root`; identical `extern` declarations in several modules merge |
| Opaque handles | `handle VkBuffer;` — `handle` is a keyword only there (top level, followed by a name), so fields, locals and params may be called `handle`; no type may be named `handle` |

A fixed array passed where an extern takes a `ptr` decays to its first element, as in C:
`snprintf(buffer, 32ul, "%d", n)` with `byte[32] buffer`. `(ptr)fixedArray` does the same
explicitly.

## Ownership

**`Drop`.** A struct whose `impl` has `void Drop(ref T self)` runs it exactly once, when its owned
value goes away (scope end, overwrite, `drop()`, its owner dropping). Such values cannot be
`copy()`d or passed by value.

**`rc<T>`.** A box shared by reference count: copying retains, dropping releases, the last
release drops the value. `rc<T>?` may be empty. Moving a field out of an rc is an error — copy it.

A `^T` given where an `rc<T>` or `rc<T>?` is wanted (a `T?` where an `rc<T>?` is) moves into a
fresh rc cell — initialization, assignment, field, argument, return, array literal, `array_push`:

```c
rc<DynParams> block = DynParams.Create(layout);   // Create returns ^DynParams
```

It is a move: the source is gone afterwards ("used after move"), and a global or `ref` source is
an error. The value is copied once into the new cell (its `Drop` still runs exactly once); the
other direction, `rc<T>` to `^T`, does not exist.

**Globals are never moved out of.** No function owns a global, so reading an owning global by
value — a `string`, array, box, optional, or any such field of a global struct — is an error
(`string dir = gHost.dir;`); borrow it (`const ref string dir = gHost.dir;`, a read-only param),
`copy()` it, or assign it a new value. Copying an `rc<T>` out of a global is fine (it retains).

**Smart cast.** Once flow typing proves a `T?` / `rc<T>?` set, it is a `^T` / `rc<T>`:

```c
rc<Image> DepthOr(ref Target t, rc<Image> fallback)
{
    if (t.depth?) { return t.depth; }      // rc<Image>? -> rc<Image>
    return fallback;
}
```

**`ref` and `const ref` locals.** An alias of existing storage — nothing moved, nothing copied:

```c
const ref string label = frame.passes[i].label;   // read without taking it out of the array
ref Pass pass = frame.passes[0];
array_push(pass.draws, 7);                        // writes through
ref Image image = frame.target;                   // the value inside a ^T / rc<T> / proven T?
```

A ref must be bound to a variable, field or element. It cannot be rebound or moved from, and it
dies as soon as what it refers to may have moved: a push/resize/remove/reserve/pop on an array it
reaches through, an assignment to (or move or `drop` of) a path it reaches through, passing such
a path by mutable `ref`, or — for refs into globals — any call. Using it afterwards is an error
("may dangle"); bind it again after the change.

**Out-parameters.** `=` through a `ref` param or `ref` local of a `string`, `T[]`, `T?`, `rc<T>` or
`rc<T>?` drops what the caller's slot held and stores the new value there:

```c
void Describe(int code, ref string error, ref rc<Image>? image)
{
    if (code != 0) { error = "failed"; return; }
    image = Image { .id = 12 };
}
```

A `ref ^T` still aliases the boxed value, so `=` on it assigns the contents.

**Read-only parameters borrow.** A by-value `string`, `T[]` or owning-struct parameter that the
function never consumes (never moves it or a part of it, writes it, or passes it on to something
that does) is passed as `const ref`: callers keep their value and nothing is copied.

```c
int Len(string s) { return (int)s.length; }     // borrows
string Keep(string s) { return s; }             // takes ownership, as before
Len(names[0]);                                   // names keeps its element
```

`extern` and `@export` signatures are never changed. Externs and C varargs only ever borrow.

**Moving out of an array element** is allowed and leaves the array poisoned until the slot is
refilled; the error names the moved element and suggests `const ref` or `copy()`.

## Control flow

**`@noreturn`.** `@noreturn void Fatal(string m) { ...; exit(1); }` — a call to it ends the path
for flow typing, so `if (!x?) { Fatal("..."); }` proves `x` below. Also usable on externs
(`@noreturn extern void abort();`). The function must return `void` and every path must end in
another `@noreturn` call.

**Narrowing through conditions.** `x?` tests combine: `if (a? && b?)` proves both inside;
`if (!a? || !b?) { return; }` proves both after; `a? ? a.x : 0` and `a? && a.ok` narrow their
right-hand side.

**Ternary.** `c ? a : b`; two string literals give a `cstring`.

## Arrays and strings

| Feature | Example |
|---|---|
| Sized / filled arrays | `VkPhysicalDevice[] adapters = { null; count };` — `count` copies of the value (evaluated once; a variable stays usable; strings are copied, rcs retained) |
| Default-filled | `Record[] slots = { {}; 8u };` — each element's default value (Drop types are alive and dropped); rejected for types that must hold a `^T` / `rc<T>` |
| Fixed fill | `byte[256] fill = { 0xFF; 256 };` — the count must be the length |
| Capacity | `array_reserve(list, 100u);` |
| Removal | `array_remove(a, i)` keeps order; `array_swap_remove(a, i)` moves the last element into the hole; both drop the removed element |
| Concatenation | `"effect '" + name + "': " + message` |
| Text comparison | `string == string`, `string == cstring`, `cstring == cstring` all compare text |
| Character literals | `'/'`, `'\\'`, `'\''`, `'\n'` — a `byte` constant (exactly `(byte)47`); one ASCII byte or one of the string escapes. Arithmetic between bytes stays `byte` and wraps: widen first (`int c = 'a';`) |
| Byte buffers as text | `(cstring)buf` borrows a `byte[N]`; `(string)buf` copies up to the first NUL (or all N bytes) |
| String as C string | `(cstring)s` borrows the string's NUL-terminated bytes |

```c
if ((cstring)layers[i].layerName == name) { ... }  // a Vulkan char[256] against a string

string IntToString(long value)
{
    byte[32] buffer = {};
    snprintf(buffer, 32ul, "%lld", value);
    return (string)buffer;
}
```
