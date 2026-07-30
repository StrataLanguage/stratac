// Strata compiler: abstract syntax tree.
//
// All AST nodes derive from Node and carry their source range. Composite nodes
// own their children via std::unique_ptr. The parser produces this tree and the
// code generators consume it. Types are stored textually (a name like "int" or
// "float4") so the AST is independent of any particular type system decision;
// the generators map names to concrete representations.
//
// Ownership note: std::unique_ptr<Node> is abbreviated as NodePtr below.
#pragma once

#include "strata/Core/SourceLocation.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace strata
{

enum class NodeKind : std::uint8_t
{
    // Declarations
    Module,
    Struct,
    Handle,
    Function,
    Param,
    // Statements
    Block,
    Return,
    If,
    While,
    For,
    VarDecl,
    ExprStmt,
    Break,
    Continue,
    // Expressions
    IntLiteral,
    FloatLiteral,
    BoolLiteral,
    Ident,
    Unary,
    Binary,
    Assign,
    Call,
    Member,
    StructInit,
};

struct Node
{
    NodeKind kind;
    SourceRange range;

    explicit Node(NodeKind k, SourceRange r = {}) noexcept : kind(k), range(r)
    {
    }

    virtual ~Node() = default;
};

using NodePtr = std::unique_ptr<Node>;

// ---- Types (textual) ----

struct TypeName
{
    std::string name; // e.g. "int", "float4", "MyStruct"
    SourceRange range{};
    bool isConst = false;

    bool Valid() const noexcept
    {
        return !name.empty();
    }
};

enum class ParamMod : std::uint8_t
{
    None,
    In,
    Out,
    InOut
};

inline std::string_view ParamModSpelling(ParamMod m) noexcept
{
    switch (m)
    {
    case ParamMod::In:
        return "in";
    case ParamMod::Out:
        return "out";
    case ParamMod::InOut:
        return "inout";
    case ParamMod::None:
        return "";
    }

    return "";
}

// ---- Declarations ----

struct ParamDecl : Node
{
    ParamMod mod = ParamMod::None;
    TypeName type;
    std::string name;

    ParamDecl(SourceRange r, ParamMod m, TypeName t, std::string n)
        : Node(NodeKind::Param, r), mod(m), type(std::move(t)), name(std::move(n))
    {
    }
};

// A named field of a struct.
struct FieldDecl
{
    TypeName type;
    std::string name;
};

// A user-defined aggregate type.
struct StructDecl : Node
{
    std::string name;
    std::vector<FieldDecl> fields;

    explicit StructDecl(SourceRange r, std::string n) : Node(NodeKind::Struct, r), name(std::move(n))
    {
    }
};

// An opaque, pointer-sized handle whose layout Strata never sees. Engine
// objects (Entity, Texture, ...) are exposed this way; they can be held, passed
// to `extern` functions, and returned, but never have their fields accessed.
struct HandleDecl : Node
{
    std::string name;

    explicit HandleDecl(SourceRange r, std::string n) : Node(NodeKind::Handle, r), name(std::move(n))
    {
    }
};

struct FunctionDecl : Node
{
    TypeName returnType;
    std::string name;
    std::vector<std::unique_ptr<ParamDecl>> params; // owned
    NodePtr body;                                   // a Block, or nullptr for a declaration
    bool isExtern = false;                          // provided by the host runtime
    std::string mangledName;                        // unique IR symbol (set by overload resolution)

    FunctionDecl(SourceRange r, TypeName ret, std::string n)
        : Node(NodeKind::Function, r), returnType(std::move(ret)), name(std::move(n)), mangledName(name)
    {
    }
};

struct Module : Node
{
    std::string name;
    std::vector<std::unique_ptr<StructDecl>> structs;
    std::vector<std::unique_ptr<HandleDecl>> handles;
    std::vector<std::unique_ptr<FunctionDecl>> functions;

    explicit Module(std::string n) : Node(NodeKind::Module), name(std::move(n))
    {
    }
};

// ---- Statements ----

struct Block : Node
{
    std::vector<NodePtr> statements;
    explicit Block(SourceRange r) : Node(NodeKind::Block, r)
    {
    }
};

struct ReturnStmt : Node
{
    NodePtr value; // optional
    explicit ReturnStmt(SourceRange r) : Node(NodeKind::Return, r)
    {
    }
};

struct IfStmt : Node
{
    NodePtr condition;
    NodePtr thenBranch;
    NodePtr elseBranch; // optional
    explicit IfStmt(SourceRange r) : Node(NodeKind::If, r)
    {
    }
};

struct WhileStmt : Node
{
    NodePtr condition;
    NodePtr body;
    explicit WhileStmt(SourceRange r) : Node(NodeKind::While, r)
    {
    }
};

struct ForStmt : Node
{
    NodePtr init;      // a VarDecl, an expression, or null
    NodePtr condition; // expression or null (empty -> always true)
    NodePtr update;    // expression or null
    NodePtr body;
    explicit ForStmt(SourceRange r) : Node(NodeKind::For, r)
    {
    }
};

struct VarDeclStmt : Node
{
    TypeName type;
    std::string name;
    NodePtr init; // optional initializer
    VarDeclStmt(SourceRange r, TypeName t, std::string n)
        : Node(NodeKind::VarDecl, r), type(std::move(t)), name(std::move(n))
    {
    }
};

struct ExprStmt : Node
{
    NodePtr expr;
    explicit ExprStmt(SourceRange r, NodePtr e) : Node(NodeKind::ExprStmt, r), expr(std::move(e))
    {
    }
};

struct BreakStmt : Node
{
    explicit BreakStmt(SourceRange r) : Node(NodeKind::Break, r)
    {
    }
};

struct ContinueStmt : Node
{
    explicit ContinueStmt(SourceRange r) : Node(NodeKind::Continue, r)
    {
    }
};

// ---- Expressions ----

enum class UnaryOp : std::uint8_t
{
    Neg,
    Pos,
    Not,
    BitNot
};

struct UnaryExpr : Node
{
    UnaryOp op;
    NodePtr operand;
    UnaryExpr(SourceRange r, UnaryOp o, NodePtr e) : Node(NodeKind::Unary, r), op(o), operand(std::move(e))
    {
    }
};

enum class BinaryOp : std::uint8_t
{
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    BitAnd,
    BitOr,
    BitXor,
    Shl,
    Shr,
    EqEq,
    NotEq,
    Lt,
    LtEq,
    Gt,
    GtEq,
    LogicAnd,
    LogicOr,
};

struct BinaryExpr : Node
{
    BinaryOp op;
    NodePtr lhs;
    NodePtr rhs;

    BinaryExpr(SourceRange r, BinaryOp o, NodePtr l, NodePtr rhs)
        : Node(NodeKind::Binary, r), op(o), lhs(std::move(l)), rhs(std::move(rhs))
    {
    }
};

enum class AssignOp : std::uint8_t
{
    Assign,
    PlusEq,
    MinusEq,
    StarEq,
    SlashEq,
    PercentEq
};

struct AssignExpr : Node
{
    AssignOp op;
    NodePtr target; // an lvalue (Ident/Member for now)
    NodePtr value;

    AssignExpr(SourceRange r, AssignOp o, NodePtr t, NodePtr v)
        : Node(NodeKind::Assign, r), op(o), target(std::move(t)), value(std::move(v))
    {
    }
};

struct IntLiteral : Node
{
    std::uint64_t value = 0;
    bool isUnsigned = false;

    IntLiteral(SourceRange r, std::uint64_t v, bool u) : Node(NodeKind::IntLiteral, r), value(v), isUnsigned(u)
    {
    }
};

struct FloatLiteral : Node
{
    double value = 0.0;

    explicit FloatLiteral(SourceRange r, double v) : Node(NodeKind::FloatLiteral, r), value(v)
    {
    }
};

struct BoolLiteral : Node
{
    bool value = false;

    BoolLiteral(SourceRange r, bool v) : Node(NodeKind::BoolLiteral, r), value(v)
    {
    }
};

struct IdentExpr : Node
{
    std::string name;

    IdentExpr(SourceRange r, std::string n) : Node(NodeKind::Ident, r), name(std::move(n))
    {
    }
};

struct CallExpr : Node
{
    std::string callee;
    const FunctionDecl* resolvedDecl = nullptr; // set by overload resolution
    std::vector<NodePtr> args;

    CallExpr(SourceRange r, std::string c) : Node(NodeKind::Call, r), callee(std::move(c))
    {
    }
};

struct MemberExpr : Node
{
    NodePtr base;
    std::string member;

    MemberExpr(SourceRange r, NodePtr b, std::string m)
        : Node(NodeKind::Member, r), base(std::move(b)), member(std::move(m))
    {
    }
};

// Braced struct initializer.
// Each entry has an optional field name (empty = positional) and a value expression.
struct StructInitField
{
    std::string name; // empty for positional
    NodePtr value;
};

struct StructInitExpr : Node
{
    std::string typeName;
    std::vector<StructInitField> fields;

    StructInitExpr(SourceRange r, std::string tn) : Node(NodeKind::StructInit, r), typeName(std::move(tn))
    {
    }
};

// Typed downcast helpers (checked in debug).
template <typename T> T* AsNode(Node* n) noexcept
{
    return n ? static_cast<T*>(n) : nullptr;
}

} // namespace strata
