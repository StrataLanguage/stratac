#pragma once

#include <stdbool.h>

// What an array builtin does to the array it is given as its first argument.
enum ArrayBuiltinFlags
{
    ABF_NONE = (0),
    ABF_MUTATES = (1 << 0),        // writes the array: its length, its capacity or one of its elements
    ABF_RESIZES = (1 << 1),        // changes the length or capacity, so the buffer may be reallocated
    ABF_REMOVES = (1 << 2),        // drops element i and closes the hole (order kept, or last element moved in)
    ABF_KEEPS_ELEMENTS = (1 << 3), // touches no element; only the capacity can change
};

typedef enum ArrayBuiltinFlags ArrayBuiltinFlags;

// The flags of an array builtin by name, or ABF_NONE when `name` is not one.
ArrayBuiltinFlags ArrayBuiltinFlagsOf(const char* name);

// Does the builtin `name` write its array argument?
bool IsArrayMutator(const char* name);
