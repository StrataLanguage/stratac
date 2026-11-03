#pragma once
// Per-expression-node types that sema infers.

#include "AST/AST.h"

// The type sema inferred for an expression node, kept beside the AST instead of in it. Filled as
// `InferType` runs, so a node sema never inferred has no entry. The CFG checkers read it.
typedef struct
{
    const Node** keys;
    const TypeName** values;
    size_t cap; // power of two, or 0
    size_t count;
} NodeTypes;

void NodeTypesFree(NodeTypes* map);
void NodeTypesPut(NodeTypes* map, const Node* node, const TypeName* type);
const TypeName* NodeTypesGet(const NodeTypes* map, const Node* node);
