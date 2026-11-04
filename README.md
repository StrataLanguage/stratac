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

- A generic function's type arguments can be written (`make_number<int>(3)`) or inferred from the
  arguments' declared types (`make_number(3)`); see "Generic methods" below for the rules.
- There are no constraints. A body is checked for each argument it is instantiated with, so an
  operation that one argument does not support is an error for that instance.
- Every method of a generic `impl` is instantiated for each instance of the struct, so each must
  type-check for that argument.
- Extern functions, `@component` structs, `extern struct`, struct aliases and handles cannot be generic.
- Properties and extern methods are not allowed in a generic `impl`.

### Parameter packs

The last type parameter of a generic function may be a pack, written `Name...`. It binds any number of
type arguments, including none. A rest parameter typed with the pack takes one value per type:

```c
void forward<T, TArgs...>(TArgs... args)
{
    T.consume(args...);              // expands to T.consume(args[0], args[1], ...)
}

void each<TArgs...>(TArgs... args)
{
    for (uint i = 0; i < args.length; i++)   // unrolled: one copy of the body per element
    {
        show(args[i]);
    }
}

void print_all<T, Rest...>(T first, Rest... rest)
{
    show(first);
    if (rest.length > 0)             // folds: the branch not taken is never instantiated
    {
        print_all<Rest...>(rest...);
    }
}

int main()
{
    each<int, float, string>(1, 2.5, "three");
    forward<Sink>();                 // empty pack
    each<>();                        // empty pack, no fixed parameters
    print_all<int, float>(1, 2.0);
    return 0;
}
```

- `args...` in a call's argument list splices the elements; `TArgs...` in a type-argument list splices the
  pack's types, as in `g<int, TArgs...>(1, args...)`.
- `args.length` is the element count, and `args[N]` with a literal `N` names one element. Inside a
  `for (T i = 0; i < args.length; i++)` loop, `args[i]` names the element of the current iteration.
- An `if` or `?:` whose condition involves `args.length` keeps only the branch taken, so recursion
  ends at an empty pack.
- Limits: a pack must be the last type parameter, and only generic functions can have one. A pack is
  never inferred, so its type arguments are written. `break` and `continue` are not allowed directly in
  a pack loop. Only a bare `name...` expands, and `args.length`, `args[i]` and `args...` are the only
  uses of a pack value. Indexing a type pack and patterns such as `f(args)...` are not supported.

### Generic methods

A method inside an `impl` may take type parameters of its own, and a parameter pack. Its instances are
made with the impl's, one per type argument list:

```c
struct Box2<T> { T value; }

impl Box2<T>
{
    U convert<U>(const ref Box2<T> self) { return (U)self.value; }
    void set_all<U>(ref Box2<T> self, U value) { self.value = (T)value; }
    void init<TArgs...>(ref Box2<T> self, TArgs... args) { self.set_all(args...); }
}

int main()
{
    Box2<int> b = Box2<int>(2.0);       // constructor: TArgs inferred as double
    float f = b.convert<float>();       // explicit type arguments
    b.set_all(2.5f);                    // inferred: U = float
    return 0;
}
```

- The type arguments of a call are written (`x.m<float>()`, `Type.m<int>()`, `x.m<>()` for an empty pack),
  or inferred from the arguments: a parameter whose type is exactly a type parameter (`U`, `ref U`,
  `const U`) takes the argument's type, and a pack takes the types of the arguments after the fixed
  parameters. The receiver is not an argument. Inference sees the declared type of a name, a field, a
  literal (`2.0` is `double`, `2.0f` is `float`), a cast, a unary operator, a constructor call, a call of a
  function declared once, or a struct literal.
- Errors: a type parameter that no argument fixes ("can't infer type argument 'U' of 'm': write
  'x.m<...>(...)'"); two arguments that disagree ("is inferred as both 'int' and 'float'"); an argument
  whose type is not known before type checking, such as `1 + 2`, when inference needs it; a receiver with no
  known type when some generic method has that name.
- A method template's name is its own: a handle has at most one generic method of each name, and it may not
  share a name with a plain method. `init` is the exception: a generic `init` is a constructor overload beside
  the plain ones. `T(args)` and `p.init(args)` through a pointer pick it, and an instance with the same
  parameters as a plain `init` is not made.
- Not allowed: a generic `drop`, an `extern` or `@export` method with type parameters, and a method type
  parameter with the name of one of the impl's.

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
