#pragma once

// Deep copies of AST subtrees with type parameters replaced by type arguments (generics: clone with
// substitution). The templates the Monomorphize pass copies are never changed.

#include "AST/AST.h"
#include "Core/Diagnostics.h"

// The pack loop being unrolled: `for (T i = 0; i < args.length; i++)` binds `var` to `index` in each copy.
typedef struct PackLoop
{
    const char* var; // the loop variable
    size_t index;    // the iteration being cloned
    const struct PackLoop* outer;
} PackLoop;

// `args[i]` replaces the type parameter named by `params[i]`. A NULL TypeSubst, or one whose `params`
// is NULL or empty, copies the tree without replacing anything. The pack fields are zero when the template
// has no parameter pack (generics: parameter packs).
typedef struct
{
    const Vec* params;        // char*: type parameter names (the pack name is NOT in here)
    const TypeName* args;     // one per param
    const char* packType;     // `TArgs`, NULL when the template has no pack
    const char* packValue;    // `args`: the rest parameter typed with the pack, NULL if none
    const TypeName* packArgs; // the pack's types
    size_t packCount;
    DiagnosticEngine* diag; // pack misuse is reported here
    const PackLoop* loop;   // innermost pack loop being unrolled
    bool inPackLoopBody;    // directly inside a pack loop body (break and continue are errors)
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
