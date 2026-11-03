# Strata

A small, statically typed scripting language for embedding in game engines. Built on LLVM, written in C11.

```c
extern int printf(string fmt, ...);

// float3 / float4 are SIMD vectors
float4 add(ref float4 a, ref float4 b)
{
    return a + b;
}

struct Particle
{
    float4 pos;
    Particle? next;               // optional: may be empty
};

int main()
{
    ^float4 acc = float4(1.0, 2.0, 3.0, 4.0);   // owned box, freed at scope end

    float4[] points = { float4(10.0, 20.0, 30.0, 40.0) };

    for (uint i = 0; i < points.length; i++)
    {
        acc = add(acc, points[i]);
    }

    ^Particle head = Particle { .pos = acc };
    Particle? cur = head;
    while (cur?)                  // checks non-empty before access
    {
        printf("pos = %f %f %f %f\n", cur.pos.x, cur.pos.y, cur.pos.z, cur.pos.w);
        cur = cur.next;
    }

    defer printf("done\n");       // runs at scope exit

    return (int)(acc.x + acc.y);  // 33
}
```

## Generics

Structs, `impl` blocks and functions take type parameters. Each distinct list of type arguments is
compiled as its own copy, so `number<int>` and `number<float>` are separate types.

```c
struct number<T>
{
    T value;
}

impl number<T>
{
    T get_value(const ref number<T> self) { return self.value; }
    void set_value(ref number<T> self, T value) { self.value = value; }
}

number<T> make_number<T>(T value)
{
    number<T> num;
    num.set_value(value);
    return num;
}

int main()
{
    number<int> a = make_number<int>(3);
    number<float> b = number<float> { .value = 1.5 };
    return a.get_value();
}
```

v1 limits:

- Type arguments are always written: `make_number<int>(3)`, not `make_number(3)`.
- Methods cannot have type parameters of their own; they belong to the `impl`.
- There are no constraints. A body is checked for each argument it is instantiated with, so an
  operation that one argument does not support is an error for that instance.
- Every method of a generic `impl` is instantiated for each instance of the struct, so each must
  type-check for that argument.
- Extern functions, `@component` structs, `extern struct`, struct aliases and handles cannot be generic.
- Properties and extern methods are not allowed in a generic `impl`.

## Meta functions

A `@meta` function runs at compile time. Its calls are written with an `@` prefix and fold to a `bool`
or integer constant before type checking. In a generic function each instance is checked separately, so
a branch that does not apply to one type is dropped before it is checked:

```c
@meta
bool droppable(type T)
{
    // Whether T's impl defines a method `drop`.
    return @has_function(T, drop);
}

void free_object<T>(*T ptr_value)
{
    if (@droppable(T))
    {
        ptr_value.drop();
    }
}
```

The built-ins are `@has_function(T, name)`, true when T's impl has a method `name`, and `@is_same(A, B)`,
true when the two types have the same name (`const` ignored). A `type` parameter is written only in a
`@meta` function. Meta functions have their own namespace, so `@hello` and `hello` can both exist.

Limits: parameters are `type`, `bool` or an integer type, and the result is `bool` or an integer. The
body may use locals, `if`, `return`, `?:`, bool and integer operators, and other meta calls. Loops,
ordinary function calls and member access are errors. Built-in names cannot be redefined.

## Local type inference

`var` declares a local whose type is the type of its initializer, like `auto` in C++. `const var` makes
it const. A top-level `const` on the value is dropped, since a copy is mutable. `var` also works in a
`for` initializer. Typed dynamic-array literals (`int[] { 1, 2, 3 }`, `int[] { 0; n }`) are expressions,
so they can be used with `var` or as arguments.

```c
int get_sum(int a, int b) { return a + b; }

int main()
{
    var some_int = 10;                      // int
    var some_float = 10.0f;                 // float
    var some_array = int[] { 10, 20, 30 }; // int[]
    var result = get_sum(10, 20);           // int
    const var limit = 64;                   // const int
    for (var i = 0; i < 3; i++) { }         // int
    return 0;
}
```

`var` needs an initializer and is only for locals: not for parameters, fields, return types, globals,
`ref` locals or `@meta` functions. The initializer must have a type, so `null`, a braced list and a
void call are errors.

## Building

Requires CMake 3.20+, Ninja, a C11 compiler, and LLVM (set `LLVM_C_DIR` in `CMakePresets.json`).

```sh
cmake --preset default
cmake --build --preset default
ctest --preset default
```

## Systems features

Raw pointers, `@export` callbacks, C function pointers, destructors, `ref` locals, inferred
borrowing, `@noreturn`, sized arrays and C string interop are described in
[docs/engine-ffi.md](docs/engine-ffi.md).
