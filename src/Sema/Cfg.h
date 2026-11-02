#pragma once

#include "AST/AST.h"
#include "Core/Util.h"

#include <stddef.h>

/* Control-flow graph of one function body (roadmap phase 4). It carries no types and makes no
   judgement: the move, narrowing and loan analyses run over it. Every expression node is one op,
   emitted after its operands, so ops list evaluation order (left to right; for `=` the right side,
   then the destination's subexpressions, then the assignment itself). `&&`, `||` and `?:` split
   blocks. `defer`red statements are copied onto every path that leaves their scope. */

typedef enum {
    CfgEval,      /* an expression node, evaluated after its operands */
    CfgDeclare,   /* a VarDeclStmt: its initializer has been evaluated, the local now exists */
    CfgCondition, /* the value of `node` decides the block's two successors: [0] true, [1] false */
    CfgReturn,    /* a ReturnStmt: its value, if any, has been evaluated */
    CfgScopeExit, /* leaving the Block `node`; its locals drop here, in reverse declaration order */
} CfgOpKind;

typedef struct {
    CfgOpKind kind;
    const Node* node;
} CfgOp;

typedef struct {
    size_t id;
    CfgOp* ops;
    size_t opCount;
    size_t opCap;
    size_t succ[2];
    unsigned succCount;
    bool reachable; /* some path from the entry gets here */
} CfgBlock;

typedef struct {
    CfgBlock* blocks;
    size_t count;
    size_t cap;
    size_t entry;
    size_t exit; /* every `return` and the end of the body lead here */
} Cfg;

/* An empty graph when the function has no body. */
Cfg CfgBuild(const FunctionDecl* fn);
void CfgFree(Cfg* cfg);

/* One line per block: `bb1: ident(x) call(F) cond -> bb2, bb3`. The caller frees the result. */
char* CfgDump(const Cfg* cfg);
