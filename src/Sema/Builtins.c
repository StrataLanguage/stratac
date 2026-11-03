#include "Sema/Builtins.h"

#include "Core/Util.h"

#include <string.h>

typedef struct
{
    const char* name;
    ArrayBuiltinFlags flags;
} ArrayBuiltinInfo;

// Every array builtin the type checker and the move checker know by name.
static const ArrayBuiltinInfo ARRAY_BUILTINS[] = {
    {"array_push",        ABF_MUTATES | ABF_RESIZES | ABF_KEEPS_ELEMENTS},
    {"array_pop",         ABF_MUTATES | ABF_RESIZES                     },
    {"array_resize",      ABF_MUTATES | ABF_RESIZES                     },
    {"array_remove",      ABF_MUTATES | ABF_RESIZES | ABF_REMOVES       },
    {"array_swap_remove", ABF_MUTATES | ABF_RESIZES | ABF_REMOVES       },
    {"array_reserve",     ABF_MUTATES | ABF_RESIZES | ABF_KEEPS_ELEMENTS},
    {"array_replace",     ABF_MUTATES                                   },
    {"array_take",        ABF_MUTATES                                   },
};

ArrayBuiltinFlags ArrayBuiltinFlagsOf(const char* name)
{
    if (!name)
    {
        return ABF_NONE;
    }

    for (size_t i = 0; i < ARRAY_COUNT(ARRAY_BUILTINS); i++)
    {
        if (strcmp(ARRAY_BUILTINS[i].name, name) == 0)
        {
            return ARRAY_BUILTINS[i].flags;
        }
    }

    return ABF_NONE;
}

bool IsArrayMutator(const char* name)
{
    return (ArrayBuiltinFlagsOf(name) & ABF_MUTATES) != 0;
}
