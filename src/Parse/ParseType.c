// Type syntax: builtin names, arrays, boxes, optionals and handles.

#include "Parse/ParserInternal.h"

#include "Codegen/TypeRegistry.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

bool LooksLikeVarDecl(Parser* p)
{
    switch (p->cur.kind)
    {
    case TokKwRef:
    case TokKwConst:
    case TokKwVoid:
    case TokKwBool:
    case TokKwUint:
    case TokKwInt:
    case TokKwLong:
    case TokKwUlong:
    case TokKwByte:
    case TokKwSbyte:
    case TokKwShort:
    case TokKwUshort:
    case TokKwFloat:
    case TokKwDouble:
    case TokKwFloat2:
    case TokKwFloat3:
    case TokKwFloat4:
    case TokKwString:
    case TokKwCstring:
    case TokCaret:
        return true;
    case TokStar:
    case TokIdent:
    {
        // A user-type decl may carry postfix array brackets before the name; speculatively parse, then undo.
        // A leading `*` is a pointer type or a dereference (`*p = 1;`); only a type followed by a name is a decl.
        LexerCheckpoint saved = SaveLexerState(p);

        TypeName tn = {0};
        bool isDecl = false;

        if (ParserTryParseType(p, &tn) && tn.name)
        {
            isDecl = p->cur.kind == TokIdent;
        }

        RestoreLexerState(p, saved);

        return isDecl;
    }
    default:
        return false;
    }
}

// Consume postfix `[]`/`[N]`. A literal N declares a fixed (C-ABI inline) array;
// non-literal indices stay intact so speculation is diagnostic-free. Wraps apply
// inside-out and the canonical name is rebuilt in source order to round-trip C
// dimension order (`int[2][6]` = 2 of `int[6]`).
typedef struct
{
    bool isFixed;
    ArrayLength length;
    char* lengthName; // `[constName]` — resolved to `length` by sema
} ArrayDim;

static void ApplyArrayBrackets(Parser* p, TypeName* out, SourceRange constRange)
{
    ArrayDim dims[8];
    int dimCount = 0;

    while (p->cur.kind == TokLBracket && dimCount < 8)
    {
        Token next = LexerPeekToken(p->lex);
        ArrayDim dim;
        bool consumed = false;

        if (next.kind == TokRBracket)
        {
            Advance(p);
            Advance(p);

            dim.isFixed = false;
            dim.length = -1;
            dim.lengthName = NULL;
            consumed = true;
        }
        else if (next.kind == TokIntLit)
        {
            LexerCheckpoint saved = SaveLexerState(p);

            Advance(p);

            Token lenTok = p->cur;
            Advance(p);

            if (p->cur.kind == TokRBracket)
            {
                Advance(p);

                // Length < 1 is diagnosed later in sema.
                uint64_t length = IntLiteralValue(p, lenTok, NULL);

                if (length > (uint64_t)LONG_MAX)
                {
                    DiagError(p->diag, lenTok.range, "array dimension is too large");
                    length = 1;
                }

                dim.isFixed = true;
                dim.length = (ArrayLength)length;
                dim.lengthName = NULL;
                consumed = true;
            }
            else
            {
                // Not a fixed dimension after all — put everything back.
                RestoreLexerState(p, saved);
            }
        }
        else if (next.kind == TokIdent)
        {
            // `[constName]` — a manifest-constant dimension, resolved by sema.
            LexerCheckpoint saved = SaveLexerState(p);

            Advance(p);

            Token nameTok = p->cur;
            Advance(p);

            if (p->cur.kind == TokRBracket)
            {
                Advance(p);

                dim.isFixed = true;
                dim.length = -2; // unresolved sentinel
                dim.lengthName = ToOwned(p->arena, ParserIdentText(p, nameTok));
                consumed = true;
            }
            else
            {
                RestoreLexerState(p, saved);
            }
        }

        if (!consumed)
        {
            break;
        }

        dims[dimCount++] = dim;
    }

    if (dimCount == 0)
    {
        return;
    }

    // Apply inside-out (last source group is the innermost array) and rebuild
    // the canonical name in source order; the pre-bracket type is the leaf.
    const char* leafName = out->name;

    for (int i = dimCount - 1; i >= 0; i--)
    {
        TypeName wrapped = dims[i].isFixed ? TypeNameFixedArrayWrap(p->arena, *out, dims[i].length)
                                           : TypeNameArrayWrap(p->arena, *out);
        *out = wrapped;

        if (dims[i].lengthName)
        {
            out->lengthName = dims[i].lengthName;
        }
    }

    Sb sb;
    SbInit(&sb);
    SbPuts(&sb, leafName);

    for (int i = 0; i < dimCount; i++)
    {
        if (dims[i].isFixed)
        {
            if (dims[i].lengthName)
            {
                SbPrintf(&sb, "[%s]", dims[i].lengthName);
            }
            else
            {
                SbPrintf(&sb, "[%ld]", dims[i].length);
            }
        }
        else
        {
            SbPuts(&sb, "[]");
        }
    }

    out->name = SbFinish(&sb, p->arena);
    out->range = SpanToCur(p, constRange);
}

static bool ParseTypeImpl(Parser* p, TypeName* out, bool bare);

// Closes a type-argument list. A `>>` closes two lists: its first `>` is consumed, and the current token
// becomes the second `>` (one character later) for the enclosing list to close.
static bool ConsumeTypeArgClose(Parser* p)
{
    if (p->cur.kind == TokGt)
    {
        Advance(p);

        return true;
    }

    if (p->cur.kind == TokShr)
    {
        p->cur.kind = TokGt;
        p->cur.range.start += 1;
        p->cur.range.length = 1;

        return true;
    }

    return false;
}

// `<T1, T2, ...>` after a type name, the current token being the `<`. Each argument is a full type,
// so wrappers and nested lists work inside. Returns false when the list does not close.
static bool ParseTypeArgList(Parser* p, TypeName** outArgs, size_t* outCount)
{
    Advance(p);

    Vec args;
    VecInit(&args);

    bool ok = true;

    do
    {
        TypeName arg = {0};

        if (!ParseTypeImpl(p, &arg, false))
        {
            ok = false;
            break;
        }

        VecPush(&args, TypeNameWrapInner(p->arena, arg));
    } while (ParserConsume(p, TokComma));

    ok = ok && ConsumeTypeArgClose(p);

    if (ok)
    {
        TypeName* copy = (TypeName*)ArenaAlloc(p->arena, args.count * sizeof(TypeName));

        for (size_t i = 0; i < args.count; i++)
        {
            copy[i] = *(TypeName*)VecGet(&args, i);
        }

        *outArgs = copy;
        *outCount = args.count;
    }

    free(args.items);

    return ok;
}

bool ParseTypeArgsAfter(Parser* p, const char* base, SourceRange baseRange, TypeName* out)
{
    TypeName* args = NULL;
    size_t argCount = 0;

    if (!ParseTypeArgList(p, &args, &argCount))
    {
        return false;
    }

    out->genericBase = (char*)base;
    out->typeArgs = args;
    out->typeArgCount = argCount;
    out->name = TypeNameGenericSpelling(p->arena, base, args, argCount);
    out->primitiveType = GetPrimitiveType(out->name);
    out->range = SpanToCur(p, baseRange);

    return true;
}

// The name of a builtin type keyword or of an identifier. The token is left for the caller to
// consume. Returns NULL when the token starts no type.
static const char* ParseBaseTypeName(Parser* p, bool isConst, SourceRange constRange, bool* isVector)
{
    const char* name = NULL;

    switch (p->cur.kind)
    {
    case TokKwVoid:
        name = "void";
        break;
    case TokKwBool:
        name = "bool";
        break;
    case TokKwInt:
        name = "int";
        break;
    case TokKwUint:
        name = "uint";
        break;
    case TokKwLong:
        name = "long";
        break;
    case TokKwUlong:
        name = "ulong";
        break;
    case TokKwByte:
        name = "byte";
        break;
    case TokKwSbyte:
        name = "sbyte";
        break;
    case TokKwShort:
        name = "short";
        break;
    case TokKwUshort:
        name = "ushort";
        break;
    case TokKwFloat:
        name = "float";
        break;
    case TokKwDouble:
        name = "double";
        break;
    case TokKwFloat2:
        name = "float2";
        *isVector = true;
        break;
    case TokKwFloat3:
        name = "float3";
        *isVector = true;
        break;
    case TokKwFloat4:
        name = "float4";
        *isVector = true;
        break;
    case TokKwString:
        name = "string";
        break;
    case TokKwCstring:
        name = "cstring";
        break;
    case TokIdent:
        name = ToOwned(p->arena, ParserIdentText(p, p->cur));
        break;
    default:
        if (isConst)
        {
            DiagError(p->diag, constRange, "'const' must be followed by a type");
        }

        return NULL;
    }

    return name;
}

// `T?` — optional (maybe-empty box). Binds tighter than a trailing `[]`,
// so `Weapon?[]` is an array of optionals. `cstring?` is not supported.
static void ParseOptionalSuffix(Parser* p, TypeName* out, SourceRange constRange)
{
    if (p->cur.kind == TokQuestion)
    {
        // At this point the type is always bare (boxes and array brackets
        // are parsed elsewhere), so a plain cstring is all that can get
        // here.
        if (out->primitiveType == PrimCString)
        {
            DiagError(p->diag, p->cur.range, "type 'cstring' cannot be optional");
            Advance(p);
        }
        else
        {
            SourceRange qRange = p->cur.range;
            Advance(p);

            TypeName wrapped = TypeNameOptionalWrap(p->arena, *out);
            wrapped.range = SpanToCur(p, constRange);
            (void)qRange;
            *out = wrapped;
        }
    }
}

// A trailing `?` after brackets wraps the whole array in an optional:
// `int[]?` (maybe-empty dynamic array) differs from `int?[]` (array of
// optionals). Fixed-size arrays have no empty state, so cannot be optional.
static void ParseArrayOptionalSuffix(Parser* p, TypeName* out, SourceRange constRange)
{
    if (p->cur.kind == TokQuestion)
    {
        SourceRange qRange = p->cur.range;

        if (out->isArray && out->length >= 0)
        {
            DiagErrorFmt(p->diag, qRange, "fixed-size array '%s' cannot be optional; it is never empty", out->name);
            Advance(p);
        }
        else if (out->isArray)
        {
            Advance(p);

            TypeName arr = *out;
            TypeName optWrapped = TypeNameOptionalWrap(p->arena, arr);
            optWrapped.range = SpanToCur(p, constRange);
            *out = optWrapped;
        }
        else
        {
            // A second `?` with no brackets (`int??`) - not a type.
            DiagErrorFmt(p->diag, qRange, "unexpected '?' after type '%s'", out->name);
            Advance(p);
        }
    }
}

// The suffixes after `^T`, which bind to the box: a `?` right after the box is reported, `[]`
// brackets make an array of boxes, and a `?` after those wraps the array.
static void ParseBoxSuffixes(Parser* p, TypeName* out, const char* baseName, SourceRange constRange)
{
    // `^S?` would wrap the box, which is never empty. Report it and keep
    // going as `^S`, so a following `[]` still reads as an array of boxes.
    if (p->cur.kind == TokQuestion)
    {
        DiagErrorFmt(p->diag, p->cur.range,
                     "'^%s' cannot be optional; a box is never empty - declare the field/variable as '%s?' instead",
                     baseName, baseName);
        Advance(p);
    }

    ApplyArrayBrackets(p, out, constRange);

    // A trailing `?` after brackets wraps the ARRAY in an optional
    // (`^S[]?` = optional array of boxes). Fixed dims have no empty state.
    if (p->cur.kind == TokQuestion)
    {
        bool anyFixed = false;

        for (const TypeName* t = out; t && t->isArray; t = t->elem)
        {
            anyFixed = anyFixed || t->length >= 0;
        }

        if (!out->isArray)
        {
            DiagErrorFmt(p->diag, p->cur.range,
                         "'^%s' cannot be optional; a box is never empty - declare the field/variable as '%s?' instead",
                         baseName, baseName);
            Advance(p);
        }
        else if (anyFixed)
        {
            DiagErrorFmt(p->diag, p->cur.range, "fixed-size array '%s' cannot be optional; it is never empty",
                         out->name);
            Advance(p);
        }
        else
        {
            SourceRange qRange = p->cur.range;
            Advance(p);

            TypeName arr = *out;
            TypeName optWrapped = TypeNameOptionalWrap(p->arena, arr);
            optWrapped.range = qRange;
            *out = optWrapped;
        }
    }
}

// `^T` — boxed type. `^` binds tighter than a trailing `[]` or `?`, so
// `^S[]` is an array of boxed S and `^S[]?` an optional array of boxed
// S. The inner type is parsed bare so the suffixes are seen here.
static bool ParseBoxedType(Parser* p, TypeName* out, bool isConst, SourceRange constRange, bool bare)
{
    SourceRange caretRange = p->cur.range;
    Advance(p);

    TypeName base = {0};

    if (!ParseTypeImpl(p, &base, true) || !TypeNameValid(&base))
    {
        DiagError(p->diag, p->cur.range, "expected a type after '^'");
        return false;
    }

    TypeName wrapped = TypeNameBoxWrap(p->arena, base);

    // `^string` is redundant: `string?` is already the maybe-empty
    // string, so a box that can never be empty but holds a string has
    // no distinct meaning.
    if (base.primitiveType == PrimString)
    {
        DiagError(p->diag, caretRange, "redundant boxing of type 'string'");
    }

    // `^cstring` is not supported: a constant string is already a
    // plain pointer with no ownership to box.
    if (base.primitiveType == PrimCString)
    {
        DiagError(p->diag, caretRange, "type 'cstring' cannot be boxed");
    }

    wrapped.range
        = (SourceRange){caretRange.start, (uint32_t)(p->cur.range.start - caretRange.start), caretRange.fileId};
    *out = wrapped;

    if (bare)
    {
        out->isConst = isConst;

        return true;
    }

    ParseBoxSuffixes(p, out, base.name, constRange);

    out->isConst = isConst;

    return true;
}

// `*T` — pointer. `*` binds tighter than a trailing `[]`, so `*S[]` is an array of pointers to S. A pointer
// is already nullable, so `*S?` is left unparsed and rejected by the caller.
static bool ParsePointerType(Parser* p, TypeName* out, bool isConst, SourceRange constRange, bool bare)
{
    SourceRange starRange = p->cur.range;
    Advance(p);

    TypeName base = {0};

    // No diagnostic on failure: a statement may start with a dereference, and LooksLikeVarDecl speculates.
    if (!ParseTypeImpl(p, &base, true) || !TypeNameValid(&base))
    {
        return false;
    }

    TypeName wrapped = TypeNamePointerWrap(p->arena, base);

    wrapped.range = (SourceRange){starRange.start, (uint32_t)(p->cur.range.start - starRange.start), starRange.fileId};
    *out = wrapped;

    if (!bare)
    {
        ApplyArrayBrackets(p, out, constRange);

        if (out->isArray)
        {
            ParseArrayOptionalSuffix(p, out, constRange);
        }
    }

    out->isConst = isConst;

    return true;
}

// `bare` parses only the base type (`const`, `^`, a keyword or a name) and
// leaves any trailing `?` / `[]` for the caller: the box branch uses it so
// that `^` binds tighter than those suffixes.
static bool ParseTypeImpl(Parser* p, TypeName* out, bool bare)
{
    bool isConst = false;
    bool isVector = false;
    SourceRange constRange = p->cur.range;

    if (p->cur.kind == TokKwConst)
    {
        isConst = true;
        Advance(p);
    }

    if (p->cur.kind == TokCaret)
    {
        return ParseBoxedType(p, out, isConst, constRange, bare);
    }

    if (p->cur.kind == TokStar)
    {
        return ParsePointerType(p, out, isConst, constRange, bare);
    }

    bool isIdent = p->cur.kind == TokIdent;
    const char* name = ParseBaseTypeName(p, isConst, constRange, &isVector);

    if (!name)
    {
        return false;
    }

    Advance(p);

    out->name = (char*)name;
    out->primitiveType = GetPrimitiveType(name);
    out->range = SpanToCur(p, constRange);

    // `Name<T, ...>`: a generic use; `name` becomes the canonical spelling.
    if (isIdent && p->cur.kind == TokLt && !ParseTypeArgsAfter(p, name, constRange, out))
    {
        return false;
    }

    if (bare)
    {
        out->isConst = isConst;
        out->isVector = isVector;

        return true;
    }

    ParseOptionalSuffix(p, out, constRange);
    ApplyArrayBrackets(p, out, constRange);
    ParseArrayOptionalSuffix(p, out, constRange);

    out->isConst = isConst;
    out->isVector = isVector;

    return true;
}

bool ParserTryParseType(Parser* p, TypeName* out)
{
    return ParseTypeImpl(p, out, false);
}

// `handle` is a keyword only where a declaration starts with it: `handle Name;` at top level. Anywhere
// else it is an ordinary identifier (a field, local or parameter).
bool AtHandleDecl(Parser* p)
{
    return p->cur.kind == TokIdent && StrEqC(ParserIdentText(p, p->cur), "handle")
           && LexerPeekToken(p->lex).kind == TokIdent;
}

// A type named `handle` would turn every global of that type into a handle declaration.
void CheckTypeNameNotHandle(Parser* p, Token nameTok)
{
    if (StrEqC(ParserIdentText(p, nameTok), "handle"))
    {
        DiagError(p->diag, nameTok.range, "'handle' cannot name a type; it starts handle declarations");
    }
}
