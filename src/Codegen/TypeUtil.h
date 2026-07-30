// Strata compiler: internal type mapping shared by the IR generators.
//
// Maps Strata textual type names (int, float, float4, ...) to a scalar/vector
// description expressed in LLVM IR terms. Both the text back-end and the LLVM
// C API back-end use this so type decisions live in one place.
#pragma once

#include "strata/AST/AST.h"

#include <cstring>
#include <string>

namespace strata::detail
{

struct MappedType
{
    bool valid = false;
    bool isVoid = false;
    bool isFloat = false;    // element is floating-point
    bool isUnsigned = false; // element is unsigned integer
    int bits = 0;            // element bit width (0 for void)
    int vec = 1;             // 1 = scalar, >1 = vector
    std::string elemIr;      // e.g. "i32", "float"
    std::string ir;          // full IR type, e.g. "i32" or "<4 x float>"

    bool IsVector() const noexcept
    {
        return vec > 1;
    }
};

inline MappedType MapType(const TypeName& t)
{
    MappedType m;
    const std::string& n = t.name;
    if (n.empty()) return m;

    // Split a possible trailing vector size, e.g. "float4" -> base "float", 4.
    std::string base = n;
    int vsize = 1;
    std::size_t split = base.size();
    while (split > 0 && base[split - 1] >= '0' && base[split - 1] <= '9') --split;
    if (split < base.size() && split > 0)
    {
        // Only treat as a vector if the base (without digits) is a known scalar
        // and the size is in 1..4 (HLSL vector range).
        int v = 0;
        for (std::size_t i = split; i < base.size(); ++i) v = (v * 10) + (base[i] - '0');
        std::string maybe = base.substr(0, split);
        if ((maybe == "float" || maybe == "int" || maybe == "uint" || maybe == "half" || maybe == "double" ||
             maybe == "bool") &&
            v >= 1 && v <= 4)
        {
            base = maybe;
            vsize = v;
        }
    }

    auto set = [&](bool flt, bool uns, int b, const char* ir)
    {
        m.valid = true;
        m.isFloat = flt;
        m.isUnsigned = uns;
        m.bits = b;
        m.elemIr = ir;
        m.vec = vsize;
        if (vsize == 1)
            m.ir = ir;
        else
            m.ir = "<" + std::to_string(vsize) + " x " + ir + ">";
    };

    if (base == "void")
    {
        m.valid = true;
        m.isVoid = true;
        m.ir = "void";
        m.elemIr = "void";
    }
    else if (base == "bool")
    {
        set(false, false, 1, "i1");
    }
    else if (base == "int")
    {
        set(false, false, 32, "i32");
    }
    else if (base == "uint")
    {
        set(false, true, 32, "i32");
    }
    else if (base == "half")
    {
        set(true, false, 16, "half");
    }
    else if (base == "float")
    {
        set(true, false, 32, "float");
    }
    else if (base == "double")
    {
        set(true, false, 64, "double");
    }
    else
    {
        m.valid = false;
    }

    return m;
}

} // namespace strata::detail
