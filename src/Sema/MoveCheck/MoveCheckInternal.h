#pragma once
// State and helpers shared by the MoveCheck/ source files.

#include "Sema/MoveCheck.h"

#include "Codegen/TypeUtil.h"
#include "Sema/Builtins.h"
#include "Sema/Cfg.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// State of one place (`a`, `a.b`, `a.b.c`) at a program point. A place is unusable when it or an
// ancestor is Moved or Maybe; using it whole is also unusable when a part of it is.
enum
{
    StateUnvisited = 0, // no path has reached this block yet
    StateLive = 1,
    StateMoved = 2,
    StateMaybe = 3,
};

typedef struct
{
    const char* name;
    bool owning;
    const TypeName* type;
} Local;

typedef struct
{
    Local* locals;
    size_t count;
    size_t cap;
} Locals;

typedef struct
{
    const TypeRegistry* reg;
    const NodeTypes* types;
    Arena* arena;
    Locals locals;
    Locals named; // every parameter and local, whatever its type; any other root is a global
    const TypeName* fnReturn;
    const FunctionDecl* fn;
    bool inDropMethod; // a drop method may take its own fields apart
    const WriteSets* ws;
    const Node** copied; // boxes handed to a bare `T`: the pointee is copied, the box stays
    size_t copiedCount;
    size_t copiedCap;
    char** aliasName; // `ref` locals and the place each was bound to
    char** aliasTarget;
    size_t aliasCount;
    size_t aliasCap;
    char** paths;
    size_t pathCount;
    size_t pathCap;
    char** facts; // optional places that narrowing can prove non-empty
    size_t factCount;
    size_t factCap;
} Env;

typedef struct
{
    MoveDiag* items;
    size_t count;
    size_t cap;
} Found;

// Write sets: the global places each function may write, directly or through the functions it calls. A
// call changes only those, so it ends narrowing only for them, and an argument that borrows one of them
// conflicts with a callee that writes it. `all` is for a callee nothing is known about.
typedef struct
{
    const FunctionDecl* fn;
    char** keys;
    size_t count;
    size_t cap;
    const FunctionDecl** callees;
    size_t calleeCount;
    size_t calleeCap;
    char** reads; // names of the globals it reads, directly or through callees
    size_t readCount;
    size_t readCap;
    bool all;
} FnWrites;

struct WriteSets
{
    FnWrites* fns;
    size_t count;
};

// Narrowing: a `T?` place is proven non-empty by a null test, or by being set from a value that is.
// The proof is a must-fact: it holds only if every path to the point agrees. Facts live after the move
// states in a block's state array: 1 = not proven, 2 = non-empty (3 = paths disagree).

enum
{
    FactNone = 1,
    FactNonEmpty = 2,
    FactEmpty = 4, // known to be empty: tested and found null, or moved out of
};

// A place split into segments: names (`a`, `b`) and index groups (`[i]`, `[0]`).
typedef struct
{
    const char* p;
    size_t n;
} Seg;

enum
{
    MaxSegs = 24
};

// Loans. Within one expression, a place taken by an earlier operand stays loaned until the expression
// ends: shared for a borrow, mutable for a `ref`. A later operand may not move or mutate a place that
// overlaps a shared loan, nor touch one that overlaps a mutable loan. A move needs no loan of its own:
// using the place afterwards is the move checker's error. Trivial values are copied at once, so they
// hold no loan.
typedef enum
{
    LoanShared,
    LoanMut,
    LoanMove,
} LoanMode;

typedef struct
{
    char key[160];
    LoanMode mode;
    const Node* node;
} Access;

typedef struct
{
    Access* items;
    size_t count;
    size_t cap;
} Accesses;

typedef struct
{
    const Module* mod;
    const WriteSets* ws;
    DiagnosticEngine* diag;
    size_t self;
} InitCtx;

// -- Places.c
int FindLocal(const Locals* ls, const char* name);
bool TypeOwns(const Env* env, const TypeName* t);
const TypeName* DeclType(const Env* env, const VarDeclStmt* vd);
bool PlaceOf(const Node* n, char* buf, size_t cap, const char** root);
bool TrackedPlace(const Env* env, const Node* n, char* buf, size_t cap);
bool IsAncestor(const char* a, const char* d);
const TypeName* FieldType(const Env* env, const TypeName* t, const char* name);
const TypeName* PlaceType(const Env* env, const Node* n);
void CollectLocals(Env* env, const FunctionDecl* fn, const Cfg* g);
void CollectPaths(Env* env, const Cfg* g);
unsigned char Join(unsigned char a, unsigned char b);
int PlaceBlocker(const Env* env, const unsigned char* state, const char* path, bool wholeUse);
void SetPlace(const Env* env, unsigned char* state, const char* path, unsigned char value);
bool IsCopied(const Env* env, const Node* n);
void CollectCopies(Env* env, const Cfg* g);
void FreeAliases(Env* env);
void BuildAliases(Env* env, const Cfg* g);
const FnWrites* FindWrites(const WriteSets* ws, const FunctionDecl* d);
const char* AliasTarget(const char* name);
bool IndexSpelling(const Node* n, char* out, size_t cap);

// -- Facts.c
bool PlaceKeyImpl(const Node* n, char* buf, size_t cap, const char** root, bool lax);
size_t SplitPlace(const char* path, Seg* segs);
bool SegsAlias(Seg a, Seg b);
const TypeName* ValueType(const Env* env, const Node* n);
void CollectFacts(Env* env, const Cfg* g);
void ApplyCond(const Env* env, unsigned char* f, const Node* c, bool whenTrue, bool loopExit);
const IdentExpr* RootIdent(const Node* n);
void DanglingCheck(const Env* env, const unsigned char* f, Found* found, const Node* n);
void FactStep(const Env* env, unsigned char* f, const CfgOp* op, Found* found);

// -- MoveCheck.c
MoveDiag* ReportAs(Found* f, MoveDiagKind kind, SourceRange range, const char* name);

// -- Loans.c
bool NonTrivial(const Env* env, const Node* n);
bool PlacesOverlap(const char* a, const char* b);
LoanMode ArgMode(const CallExpr* e, size_t i);
void LoanCheck(const Env* env, Found* found, const Node* n);

// -- WriteSets.c
void CheckGlobalArgs(const Env* env, Found* found, const CallExpr* e);
void TouchGlobal(const InitCtx* c, const char* name, SourceRange range, bool write);
