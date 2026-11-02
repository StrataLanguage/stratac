# Roadmap phase 3 gate: `isBox`, `isOptional`, `isRc` and `isArray` are read only by the type layer.
# Files outside it are counted against tests/typelayer/baseline.txt, which may only go down.
# Run with -DSRC=<src dir> -DBASELINE=<baseline file>; add -DUPDATE=1 to lower the baseline.
set(outside
    Sema/ResolveOverloads.c
    Codegen/LLVMModuleBuilder.c
    Codegen/LLVMModuleBuilder.h
    Codegen/ASTDump.c)

file(STRINGS "${BASELINE}" lines)
set(failed FALSE)
set(report "")

foreach(f ${outside})
    file(READ "${SRC}/${f}" text)
    string(REGEX MATCHALL "[.>](isBox|isOptional|isRc|isArray)[^A-Za-z0-9_]" hits "${text}")
    list(LENGTH hits count)
    set(allowed "")
    foreach(line ${lines})
        if(line MATCHES "^${f}=([0-9]+)$")
            set(allowed ${CMAKE_MATCH_1})
        endif()
    endforeach()
    if("${allowed}" STREQUAL "")
        set(allowed 0)
    endif()
    if(count GREATER allowed)
        message(SEND_ERROR "${f}: ${count} type-flag reads, baseline allows ${allowed}. Use a type-layer predicate.")
        set(failed TRUE)
    elseif(count LESS allowed)
        message(STATUS "${f}: ${count} (baseline ${allowed}) - lower the baseline")
    endif()
    string(APPEND report "${f}=${count}\n")
endforeach()

if(UPDATE)
    file(WRITE "${BASELINE}" "${report}")
endif()
