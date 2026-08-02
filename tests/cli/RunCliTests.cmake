if(NOT DEFINED STRATAC OR NOT DEFINED SAMPLE_DIR OR NOT DEFINED OUTPUT_DIR
   OR NOT DEFINED C_COMPILER OR NOT DEFINED C_COMPILER_ID)
    message(FATAL_ERROR "RunCliTests.cmake is missing a required path or compiler setting")
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

set(hello_exe "${OUTPUT_DIR}/cli-hello${EXE_SUFFIX}")
if(C_COMPILER_ID STREQUAL "MSVC")
    execute_process(
        COMMAND "${C_COMPILER}" /nologo "${c_output}" "/Fe:${hello_exe}"
        RESULT_VARIABLE host_c_result
        OUTPUT_VARIABLE host_c_stdout
        ERROR_VARIABLE host_c_stderr
    )
else()
    execute_process(
        COMMAND "${C_COMPILER}" -std=c11 "${c_output}" -o "${hello_exe}"
        RESULT_VARIABLE host_c_result
        OUTPUT_VARIABLE host_c_stdout
        ERROR_VARIABLE host_c_stderr
    )
endif()
if(NOT host_c_result EQUAL 0)
    message(FATAL_ERROR "host compiler rejected generated C: ${host_c_stdout}; ${host_c_stderr}")
endif()
execute_process(COMMAND "${hello_exe}" RESULT_VARIABLE hello_result)
if(NOT hello_result EQUAL 25)
    message(FATAL_ERROR "host-compiled generated C returned ${hello_result}, expected 25")
endif()

set(engine_c "${OUTPUT_DIR}/cli-engine-api.c")
execute_process(
    COMMAND "${STRATAC}" --emit-c "${SAMPLE_DIR}/engine_api.strata" -o "${engine_c}"
    RESULT_VARIABLE engine_c_result
    ERROR_VARIABLE engine_c_error
)
if(NOT engine_c_result EQUAL 0)
    message(FATAL_ERROR "engine ABI C generation failed: ${engine_c_error}")
endif()
set(engine_exe "${OUTPUT_DIR}/cli-engine-api${EXE_SUFFIX}")
if(C_COMPILER_ID STREQUAL "MSVC")
    execute_process(
        COMMAND "${C_COMPILER}" /nologo "${engine_c}"
                "${SAMPLE_DIR}/hosts/engine_api_host.c" "/Fe:${engine_exe}"
        RESULT_VARIABLE engine_build_result
        OUTPUT_VARIABLE engine_build_stdout
        ERROR_VARIABLE engine_build_stderr
    )
else()
    execute_process(
        COMMAND "${C_COMPILER}" -std=c11 "${engine_c}"
                "${SAMPLE_DIR}/hosts/engine_api_host.c" -o "${engine_exe}"
        RESULT_VARIABLE engine_build_result
        OUTPUT_VARIABLE engine_build_stdout
        ERROR_VARIABLE engine_build_stderr
    )
endif()
if(NOT engine_build_result EQUAL 0)
    message(FATAL_ERROR "extern-struct ABI C link failed: ${engine_build_stdout}; ${engine_build_stderr}")
endif()
execute_process(
    COMMAND "${engine_exe}"
    RESULT_VARIABLE engine_result
    OUTPUT_VARIABLE engine_output
)
if(NOT engine_result EQUAL 0 OR NOT engine_output MATCHES "run\\(\\) = -9")
    message(FATAL_ERROR "extern-struct ABI executable failed: ${engine_result}; ${engine_output}")
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
