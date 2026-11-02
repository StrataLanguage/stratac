#include "Sema/NodeTypes.h"

#include <stdint.h>
#include <stdlib.h>

static size_t Slot(const Node* node, size_t cap)
{
    uintptr_t p = (uintptr_t)node;
    p ^= p >> 17;
    p *= (uintptr_t)0x9E3779B97F4A7C15ULL;
    return (size_t)(p >> 7) & (cap - 1);
}

void NodeTypesFree(NodeTypes* map)
{
    free((void*)map->keys);
    free((void*)map->values);
    *map = (NodeTypes){0};
}

static void Grow(NodeTypes* map)
{
    NodeTypes old = *map;
    map->cap = old.cap ? old.cap * 2 : 256;
    map->keys = (const Node**)calloc(map->cap, sizeof(*map->keys));
    map->values = (const TypeName**)calloc(map->cap, sizeof(*map->values));
    map->count = 0;

    for (size_t i = 0; i < old.cap; i++)
    {
        if (old.keys[i])
        {
            NodeTypesPut(map, old.keys[i], old.values[i]);
        }
    }

    free((void*)old.keys);
    free((void*)old.values);
}

void NodeTypesPut(NodeTypes* map, const Node* node, const TypeName* type)
{
    if (!node || !type)
    {
        return;
    }

    if ((map->count + 1) * 2 > map->cap)
    {
        Grow(map);
    }

    size_t i = Slot(node, map->cap);

    while (map->keys[i] && map->keys[i] != node)
    {
        i = (i + 1) & (map->cap - 1);
    }

    if (!map->keys[i])
    {
        map->keys[i] = node;
        map->count++;
    }

    map->values[i] = type;
}

const TypeName* NodeTypesGet(const NodeTypes* map, const Node* node)
{
    if (!map->cap || !node)
    {
        return NULL;
    }

    size_t i = Slot(node, map->cap);

    while (map->keys[i])
    {
        if (map->keys[i] == node)
        {
            return map->values[i];
        }

        i = (i + 1) & (map->cap - 1);
    }

    return NULL;
}
