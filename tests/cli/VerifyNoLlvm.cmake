if(NOT DEFINED STRATAC OR NOT DEFINED COMPILE_COMMANDS)
    message(FATAL_ERROR "VerifyNoLlvm.cmake requires STRATAC and COMPILE_COMMANDS")
endif()

file(READ "${COMPILE_COMMANDS}" commands)
string(TOLOWER "${commands}" commands_lower)
if(commands_lower MATCHES "codegen[/\\\\]llvm"
   OR commands_lower MATCHES "llvm_c_dir"
   OR commands_lower MATCHES "llvm-c\\.(lib|dll|so|dylib)"
   OR commands_lower MATCHES "-l+llvm")
    message(FATAL_ERROR "LLVM source, include, or link input found in no-LLVM compile commands")
endif()

if(DEFINED NM_TOOL AND EXISTS "${NM_TOOL}")
    execute_process(
        COMMAND "${NM_TOOL}" -g "${STRATAC}"
        RESULT_VARIABLE nm_result
        OUTPUT_VARIABLE symbols
        ERROR_VARIABLE nm_error
    )
    if(NOT nm_result EQUAL 0)
        message(FATAL_ERROR "symbol inspection failed: ${nm_error}")
    endif()
    if(symbols MATCHES "[ \t]_?LLVM[A-Za-z0-9_]+([\r\n]|$)")
        message(FATAL_ERROR "LLVM C API symbol found in no-LLVM stratac")
    endif()
    if(NOT symbols MATCHES "[ \t]_?tcc_compile_string([\r\n]|$)")
        message(FATAL_ERROR "vendored libtcc was not incorporated into stratac")
    endif()
endif()

if(DEFINED LDD_TOOL AND EXISTS "${LDD_TOOL}")
    execute_process(
        COMMAND "${LDD_TOOL}" "${STRATAC}"
        RESULT_VARIABLE ldd_result
        OUTPUT_VARIABLE dependencies
        ERROR_VARIABLE ldd_error
    )
    if(NOT ldd_result EQUAL 0)
        message(FATAL_ERROR "dependency inspection failed: ${ldd_error}")
    endif()
    string(TOLOWER "${dependencies}" dependencies_lower)
    if(dependencies_lower MATCHES "llvm")
        message(FATAL_ERROR "LLVM runtime dependency found in no-LLVM stratac: ${dependencies}")
    endif()
    if(dependencies_lower MATCHES "libtcc")
        message(FATAL_ERROR "dynamic libtcc dependency found; TinyCC must be incorporated statically")
    endif()
endif()
