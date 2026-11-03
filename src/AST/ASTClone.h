#pragma once

// Deep copies of AST subtrees with type parameters replaced by type arguments (generics: clone with
// substitution). The templates the Monomorphize pass copies are never changed.

#include "AST/AST.h"

// `args[i]` replaces the type parameter named by `params[i]`. A NULL TypeSubst, or one whose `params`
// is NULL or empty, copies the tree without replacing anything.
typedef struct
{
    const Vec* params;    // char*: type parameter names
    const TypeName* args; // one per param
} TypeSubst;

// `t` rebuilt with every type parameter replaced. A generic use (`number<T>`) gets its arguments
// substituted and its canonical name rebuilt. Wrappers and argument arrays are newly allocated; leaf names
// are shared, as arena strings are.
TypeName TypeNameSubstitute(Arena* arena, const TypeName* t, const TypeSubst* s);

// A deep copy of the node `n`, with every type and every identifier naming a parameter substituted. Each
// copy's Vecs are freshly allocated, so a copy and its source can each be AstDispose'd on their own.
// Sema-only fields are cleared in the copy. A Module is not cloned. Calls abort on a node kind with no
// clone, so a new NodeKind cannot be silently skipped.
//
// The method FunctionDecls of a cloned ImplDecl are copies of their own: AstDispose of the impl frees
// only its lists, so they are disposed like any method (a method is listed in Module::functions too).
Node* AstCloneSubst(Arena* arena, const Node* n, const TypeSubst* s);
