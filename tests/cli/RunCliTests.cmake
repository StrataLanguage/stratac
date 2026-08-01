if(NOT DEFINED STRATAC OR NOT DEFINED SAMPLE_DIR OR NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "RunCliTests.cmake requires STRATAC, SAMPLE_DIR, and OUTPUT_DIR")
endif()

execute_process(
    COMMAND "${STRATAC}" --run "${SAMPLE_DIR}/hello.strata"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_output
    ERROR_VARIABLE run_error
)
if(NOT run_result EQUAL 25)
    message(FATAL_ERROR "--run returned ${run_result}; stdout=${run_output}; stderr=${run_error}")
endif()

set(c_output "${OUTPUT_DIR}/cli-hello.c")
execute_process(
    COMMAND "${STRATAC}" --emit-c "${SAMPLE_DIR}/hello.strata" -o "${c_output}"
    RESULT_VARIABLE c_result
    OUTPUT_VARIABLE c_stdout
    ERROR_VARIABLE c_stderr
)
if(NOT c_result EQUAL 0 OR NOT EXISTS "${c_output}")
    message(FATAL_ERROR "--emit-c failed: ${c_result}; ${c_stdout}; ${c_stderr}")
endif()

execute_process(
    COMMAND "${STRATAC}" --run --entry add "${SAMPLE_DIR}/hello.strata"
    RESULT_VARIABLE signature_result
    ERROR_VARIABLE signature_error
)
if(signature_result EQUAL 0 OR NOT signature_error MATCHES "must be a defined int\\(void\\) function")
    message(FATAL_ERROR "invalid entry signature was not rejected: ${signature_error}")
endif()

execute_process(
    COMMAND "${STRATAC}" --run "${SAMPLE_DIR}/extern_math.strata"
    RESULT_VARIABLE extern_result
    ERROR_VARIABLE extern_error
)
if(extern_result EQUAL 0 OR NOT extern_error MATCHES "cannot resolve host externs")
    message(FATAL_ERROR "unresolved externs were not rejected: ${extern_error}")
endif()

if(NOT HAS_LLVM)
    execute_process(
        COMMAND "${STRATAC}" "${SAMPLE_DIR}/hello.strata" -o "${OUTPUT_DIR}/no-llvm.o"
        RESULT_VARIABLE object_result
        ERROR_VARIABLE object_error
    )
    if(object_result EQUAL 0 OR NOT object_error MATCHES "LLVM backend not built")
        message(FATAL_ERROR "no-LLVM object request was not rejected: ${object_error}")
    endif()
endif()
