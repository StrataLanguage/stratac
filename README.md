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

## Building

Requires CMake 3.20+, Ninja, a C11 compiler, and LLVM (set `LLVM_C_DIR` in `CMakePresets.json`).

```sh
cmake --preset default
cmake --build --preset default
ctest --preset default
```
