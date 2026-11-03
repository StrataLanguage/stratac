# Runs one conformance case: prepends prelude.strata, compiles it with stratac, and for `run` and
# `panic` cases links and runs the object. See tests/conformance/README.md for the header format.
#
# Inputs: CASE, PRELUDE, STRATAC, OUTPUT_DIR, C_COMPILER, C_COMPILER_ID, EXE_SUFFIX.
# With ASAN_CLANG (a clang for the MSVC target) and ASAN_DLL_DIR (its runtime), the program is compiled with
# STRATA_ASAN=1, linked with -fsanitize=address and run under AddressSanitizer; any report fails the case.

if(NOT DEFINED CASE OR NOT DEFINED PRELUDE OR NOT DEFINED STRATAC OR NOT DEFINED OUTPUT_DIR
   OR NOT DEFINED C_COMPILER OR NOT DEFINED C_COMPILER_ID)
    message(FATAL_ERROR "RunCase.cmake is missing a required setting")
endif()

get_filename_component(case_name "${CASE}" NAME_WE)
get_filename_component(case_dir "${CASE}" DIRECTORY)
get_filename_component(case_kind "${case_dir}" NAME)
set(work_dir "${OUTPUT_DIR}/conformance/${case_kind}")
file(MAKE_DIRECTORY "${work_dir}")

# -- Header -------------------------------------------------------------------------------------

file(STRINGS "${CASE}" expect_lines REGEX "^// expect:")
list(LENGTH expect_lines expect_count)
if(NOT expect_count EQUAL 1)
    message(FATAL_ERROR "${CASE}: needs exactly one '// expect:' line")
endif()
list(GET expect_lines 0 expect_line)

if(expect_line MATCHES "^// expect: (run|reject|panic)( \"(.*)\")?[ \t]*$")
    set(expect_kind "${CMAKE_MATCH_1}")
    set(expect_text "${CMAKE_MATCH_3}")
else()
    message(FATAL_ERROR "${CASE}: bad expect line '${expect_line}' (want run, reject \"...\" or panic \"...\")")
endif()

if(NOT case_kind STREQUAL "reject" AND expect_kind STREQUAL "reject")
    message(FATAL_ERROR "${CASE}: a reject case belongs in reject/")
endif()
if(case_kind STREQUAL "reject" AND NOT expect_kind STREQUAL "reject")
    message(FATAL_ERROR "${CASE}: only reject cases belong in reject/")
endif()

file(STRINGS "${CASE}" output_lines REGEX "^// expect-output: ")

# -- Compile ------------------------------------------------------------------------------------

file(READ "${PRELUDE}" prelude_text)
file(READ "${CASE}" case_text)
set(source "${work_dir}/${case_name}.strata")
file(WRITE "${source}" "${prelude_text}\n${case_text}")

if(C_COMPILER_ID STREQUAL "MSVC")
    set(object "${work_dir}/${case_name}.obj")
else()
    set(object "${work_dir}/${case_name}.o")
endif()
file(REMOVE "${object}")

if(ASAN_CLANG)
    set(strata_command ${CMAKE_COMMAND} -E env STRATA_ASAN=1 "${STRATAC}")
else()
    set(strata_command "${STRATAC}")
endif()

execute_process(
    COMMAND ${strata_command} "${source}" -o "${object}"
    RESULT_VARIABLE compile_result
    OUTPUT_VARIABLE compile_out
    ERROR_VARIABLE compile_err
    TIMEOUT 60
)
set(compile_log "${compile_out}${compile_err}")

if(expect_kind STREQUAL "reject")
    # A rejection is exit code 1 with an error diagnostic; anything else (a crash, a hang, an
    # accepted program) fails the case.
    if(NOT compile_result EQUAL 1 OR NOT compile_log MATCHES "error:")
        message(FATAL_ERROR "expected a compile error, got exit '${compile_result}':\n${compile_log}")
    endif()
    if(NOT expect_text STREQUAL "")
        string(FIND "${compile_log}" "${expect_text}" found)
        if(found EQUAL -1)
            message(FATAL_ERROR "the compile error does not mention \"${expect_text}\":\n${compile_log}")
        endif()
    endif()
    return()
endif()

if(NOT compile_result EQUAL 0)
    message(FATAL_ERROR "expected the program to compile, got exit '${compile_result}':\n${compile_log}")
endif()

# -- Link and run -------------------------------------------------------------------------------

set(exe "${work_dir}/${case_name}${EXE_SUFFIX}")
if(ASAN_CLANG)
    execute_process(COMMAND "${ASAN_CLANG}" --target=x86_64-pc-windows-msvc -fsanitize=address "${object}" -o "${exe}"
                    RESULT_VARIABLE link_result OUTPUT_VARIABLE link_out ERROR_VARIABLE link_err)
elseif(C_COMPILER_ID STREQUAL "MSVC")
    execute_process(COMMAND "${C_COMPILER}" /nologo "${object}" "/Fe:${exe}"
                    RESULT_VARIABLE link_result OUTPUT_VARIABLE link_out ERROR_VARIABLE link_err)
else()
    execute_process(COMMAND "${C_COMPILER}" "${object}" -o "${exe}"
                    RESULT_VARIABLE link_result OUTPUT_VARIABLE link_out ERROR_VARIABLE link_err)
endif()
if(NOT link_result EQUAL 0)
    message(FATAL_ERROR "link failed:\n${link_out}${link_err}")
endif()

set(run_command "${exe}")
if(ASAN_CLANG)
    # The instrumented program loads clang's ASan runtime DLL.
    set(ENV{PATH} "${ASAN_DLL_DIR};$ENV{PATH}")
endif()

execute_process(
    COMMAND ${run_command}
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_out
    ERROR_VARIABLE run_err
    TIMEOUT 30
)
set(run_log "${run_out}${run_err}")

if(run_log MATCHES "AddressSanitizer")
    message(FATAL_ERROR "AddressSanitizer reported an error:\n${run_log}")
endif()

# Signs of a memory error, whatever the case expects.
if(run_log MATCHES "-572662307" OR run_log MATCHES "!!! DOUBLE/BAD FREE")
    message(FATAL_ERROR "memory error (read of freed memory or bad free):\n${run_log}")
endif()

if(expect_kind STREQUAL "panic")
    if(NOT run_log MATCHES "panic: ")
        message(FATAL_ERROR "expected a panic, got exit '${run_result}':\n${run_log}")
    endif()
    if(NOT expect_text STREQUAL "")
        string(FIND "${run_log}" "panic: ${expect_text}" found)
        if(found EQUAL -1)
            message(FATAL_ERROR "the panic does not say \"${expect_text}\":\n${run_log}")
        endif()
    endif()
else()
    if(NOT run_result EQUAL 0)
        message(FATAL_ERROR "expected exit 0, got '${run_result}':\n${run_log}")
    endif()
    if(run_log MATCHES "panic: ")
        message(FATAL_ERROR "unexpected panic:\n${run_log}")
    endif()
    if(run_log MATCHES "END live=" AND NOT run_log MATCHES "END live=0")
        message(FATAL_ERROR "drop values leaked or dropped twice:\n${run_log}")
    endif()
endif()

foreach(line IN LISTS output_lines)
    string(REGEX REPLACE "^// expect-output: " "" wanted "${line}")
    string(FIND "${run_log}" "${wanted}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "output is missing \"${wanted}\":\n${run_log}")
    endif()
endforeach()
