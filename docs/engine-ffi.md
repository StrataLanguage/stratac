# Systems features (branch `engine-ffi`)

What was added to Strata so that a game engine (Strafe: Vulkan, Win32, Lua, Dear ImGui) can be
written in it with almost no C glue. Each feature has a sample run by `ctest`:
`samples/ffi_pointers.strata`, `samples/ownership.strata`, `samples/arrays_strings.strata`.

## Native interop

| Feature | Example |
|---|---|
| Raw pointers | `ptr p = ptr.null;` — every handle converts to `ptr`; `(ulong)p`, `(ptr)addr`, `(cstring)p` |
| Typed pointers | `*int p = null;` — a nullable, non-owning address of a `T`, usable as a field, parameter, return type or array element (`*T[]`). Compares with `null` and the same `*T`; converts to `ptr` implicitly; `(*T)ptrOrAddress`, `(ulong)p` cast. `&x` takes the address of a variable, field or element; `*p` reads and writes the pointee; `p.field` follows the pointer, and `p.Method()` passes `*p` as self (`ref`, `const ref` or by value). A null dereference panics; a pointer to an opaque handle or an owning type cannot be dereferenced, so its methods cannot be called through one. Only non-owning pointees (no `string`, `T[]`, `^T`, `T?`) |
| The null pointer | `ptr.null` converts to `ptr`, any handle and any delegate: `vkCreateDevice(gpu, info, ptr.null, device)`, `if (f == ptr.null)`. A bare `null` names the same constant (unless a variable called `null` is in scope); an empty optional is one never assigned |
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
value goes away (scope end, overwrite, `drop()`, its owner dropping). The struct keeps the layout
it declares. Such values cannot be `copy()`d or passed by value (pass `ref T` or a box `^T`), but a
local may hold one by value (`T v = T { ... };`, or `T v;` for its field defaults). A field that
holds one by value only moves with its parent; declare it `^T` or `T?` to move it out on its own.

**`Init`.** `void Init(ref T self, ...)` in the impl is a constructor, and may be overloaded: `T(args)` builds a
value from the field defaults and runs the matching `Init` on it, and `T v;` runs the one without arguments. A
type with `Init` loses its positional `T(a, b)` field constructor (braced `T { .a = ... }` still works), cannot
hold a non-optional `^T` field (use `T?` and set it in `Init`), and its `Init` is never called directly.

**Globals are never moved out of.** No function owns a global, so reading an owning global by
value — a `string`, array, box, optional, or any such field of a global struct — is an error
(`string dir = gHost.dir;`); borrow it (`const ref string dir = gHost.dir;`, a read-only param),
`copy()` it, or assign it a new value.

**Nor is a `const ref`.** A `const ref` param or local only borrows, and nothing can be put back
through it, so moving any owning part out of one is the same error: a field, an element, or a set
optional (`TextureHandle Texture(const ref Target self) { return self.texture; }`). `copy()` it or
bind it with `const ref`. A mutable `ref` still allows
take-and-refill.

**Smart cast.** Once flow typing proves a `T?` set, it is a `^T`:

```c
^Image DepthOr(ref Target t, ^Image fallback)
{
    if (t.depth?) { return t.depth; }      // Image? -> ^Image
    return fallback;
}
```

**`ref` and `const ref` locals.** An alias of existing storage — nothing moved, nothing copied:

```c
const ref string label = frame.passes[i].label;   // read without taking it out of the array
ref Pass pass = frame.passes[0];
array_push(pass.draws, 7);                        // writes through
ref Image image = frame.target;                   // the value inside a ^T / proven T?
```

A ref must be bound to a variable, field or element. It cannot be rebound or moved from, and it
dies as soon as what it refers to may have moved: a push/resize/remove/reserve/pop on an array it
reaches through, an assignment to (or move or `drop` of) a path it reaches through, passing such
a path by mutable `ref`, or — for refs into globals — any call. Using it afterwards is an error
("may dangle"); bind it again after the change.

**Out-parameters.** `=` through a `ref` param or `ref` local of a `string`, `T[]` or `T?` drops what the caller's slot held and stores the new value there:

```c
void Describe(int code, ref string error, ref Image? image)
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
| Sized / filled arrays | `VkPhysicalDevice[] adapters = { ptr.null; count };` — `count` copies of the value (evaluated once; a variable stays usable; strings are copied, rcs retained) |
| Default-filled | `Record[] slots = { {}; 8u };` — each element's default value (Drop types are alive and dropped); rejected for types that must hold a `^T` |
| Fixed fill | `byte[256] fill = { 0xFF; 256 };` — the count must be the length |
| Capacity | `array_reserve(list, 100u);` |
| Removal | `array_remove(a, i)` keeps order; `array_swap_remove(a, i)` moves the last element into the hole; both drop the removed element |
| Take | `array_take(a, i)` moves a `string`, array or optional element out and leaves its empty value; the length stays |
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
