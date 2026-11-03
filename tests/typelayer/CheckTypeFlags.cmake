# Roadmap phase 3 gate: `isBox`, `isOptional` and `isArray` are read and written only by the type
# layer. Every other source file asks a predicate (TypeNameIsBox, TypeDescIsOptional, ...). Run with
# -DSRC=<src dir>.
set(type_layer
    AST/AST.h
    Parse/ParseType.c
    Parse/ParseDecl.c
    Parse/ParseStmt.c
    Codegen/TypeUtil.c
    Codegen/TypeRegistry.c
    Codegen/TypeMetadata.c
    Codegen/TypeDescFlags.h)

file(GLOB_RECURSE sources RELATIVE "${SRC}" "${SRC}/*.c" "${SRC}/*.h")
set(failed FALSE)

foreach(f ${sources})
    if(f IN_LIST type_layer)
        continue()
    endif()
    file(READ "${SRC}/${f}" text)
    string(REGEX MATCHALL "[.>](isBox|isOptional|isArray)[^A-Za-z0-9_]" hits "${text}")
    list(LENGTH hits count)
    if(count GREATER 0)
        message(SEND_ERROR "${f}: ${count} type-flag accesses outside the type layer. Use a predicate from AST.h or Codegen/TypeDescFlags.h.")
        set(failed TRUE)
    endif()
endforeach()
