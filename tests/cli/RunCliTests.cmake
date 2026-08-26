if(NOT DEFINED STRATAC OR NOT DEFINED SAMPLE_DIR OR NOT DEFINED OUTPUT_DIR
   OR NOT DEFINED C_COMPILER OR NOT DEFINED C_COMPILER_ID)
    message(FATAL_ERROR "RunCliTests.cmake is missing a required path or compiler setting")
endif()

# --run: JIT-compile and execute in-process (LLVM backend).
execute_process(
    COMMAND "${STRATAC}" --run "${SAMPLE_DIR}/hello.strata"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_output
    ERROR_VARIABLE run_error
)
if(NOT run_result EQUAL 25)
    message(FATAL_ERROR "--run returned ${run_result}; stdout=${run_output}; stderr=${run_error}")
endif()

# AOT: stratac's default path emits a real native object; link it with the
# engine host and verify the ABI end to end.
set(engine_obj "${OUTPUT_DIR}/cli-engine-api${CMAKE_C_OUTPUT_EXTENSION}")
execute_process(
    COMMAND "${STRATAC}" "${SAMPLE_DIR}/engine_api.strata" -o "${engine_obj}"
    RESULT_VARIABLE engine_obj_result
    ERROR_VARIABLE engine_obj_error
)
if(NOT engine_obj_result EQUAL 0 OR NOT EXISTS "${engine_obj}")
    message(FATAL_ERROR "AOT object emission failed: ${engine_obj_result}; ${engine_obj_error}")
endif()

set(engine_exe "${OUTPUT_DIR}/cli-engine-api${EXE_SUFFIX}")
if(C_COMPILER_ID STREQUAL "MSVC")
    execute_process(
        COMMAND "${C_COMPILER}" /nologo "${SAMPLE_DIR}/hosts/engine_api_host.c" "${engine_obj}" "/Fe:${engine_exe}"
        RESULT_VARIABLE engine_build_result
        OUTPUT_VARIABLE engine_build_stdout
        ERROR_VARIABLE engine_build_stderr
    )
else()
    execute_process(
        COMMAND "${C_COMPILER}" -std=c11 "${SAMPLE_DIR}/hosts/engine_api_host.c" "${engine_obj}" -o "${engine_exe}"
        RESULT_VARIABLE engine_build_result
        OUTPUT_VARIABLE engine_build_stdout
        ERROR_VARIABLE engine_build_stderr
    )
endif()
if(NOT engine_build_result EQUAL 0)
    message(FATAL_ERROR "extern-struct host link failed: ${engine_build_stdout}; ${engine_build_stderr}")
endif()
execute_process(
    COMMAND "${engine_exe}"
    RESULT_VARIABLE engine_result
    OUTPUT_VARIABLE engine_output
)
if(NOT engine_result EQUAL 0 OR NOT engine_output MATCHES "run\\(\\) = -9")
    message(FATAL_ERROR "extern-struct ABI executable failed: ${engine_result}; ${engine_output}")
endif()

# Entry-signature validation.
execute_process(
    COMMAND "${STRATAC}" --run --entry add "${SAMPLE_DIR}/hello.strata"
    RESULT_VARIABLE signature_result
    ERROR_VARIABLE signature_error
)
if(signature_result EQUAL 0 OR NOT signature_error MATCHES "must be a defined int\\(void\\) function")
    message(FATAL_ERROR "invalid entry signature was not rejected: ${signature_error}")
endif()

# Unresolved externs must be rejected at --run time.
execute_process(
    COMMAND "${STRATAC}" --run "${SAMPLE_DIR}/extern_math.strata"
    RESULT_VARIABLE extern_result
    ERROR_VARIABLE extern_error
)
if(extern_result EQUAL 0 OR NOT extern_error MATCHES "cannot resolve host externs")
    message(FATAL_ERROR "unresolved externs were not rejected: ${extern_error}")
endif()
