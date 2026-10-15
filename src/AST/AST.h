#pragma once

#include "Core/SourceLocation.h"
#include "Core/Util.h"

#include "../PrimitiveTypes.h"

#include <stdint.h>
#include <string.h>

typedef enum {
    NodeModule,
    NodeImport,
    NodeStruct,
    NodeHandle,
    NodeEnum,
    NodeEnumMember,
    NodeImpl,
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
    NodeNullTest,
    NodeDefer,
    NodeConditional,
} NodeKind;

typedef struct {
    NodeKind kind;
    SourceRange range;
} Node;

#define AST_NEW(arena, type) ((type*)arena_alloc((arena), sizeof(type)))
#define AsNode(type, node) ((type*)(node))

/* Types are structural: `^T` (box), `T?` (optional), and `T[]`/`T[N]` (array)
   wrap an inner/element TypeName. `name` is the canonical spelling ("^Foo",
   "int[4]"), used for display/mangling/map keys — never parse it for shape.
   A dynamic array has length < 0; a fixed `T[N]` (inline storage) >= 1. */
typedef struct TypeName {
    char* name;
    PrimitiveType primitiveType;
    SourceRange range;
    bool isConst;
    bool isVector;
    bool isArray;
    long length;
    char* lengthName; // fixed dim spelled `[constName]` — sema resolves to `length`
    struct TypeName* elem;
    bool isBox;
    struct TypeName* inner;
    bool isOptional; // `T?` — maybe-empty box; `inner` is the wrapped type
    bool isRc;       // `rc<T>` — a box shared by reference count (isBox is set too); `inner` is T
} TypeName;

static inline bool TypeNameValid(const TypeName* t)
{
    return t->name != NULL && t->name[0] != '\0';
}

// `^T` wraps `inner` into a boxed type; the inner node keeps its own const flags.
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

// `rc<T>`: a shared box. Laid out like `^T` (a pointer to the T), with the count in a header before it.
static inline TypeName TypeNameRcWrap(Arena* arena, TypeName inner)
{
    TypeName* i = (TypeName*)arena_alloc(arena, sizeof(TypeName));
    *i = inner;

    TypeName t = {0};
    t.isBox = true;
    t.isRc = true;
    t.inner = i;
    t.name = arena_format(arena, "rc<%s>", i->name);
    t.range = i->range;
    return t;
}

// `rc<T>?`: an rc that may be empty. Shaped like `T?` (inner T, isOptional) and refcounted (isRc).
static inline TypeName TypeNameOptionalRcWrap(Arena* arena, TypeName rcType)
{
    TypeName t = rcType;
    t.isBox = false;
    t.isOptional = true;
    t.isRc = true;
    t.name = arena_format(arena, "%s?", rcType.name);
    return t;
}

// `T?` wraps `inner`; same representation as `^T`, sema tells them apart.
static inline TypeName TypeNameOptionalWrap(Arena* arena, TypeName inner)
{
    TypeName* i = (TypeName*)arena_alloc(arena, sizeof(TypeName));
    *i = inner;
    i->isOptional = false;

    TypeName t = {0};
    t.isOptional = true;
    t.inner = i;
    t.name = arena_format(arena, "%s?", i->name);
    t.isConst = inner.isConst;
    t.range = inner.range;
    return t;
}

// `T[]` — wraps `elem` into a dynamic (fat pointer) array type.
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

// `T[N]` — wraps `elem` into a fixed-size inline array type (C ABI).
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

// Structural type queries — the authority for a type's shape; `name` is derived, never parse it.

static inline bool TypeNameIsArray(const TypeName* t)
{
    return t && t->isArray;
}

// `T[]` — owning fat-pointer array.
static inline bool TypeNameIsDynamicArray(const TypeName* t)
{
    return t && t->isArray && t->length < 0;
}

// `T[N]` — C-ABI inline storage (struct fields and stack-allocated locals).
static inline bool TypeNameIsFixedArray(const TypeName* t)
{
    return t && t->isArray && t->length >= 0;
}

// N for `T[N]`; -1 otherwise (including dynamic arrays).
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

// `T?` — the maybe-empty form of a box.
static inline bool TypeNameIsOptional(const TypeName* t)
{
    return t && t->isOptional;
}

// Inner `T` of a box/optional, or NULL. A box never wraps an array; an optional may (`T[]?`).
static inline const TypeName* TypeNameBoxInner(const TypeName* t)
{
    if (!t || !(t->isBox || t->isOptional))
    {
        return NULL;
    }

        // `T?` always wraps an explicit inner; `^string` is the one box with a NULL inner.
        return t->inner;
}

// Owning types: `string`, `^T`, `T?` and dynamic `T[]`. Fixed `T[N]` is plain inline storage, never owning.
static inline bool TypeNameIsOwning(const TypeName* t)
{
    if (!t || !t->name)
    {
        return false;
    }

    if (t->primitiveType == PrimString)
    {
        return true;
    }

    return t->isBox || t->isOptional || TypeNameIsDynamicArray(t);
}


// A leaf TypeName for a spelling without structure ("int", "Foo").
static inline TypeName TypeNameLeaf(char* name)
{
    TypeName t = {0};

    t.name = name;
    t.primitiveType = GetPrimitiveType(name);

    return t;
}

static inline TypeName TypeNameParseGroups(Arena* arena, const char* base, size_t baseLen, const char* groups,
                                            size_t groupsLen)
{
    /* A trailing '?' AFTER bracket groups wraps the whole type: `int[]?`
       is an optional array (distinct from `int?[]`, an array of optionals).
       Boxes still never wrap arrays, but an optional may. */
    if (groupsLen > 0 && groups[groupsLen - 1] == '?')
    {
        TypeName inner = TypeNameParseGroups(arena, base, baseLen, groups, groupsLen - 1);

        if (inner.isRc && !inner.isOptional)
        {
            return TypeNameOptionalRcWrap(arena, inner);
        }

        TypeName* i = (TypeName*)arena_alloc(arena, sizeof(TypeName));
        *i = inner;

        TypeName t = {0};
        t.isOptional = true;
        t.inner = i;
        t.name = arena_format(arena, "%s?", i->name);
        return t;
    }

    if (groupsLen == 0)
    {
        // Trailing '?' marks an optional (`Weapon?`).
        if (baseLen >= 2 && base[baseLen - 1] == '?')
        {
            TypeName inner = TypeNameParseGroups(arena, base, baseLen - 1, NULL, 0);

            if (inner.isRc && !inner.isOptional)
            {
                return TypeNameOptionalRcWrap(arena, inner);
            }

            TypeName* i = (TypeName*)arena_alloc(arena, sizeof(TypeName));
            *i = inner;

            TypeName t = {0};
            t.isOptional = true;
            t.inner = i;
            t.name = arena_strndup(arena, base, baseLen);
            return t;
        }

        if (baseLen > 4 && strncmp(base, "rc<", 3) == 0 && base[baseLen - 1] == '>')
        {
            TypeName inner = TypeNameParseGroups(arena, base + 3, baseLen - 4, NULL, 0);
            TypeName t = TypeNameRcWrap(arena, inner);
            t.name = arena_strndup(arena, base, baseLen);
            return t;
        }

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

    // First bracket group is the outermost dim; wrap inner groups first.
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
            v = (v * 10) + (groups[i] - '0');
        }

        t.length = v;
    }

    // Canonical name: base + every group, in source order.
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
    bool isReturn; /* `return T x` — last param of an extern fn: the callee writes `x` as its
                      out-param; the Strata-level return type becomes T. */
    bool growsArray; /* `ref T[]` the body grows, shrinks or rebinds (sema): callers must pass a real
                        heap array, never a stack view (fixed array, array literal, varargs rest) */
    /* A by-value `string` / `T[]` / owning-struct param the body only reads: sema passes it as
       `const ref` instead (callers keep their value; nothing is copied or moved). */
    bool inferredBorrow;
    bool borrowCandidate; /* working state of that inference */
} ParamDecl;

/* A field's `= expr` default, folded by sema. Scalars use lane 0; float2/3/4 use one lane each. */
typedef struct {
    bool isSet;
    bool isFloat;          // lanes hold floating-point values (float/double fields and vectors)
    unsigned laneCount;
    long long intValue;    // integer, bool and enum fields
    double floatLanes[4];
} FieldDefaultValue;

typedef struct {
    TypeName type;
    char* name;
    long offset; // explicit byte offset via `fieldoffset(N)`, or -1
    SourceRange nameRange;
    Node* defaultValue; // `T name = expr;`, NULL when the field has no default
    FieldDefaultValue folded;
} FieldDecl;

// `@name` before a declaration.
typedef struct {
    char* name;
    SourceRange range;
} Attribute;

typedef struct {
    Node base;
    char* name;
    Vec fields;
    bool incomplete;
    bool isExtern; // `extern struct` — mirrors a host-defined layout
    bool isUnion;  // `union`: every field overlays byte 0 (C union layout)
    bool isTypeAlias;       // `struct Foo = uint;` — strongly typed alias
    char* underlyingType;   // underlying type name for type aliases (NULL for normal structs)
    Vec attributes;         // Vec<Attribute*>
    bool isComponent;       // `@component`: a plain-data type the host registers (see strata_types.h)
    const char* moduleName; // module (file) the struct was declared in
    /* The `void Drop(ref T self)` of the struct's impl, bound by sema: runs when an owned value is dropped.
       Such a struct carries a hidden `__alive` byte (STRATA_ALIVE_FIELD) so a moved-from value is skipped. */
    const struct FunctionDecl* dropFunction;
} StructDecl;

typedef struct {
    Node base;
    char* name;
    char* extendsName;
} HandleDecl;

typedef struct {
    Node base;
    char* name;
    // Explicit `A = expr` value; NULL for sequential members. Sema folds it.
    Node* valueExpr;
    uint64_t value;
    bool isNegative;
    bool hasExplicitValue;
} EnumMemberDecl;

typedef struct {
    Node base;
    char* name;
    // Underlying integral type (NULL = `int`).
    char* underlyingType;
    Vec members; // Vec<EnumMemberDecl*>
} EnumDecl;

typedef struct FunctionDecl {
    Node base;
    TypeName returnType;
    char* name;
    Vec params;
    Node* body;
    bool isExtern;
    bool isPrivate;
    /* `@export`: a defined function with the plain C ABI — no hidden context parameter and never
       symbol-prefixed — so C code can call it by name or through `addressof(fn)` (callbacks). Its
       body reaches the module's globals through the context created most recently. */
    bool isExport;
    /* `@noreturn`: never returns to its caller (exits, aborts, panics). A call to it as a statement
       ends the path for flow typing, like `return`; its body must not reach its end or `return`. */
    bool isNoReturn;
    /* `extern delegate R Name(params);`: not a function but a C function-pointer type `Name` with this
       signature. Kept in Module::delegates; `mangledName` is the internal `__delegate_Name`. */
    bool isDelegate;
    bool hasReturnStmt;
    char* mangledName;
    // Variadic tails: bare `...` is extern-only; else the last param collects into T[].
    bool isVariadic;
    bool isCVararg;
    // `return T x` last param (extern only): Strata-level return is T, C ABI is void + out-pointer.
    bool hasReturnParam;
    // Impl members: `methodName` is the short name, `name` carries the `H_M` symbol.
    char* methodName;
    bool fromImpl;
    // `extern<T>`: the type parameter's name, NULL for ordinary functions.
    char* typeParam;
    // A per-type instantiation of an `extern<T>` (sema): `typeArg` replaced T, the host symbol is
    // still `name`, and calls pass T's StrataTypeDesc as a hidden trailing argument.
    const struct FunctionDecl* genericOf;
    const char* typeArg;
} FunctionDecl;

static inline const ParamDecl* FunctionReturnParam(const FunctionDecl* f)
{
    if (!f || !f->hasReturnParam || f->params.count == 0)
    {
        return NULL;
    }

    const ParamDecl* last = (const ParamDecl*)VecGet(&f->params, f->params.count - 1);

    return (last && last->isReturn) ? last : NULL;
}

static inline bool FunctionHasReturnParam(const FunctionDecl* f)
{
    return FunctionReturnParam(f) != NULL;
}

// One property in an `impl` block. Either side may be absent.
typedef struct {
    TypeName returnType;
    char* name;
    char* getterSymbol; // NULL = write-only
    char* setterSymbol; // NULL = read-only
    SourceRange range;
} PropertyDecl;

// `impl H { ... }`: methods hoist as `H_M` externs sharing module pointers.
typedef struct {
    Node base;
    char* handleName;
    Vec methods;    // Vec<FunctionDecl*> (also in Module::functions)
    Vec properties; // Vec<PropertyDecl*>
} ImplDecl;

typedef struct {
    Node base;
    char* name;
    Vec structs;
    Vec handles;
    Vec enums;
    Vec functions;
    Vec globals;
    Vec imports;
    Vec impls;
    Vec genericInstances; // FunctionDecl*: per-type `extern<T>` instantiations, created by sema
    Vec delegates;        // FunctionDecl* (isDelegate): `extern delegate` function-pointer types
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
    /* `ref T x = path;` / `const ref T x = path;`: an alias of the storage at `path`, not a copy. */
    bool isRef;
    /* Set by sema: the path holds a `^T` / `rc<T>` / proven `T?` and the ref aliases the value inside. */
    bool refThroughBox;
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
    bool isImplPropertyWrite; /* set by sema: target is an impl property (no assigned-value semantics) */
} AssignExpr;

typedef struct {
    Node base;
    uint64_t value;
    bool isUnsigned;
    bool isLong; /* `l` / `ul` suffix: always 64-bit */
} IntLiteral;

typedef struct {
    Node base;
    double value;
} FloatLiteral;

typedef struct {
    Node base;
    bool value;
} BoolLiteral;

/* `value` is NUL-terminated for convenience, but `\0` escapes may embed
   NULs, so `length` (the decoded byte count) is authoritative. */
typedef struct {
    Node base;
    char* value;
    size_t length;
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
    bool isIntrinsicCall;
    TypeName* typeArg; /* explicit `Name<T>(...)` type argument (generic externs), NULL otherwise */
    /* `expr.Member(args)` — non-NULL until sema resolves the member against
       impl blocks (then the base moves into `args` as the self argument). */
    Node* calleeBase;
    /* `addressof(fn)`: the extern or `@export` function whose address is taken (set by sema). */
    const struct FunctionDecl* addressOfFunction;
    /* A call through a delegate value (sema): the expression yielding the function pointer (a
       variable, or a struct field when written `obj.field(args)`) and the delegate's signature. */
    Node* delegateCallee;
    const struct FunctionDecl* delegateDecl;
} CallExpr;

/* A statement that is a call to a `@noreturn` function: control never continues past it. */
static inline bool StmtIsNoReturnCall(const Node* n)
{
    if (!n || n->kind != NodeExprStmt)
    {
        return false;
    }
    const Node* e = ((const ExprStmt*)n)->expr;
    return e && e->kind == NodeCall && ((const CallExpr*)e)->resolvedDecl
           && ((const CallExpr*)e)->resolvedDecl->isNoReturn;
}

typedef struct MemberExpr {
    Node base;
    Node* base_node;
    char* member;
    bool isImplProperty; /* set by sema: member names an impl property (call-like read, never a move source) */
    bool isEnumConst;    /* set by sema: `EnumName.Member` scoped constant read */
    uint64_t enumValue;  /* resolved bit pattern of the enum member (valid when isEnumConst) */
    const char* enumTypeName; /* the enum's name (valid when isEnumConst) */
    bool isScalarConst;  /* set by sema: `int.max` / `float.min` builtin scalar type pseudo-property read */
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

/* Casts that view or copy text (set by sema): a `byte[N]` / `sbyte[N]` buffer as a borrowed
   `cstring` or a copied `string`, and a `string` as a borrowed `cstring`. */
typedef enum {
    CastTextNone = 0,
    CastBufferToCString,
    CastBufferToString,
    CastStringToCString,
    CastFixedToPtr, /* any fixed array as a `ptr` to its first element (C's array decay) */
} CastTextKind;

typedef struct {
    Node base;
    TypeName type;
    Node* operand;
    CastTextKind textKind;
} CastExpr;

typedef struct {
    Node base;
    TypeName type;
    char* name;
    Node* init;
} GlobalDecl;

// arr[index] — array indexing; usable as both rvalue (load) and lvalue (store).
typedef struct {
    Node base;
    Node* base_node;
    Node* index;
} IndexExpr;

// { e0, ... } array literal; elementType NULL until sema infers it.
typedef struct {
    Node base;
    const TypeName* elementType;
    Vec elements;
    /* `{ value; count }`: `count` copies of the one value in `elements`; NULL for a plain list. */
    Node* repeatCount;
} ArrayInitExpr;

// `expr?` — null test on a `T?` path; also proves non-empty in sema.
typedef struct {
    Node base;
    Node* operand;
} NullTestExpr;

// `defer <stmt>` — schedules `stmt` to run at the end of the enclosing block
// (Zig-style), in LIFO order. The deferred statement is analyzed at its textual
// position but executed at scope exit (including on `return`/`break`/`continue`).
typedef struct {
    Node base;
    Node* stmt;
} DeferStmt;

// `cond ? a : b`. Branches are non-owning values; `resultType` is set by sema.
typedef struct {
    Node base;
    Node* condition;
    Node* thenExpr;
    Node* elseExpr;
    const TypeName* resultType;
} ConditionalExpr;

void AstDispose(Node* node);
void AstReleaseModuleLists(Module* module);
