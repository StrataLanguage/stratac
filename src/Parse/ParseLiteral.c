#include "Parse/ParserInternal.h"

#include "Codegen/TypeRegistry.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool StripLongSuffix(Str* sv)
{
    if (sv->len > 0 && (sv->data[sv->len - 1] == 'l' || sv->data[sv->len - 1] == 'L'))
    {
        sv->len--;
        return true;
    }
    return false;
}

static bool StripUnsignedSuffix(Str* sv)
{
    if (sv->len > 0 && (sv->data[sv->len - 1] == 'u' || sv->data[sv->len - 1] == 'U'))
    {
        sv->len--;
        return true;
    }
    return false;
}

/* The one place an integer literal's value is computed (expressions, array
   dimensions, fieldoffset). Decimal - a leading 0 is NOT octal, so `010` is
   ten everywhere - or `0x` hex, with an optional `u`/`U` suffix and any
   number of digits. Values past 64 bits are diagnosed and yield 0; malformed
   spellings such as a bare `0x` were already diagnosed by the lexer. */
uint64_t IntLiteralValueEx(Parser* p, Token tok, bool* outIsUnsigned, bool* outIsLong)
{
    Str sv = ParserIdentText(p, tok);
    bool isLong = StripLongSuffix(&sv);
    bool isUnsigned = StripUnsignedSuffix(&sv);

    if (outIsUnsigned)
    {
        *outIsUnsigned = isUnsigned;
    }

    if (outIsLong)
    {
        *outIsLong = isLong;
    }

    uint64_t base = 10;
    size_t i = 0;

    if (sv.len >= 2 && sv.data[0] == '0' && (sv.data[1] == 'x' || sv.data[1] == 'X'))
    {
        base = 16;
        i = 2;
    }

    uint64_t value = 0;

    for (; i < sv.len; i++)
    {
        char c = sv.data[i];
        uint64_t digit;

        if (c >= '0' && c <= '9')
        {
            digit = (uint64_t)(c - '0');
        }
        else if (base == 16 && c >= 'a' && c <= 'f')
        {
            digit = (uint64_t)(c - 'a' + 10);
        }
        else if (base == 16 && c >= 'A' && c <= 'F')
        {
            digit = (uint64_t)(c - 'A' + 10);
        }
        else
        {
            break;
        }

        if (value > (UINT64_MAX - digit) / base)
        {
            DiagError(p->m_diag, tok.range, "integer literal is too large to fit in 64 bits");
            return 0;
        }

        value = value * base + digit;
    }

    return value;
}

uint64_t IntLiteralValue(Parser* p, Token tok, bool* outIsUnsigned)
{
    return IntLiteralValueEx(p, tok, outIsUnsigned, NULL);
}
