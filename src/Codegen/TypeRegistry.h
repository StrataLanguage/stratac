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

namespace strata {

struct StructType {
    std::string name;
    bool opaque = false; // a `handle` (pointer-sized; layout unknown)
    std::vector<FieldDecl> fields;
};

class TypeRegistry {
public:
    void build(const Module& m) {
        types_.clear();
        for (const auto& s : m.structs) {
            types_.push_back({s->name, false, s->fields}); // structs are defined value types
        }
        for (const auto& h : m.handles) {
            types_.push_back({h->name, true, {}}); // handles are opaque
        }
    }

    const StructType* find(std::string_view name) const noexcept {
        for (const auto& t : types_) if (t.name == name) return &t;
        return nullptr;
    }
    bool isUserType(std::string_view name) const noexcept { return find(name) != nullptr; }
    bool isOpaque(std::string_view name) const noexcept {
        const auto* t = find(name);
        return t && t->opaque;
    }

    // Returns the field index of `field` within `structName`, or -1.
    int fieldIndex(std::string_view structName, std::string_view field) const noexcept {
        const auto* t = find(structName);
        if (!t) return -1;
        for (std::size_t i = 0; i < t->fields.size(); ++i) {
            if (t->fields[i].name == field) return static_cast<int>(i);
        }
        return -1;
    }

    const std::vector<StructType>& types() const noexcept { return types_; }

private:
    std::vector<StructType> types_;
};

} // namespace strata
