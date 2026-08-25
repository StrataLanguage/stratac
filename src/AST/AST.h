#pragma once

#include "Core/SourceLocation.h"
#include "Core/Util.h"

#include <stdint.h>
#include <string.h>

typedef enum {
    NodeModule,
    NodeImport,
    NodeStruct,
    NodeHandle,
    NodeFunction,
    NodeParam,
    NodeBlock,
    NodeReturn,
    NodeIf,
    NodeWhile,
    NodeFor,
    NodeVarDecl,
    NodeExprStmt,
    NodeBreak,
    NodeContinue,
    NodeIntLiteral,
    NodeFloatLiteral,
    NodeBoolLiteral,
    NodeStrLiteral,
    NodeIdent,
    NodeUnary,
    NodeBinary,
    NodeAssign,
    NodeCall,
    NodeMember,
    NodeStructInit,
    NodeIncDec,
    NodeCast,
    NodeGlobal,
    NodeIndex,
    NodeArrayInit,
} NodeKind;

typedef struct {
    NodeKind kind;
    SourceRange range;
} Node;

#define AST_NEW(arena, type) ((type*)arena_alloc((arena), sizeof(type)))
#define AsNode(type, node) ((type*)(node))

/* Types are structural: `^T` (box) and `T[]`/`T[N]` (array) are dedicated
   nodes wrapping an inner/element TypeName; `name` is the canonical spelling
   of the whole subtree ("Foo", "^Foo", "^Foo[]", "int[][]", "int[4]"),
   derived at parse time and kept consistent by the wrap helpers below. A
   node is either a leaf (name set), a box (isBox + inner), or an array
   (isArray + elem). A dynamic array `T[]` has length < 0; a fixed-size
   array `T[N]` (C-ABI inline storage, struct fields only) has length >= 1. */
typedef struct TypeName {
    char* name;
    SourceRange range;
    bool isConst;
    bool isVector;
    bool isArray;
    long length;
    struct TypeName* elem;
    bool isBox;
    struct TypeName* inner;
} TypeName;

static inline bool TypeNameValid(const TypeName* t)
{
    return t->name != NULL && t->name[0] != '\0';
}

/* `^T` — wraps `inner` into a boxed type. isConst of the result reflects the
   leading `const` (set by the caller); the inner node keeps its own flags. */
static inline TypeName TypeNameBoxWrap(Arena* arena, TypeName inner)
{
    TypeName* i = (TypeName*)arena_alloc(arena, sizeof(TypeName));
    *i = inner;

    TypeName t = {0};
    t.isBox = true;
    t.inner = i;
    t.name = arena_format(arena, "^%s", i->name);
    t.range = i->range;
    return t;
}

/* `T[]` — wraps `elem` into a dynamic (fat pointer) array type. */
static inline TypeName TypeNameArrayWrap(Arena* arena, TypeName elem)
{
    TypeName* e = (TypeName*)arena_alloc(arena, sizeof(TypeName));
    *e = elem;

    TypeName t = {0};
    t.isArray = true;
    t.length = -1;
    t.elem = e;
    t.name = arena_format(arena, "%s[]", e->name);
    t.isConst = e->isConst;
    t.range = e->range;
    return t;
}

/* `T[N]` — wraps `elem` into a fixed-size inline array type (C ABI). */
static inline TypeName TypeNameFixedArrayWrap(Arena* arena, TypeName elem, long length)
{
    TypeName* e = (TypeName*)arena_alloc(arena, sizeof(TypeName));
    *e = elem;

    TypeName t = {0};
    t.isArray = true;
    t.length = length;
    t.elem = e;
    t.name = arena_format(arena, "%s[%ld]", e->name, length);
    t.isConst = e->isConst;
    t.range = e->range;
    return t;
}

/* ---- Structural type queries -------------------------------------------
   These are the authority for "what shape is this type". The `name` spelling
   is derived data (display, mangling, map keys) — never parse it for shape. */

static inline bool TypeNameIsArray(const TypeName* t)
{
    return t && t->isArray;
}

/* `T[]` — owning fat-pointer array. */
static inline bool TypeNameIsDynamicArray(const TypeName* t)
{
    return t && t->isArray && t->length < 0;
}

/* `T[N]` — C-ABI inline storage (struct fields only). */
static inline bool TypeNameIsFixedArray(const TypeName* t)
{
    return t && t->isArray && t->length >= 0;
}

/* N for `T[N]`; -1 otherwise (including dynamic arrays). */
static inline long TypeNameArrayLength(const TypeName* t)
{
    return (t && t->isArray) ? t->length : -1;
}

static inline const TypeName* TypeNameArrayElem(const TypeName* t)
{
    return (t && t->isArray) ? t->elem : NULL;
}

static inline bool TypeNameIsBox(const TypeName* t)
{
    return t && t->isBox;
}

/* Inner `T` of a box `^T`, or NULL. Never an array (no such spelling). */
static inline const TypeName* TypeNameBoxInner(const TypeName* t)
{
    return (t && t->isBox) ? t->inner : NULL;
}

/* Owning types: `string`, `^T` and dynamic `T[]`. Fixed `T[N]` is plain
   inline storage and never owning. */
static inline bool TypeNameIsOwning(const TypeName* t)
{
    if (!t || !t->name)
    {
        return false;
    }

    if (strcmp(t->name, "string") == 0)
    {
        return true;
    }

    return t->isBox || TypeNameIsDynamicArray(t);
}

/* A leaf TypeName for a spelling without structure ("int", "Foo"). */
static inline TypeName TypeNameLeaf(char* name)
{
    TypeName t = {0};
    t.name = name;
    return t;
}

static inline TypeName TypeNameParseGroups(Arena* arena, const char* base, size_t baseLen, const char* groups,
                                           size_t groupsLen)
{
    if (groupsLen == 0)
    {
        if (baseLen >= 2 && base[0] == '^')
        {
            TypeName* inner = (TypeName*)arena_alloc(arena, sizeof(TypeName));
            *inner = TypeNameLeaf(arena_strndup(arena, base + 1, baseLen - 1));

            TypeName t = {0};
            t.isBox = true;
            t.inner = inner;
            t.name = arena_strndup(arena, base, baseLen);
            return t;
        }

        return TypeNameLeaf(arena_strndup(arena, base, baseLen));
    }

    /* The first bracket group is the outermost dimension (`int[2][6]` is
        2 x int[6]), so wrap the remaining (inner) groups first. */
    const char* close = (const char*)memchr(groups, ']', groupsLen);
    size_t groupLen = close ? (size_t)(close - groups) + 1 : groupsLen;

    TypeName elem = TypeNameParseGroups(arena, base, baseLen, groups + groupLen, groupsLen - groupLen);

    TypeName* e = (TypeName*)arena_alloc(arena, sizeof(TypeName));
    *e = elem;

    TypeName t = {0};
    t.isArray = true;
    t.elem = e;

    if (groupLen == 2 && groups[1] == ']')
    {
        t.length = -1;
    }
    else
    {
        long v = 0;

        for (size_t i = 1; i + 1 < groupLen; i++)
        {
            v = v * 10 + (groups[i] - '0');
        }

        t.length = v;
    }

    /* Canonical name: base + every group, in source order. */
    Sb sb;
    SbInit(&sb);
    SbPrintf(&sb, "%.*s%.*s", (int)baseLen, base, (int)groupsLen, groups);
    t.name = SbFinish(&sb, arena);
    return t;
}

/* Rebuild a structural TypeName tree from a canonical spelling ("Foo",
   "^Foo", "int[4]", "^Foo[]", "int[2][6]"). Only for boundary code that
   genuinely starts from a name string; the pipeline itself carries trees. */
static inline TypeName TypeNameParse(Arena* arena, const char* spelling)
{
    const char* open = strchr(spelling, '[');

    if (!open)
    {
        return TypeNameParseGroups(arena, spelling, strlen(spelling), NULL, 0);
    }

    size_t baseLen = (size_t)(open - spelling);

    return TypeNameParseGroups(arena, spelling, baseLen, open, strlen(open));
}

typedef enum {
    ModNone,
    ModRef,
} ParamMod;

static inline bool ByRef(ParamMod m)
{
    return m != ModNone;
}

static inline const char* ParamModSpelling(ParamMod m)
{
    switch (m)
    {
    case ModRef:
        return "ref";
    case ModNone:
        return "";
    }
    return "";
}

typedef struct {
    Node base;
    ParamMod mod;
    TypeName type;
    char* name;
    bool isVarargRest;
} ParamDecl;

typedef struct {
    TypeName type;
    char* name;
    long offset; /* explicit byte offset via `fieldoffset(N)`, or -1 */
} FieldDecl;

typedef struct {
    Node base;
    char* name;
    Vec fields;
    bool incomplete;
    bool isExtern; /* `extern struct` — mirrors a host-defined layout */
} StructDecl;

typedef struct {
    Node base;
    char* name;
    char* extendsName;
} HandleDecl;

typedef struct {
    Node base;
    TypeName returnType;
    char* name;
    Vec params;
    Node* body;
    bool isExtern;
    bool hasReturnStmt;
    char* mangledName;
    /* Accepts a variable number of trailing arguments.
       If isCVararg is set, the function is extern and takes bare `...`
       (call-only; the host provides the body). Otherwise the last param
       is a typed rest param (isVarargRest) collecting the extras into a T[]. */
    bool isVariadic;
    bool isCVararg;
} FunctionDecl;

typedef struct {
    Node base;
    char* name;
    Vec structs;
    Vec handles;
    Vec functions;
    Vec globals;
    Vec imports;
} Module;

typedef struct {
    Node base;
    char* importPath;
    SourceRange pathRange;
} ImportDecl;

typedef struct {
    Node base;
    Vec statements;
} Block;

typedef struct {
    Node base;
    Node* value;
} ReturnStmt;

typedef struct {
    Node base;
    Node* condition;
    Node* thenBranch;
    Node* elseBranch;
} IfStmt;

typedef struct {
    Node base;
    Node* condition;
    Node* body;
} WhileStmt;

typedef struct {
    Node base;
    Node* init;
    Node* condition;
    Node* update;
    Node* body;
} ForStmt;

typedef struct {
    Node base;
    TypeName type;
    char* name;
    Node* init;
} VarDeclStmt;

typedef struct {
    Node base;
    Node* expr;
} ExprStmt;

typedef struct {
    Node base;
} BreakStmt;

typedef struct {
    Node base;
} ContinueStmt;

typedef enum {
    UnNeg,
    UnPos,
    UnNot,
    UnBitNot,
} UnaryOp;

typedef struct {
    Node base;
    UnaryOp op;
    Node* operand;
} UnaryExpr;

typedef enum {
    BinAdd,
    BinSub,
    BinMul,
    BinDiv,
    BinMod,
    BinBitAnd,
    BinBitOr,
    BinBitXor,
    BinShl,
    BinShr,
    BinEqEq,
    BinNotEq,
    BinLt,
    BinLtEq,
    BinGt,
    BinGtEq,
    BinLogicAnd,
    BinLogicOr,
} BinaryOp;

typedef struct BinaryExpr {
    Node base;
    BinaryOp op;
    Node* lhs;
    Node* rhs;
} BinaryExpr;

typedef enum {
    AssignSet,
    AssignAdd,
    AssignSub,
    AssignMul,
    AssignDiv,
    AssignMod,
} AssignOp;

typedef struct {
    Node base;
    AssignOp op;
    Node* target;
    Node* value;
} AssignExpr;

typedef struct {
    Node base;
    uint64_t value;
    bool isUnsigned;
} IntLiteral;

typedef struct {
    Node base;
    double value;
} FloatLiteral;

typedef struct {
    Node base;
    bool value;
} BoolLiteral;

typedef struct {
    Node base;
    char* value;
} StrLiteral;

typedef struct {
    Node base;
    char* name;
} IdentExpr;

typedef struct CallExpr {
    Node base;
    char* callee;
    const FunctionDecl* resolvedDecl;
    Vec args;
    bool isPseudoCall;
} CallExpr;

typedef struct MemberExpr {
    Node base;
    Node* base_node;
    char* member;
} MemberExpr;

typedef struct {
    char* name;
    Node* value;
} StructInitField;

typedef struct {
    Node base;
    char* typeName;
    Vec fields;
} StructInitExpr;

typedef struct {
    Node base;
    bool isDec;
    bool isPrefix;
    Node* operand;
} IncDecExpr;

typedef struct {
    Node base;
    TypeName type;
    Node* operand;
} CastExpr;

typedef struct {
    Node base;
    TypeName type;
    char* name;
    Node* init;
} GlobalDecl;

/* arr[index] — indexing into an array (or array-typed member/expr). Usable
   as both an rvalue (element load) and an lvalue (element store). */
typedef struct {
    Node base;
    Node* base_node;
    Node* index;
} IndexExpr;

/* { e0, e1, ... } — array literal. elementType is the parsed element type
   (NULL when untyped — inferred later by sema from the surrounding context);
   elements holds the initializer expressions. */
typedef struct {
    Node base;
    const TypeName* elementType;
    Vec elements;
} ArrayInitExpr;

void AstDispose(Node* node);
void AstReleaseModuleLists(Module* module);
