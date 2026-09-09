/* Bake out hashes for primitive types */

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>

uint64_t HashStr64(const char* s)
{
    uint64_t h = 1469598103934665603ULL;
    while (*s)
    {
        h ^= (unsigned char)*s++;
        h *= 1099511628211ULL;
    }
    return h;
}

int main()
{
    const char* primNames[] = {
        "void",
        "bool",

        /* */
        "int",
        "uint",

        /* */
        "long",
        "ulong",

        /* */
        "sbyte",
        "byte",

        /* */
        "short",
        "ushort",

        /* */
        "float",
        "double",

        /* */
        "string",

        /* */
        "float2",
        "float3",
        "float4",

    };

    char buffer[256];
    char* bufferCopy = buffer;

    for (int i = 0; i < sizeof(primNames) / sizeof(primNames[0]); i++)
    {
        const char* typeName = primNames[i];
        const char* tmpName = typeName;

        bufferCopy = buffer;

        while (*tmpName)
        {
            (*bufferCopy) = (char)toupper((unsigned char)*tmpName);

            ++bufferCopy;
            ++tmpName;
        }

        (*bufferCopy) = '\0';

        uint64_t hash = HashStr64(typeName);

        printf("#define PRIM_NAME_%s %p\n", buffer, (void*)hash);
    }
}
