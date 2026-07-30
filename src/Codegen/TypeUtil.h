#pragma once

#include "strata/AST/AST.h"

#include <string>
#include <string_view>

namespace strata::detail
{

struct MappedType
{
    bool valid = false;
    
    bool isVoid = false;
    bool isFloat = false;
    bool isUnsigned = false;
    
    int bits = 0;
    int vec = 1;

    std::string elemIr;
    std::string ir;

    bool IsVector() const noexcept
    {
        return vec > 1;
    }
};

namespace
{

MappedType MakePrimitive(bool isFloat, bool isUnsigned, int bits, std::string_view elemIr, int vec)
{
    MappedType m;
    m.valid = true;
    m.isFloat = isFloat;
    m.isUnsigned = isUnsigned;
    m.bits = bits;
    m.elemIr = elemIr;
    m.vec = vec;

    if (vec == 1)
    {
        m.ir = elemIr;
    }
    else
    {
        m.ir = '<' + std::to_string(vec) + " x " + elemIr.data() + '>';
    }

    return m;
}

} // namespace

inline MappedType MapType(const TypeName& t)
{
    if (t.name.empty())
    {
        return {};
    }

    std::string_view base = t.name;
    int vec = 1;

    size_t split = base.size();
    
    while (split > 0 && base[split - 1] >= '0' && base[split - 1] <= '9')
    {
        --split;
    }

    if (split > 0 && split < base.size())
    {
        int parsedSize = 0;
        for (auto i = split; i < base.size(); ++i)
        {
            parsedSize = (parsedSize * 10) + (base[i] - '0');
        }
        if (parsedSize >= 1 && parsedSize <= 4)
        {
            std::string_view candidateBase = base.substr(0, split);
            if (candidateBase == "float" || candidateBase == "int" || candidateBase == "uint" ||
                candidateBase == "half" || candidateBase == "double" || candidateBase == "bool")
            {
                base = candidateBase;
                vec = parsedSize;
            }
        }
    }

    if (base == "void")
    {
        MappedType m;
        m.valid = true;
        m.isVoid = true;
        m.ir = "void";
        m.elemIr = "void";

        return m;
    }

    if (base == "bool")
    {
        return MakePrimitive(false, false, 1, "i1", vec);
    }
    if (base == "int")
    {
        return MakePrimitive(false, false, 32, "i32", vec);
    }
    if (base == "uint")
    {
        return MakePrimitive(false, true, 32, "i32", vec);
    }
    if (base == "half")
    {
        return MakePrimitive(true, false, 16, "half", vec);
    }
    if (base == "float")
    {
        return MakePrimitive(true, false, 32, "float", vec);
    }
    if (base == "double")
    {
        return MakePrimitive(true, false, 64, "double", vec);
    }

    return {};
}

} // namespace strata::detail
