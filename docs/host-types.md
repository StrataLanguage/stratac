# Sharing types with the host

How a script declares data types a host (game engine) can register, store and edit:
`@component` structs, field default values, type metadata, generic externs and symbol
prefixes.

## `@component` structs

```strata
enum Team : byte { Red = 1, Blue = 2 };

struct Stats { float armor = 2.5; int level = 3; };

@component
struct Health
{
    float current = 100.0;
    float maximum = 150.0;
    Team team = Team.Blue;
    float3 tint = float3(1.0, 1.0, 1.0);
    Stats stats;          // nested plain structs use their own defaults
    int[3] slots;
    Entity owner;         // a handle: an opaque host pointer
};
```

A component is plain data, because the host copies it as raw bytes. Its fields (and the
fields of structs it holds by value) may be scalars, enums, `float2/3/4`, handles, fixed-size
arrays and structs of those. Owning types (`string`, `^T`, `T?`, `T[]`) and `cstring` are
compile errors.

`@component` needs a struct with a body: not an alias, a forward declaration or an
`extern struct`. `@private` stays a function-only attribute.

## Field default values

`T name = expr;` gives a field a default. `expr` must be a compile-time constant: literals,
arithmetic on them, `const` globals, enum members, casts, and `float2/3/4(...)` of constants.
Defaults are only supported on scalar, enum and `float2/3/4` fields; struct-typed fields use
the nested struct's own defaults.

Defaults apply to struct literals (fields the literal leaves out), uninitialized locals and
globals, and fixed-size arrays of structs. Growing a dynamic array (`array_resize`,
`array_push` of a default) still zero-fills.

## Type metadata

The compiler describes a module's components, plus every struct and enum they use, in a
versioned binary blob. `include/strata/strata_types.h` is a header-only reader, so a host
without the compiler (an AOT build) can still read it. Structs come in dependency order;
each has its field layout, a default value (the struct with its defaults applied) and a
layout hash that changes when the layout does (not when only defaults do).

Where it comes from:

| Build | Source |
|---|---|
| JIT | `strataJitGetTypeMetadata(jit, &size)` |
| AOT | the read-only symbol `<prefix>__strata_types` in the object |
| Neither (editor preload, validation) | `strataCompileTypeMetadata(compiler, path, &bytes, &size, &err)` |

All three produce identical bytes. A module without components has no blob.

## Generic externs

```strata
extern<T> bool GetComponent(Entity entity, ref T value);
extern<T> void SetComponent(Entity entity, T value);
extern<T> bool HasComponent(Entity entity);

Health health;
if (GetComponent(entity, health))      // T inferred from `health`
{
    health.current = health.current - 10.0;
    SetComponent(entity, health);
}

if (HasComponent<Health>(entity)) { }  // T named: no T argument to infer from
```

One host function serves every `T`. Each call passes `T`'s `StrataTypeDesc` (name, name hash,
layout hash, size, alignment) as a hidden last argument, and every `T` parameter crosses as a
pointer:

```c
bool GetComponent(void* entity, void* value, const StrataTypeDesc* type);
bool HasComponent(void* entity, const StrataTypeDesc* type);
```

`T` may only be a whole parameter type (`T` or `ref T`), never the return type, and each
call's `T` must be a plain-data struct. Generic externs can't be variadic or take a `return`
parameter.

## Symbol prefixes

By default an AOT object exports its functions under their own names, so two objects built
from different modules collide at link time. `strataSetSymbolPrefix(compiler, "Gun_")` (or
`stratac --symbol-prefix Gun_`) prefixes every exported symbol: functions,
`__strata_context_create/destroy` and `__strata_types`. The string helpers become
module-local, and externs are unaffected. JIT lookups (`strataJitGetFunction`) keep taking
the unprefixed name.

`samples/components.strata` and `samples/hosts/components_host.c` link two prefixed copies of
one module into a single program and read each copy's metadata.
