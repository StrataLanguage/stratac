# Roadmap phase 4 gate: the CFG move checker, run in shadow mode over the conformance corpus, may differ
# from the old checker only where expected.txt says so ("<dir>/<case> only-new|only-old"). Any other
# difference fails, and so does an expected one that no longer happens. Run with
# -DSTRATAC=<stratac> -DROOT=<repo root> -DWORK=<scratch dir>.
file(MAKE_DIRECTORY "${WORK}")
file(READ "${ROOT}/tests/conformance/prelude.strata" prelude)
file(STRINGS "${ROOT}/tests/shadow/expected.txt" expected_lines)
set(seen "")
set(failed FALSE)

foreach(dir accept reject)
    file(GLOB cases "${ROOT}/tests/conformance/${dir}/*.strata")
    foreach(case ${cases})
        get_filename_component(name "${case}" NAME_WE)
        file(READ "${case}" body)
        set(src "${WORK}/${name}.strata")
        file(WRITE "${src}" "${prelude}\n${body}")
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E env STRATA_SHADOW=1 "${STRATAC}" "${src}" -o "${WORK}/${name}.o"
            OUTPUT_QUIET ERROR_VARIABLE err TIMEOUT 60)
        string(REGEX MATCHALL "only-(new|old)" hits "${err}")
        foreach(hit ${hits})
            set(key "${dir}/${name} ${hit}")
            list(APPEND seen "${key}")
            if(NOT key IN_LIST expected_lines)
                message(SEND_ERROR "unexpected shadow disagreement: ${key}")
                set(failed TRUE)
            endif()
        endforeach()
    endforeach()
endforeach()

foreach(line ${expected_lines})
    if(NOT line IN_LIST seen)
        message(SEND_ERROR "expected shadow disagreement did not happen (remove it from expected.txt): ${line}")
    endif()
endforeach()
