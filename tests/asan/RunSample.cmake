# Roadmap phase 5 gate for the AOT samples: builds one sample (and its C host, if any) with the generated code
# instrumented by AddressSanitizer, runs it, and checks the same output the plain test checks. Any ASan report fails.
# Inputs: STRATAC, SAMPLE, HOST (may be empty), WORK, EXPECT_FILE, ASAN_CLANG, ASAN_DLL_DIR, INCLUDE_DIR and
# PREFIXES (optional): compile the sample once per --symbol-prefix and link every copy into the one host.
file(MAKE_DIRECTORY "${WORK}")
get_filename_component(name "${SAMPLE}" NAME_WE)
set(object "${WORK}/${name}.o")
set(exe "${WORK}/${name}.exe")

set(inputs "")
set(variants ${PREFIXES})
if(NOT variants)
    set(variants "-")
endif()

foreach(prefix ${variants})
    if(prefix STREQUAL "-")
        set(prefix_args "")
        set(asan_tag 1)
        set(variant_object "${object}")
    else()
        set(prefix_args --symbol-prefix ${prefix})
        set(asan_tag ${prefix})
        set(variant_object "${WORK}/${name}_${prefix}.o")
    endif()
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env STRATA_ASAN=${asan_tag} "${STRATAC}" "${SAMPLE}" ${prefix_args} -o "${variant_object}"
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "stratac failed (${rc}):\n${out}${err}")
    endif()
    list(APPEND inputs "${variant_object}")
endforeach()

# Every instrumented object defines its own copy of the section markers the ASan runtime walks.
set(link_extra "")
list(LENGTH inputs input_count)
if(input_count GREATER 1)
    set(link_extra -Wl,/force:multiple)
endif()

if(HOST)
    set(host_object "${WORK}/${name}_host.o")
    execute_process(
        COMMAND "${ASAN_CLANG}" --target=x86_64-pc-windows-msvc -fsanitize=address -I "${INCLUDE_DIR}" -c "${HOST}"
                -o "${host_object}"
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "host compile failed (${rc}):\n${out}${err}")
    endif()
    list(APPEND inputs "${host_object}")
endif()

execute_process(
    COMMAND "${ASAN_CLANG}" --target=x86_64-pc-windows-msvc -fsanitize=address ${inputs} -o "${exe}" ${link_extra}
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "link failed (${rc}):\n${out}${err}")
endif()

set(ENV{PATH} "${ASAN_DLL_DIR};$ENV{PATH}")
execute_process(COMMAND "${exe}" RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 60)
set(log "${out}${err}")

if(log MATCHES "AddressSanitizer")
    message(FATAL_ERROR "AddressSanitizer reported an error:\n${log}")
endif()

file(READ "${EXPECT_FILE}" expect)
if(NOT log MATCHES "${expect}")
    message(FATAL_ERROR "output does not match the plain test's expectation (exit ${rc}):\n${log}")
endif()
