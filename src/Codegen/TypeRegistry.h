// Strata compiler: registry of user-defined (struct) and opaque types.
//
// Both the text and LLVM back-ends consult this to resolve a textual type name
// that is not a built-in scalar/vector. Struct layouts are captured here so the
// back-ends can lower member access (field index + element type).
//
// Internal header.
#pragma once

#include "strata/AST/AST.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace strata
{

struct StructType
{
    std::string name;
    bool opaque = false; // a `handle` (pointer-sized; layout unknown)
    std::vector<FieldDecl> fields;
};

class TypeRegistry
{
  public:
    void Build(const Module& m)
    {
        m_types.clear();
        for (const auto& s : m.structs)
        {
            m_types.push_back(
                {.name = s->name, .opaque = false, .fields = s->fields}); // structs are defined value types
        }
        for (const auto& h : m.handles)
        {
            m_types.push_back({.name = h->name, .opaque = true, .fields = {}}); // handles are opaque
        }
    }

    const StructType* Find(std::string_view name) const noexcept
    {
        for (const auto& t : m_types)
            if (t.name == name) return &t;
        return nullptr;
    }
    bool IsUserType(std::string_view name) const noexcept
    {
        return Find(name) != nullptr;
    }
    bool IsOpaque(std::string_view name) const noexcept
    {
        const auto* t = Find(name);
        return t && t->opaque;
    }

    // Returns the field index of `field` within `structName`, or -1.
    int FieldIndex(std::string_view structName, std::string_view field) const noexcept
    {
        const auto* t = Find(structName);
        if (!t) return -1;
        for (std::size_t i = 0; i < t->fields.size(); ++i)
        {
            if (t->fields[i].name == field) return static_cast<int>(i);
        }
        return -1;
    }

    const std::vector<StructType>& Types() const noexcept
    {
        return m_types;
    }

  private:
    std::vector<StructType> m_types;
};

} // namespace strata
