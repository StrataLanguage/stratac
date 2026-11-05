# Strata

A small, statically typed scripting language for embedding in game engines. Built on LLVM, written in C11.

## Features

- Supports Just-In-Time and Ahead-Of-Time compilation for native performance.
- Interoperable with existing codebases.
- Compile-time nullability tracking.
- Move-only box allocations.
- Supports SIMD accelerated vectors `float3` and `float4` with intrinsics for `cross`, `dot` and more.
- Generics for data structures and functions.

## Sample

```hlsl
extern int printf(string fmt, ...);

// float3 / float4 are SIMD vectors
float4 add(ref float4 a, ref float4 b)
{
    return a + b;
}

struct Particle
{
    float4 pos;
    Particle? next;               // optional, may be empty
};

int main()
{
    float4 acc = float4(1.0, 2.0, 3.0, 4.0);

    float4[] points = { float4(10.0, 20.0, 30.0, 40.0) };

    for (uint i = 0; i < points.length; i++)
    {
        acc = add(acc, points[i]);
    }

    ^Particle head = Particle { .pos = acc };    // owned box, freed at scope end
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

Structs, `impl` blocks and functions can take type parameters. Each distinct list of type arguments is
compiled as its own copy, so `number<int>` and `number<float>` are distinct types.

```hlsl
struct number<T>
{
    T value;
}

impl number<T>
{
    T get_value(const ref number<T> self) { return self.value; }
    void set_value(ref self, T value) { self.value = value; }  // `self` alone takes the impl's type
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

## Parameter packs

The last type parameter of a generic function may be a pack, written `Name...`. It binds any number of
type arguments, including zero.

```hlsl
void forward<T, TArgs...>(TArgs... args)
{
    T.consume(args...);              // expands to T.consume(args[0], args[1], ...)
}

void each<TArgs...>(TArgs... args)
{
    for (uint i = 0; i < args.length; i++)   // Unrolled at compile time
    {
        show(args[i]);
    }
}

void print_all<T, Rest...>(T first, Rest... rest)
{
    show(first);
    if (rest.length > 0)             // The branch is folded at compile time
    {
        print_all<Rest...>(rest...);
    }
}

int main()
{
    each<int, float, string>(1, 2.5, "three");
    forward<Sink>();
    each<>();
    print_all<int, float>(1, 2.0);
    return 0;
}
```

### Splat Syntax

- `args...` expands and splices individual elements into a call's argument list, like a splat operator. Each argument inside of the pack is passed individually to the function call.
- `TArgs...` expands a type pack's types into a type argument list (e.g. `g<int, TArgs...>(1, args...)`).

### Introspection and Access

- `args.length` provides the total element count of the pack.
- `args[N]` accesses a specific element using a literal integer N.

### Conditional Evaluation & Recursion

- An any branch whose condition depends on args.length is evaluated at compile time and keeps only the branch that is taken. This allows recursive pack processing to safely terminate at an empty pack.

### Restrictions and Limitations

- A parameter pack must always be the final type parameter in the list.
- Only generic functions are permitted to have a pack, not `struct`s, `union`s or `impl`s.
- Packs are never automatically inferred; their type arguments must always be written out explicitly. This will likely change in future versions of the compiler.
- `break` and `continue` are prohibited directly inside a pack loop, as they are unrolled at compile time.

## Generic Methods & Type Inference

### Type Arguments and Inference

Type arguments for a method call can either be **explicitly written** or **inferred** from the call arguments.

- Written directly on the method call (e.g., `x.m<float>()`, `Type.m<int>()`, or `x.m<>()` for an empty pack).
- Inference Rules:
    - A parameter whose type matches a type parameter exactly (`U`, `ref U`, `const U`) adopts the argument's type.
    - A parameter pack captures the types of all arguments following the fixed parameters.

### Naming Rules and Overloading

- A handle can have at most one generic method for any given name, and generic methods **cannot** share a name with a non-generic method.
- A generic `init` serves as a constructor overload alongside plain constructors
    - Invoked via `T(args)` or via `p.init(args)` when constructing through a pointer (placement new style).
    - If a generic `init` has the exact same parameters as a plain `init`, it is ignored.

### Restrictions

- Generic `drop` methods are not allowed.
- `extern` methods with type parameters.
- A method type parameter that shares a name with one of the enclosing `impl`'s type parameters.

## Meta functions

A `@meta` function runs at compile time. Its calls are written with an `@` prefix and fold to a `bool`
or integer constant before type checking. In a generic function each instance is checked separately, so
a branch that does not apply to one type can be included in other specializations.

```hlsl
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

## Var Syntax

`var` declares a local whose type is the type of its initializer.

```hlsl
int get_sum(int a, int b) { return a + b; }

int main()
{
    var some_int = 10;                      // int
    var some_float = 10.0f;                 // float
    var some_array = int[] { 10, 20, 30 };  // int[]
    var result = get_sum(10, 20);           // int
    const var limit = 64;                   // const int
    for (var i = 0; i < 3; i++) { }         // int
    return 0;
}
```

`var` needs an initializer and is only for locals. `var` is not supported for parameters, fields, return types, globals,
`ref` locals or `@meta` functions as the type can often be ambiguous.

## Building

Requirements:

- CMake 3.20+
- Ninja
- A C compiler that supports C11 or above
- LLVM (you can provide `LLVM_C_DIR` in CMakePresets)

```sh
cmake --preset default
cmake --build --preset default
ctest --preset default
```
