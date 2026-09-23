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

# --run with a module-level global: the compiled entry point gains a hidden
# leading context pointer (see the "instanced globals" feature); --run must
# create/pass/destroy one transparently, same UX as a globals-free script.
set(global_run_script "${OUTPUT_DIR}/cli-global-run.strata")
file(WRITE "${global_run_script}"
    "int g_count = 0;\n"
    "void bump() { g_count = g_count + 1; }\n"
    "int main() { bump(); bump(); bump(); return g_count; }\n"
)
execute_process(
    COMMAND "${STRATAC}" --run "${global_run_script}"
    RESULT_VARIABLE global_run_result
    OUTPUT_VARIABLE global_run_output
    ERROR_VARIABLE global_run_error
)
if(NOT global_run_result EQUAL 3)
    message(FATAL_ERROR "--run (globals) returned ${global_run_result}; stdout=${global_run_output}; stderr=${global_run_error}")
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

# --run with globals still validates the user-visible entry signature (the
# hidden context parameter must not bypass the int(void) check).
set(bad_entry_script "${OUTPUT_DIR}/cli-global-bad-entry.strata")
file(WRITE "${bad_entry_script}"
    "int g = 1;\n"
    "float main() { return 1.5; }\n"
)
execute_process(
    COMMAND "${STRATAC}" --run "${bad_entry_script}"
    RESULT_VARIABLE bad_entry_result
    ERROR_VARIABLE bad_entry_error
)
if(bad_entry_result EQUAL 0 OR NOT bad_entry_error MATCHES "must be a defined int\\(void\\) function")
    message(FATAL_ERROR "--run (globals) accepted a non-int(void) entry: ${bad_entry_result}; ${bad_entry_error}")
endif()

# --run, --ast and --emit-ir produce their own output only: no object file.
set(no_obj_script "${OUTPUT_DIR}/cli-no-object.strata")
set(no_obj_object "${OUTPUT_DIR}/cli-no-object.o")
file(WRITE "${no_obj_script}" "int main() { return 0; }\n")
foreach(mode --run --ast --emit-ir)
    file(REMOVE "${no_obj_object}")
    execute_process(
        COMMAND "${STRATAC}" ${mode} "${no_obj_script}"
        RESULT_VARIABLE no_obj_result
        OUTPUT_QUIET
        ERROR_QUIET
    )
    if(NOT no_obj_result EQUAL 0)
        message(FATAL_ERROR "${mode} failed on a valid script: ${no_obj_result}")
    endif()
    if(EXISTS "${no_obj_object}")
        message(FATAL_ERROR "${mode} wrote an object file: ${no_obj_object}")
    endif()
endforeach()

# Compile errors: every mode fails, and each diagnostic is printed once.
set(error_script "${OUTPUT_DIR}/cli-compile-error.strata")
file(WRITE "${error_script}" "int main() { return undefined_name; }\n")
foreach(mode "" --run --ast --emit-ir --asm)
    set(error_args ${mode} "${error_script}")
    if(mode STREQUAL "--asm")
        list(APPEND error_args -o "${OUTPUT_DIR}/cli-compile-error.s")
    endif()
    execute_process(
        COMMAND "${STRATAC}" ${error_args}
        RESULT_VARIABLE error_result
        OUTPUT_VARIABLE error_stdout
        ERROR_VARIABLE error_stderr
    )
    if(error_result EQUAL 0)
        message(FATAL_ERROR "'${mode}' exited 0 on a compile error: ${error_stderr}")
    endif()
    string(REGEX MATCHALL "unknown variable 'undefined_name'" error_hits "${error_stdout}${error_stderr}")
    list(LENGTH error_hits error_hit_count)
    if(NOT error_hit_count EQUAL 1)
        message(FATAL_ERROR "'${mode}' printed the diagnostic ${error_hit_count} times: ${error_stderr}")
    endif()
endforeach()

# Defining a reserved runtime symbol is a clean diagnostic (it used to hang).
set(reserved_script "${OUTPUT_DIR}/cli-reserved-name.strata")
file(WRITE "${reserved_script}"
    "int strata_alloc() { return 1; }\n"
    "int main() { return strata_alloc(); }\n"
)
execute_process(
    COMMAND "${STRATAC}" --run "${reserved_script}"
    RESULT_VARIABLE reserved_result
    ERROR_VARIABLE reserved_error
    TIMEOUT 60
)
if(reserved_result EQUAL 0 OR NOT reserved_error MATCHES "reserved Strata runtime name")
    message(FATAL_ERROR "reserved runtime name was not rejected: ${reserved_result}; ${reserved_error}")
endif()

# --no-simd cannot be honored yet, so it is an explicit error.
execute_process(
    COMMAND "${STRATAC}" --no-simd --emit-ir "${no_obj_script}"
    RESULT_VARIABLE no_simd_result
    ERROR_VARIABLE no_simd_error
)
if(no_simd_result EQUAL 0 OR NOT no_simd_error MATCHES "--no-simd is not supported")
    message(FATAL_ERROR "--no-simd was silently accepted: ${no_simd_result}; ${no_simd_error}")
endif()

# --arch selects the object's target machine.
set(arch_object "${OUTPUT_DIR}/cli-arch-arm64.o")
file(REMOVE "${arch_object}")
execute_process(
    COMMAND "${STRATAC}" --arch arm64 "${no_obj_script}" -o "${arch_object}"
    RESULT_VARIABLE arch_result
    ERROR_VARIABLE arch_error
)
if(NOT arch_result EQUAL 0 OR NOT EXISTS "${arch_object}")
    message(FATAL_ERROR "--arch arm64 failed: ${arch_result}; ${arch_error}")
endif()
file(READ "${arch_object}" arch_header LIMIT 20 HEX)
# COFF machine 0xAA64 (little-endian "64aa"), or ELF EM_AARCH64 (183 = 0xb7),
# or Mach-O CPU_TYPE_ARM64 (0x0100000c).
if(NOT arch_header MATCHES "^64aa" AND NOT arch_header MATCHES "^7f454c46.*b700$"
   AND NOT arch_header MATCHES "^cffaedfe0c000001")
    message(FATAL_ERROR "--arch arm64 produced a non-arm64 object: ${arch_header}")
endif()
file(REMOVE "${arch_object}")
