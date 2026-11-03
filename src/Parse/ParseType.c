#include "Parse/ParserInternal.h"

#include "Codegen/TypeRegistry.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

bool LooksLikeVarDecl(Parser* p)
{
    switch (p->m_cur.kind)
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
    case TokIdent:
    {
        // A user-type decl may carry postfix array brackets before the name; speculatively parse, then undo.
        LexerCheckpoint saved = SaveLexerState(p);

        TypeName tn = {0};
        bool isDecl = false;

        if (ParserTryParseType(p, &tn) && tn.name)
        {
            isDecl = p->m_cur.kind == TokIdent;
        }

        RestoreLexerState(p, saved);

        return isDecl;
    }
    default:
        return false;
    }
}

/* Consume postfix `[]`/`[N]`. A literal N declares a fixed (C-ABI inline) array;
   non-literal indices stay intact so speculation is diagnostic-free. Wraps apply
   inside-out and the canonical name is rebuilt in source order to round-trip C
   dimension order (`int[2][6]` = 2 of `int[6]`). */
typedef struct
{
    bool isFixed;
    long length;
    char* lengthName; // `[constName]` — resolved to `length` by sema
} ArrayDim;

static void ApplyArrayBrackets(Parser* p, TypeName* out, SourceRange constRange)
{
    ArrayDim dims[8];
    int dimCount = 0;

    while (p->m_cur.kind == TokLBracket && dimCount < 8)
    {
        Token next = LexerPeekToken(p->m_lex);
        ArrayDim dim;
        bool consumed = false;

        if (next.kind == TokRBracket)
        {
            Advance(p); /* '[' */
            Advance(p); /* ']' */

            dim.isFixed = false;
            dim.length = -1;
            dim.lengthName = NULL;
            consumed = true;
        }
        else if (next.kind == TokIntLit)
        {
            LexerCheckpoint saved = SaveLexerState(p);

            Advance(p); /* '[' */

            Token lenTok = p->m_cur;
            Advance(p);

            if (p->m_cur.kind == TokRBracket)
            {
                Advance(p); /* ']' */

                // Length < 1 is diagnosed later in sema.
                uint64_t length = IntLiteralValue(p, lenTok, NULL);

                if (length > (uint64_t)LONG_MAX)
                {
                    DiagError(p->m_diag, lenTok.range, "array dimension is too large");
                    length = 1;
                }

                dim.isFixed = true;
                dim.length = (long)length;
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

            Advance(p); /* '[' */

            Token nameTok = p->m_cur;
            Advance(p);

            if (p->m_cur.kind == TokRBracket)
            {
                Advance(p); /* ']' */

                dim.isFixed = true;
                dim.length = -2; /* unresolved sentinel */
                dim.lengthName = ToOwned(p->m_arena, ParserIdentText(p, nameTok));
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

    /* Apply inside-out (last source group is the innermost array) and rebuild
        the canonical name in source order; the pre-bracket type is the leaf. */
    const char* leafName = out->name;

    for (int i = dimCount - 1; i >= 0; i--)
    {
        TypeName wrapped = dims[i].isFixed ? TypeNameFixedArrayWrap(p->m_arena, *out, dims[i].length)
                                           : TypeNameArrayWrap(p->m_arena, *out);
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

    out->name = SbFinish(&sb, p->m_arena);
    out->range = SpanToCur(p, constRange);
}

/* `bare` parses only the base type (`const`, `^`, a keyword or a name) and
   leaves any trailing `?` / `[]` for the caller: the box branch uses it so
   that `^` binds tighter than those suffixes. */
static bool ParseTypeImpl(Parser* p, TypeName* out, bool bare)
{
    bool isConst = false;
    bool isVector = false;
    SourceRange constRange = p->m_cur.range;

    if (p->m_cur.kind == TokKwConst)
    {
        isConst = true;
        Advance(p);
    }

    if (p->m_cur.kind == TokCaret)
    {
        /* `^T` — boxed type. `^` binds tighter than a trailing `[]` or `?`, so
           `^S[]` is an array of boxed S and `^S[]?` an optional array of boxed
           S. The inner type is parsed bare so the suffixes are seen here. */
        SourceRange caretRange = p->m_cur.range;
        Advance(p);

        TypeName base = {0};

        if (!ParseTypeImpl(p, &base, true) || !TypeNameValid(&base))
        {
            DiagError(p->m_diag, p->m_cur.range, "expected a type after '^'");
            return false;
        }

        TypeName wrapped = TypeNameBoxWrap(p->m_arena, base);

        /* `^string` is redundant: `string?` is already the maybe-empty
           string, so a box that can never be empty but holds a string has
           no distinct meaning. */
        if (base.primitiveType == PrimString)
        {
            DiagError(p->m_diag, caretRange, "redundant boxing of type 'string'");
        }

        /* `^cstring` is not supported: a constant string is already a
           plain pointer with no ownership to box. */
        if (base.primitiveType == PrimCString)
        {
            DiagError(p->m_diag, caretRange, "type 'cstring' cannot be boxed");
        }

        wrapped.range
            = (SourceRange){caretRange.start, (uint32_t)(p->m_cur.range.start - caretRange.start), caretRange.fileId};
        *out = wrapped;

        if (bare)
        {
            out->isConst = isConst;

            return true;
        }

        /* `^S?` would wrap the box, which is never empty. Report it and keep
           going as `^S`, so a following `[]` still reads as an array of boxes. */
        if (p->m_cur.kind == TokQuestion)
        {
            DiagErrorFmt(p->m_diag, p->m_cur.range,
                         "'^%s' cannot be optional; a box is never empty - declare the field/variable as '%s?' instead",
                         base.name, base.name);
            Advance(p);
        }

        ApplyArrayBrackets(p, out, constRange);

        /* A trailing `?` after brackets wraps the ARRAY in an optional
           (`^S[]?` = optional array of boxes). Fixed dims have no empty state. */
        if (p->m_cur.kind == TokQuestion)
        {
            bool anyFixed = false;

            for (const TypeName* t = out; t && t->isArray; t = t->elem)
            {
                anyFixed = anyFixed || t->length >= 0;
            }

            if (!out->isArray)
            {
                DiagErrorFmt(
                    p->m_diag, p->m_cur.range,
                    "'^%s' cannot be optional; a box is never empty - declare the field/variable as '%s?' instead",
                    base.name, base.name);
                Advance(p);
            }
            else if (anyFixed)
            {
                DiagErrorFmt(p->m_diag, p->m_cur.range, "fixed-size array '%s' cannot be optional; it is never empty",
                             out->name);
                Advance(p);
            }
            else
            {
                SourceRange qRange = p->m_cur.range;
                Advance(p);

                TypeName arr = *out;
                TypeName optWrapped = TypeNameOptionalWrap(p->m_arena, arr);
                optWrapped.range = qRange;
                *out = optWrapped;
            }
        }

        out->isConst = isConst;

        return true;
    }

    const char* name = NULL;

    switch (p->m_cur.kind)
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
        isVector = true;
        break;
    case TokKwFloat3:
        name = "float3";
        isVector = true;
        break;
    case TokKwFloat4:
        name = "float4";
        isVector = true;
        break;
    case TokKwString:
        name = "string";
        break;
    case TokKwCstring:
        name = "cstring";
        break;
    case TokIdent:
        name = ToOwned(p->m_arena, ParserIdentText(p, p->m_cur));
        break;
    default:
        if (isConst)
        {
            DiagError(p->m_diag, constRange, "'const' must be followed by a type");
        }

        return false;
    }

    Advance(p);

    out->name = (char*)name;
    out->primitiveType = GetPrimitiveType(name);
    out->range = SpanToCur(p, constRange);

    if (bare)
    {
        out->isConst = isConst;
        out->isVector = isVector;

        return true;
    }

    /* `T?` — optional (maybe-empty box). Binds tighter than a trailing `[]`,
       so `Weapon?[]` is an array of optionals. `cstring?` is not supported. */
    if (p->m_cur.kind == TokQuestion)
    {
        /* At this point the type is always bare (boxes and array brackets
           are parsed elsewhere), so a plain cstring is all that can get
           here. */
        if (out->primitiveType == PrimCString)
        {
            DiagError(p->m_diag, p->m_cur.range, "type 'cstring' cannot be optional");
            Advance(p);
        }
        else
        {
            SourceRange qRange = p->m_cur.range;
            Advance(p);

            TypeName wrapped = TypeNameOptionalWrap(p->m_arena, *out);
            wrapped.range = SpanToCur(p, constRange);
            (void)qRange;
            *out = wrapped;
        }
    }

    ApplyArrayBrackets(p, out, constRange);

    /* A trailing `?` after brackets wraps the whole array in an optional:
       `int[]?` (maybe-empty dynamic array) differs from `int?[]` (array of
       optionals). Fixed-size arrays have no empty state, so cannot be optional. */
    if (p->m_cur.kind == TokQuestion)
    {
        SourceRange qRange = p->m_cur.range;

        if (out->isArray && out->length >= 0)
        {
            DiagErrorFmt(p->m_diag, qRange, "fixed-size array '%s' cannot be optional; it is never empty", out->name);
            Advance(p);
        }
        else if (out->isArray)
        {
            Advance(p);

            TypeName arr = *out;
            TypeName optWrapped = TypeNameOptionalWrap(p->m_arena, arr);
            optWrapped.range = SpanToCur(p, constRange);
            *out = optWrapped;
        }
        else
        {
            /* A second `?` with no brackets (`int??`) - not a type. */
            DiagErrorFmt(p->m_diag, qRange, "unexpected '?' after type '%s'", out->name);
            Advance(p);
        }
    }

    out->isConst = isConst;
    out->isVector = isVector;

    return true;
}

bool ParserTryParseType(Parser* p, TypeName* out)
{
    return ParseTypeImpl(p, out, false);
}

/* `handle` is a keyword only where a declaration starts with it: `handle Name;` at top level. Anywhere
   else it is an ordinary identifier (a field, local or parameter). */
bool AtHandleDecl(Parser* p)
{
    return p->m_cur.kind == TokIdent && StrEqC(ParserIdentText(p, p->m_cur), "handle")
           && LexerPeekToken(p->m_lex).kind == TokIdent;
}

/* A type named `handle` would turn every global of that type into a handle declaration. */
void CheckTypeNameNotHandle(Parser* p, Token nameTok)
{
    if (StrEqC(ParserIdentText(p, nameTok), "handle"))
    {
        DiagError(p->m_diag, nameTok.range, "'handle' cannot name a type; it starts handle declarations");
    }
}
