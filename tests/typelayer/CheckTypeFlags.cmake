# Type-layer gate. Run with -DSRC=<src dir>.
#  1. `isBox`, `isOptional` and `isArray` are TypeName fields, read and written only by the type layer.
#     Every other source file asks a predicate (TypeNameIsBox, ...).
#  2. The shape bits of a TypeDesc live in its `flags` field, written only by the type layer
#     (Codegen/TypeDescFlags.h and TypeDescMake). Every other source file uses the TypeDescIs* /
#     TypeDescSet* accessors.
set(type_layer
    AST/AST.h
    Parse/ParseType.c
    Parse/ParseDecl.c
    Parse/ParseStmt.c
    Codegen/TypeUtil.c
    Codegen/TypeRegistry.c
    Codegen/TypeMetadata.c
    Codegen/TypeDescFlags.h)

set(typedesc_layer
    Codegen/LLVMModuleBuilder.h
    Codegen/TypeDescFlags.h
    Codegen/Builder/Types.c)

file(GLOB_RECURSE sources RELATIVE "${SRC}" "${SRC}/*.c" "${SRC}/*.h")
set(failed FALSE)

foreach(f ${sources})
    file(READ "${SRC}/${f}" text)

    if(NOT f IN_LIST type_layer)
        string(REGEX MATCHALL "[.>](isBox|isOptional|isArray)[^A-Za-z0-9_]" hits "${text}")
        list(LENGTH hits count)
        if(count GREATER 0)
            message(SEND_ERROR "${f}: ${count} type-flag accesses outside the type layer. Use a predicate from AST.h or Codegen/TypeDescFlags.h.")
            set(failed TRUE)
        endif()
    endif()

    # A `.flags` / `->flags` read or write in a file that works with TypeDescs (or the TDF_ bits) must go
    # through the accessors. Files that never mention a TypeDesc may have their own `flags` fields.
    if(NOT f IN_LIST typedesc_layer)
        string(REGEX MATCHALL "[.>]flags[^A-Za-z0-9_]" flag_hits "${text}")
        list(LENGTH flag_hits flag_count)
        string(REGEX MATCH "[Tt]ype[Dd]esc|TDF_" mentions "${text}")
        if(flag_count GREATER 0 AND mentions)
            message(SEND_ERROR "${f}: ${flag_count} direct TypeDesc flags accesses outside the type layer. Use TypeDescIs*/TypeDescSet* from Codegen/TypeDescFlags.h.")
            set(failed TRUE)
        endif()
    endif()
endforeach()
