// Declarations: structs, enums, handles, impls, functions and globals.

#include "Parse/ParserInternal.h"

#include "Codegen/TypeRegistry.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

HandleDecl* ParseHandleDecl(Parser* p)
{
    if (!AtHandleDecl(p))
    {
        DiagError(p->diag, p->cur.range, "expected 'handle' and a handle name");
        return NULL;
    }

    Advance(p);

    Token nameTok = p->cur;
    CheckTypeNameNotHandle(p, nameTok);
    Advance(p);

    HandleDecl* node = AST_NEW(p->arena, HandleDecl);
    node->base.kind = NodeHandle;
    node->base.range = nameTok.range;
    node->name = ToOwned(p->arena, ParserIdentText(p, nameTok));
    node->extendsName = NULL;

    if (ParserConsume(p, TokColon))
    {
        if (p->cur.kind != TokIdent)
        {
            DiagError(p->diag, p->cur.range, "expected a base handle name after ':'");
        }
        else
        {
            Token baseTok = p->cur;
            Advance(p);
            node->extendsName = ToOwned(p->arena, ParserIdentText(p, baseTok));
        }
    }
    else if (p->cur.kind == TokIdent && StrEqC(ParserIdentText(p, p->cur), "extends"))
    {
        DiagErrorFmt(p->diag, p->cur.range, "'extends' was removed; write 'handle %s : Base'", node->name);
        Advance(p);

        if (p->cur.kind == TokIdent)
        {
            Advance(p);
        }
    }

    if (ParserConsume(p, TokLBrace))
    {
        DiagError(p->diag, nameTok.range, "opaque handles cannot have a body");
        Synchronize(p);
    }

    ParserExpect(p, TokSemicolon, "';'");

    return node;
}

static bool TypeParamListHas(const Vec* params, const char* name)
{
    for (size_t i = 0; i < params->count; i++)
    {
        if (strcmp((const char*)VecGet(params, i), name) == 0)
        {
            return true;
        }
    }

    return false;
}

// `<T, U, ...>` after a declaration's name, the current token being the `<`. Appends the names to
// `outParams`, the caller's Vec. Duplicates and builtin type names are reported and dropped.
static void ParseTypeParamList(Parser* p, Vec* outParams)
{
    Advance(p);

    while (true)
    {
        Token nameTok = p->cur;
        char* name = ToOwned(p->arena, ParserIdentText(p, nameTok));
        bool isBuiltin = GetPrimitiveType(name) != PrimNone;

        // Builtin type names lex as keywords: they are named in the message, not reported as a bad token.
        if (nameTok.kind != TokIdent && !isBuiltin)
        {
            DiagError(p->diag, nameTok.range, "expected a type parameter name");
            break;
        }

        Advance(p);

        if (isBuiltin)
        {
            DiagErrorFmt(p->diag, nameTok.range, "'%s' is a builtin type and cannot be a type parameter", name);
        }
        else if (TypeParamListHas(outParams, name))
        {
            DiagErrorFmt(p->diag, nameTok.range, "duplicate type parameter '%s'", name);
        }
        else
        {
            VecPush(outParams, name);
        }

        if (!ParserConsume(p, TokComma))
        {
            break;
        }
    }

    ParserExpect(p, TokGt, "'>'");
}

// `struct Name = type;`: a type alias. It has no fields of its own.
static StructDecl* ParseStructAlias(Parser* p, Token nameTok)
{
    TypeName underlying = {0};
    if (!ParserTryParseType(p, &underlying) || !underlying.name)
    {
        DiagError(p->diag, p->cur.range, "expected a type after '='");
        Synchronize(p);
        return NULL;
    }

    ParserExpect(p, TokSemicolon, "';'");

    StructDecl* node = AST_NEW(p->arena, StructDecl);
    node->base.kind = NodeStruct;
    node->base.range = nameTok.range;
    node->name = ToOwned(p->arena, ParserIdentText(p, nameTok));
    VecInit(&node->fields);
    node->isExtern = false;
    node->isTypeAlias = true;
    node->underlyingType = underlying.name;

    return node;
}

// `fieldoffset(N)` before a field. `*offset` is -1 when there is none. Returns false when the
// fieldoffset is malformed, and the caller stops reading fields.
static bool ParseFieldOffsetPrefix(Parser* p, bool isExtern, bool isUnion, long* offset)
{
    *offset = -1;

    if (p->cur.kind != TokKwFieldoffset)
    {
        return true;
    }

    Token foTok = p->cur;
    Advance(p);

    if (!ParserConsume(p, TokLParen))
    {
        DiagError(p->diag, p->cur.range, "expected '(' after 'fieldoffset'");
        return false;
    }

    if (p->cur.kind != TokIntLit)
    {
        DiagError(p->diag, p->cur.range, "expected a byte offset inside 'fieldoffset(...)'");
        return false;
    }

    Token lenTok = p->cur;
    Advance(p);

    if (!ParserConsume(p, TokRParen))
    {
        DiagError(p->diag, p->cur.range, "expected ')' to close 'fieldoffset(...)'");
        return false;
    }

    if (!isExtern || isUnion)
    {
        DiagError(p->diag, foTok.range, "'fieldoffset' is only allowed inside an 'extern struct'");
    }

    uint64_t value = IntLiteralValue(p, lenTok, NULL);

    if (value > (uint64_t)LONG_MAX)
    {
        DiagError(p->diag, lenTok.range, "field offset is too large");
        value = 0;
    }

    *offset = (long)value;

    return true;
}

// One `Type name [= default];` field, appended to the struct. Returns false when reading fields
// should stop.
static bool ParseStructField(Parser* p, StructDecl* node, long offset, bool isUnion)
{
    TypeName ft = {0};
    if (!ParserTryParseType(p, &ft))
    {
        DiagError(p->diag, p->cur.range, "expected a field type");
        return false;
    }

    if (p->cur.kind != TokIdent)
    {
        DiagError(p->diag, p->cur.range, "expected a field name");
        return false;
    }

    Token fieldTok = p->cur;
    Advance(p);

    FieldDecl* field = AST_NEW(p->arena, FieldDecl);
    field->type = ft;
    field->name = ToOwned(p->arena, ParserIdentText(p, fieldTok));
    field->offset = offset;
    field->nameRange = fieldTok.range;
    VecPush(&node->fields, field);

    // `T name = expr;` — a default value, folded to a constant by sema.
    if (ParserConsume(p, TokAssign))
    {
        field->defaultValue = ParseExpr(p);

        if (isUnion)
        {
            DiagError(p->diag, fieldTok.range, "union fields cannot have default values");
        }
    }

    Token semi = ParserExpect(p, TokSemicolon, "';'");

    return semi.kind == TokSemicolon;
}

// The fields between a struct's braces. The closing '}' is left to the caller.
static void ParseStructFields(Parser* p, StructDecl* node, bool isExtern, bool isUnion)
{
    while (p->cur.kind != TokRBrace && p->cur.kind != TokEof)
    {
        long offset;

        if (!ParseFieldOffsetPrefix(p, isExtern, isUnion, &offset))
        {
            break;
        }

        if (!ParseStructField(p, node, offset, isUnion))
        {
            break;
        }
    }
}

StructDecl* ParseStructDecl(Parser* p, bool isExtern)
{
    bool isUnion = p->cur.kind == TokKwUnion;

    if (!ParserConsume(p, TokKwStruct) && !ParserConsume(p, TokKwUnion))
    {
        DiagError(p->diag, p->cur.range, "expected 'struct'");
        return NULL;
    }

    if (p->cur.kind != TokIdent)
    {
        DiagError(p->diag, p->cur.range, "expected a struct name");
        return NULL;
    }

    Token nameTok = p->cur;
    CheckTypeNameNotHandle(p, nameTok);
    Advance(p);

    Vec genericParams;
    VecInit(&genericParams);

    if (p->cur.kind == TokLt)
    {
        ParseTypeParamList(p, &genericParams);
    }

    if (ParserConsume(p, TokAssign))
    {
        if (genericParams.count > 0)
        {
            DiagError(p->diag, nameTok.range, "a struct alias cannot have type parameters");
        }

        free(genericParams.items);

        return ParseStructAlias(p, nameTok);
    }

    StructDecl* node = AST_NEW(p->arena, StructDecl);
    node->base.kind = NodeStruct;
    node->base.range = nameTok.range;
    node->name = ToOwned(p->arena, ParserIdentText(p, nameTok));
    VecInit(&node->fields);
    node->isExtern = isExtern;
    node->isUnion = isUnion;
    node->moduleName = p->moduleName;
    node->genericParams = genericParams;

    if (isExtern && genericParams.count > 0)
    {
        DiagError(p->diag, nameTok.range, "an extern struct cannot have type parameters");
    }

    if (ParserConsume(p, TokLBrace))
    {
        ParseStructFields(p, node, isExtern, isUnion);

        ParserExpect(p, TokRBrace, "'}'");
    }
    else
    {
        if (isExtern)
        {
            DiagError(p->diag, nameTok.range, "extern struct must have a body");
        }

        if (genericParams.count > 0)
        {
            DiagError(p->diag, nameTok.range, "a forward declaration cannot have type parameters");
        }

        node->incomplete = true;
    }

    if (node->incomplete)
    {
        // Forward declaration: `struct Foo;` still needs the ';'.
        ParserExpect(p, TokSemicolon, "';'");
    }
    else
    {
        // The ';' after a struct body is optional (`struct S { ... }`).
        ParserConsume(p, TokSemicolon);
    }

    return node;
}

// Parses the constant expression for an explicit enum member value (`A = expr`).
// Expression parsing stops before the `,`/`}` that ends the member; sema folds
// the result.
static Node* ParseEnumMemberValue(Parser* p)
{
    return ParseBinary(p, 0);
}

EnumDecl* ParseEnumDecl(Parser* p)
{
    Token kw = p->cur;
    Advance(p);

    if (p->cur.kind != TokIdent)
    {
        DiagErrorFmt(p->diag, p->cur.range, "expected an enum name after 'enum'");
        Synchronize(p);
        return NULL;
    }

    Token nameTok = p->cur;
    CheckTypeNameNotHandle(p, nameTok);
    Advance(p);

    EnumDecl* node = AST_NEW(p->arena, EnumDecl);
    node->base.kind = NodeEnum;
    node->base.range = SpanFrom(kw, nameTok);
    node->name = ToOwned(p->arena, ParserIdentText(p, nameTok));
    node->underlyingType = NULL;
    node->isFlags = false;
    VecInit(&node->members);

    // `: UnderlyingType` — optional; defaults to `int`.
    if (ParserConsume(p, TokColon))
    {
        TypeName underlying = {0};

        if (!ParserTryParseType(p, &underlying) || !underlying.name)
        {
            DiagErrorFmt(p->diag, p->cur.range, "expected an underlying type after ':'");
            Synchronize(p);
            return NULL;
        }

        node->underlyingType = underlying.name;
    }

    ParserExpect(p, TokLBrace, "'{'");

    while (p->cur.kind != TokRBrace && p->cur.kind != TokEof)
    {
        if (p->cur.kind != TokIdent)
        {
            DiagErrorFmt(p->diag, p->cur.range, "expected an enum member name");
            break;
        }

        Token memberTok = p->cur;
        Advance(p);

        EnumMemberDecl* member = AST_NEW(p->arena, EnumMemberDecl);
        member->base.kind = NodeEnumMember;
        member->base.range = memberTok.range;
        member->name = ToOwned(p->arena, ParserIdentText(p, memberTok));
        member->hasExplicitValue = false;
        member->isNegative = false;
        member->value = 0;

        if (ParserConsume(p, TokAssign))
        {
            member->valueExpr = ParseEnumMemberValue(p);

            if (!member->valueExpr)
            {
                break;
            }

            member->hasExplicitValue = true;
        }

        VecPush(&node->members, member);

        if (ParserConsume(p, TokComma))
        {
            continue;
        }

        break;
    }

    Token closeBrace = ParserExpect(p, TokRBrace, "'}'");
    node->base.range = SpanFrom(kw, closeBrace.kind == TokRBrace ? closeBrace : p->cur);

    // The ';' after an enum body is optional (`enum Color { Red }`).
    ParserConsume(p, TokSemicolon);

    return node;
}

static ParamDecl* ParseParam(Parser* p)
{
    SourceRange start = p->cur.range;
    ParamMod mod = ModNone;
    bool isConst = false;
    bool isReturn = false;

    // 'const', 'ref' and 'return' may appear together in either order, each at most once.
    for (int i = 0; i < 3; i++)
    {
        if (ParserConsume(p, TokKwRef))
        {
            mod = ModRef;
            continue;
        }

        if (ParserConsume(p, TokKwConst))
        {
            isConst = true;
            continue;
        }

        if (ParserConsume(p, TokKwReturn))
        {
            mod = ModRef;
            isReturn = true;
            continue;
        }

        break;
    }

    TypeName type = {0};

    if (!ParserTryParseType(p, &type))
    {
        DiagError(p->diag, p->cur.range, "expected a parameter type");
        return NULL;
    }

    bool isVarargRest = false;

    if (ParserConsume(p, TokDotDotDot))
    {
        isVarargRest = true;

        // `ref`/`const` rest params borrow the stack array.
        type = TypeNameArrayWrap(p->arena, type);
    }

    if (p->cur.kind != TokIdent)
    {
        DiagError(p->diag, p->cur.range, "expected a parameter name");
        return NULL;
    }

    Token nameTok = p->cur;
    Advance(p);

    ParamDecl* node = AST_NEW(p->arena, ParamDecl);
    node->base.kind = NodeParam;
    node->base.range
        = (SourceRange){start.start, (uint32_t)(SourceRangeEnd(nameTok.range) - start.start), start.fileId};
    node->mod = mod;
    type.isConst = isConst || type.isConst;
    node->type = type;
    node->name = ToOwned(p->arena, ParserIdentText(p, nameTok));
    node->isVarargRest = isVarargRest;
    node->isReturn = isReturn;

    return node;
}

// Collects a run of `@name` attributes; whatever they precede decides which are valid.
void ParseAttributes(Parser* p, Vec* outAttributes)
{
    while (p->cur.kind == TokAt)
    {
        Token atTok = p->cur;
        Advance(p);

        if (p->cur.kind != TokIdent)
        {
            DiagError(p->diag, p->cur.range, "expected an attribute name after '@'");
            continue;
        }

        Attribute* attribute = AST_NEW(p->arena, Attribute);
        attribute->name = ToOwned(p->arena, ParserIdentText(p, p->cur));
        attribute->range = SpanFrom(atTok, p->cur);
        VecPush(outAttributes, attribute);

        Advance(p);
    }
}

// Function attributes: @private, @export, @noreturn.
static bool ApplyFunctionAttributes(Parser* p, const Vec* attributes, bool* outIsPrivate, bool* outIsExport,
                                    bool* outIsNoReturn)
{
    bool isPrivate = false;
    bool isExport = false;
    bool isNoReturn = false;

    for (size_t i = 0; i < attributes->count; i++)
    {
        const Attribute* attribute = (const Attribute*)VecGet(attributes, i);

        if (strcmp(attribute->name, "private") == 0)
        {
            isPrivate = true;
        }
        else if (strcmp(attribute->name, "export") == 0)
        {
            isExport = true;
        }
        else if (strcmp(attribute->name, "noreturn") == 0)
        {
            isNoReturn = true;
        }
        else
        {
            DiagErrorFmt(p->diag, attribute->range, "unknown attribute '@%s'", attribute->name);
        }
    }

    *outIsPrivate = isPrivate;
    *outIsExport = isExport;
    *outIsNoReturn = isNoReturn;

    return attributes->count > 0;
}

// Struct attributes, e.g. @component. The struct takes ownership of the list.
void ApplyStructAttributes(Parser* p, StructDecl* sd, Vec* attributes)
{
    for (size_t i = 0; i < attributes->count; i++)
    {
        const Attribute* attribute = (const Attribute*)VecGet(attributes, i);

        if (strcmp(attribute->name, "component") != 0)
        {
            DiagErrorFmt(p->diag, attribute->range, "unknown struct attribute '@%s'", attribute->name);
            continue;
        }

        if (sd->isTypeAlias || sd->incomplete || sd->isExtern)
        {
            DiagErrorFmt(p->diag, attribute->range,
                         "'@component' requires a struct definition with a body (not an alias, forward "
                         "declaration or extern struct)");
            continue;
        }

        if (sd->genericParams.count > 0)
        {
            DiagErrorFmt(p->diag, attribute->range, "'@component' cannot have type parameters");
            continue;
        }

        sd->isComponent = true;
    }

    sd->attributes = *attributes;
    VecInit(attributes);
}

Node* ParseFunction(Parser* p)
{
    Vec attributes;
    VecInit(&attributes);
    ParseAttributes(p, &attributes);

    Node* node = ParseFunctionWithAttributes(p, &attributes);

    free(attributes.items);

    return node;
}

// `extern`, `extern delegate` and `extern<T>` before a function or global declaration.
static void ParseExternPrefix(Parser* p, bool* isExtern, bool* isDelegate, char** typeParam)
{
    *isExtern = ParserConsume(p, TokKwExtern);

    // `extern delegate R Name(params);` — a C function-pointer type.
    *isDelegate = *isExtern && ParserConsume(p, TokKwDelegate);

    // `extern<T>`: one host function serving every type T (see strata.h).
    *typeParam = NULL;

    if (*isExtern && p->cur.kind == TokLt)
    {
        Advance(p);

        if (p->cur.kind == TokIdent)
        {
            *typeParam = ToOwned(p->arena, ParserIdentText(p, p->cur));
            Advance(p);
        }
        else
        {
            DiagError(p->diag, p->cur.range, "expected a type parameter name after 'extern<'");
        }

        ParserExpect(p, TokGt, "'>'");
    }
}

// Attributes decorate functions and struct definitions only. Returns true when they were put on
// something else, in which case the caller drops the declaration.
static bool RejectAttributesOnNonFunction(Parser* p, bool hasAttrs, SourceRange firstAttributeRange)
{
    if (hasAttrs
        && (p->cur.kind == TokKwStruct || AtHandleDecl(p) || p->cur.kind == TokKwEnum || p->cur.kind == TokKwImport
            || p->cur.kind == TokKwImpl))
    {
        DiagError(p->diag, firstAttributeRange, "attributes are only allowed on function and struct definitions");
        return true;
    }

    return false;
}

// The `ReturnType name` that starts every function and global declaration. Returns false after
// reporting the error, and the caller gives up on the declaration.
static bool ParseSignatureHead(Parser* p, TypeName* returnType, Token* nameTok)
{
    if (!ParserTryParseType(p, returnType))
    {
        if (p->cur.kind != TokEof)
        {
            DiagError(p->diag, p->cur.range, "expected a type to declare a function");
        }

        return false;
    }

    if (p->cur.kind != TokIdent)
    {
        DiagError(p->diag, p->cur.range, "expected a name");

        return false;
    }

    *nameTok = p->cur;
    Advance(p);

    return true;
}

// `Type name [= init];` at file scope, reached when no '(' follows the name.
static Node* ParseGlobalDecl(Parser* p, TypeName returnType, Token nameTok, bool isExtern, bool hasAttrs,
                             SourceRange firstAttributeRange)
{
    if (isExtern)
    {
        DiagError(p->diag, nameTok.range, "extern cannot be used with a global variable");
        Synchronize(p);

        return NULL;
    }

    GlobalDecl* gd = AST_NEW(p->arena, GlobalDecl);
    gd->base.kind = NodeGlobal;
    gd->base.range = nameTok.range;
    gd->type = returnType;
    gd->name = ToOwned(p->arena, ParserIdentText(p, nameTok));
    gd->init = NULL;

    if (hasAttrs)
    {
        DiagError(p->diag, firstAttributeRange, "attributes are only allowed on function and struct definitions");
    }

    if (ParserConsume(p, TokAssign))
    {
        if (p->cur.kind == TokLBrace && returnType.isArray)
        {
            gd->init = ParseArrayInitBody(p, p->cur, returnType.elem);
        }
        else if (p->cur.kind == TokLBrace && returnType.isOptional && returnType.inner && returnType.inner->isArray)
        {
            // `T[]? g = { ... };` - a braced literal initializes the inner dynamic array.
            gd->init = ParseArrayInitBody(p, p->cur, returnType.inner->elem);
        }
        else
        {
            gd->init = ParseExpr(p);
        }
    }

    ParserExpect(p, TokSemicolon, "';'");

    return (Node*)gd;
}

// The FunctionDecl for a signature that has its name, with the flags and range it carries before
// the parameter list is read.
static FunctionDecl* NewFunctionDecl(Parser* p, TypeName returnType, Token nameTok, bool isExtern, bool isDelegate,
                                     bool isPrivate, bool isExport, bool isNoReturn, char* typeParam, bool hasAttrs,
                                     SourceRange firstAttributeRange)
{
    FunctionDecl* node = AST_NEW(p->arena, FunctionDecl);
    node->base.kind = NodeFunction;
    node->base.range = SpanFrom((Token){TokIdent, returnType.range}, nameTok);
    node->returnType = returnType;
    node->name = ToOwned(p->arena, ParserIdentText(p, nameTok));
    node->mangledName = ArenaStrdup(p->arena, node->name);
    node->isExtern = isExtern;
    node->isPrivate = isPrivate;
    node->isExport = isExport;
    node->isNoReturn = isNoReturn;
    node->isDelegate = isDelegate;
    node->hasReturnStmt = false;

    if (isDelegate)
    {
        node->mangledName = ArenaFormat(p->arena, "__delegate_%s", node->name);

        if (hasAttrs || typeParam)
        {
            DiagError(p->diag, node->base.range, "a delegate takes no attributes or type parameter");
        }
    }
    node->typeParam = typeParam;
    VecInit(&node->params);

    if (hasAttrs)
    {
        node->base.range = SpanFrom((Token){TokAt, firstAttributeRange}, nameTok);
    }

    return node;
}

// The parameter list after '(' through ')': the bare `...` forms, the rest and `return`
// parameters, and the checks that depend on them.
static void ParseFunctionParams(Parser* p, FunctionDecl* node, bool isExtern)
{
    if (p->cur.kind == TokDotDotDot)
    {
        DiagError(p->diag, p->cur.range, "bare '...' requires at least one named parameter");
    }

    if (p->cur.kind != TokRParen)
    {
        while (true)
        {
            ParamDecl* param = ParseParam(p);

            if (param)
            {
                VecPush(&node->params, param);
            }
            else
            {
                break;
            }

            if (ParserConsume(p, TokComma))
            {
                // Bare '...' (extern C-style varargs): must end the list.
                if (p->cur.kind == TokDotDotDot)
                {
                    Advance(p);
                    node->isVariadic = true;
                    node->isCVararg = true;
                    break;
                }

                continue;
            }

            break;
        }
    }

    for (size_t i = 0; i < node->params.count; i++)
    {
        ParamDecl* param = (ParamDecl*)VecGet(&node->params, i);

        if (param->isVarargRest)
        {
            node->isVariadic = true;

            if (i + 1 != node->params.count)
            {
                DiagError(p->diag, param->base.range, "rest parameter must be the last parameter");
            }
        }

        if (param->isReturn)
        {
            if (!isExtern)
            {
                DiagError(p->diag, param->base.range, "'return' parameter is only allowed on extern functions");
                continue;
            }

            if (i + 1 != node->params.count)
            {
                DiagError(p->diag, param->base.range, "'return' parameter must be the last parameter");
                continue;
            }

            if (node->returnType.primitiveType != PrimVoid)
            {
                DiagErrorFmt(p->diag, node->base.range,
                             "extern function with a 'return' parameter must declare 'void' return; found '%s'",
                             node->returnType.name);
                continue;
            }

            // The Strata-level signature returns the `return` param's type;
            // at the ABI the C function is void ret + out-pointer (codegen).
            node->hasReturnParam = true;
            node->returnType = param->type;
            node->returnType.isConst = false;
        }
    }

    if (node->isCVararg && !isExtern)
    {
        DiagError(p->diag, node->base.range, "bare '...' is only allowed on extern functions");
    }

    ParserExpect(p, TokRParen, "')'");
}

// What follows the parameter list: `;` for a declaration, or the body. Extern functions and
// functions with neither are reported here.
static void ParseFunctionBody(Parser* p, FunctionDecl* node, bool isExtern)
{
    if (ParserConsume(p, TokSemicolon))
    {
        return;
    }

    if (isExtern)
    {
        DiagError(p->diag, p->cur.range, "extern function cannot have a body");

        if (p->cur.kind == TokLBrace)
        {
            Advance(p);
            Synchronize(p);
        }

        return;
    }

    if (p->cur.kind != TokLBrace)
    {
        DiagError(p->diag, p->cur.range, "expected function body '{...}' or ';'");

        return;
    }

    p->hasReturnStmt = false;

    // `return { ... };` infers its struct type from the return type: for ^T
    // and string it is the inner T, for T[] the whole array is kept so the
    // braced return parses as an array literal.
    p->returnType
        = node->returnType.isArray
              ? &node->returnType
              : (node->returnType.isBox ? node->returnType.inner
                                        : (node->returnType.primitiveType == PrimString ? NULL : &node->returnType));

    node->body = ParseBlock(p);

    p->returnType = NULL;
    node->hasReturnStmt = p->hasReturnStmt;

    p->hasReturnStmt = false;
}

// A function or global declaration, from its leading attributes through the body or ';'. The
// stages are: the extern prefix, the signature, then the parameters and the body.
Node* ParseFunctionWithAttributes(Parser* p, const Vec* attributes)
{
    bool isPrivate = false;
    bool isExport = false;
    bool isNoReturn = false;
    bool hasAttrs = ApplyFunctionAttributes(p, attributes, &isPrivate, &isExport, &isNoReturn);
    SourceRange firstAttributeRange = hasAttrs ? ((const Attribute*)VecGet(attributes, 0))->range : SRC_INVALID;

    bool isExtern = false;
    bool isDelegate = false;
    char* typeParam = NULL;
    ParseExternPrefix(p, &isExtern, &isDelegate, &typeParam);

    if (RejectAttributesOnNonFunction(p, hasAttrs, firstAttributeRange))
    {
        return NULL;
    }

    TypeName returnType = {0};
    Token nameTok = {0};
    if (!ParseSignatureHead(p, &returnType, &nameTok))
    {
        return NULL;
    }

    // A `<` right after the name starts the type parameters of a generic function.
    Vec genericParams;
    VecInit(&genericParams);

    if (p->cur.kind == TokLt)
    {
        ParseTypeParamList(p, &genericParams);

        if (p->cur.kind != TokLParen)
        {
            DiagError(p->diag, p->cur.range, "expected '(' after type parameters");
            free(genericParams.items);

            return NULL;
        }
    }

    if (p->cur.kind != TokLParen)
    {
        return ParseGlobalDecl(p, returnType, nameTok, isExtern, hasAttrs, firstAttributeRange);
    }

    FunctionDecl* node = NewFunctionDecl(p, returnType, nameTok, isExtern, isDelegate, isPrivate, isExport, isNoReturn,
                                         typeParam, hasAttrs, firstAttributeRange);
    node->genericParams = genericParams;

    if (genericParams.count > 0 && (isExtern || isExport))
    {
        DiagErrorFmt(p->diag, node->base.range, "extern or @export function '%s' cannot have type parameters",
                     node->name);
    }

    if (ParserExpect(p, TokLParen, "'('").kind != TokLParen)
    {
        return (Node*)node;
    }

    ParseFunctionParams(p, node, isExtern);
    ParseFunctionBody(p, node, isExtern);

    return (Node*)node;
}

static bool ImplHasMethod(const ImplDecl* impl, const char* methodName)
{
    for (size_t i = 0; i < impl->methods.count; i++)
    {
        const FunctionDecl* fn = (const FunctionDecl*)VecGet(&impl->methods, i);

        if (fn->methodName && strcmp(fn->methodName, methodName) == 0)
        {
            return true;
        }
    }

    return false;
}

// `property ReturnType Name { get = Symbol; set = Symbol; }` — `get`/`set` are
// contextual (identifier spellings), so user code may still use them as names.
static PropertyDecl* ParsePropertyDecl(Parser* p)
{
    Token kw = p->cur;
    Advance(p);

    TypeName retType = {0};
    if (!ParserTryParseType(p, &retType) || !retType.name)
    {
        DiagError(p->diag, p->cur.range, "expected a property type");
        return NULL;
    }

    if (p->cur.kind != TokIdent)
    {
        DiagError(p->diag, p->cur.range, "expected a property name");
        return NULL;
    }

    Token nameTok = p->cur;
    Advance(p);

    ParserExpect(p, TokLBrace, "'{'");

    char* getter = NULL;
    char* setter = NULL;

    while (p->cur.kind != TokRBrace && p->cur.kind != TokEof)
    {
        Str word = ParserIdentText(p, p->cur);
        bool isGet = p->cur.kind == TokIdent && StrEqC(word, "get");
        bool isSet = p->cur.kind == TokIdent && StrEqC(word, "set");

        if (!isGet && !isSet)
        {
            DiagError(p->diag, p->cur.range, "expected 'get' or 'set' inside property block");
            break;
        }

        Advance(p);
        ParserExpect(p, TokAssign, "'='");

        if (p->cur.kind != TokIdent)
        {
            DiagError(p->diag, p->cur.range, "expected an extern symbol name after '='");
            break;
        }

        char** slot = isGet ? &getter : &setter;

        if (*slot)
        {
            DiagError(p->diag, p->cur.range, isGet ? "duplicate 'get' accessor" : "duplicate 'set' accessor");
        }

        *slot = ToOwned(p->arena, ParserIdentText(p, p->cur));
        Advance(p);

        if (ParserExpect(p, TokSemicolon, "';'").kind != TokSemicolon)
        {
            break;
        }
    }

    ParserExpect(p, TokRBrace, "'}'");

    PropertyDecl* prop = AST_NEW(p->arena, PropertyDecl);
    prop->returnType = retType;
    prop->name = ToOwned(p->arena, ParserIdentText(p, nameTok));
    prop->getterSymbol = getter;
    prop->setterSymbol = setter;
    prop->range = kw.range;

    return prop;
}

// `impl HandleName { extern R Method(...); property T P { get = ...; set = ...; } }`
// Extern methods are parsed by ParseFunction (extern-only enforced here), then
// renamed to their extern symbol `HandleName_MethodName` and collected in
// impl->methods; ParserParseModule hoists them into Module::functions.
ImplDecl* ParseImplDecl(Parser* p)
{
    Token kw = p->cur;
    Advance(p);

    if (p->cur.kind != TokIdent)
    {
        DiagError(p->diag, p->cur.range, "expected a handle name after 'impl'");
        return NULL;
    }

    Token nameTok = p->cur;
    Advance(p);

    ImplDecl* impl = AST_NEW(p->arena, ImplDecl);
    impl->base.kind = NodeImpl;
    impl->base.range = nameTok.range;
    impl->handleName = ToOwned(p->arena, ParserIdentText(p, nameTok));
    VecInit(&impl->methods);
    VecInit(&impl->properties);
    VecInit(&impl->genericParams);

    if (p->cur.kind == TokLt)
    {
        ParseTypeParamList(p, &impl->genericParams);
    }

    Token openBrace = ParserExpect(p, TokLBrace, "'{'");

    while (p->cur.kind != TokRBrace && p->cur.kind != TokEof)
    {
        if (p->cur.kind == TokKwProperty)
        {
            PropertyDecl* prop = ParsePropertyDecl(p);

            if (prop)
            {
                VecPush(&impl->properties, prop);
            }
            else
            {
                Synchronize(p);
            }

            continue;
        }

        // Methods may be extern declarations (host-provided) or defined
        // inline with a body (Strata-implemented). ParseFunction consumes an
        // optional `extern` and returns a GlobalDecl when there is no `(`.
        Node* fnNode = ParseFunction(p);

        if (fnNode && fnNode->kind == NodeGlobal)
        {
            DiagError(p->diag, fnNode->range,
                      "impl blocks may only contain methods or properties, not global variables");
            AstDispose(fnNode);
            Synchronize(p);
            continue;
        }

        FunctionDecl* fn = (fnNode && fnNode->kind == NodeFunction) ? (FunctionDecl*)fnNode : NULL;

        if (!fn)
        {
            // ParseFunction already diagnosed the malformed declaration.
            Synchronize(p);
            continue;
        }

        if (fn->genericParams.count > 0)
        {
            DiagErrorFmt(p->diag, fn->base.range, "type parameters go on the impl, not on method '%s'", fn->name);
        }

        if (ImplHasMethod(impl, fn->name))
        {
            DiagErrorFmt(p->diag, fn->base.range, "redefinition of method '%s' in impl '%s'", fn->name,
                         impl->handleName);
            AstDispose(fnNode); // never reaches impl->methods or Module::functions
            continue;
        }

        if (fn->isPrivate || fn->isExport)
        {
            DiagError(p->diag, fn->base.range, "attributes are not supported on impl methods");
            fn->isPrivate = false;
            fn->isExport = false;
        }

        fn->methodName = fn->name;
        fn->name = ArenaFormat(p->arena, "%s_%s", impl->handleName, fn->methodName);
        fn->mangledName = ArenaStrdup(p->arena, fn->name);
        fn->fromImpl = true;
        VecPush(&impl->methods, fn);
    }

    Token closeBrace = ParserExpect(p, TokRBrace, "'}'");
    impl->base.range = SpanFrom(kw, closeBrace.kind == TokRBrace ? closeBrace : openBrace);

    return impl;
}
