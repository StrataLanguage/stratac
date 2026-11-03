# Style gate: every .c and .h file under src, include, utilities and tests/unit must already match
# .clang-format. Run with -DSRC=<repo root>. Skips with a message when clang-format is not installed.
find_program(CLANG_FORMAT NAMES clang-format)
if(NOT CLANG_FORMAT)
    message(STATUS "style_gate: clang-format not found, skipping")
    return()
endif()

set(unformatted "")
foreach(dir src include utilities tests/unit)
    file(GLOB_RECURSE files RELATIVE "${SRC}" "${SRC}/${dir}/*.c" "${SRC}/${dir}/*.h")
    foreach(f ${files})
        execute_process(COMMAND ${CLANG_FORMAT} --dry-run -Werror "${SRC}/${f}"
            RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
        if(NOT rc EQUAL 0)
            list(APPEND unformatted "${f}")
        endif()
    endforeach()
endforeach()

list(LENGTH unformatted count)
if(count GREATER 0)
    list(JOIN unformatted "\n  " listing)
    message(SEND_ERROR "style_gate: ${count} file(s) differ from .clang-format. Run clang-format -i on:\n  ${listing}")
endif()
