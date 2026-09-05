cmake_minimum_required(VERSION 3.20)

project(strata LANGUAGES C VERSION 0.1.0 DESCRIPTION "Strata scripting language")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(LLVM_C_DIR "" CACHE PATH
    "Directory of the x64 LLVM distribution (lib/LLVM-C.*, bin/LLVM-C.dll)")

# allow env var to set LLVM_C_DIR
if(DEFINED ENV{LLVM_DIR})
    message(STATUS "Using environment LLVM_DIR: $ENV{LLVM_DIR}")
    set(LLVM_C_DIR "$ENV{LLVM_DIR}")
endif()

option(STRATA_SHARED "Build Strata as a shared library (DLL)" OFF)
option(STRATA_ENABLE_LLVM "Build the LLVM IR, AOT, and LLVM JIT backends" ON)
option(STRATA_STATIC_RUNTIME "Optimize a static, self-contained Strata runtime" OFF)

set(STRATA_COMMON_SOURCES
    src/AST/AST.c
    src/Core/Util.c
    src/Core/SourceManager.c
    src/Core/Diagnostics.c
    src/Lex/Token.c
    src/Lex/Lexer.c
    src/Parse/Parser.c
    src/Sema/ResolveOverloads.c
    src/Codegen/TypeRegistry.c
    src/Codegen/TypeUtil.c
    src/Codegen/ASTDump.c
    src/Import/ModuleLoader.c
    src/Embed.c
)

set(STRATA_LLVM_SOURCES
    src/Codegen/LLVMSimd.c
    src/Codegen/LLVMCBackend.c
    src/Codegen/LLVMModuleBuilder.c
    src/Codegen/LLVMAot.c
    src/Codegen/LLVMJit.c
)

set(STRATA_SOURCES ${STRATA_COMMON_SOURCES})
if(STRATA_ENABLE_LLVM)
    list(APPEND STRATA_SOURCES ${STRATA_LLVM_SOURCES})
endif()

if(STRATA_SHARED)
    add_library(strata SHARED ${STRATA_SOURCES})
    target_compile_definitions(strata PRIVATE STRATA_EXPORTS)
    set_target_properties(strata PROPERTIES PREFIX "")
else()
    add_library(strata STATIC ${STRATA_SOURCES})
    target_compile_definitions(strata PUBLIC STRATA_STATIC)
endif()
target_include_directories(strata PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(strata PRIVATE
    STRATA_HAS_LLVM=$<BOOL:${STRATA_ENABLE_LLVM}>
)

if(STRATA_ENABLE_LLVM)
    # If there is no custom path provided, use the path pulled by find_package
    if(LLVM_C_DIR STREQUAL "")
        find_package(LLVM REQUIRED CONFIG)

        get_filename_component(LLVM_C_DIR "${LLVM_DIR}/../../.." ABSOLUTE)
    endif()

    if(APPLE)
        set(LLVM_C_LIB "${LLVM_C_DIR}/lib/libLLVM.dylib")
    elseif(WIN32)
        set(LLVM_C_LIB "${LLVM_C_DIR}/lib/LLVM-C.lib")
    else()
        find_library(LLVM_C_LIB
            NAMES LLVM
            HINTS ${LLVM_LIBRARY_DIRS}
            REQUIRED)
    endif()

    target_include_directories(strata PRIVATE ${LLVM_C_DIR}/include ${LLVM_INCLUDE_DIRS})
    target_link_libraries(strata PUBLIC ${LLVM_C_LIB})

    if(APPLE)
        set(STRATA_LLVM_DLL "${LLVM_C_DIR}/lib/libLLVM.dylib" CACHE FILEPATH "Path to LLVM")
    elseif(WIN32)
        set(STRATA_LLVM_DLL "${LLVM_C_DIR}/bin/LLVM-C.dll" CACHE FILEPATH "Path to LLVM-C")
    endif()
endif()

function(strata_stage_dll target)
    if(STRATA_ENABLE_LLVM AND EXISTS "${STRATA_LLVM_DLL}")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${STRATA_LLVM_DLL}" "$<TARGET_FILE_DIR:${target}>")
    endif()
    if(STRATA_SHARED AND TARGET strata)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:strata>" "$<TARGET_FILE_DIR:${target}>")
    endif()
endfunction()

add_executable(stratac src/stratac/main.c)
target_link_libraries(stratac PRIVATE strata)
strata_stage_dll(stratac)

if(STRATA_STATIC_RUNTIME)
    set_property(TARGET strata PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    set_property(TARGET stratac PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(strata PRIVATE -ffunction-sections -fdata-sections)
        target_link_options(stratac PRIVATE -Wl,--gc-sections)
    elseif(MSVC)
        target_compile_options(strata PRIVATE /Gy /Gw)
        target_link_options(stratac PRIVATE /OPT:REF /OPT:ICF)
    endif()
endif()

set(STRATA_SIZE_COPY
    "${CMAKE_BINARY_DIR}/bin/$<TARGET_FILE_BASE_NAME:stratac>-stripped$<TARGET_FILE_SUFFIX:stratac>")
add_custom_target(stratac-size-report
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:stratac>" "${STRATA_SIZE_COPY}"
    DEPENDS stratac
    VERBATIM
)
if(CMAKE_STRIP)
    add_custom_command(TARGET stratac-size-report POST_BUILD
        COMMAND "${CMAKE_STRIP}" "${STRATA_SIZE_COPY}"
        VERBATIM
    )
endif()
find_program(STRATA_SIZE_TOOL NAMES llvm-size size)
if(STRATA_SIZE_TOOL)
    add_custom_command(TARGET stratac-size-report POST_BUILD
        COMMAND "${STRATA_SIZE_TOOL}" "${STRATA_SIZE_COPY}"
        VERBATIM
    )
endif()
add_custom_command(TARGET stratac-size-report POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DBINARY=${STRATA_SIZE_COPY}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ReportBinarySize.cmake
    VERBATIM
)
if(UNIX AND NOT APPLE)
    find_program(STRATA_LDD_TOOL ldd)
    if(STRATA_LDD_TOOL)
        add_custom_command(TARGET stratac-size-report POST_BUILD
            COMMAND "${STRATA_LDD_TOOL}" "${STRATA_SIZE_COPY}"
            VERBATIM
        )
    endif()
endif()

add_executable(jit_demo samples/hosts/jit_demo_host.c)
target_link_libraries(jit_demo PRIVATE strata)
target_compile_definitions(jit_demo PRIVATE
    STRATA_SAMPLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/samples")
strata_stage_dll(jit_demo)

add_executable(opt_bench samples/hosts/opt_bench_host.c)
target_link_libraries(opt_bench PRIVATE strata)
strata_stage_dll(opt_bench)

add_executable(engine_demo samples/hosts/engine_demo_host.c)
target_link_libraries(engine_demo PRIVATE strata)
target_compile_definitions(engine_demo PRIVATE
    USE_JIT
    STRATA_SAMPLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/samples")
strata_stage_dll(engine_demo)

add_executable(box_demo samples/hosts/box_demo_host.c)
target_link_libraries(box_demo PRIVATE strata)
target_compile_definitions(box_demo PRIVATE
    STRATA_SAMPLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/samples")
strata_stage_dll(box_demo)

add_executable(optional_demo samples/hosts/optional_demo_host.c)
target_link_libraries(optional_demo PRIVATE strata)
target_compile_definitions(optional_demo PRIVATE
    STRATA_SAMPLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/samples")
set_target_properties(optional_demo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
strata_stage_dll(optional_demo)

add_executable(simd_demo samples/hosts/simd_demo_host.c)
target_link_libraries(simd_demo PRIVATE strata)
target_compile_definitions(simd_demo PRIVATE
    STRATA_SAMPLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/samples")
strata_stage_dll(simd_demo)

add_executable(readme_demo samples/hosts/readme_demo_host.c)
target_link_libraries(readme_demo PRIVATE strata)
target_compile_definitions(readme_demo PRIVATE
    STRATA_SAMPLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/samples")
strata_stage_dll(readme_demo)

enable_testing()

if(NOT STRATA_SHARED)
    set(STRATA_TEST_SOURCES
        tests/unit/test_main.c
        tests/unit/LexerTests.c
        tests/unit/ParserTests.c
        tests/unit/CapabilityTests.c
        tests/unit/EmbedTests.c
        tests/unit/OverloadTests.c
        tests/unit/ImplTests.c
    )

    # These exercise the public strataJit* embed API (LLVM/ORCv2 - see
    # src/Embed.c).
    if(STRATA_ENABLE_LLVM)
        list(APPEND STRATA_TEST_SOURCES
            tests/unit/JitTests.c
            tests/unit/StructTests.c
            tests/unit/ControlFlowTests.c
            tests/unit/ImportTests.c
            tests/unit/OperatorTests.c
            tests/unit/BoxTests.c
            tests/unit/StringTests.c
            tests/unit/ArrayTests.c
            tests/unit/VectorTests.c
            tests/unit/ConstRefTests.c
            tests/unit/OptionalTests.c
            tests/unit/VarargsTests.c
            tests/unit/ExternTests.c
            tests/unit/ProfileTests.c
            tests/unit/CodegenTests.c
            tests/unit/SampleTests.c
            tests/unit/ExternStructTests.c
            tests/unit/TypeAliasTests.c
            tests/unit/EnumTests.c
        )
    endif()
    add_executable(strata_tests ${STRATA_TEST_SOURCES})
    target_link_libraries(strata_tests PRIVATE strata)
    target_include_directories(strata_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit)
    target_compile_definitions(strata_tests PRIVATE
        STRATA_SAMPLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/samples"
        STRATA_TEST_HAS_LLVM=$<BOOL:${STRATA_ENABLE_LLVM}>
    )
    strata_stage_dll(strata_tests)
    add_test(NAME strata_tests COMMAND strata_tests)
    if(STRATA_ENABLE_LLVM)
        add_test(NAME stratac_cli_tests
            COMMAND ${CMAKE_COMMAND}
                -DSTRATAC=$<TARGET_FILE:stratac>
                -DSAMPLE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/samples
                -DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}
                -DHAS_LLVM=$<BOOL:${STRATA_ENABLE_LLVM}>
                -DC_COMPILER=${CMAKE_C_COMPILER}
                -DC_COMPILER_ID=${CMAKE_C_COMPILER_ID}
                -DEXE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli/RunCliTests.cmake
        )
    endif()

    set_target_properties(strata_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

if(STRATA_ENABLE_LLVM)
    # AOT smoke test: stratac's default path is a real
    # LLVM object, linked here with a plain C host - not routed through the JIT at all.
    set(STRATA_AOT_TEST_OBJECT "${CMAKE_CURRENT_BINARY_DIR}/aot_test${CMAKE_C_OUTPUT_EXTENSION}")
    add_custom_command(
        OUTPUT "${STRATA_AOT_TEST_OBJECT}"
        COMMAND $<TARGET_FILE:stratac> "${CMAKE_CURRENT_SOURCE_DIR}/samples/engine_api.strata"
                -o "${STRATA_AOT_TEST_OBJECT}"
        DEPENDS stratac "${CMAKE_CURRENT_SOURCE_DIR}/samples/engine_api.strata"
        COMMENT "stratac: AOT-compiling engine_api.strata to a native object"
        VERBATIM
    )
    add_executable(aot_test samples/hosts/engine_api_host.c "${STRATA_AOT_TEST_OBJECT}")
    set_target_properties(aot_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    add_test(NAME aot_test COMMAND aot_test)
    set_tests_properties(aot_test PROPERTIES PASS_REGULAR_EXPRESSION "run\\(\\) = -9")

    set(STRATA_STRING_TEST_OBJECT "${CMAKE_CURRENT_BINARY_DIR}/string_test${CMAKE_C_OUTPUT_EXTENSION}")
    add_custom_command(
        OUTPUT "${STRATA_STRING_TEST_OBJECT}"
        COMMAND $<TARGET_FILE:stratac> "${CMAKE_CURRENT_SOURCE_DIR}/samples/strings.strata"
                -o "${STRATA_STRING_TEST_OBJECT}"
        DEPENDS stratac "${CMAKE_CURRENT_SOURCE_DIR}/samples/strings.strata"
        COMMENT "stratac: AOT-compiling strings.strata to a native object"
        VERBATIM
    )
    add_executable(aot_strings_test samples/hosts/strings_host.c "${STRATA_STRING_TEST_OBJECT}")
    set_target_properties(aot_strings_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    add_test(NAME aot_strings_test COMMAND aot_strings_test)
    set_tests_properties(aot_strings_test PROPERTIES PASS_REGULAR_EXPRESSION "run\\(\\) = 7")

    set(STRATA_ARRAY_TEST_OBJECT "${CMAKE_CURRENT_BINARY_DIR}/array_test${CMAKE_C_OUTPUT_EXTENSION}")
    add_custom_command(
        OUTPUT "${STRATA_ARRAY_TEST_OBJECT}"
        COMMAND $<TARGET_FILE:stratac> "${CMAKE_CURRENT_SOURCE_DIR}/samples/arrays.strata"
                -o "${STRATA_ARRAY_TEST_OBJECT}"
        DEPENDS stratac "${CMAKE_CURRENT_SOURCE_DIR}/samples/arrays.strata"
        COMMENT "stratac: AOT-compiling arrays.strata to a native object"
        VERBATIM
    )
    add_executable(aot_arrays_test samples/hosts/arrays_host.c "${STRATA_ARRAY_TEST_OBJECT}")
    set_target_properties(aot_arrays_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    add_test(NAME aot_arrays_test COMMAND aot_arrays_test)
    set_tests_properties(aot_arrays_test PROPERTIES PASS_REGULAR_EXPRESSION "entry\\(\\) = 54")

    set(STRATA_VARARGS_TEST_OBJECT "${CMAKE_CURRENT_BINARY_DIR}/varargs_test${CMAKE_C_OUTPUT_EXTENSION}")
    add_custom_command(
        OUTPUT "${STRATA_VARARGS_TEST_OBJECT}"
        COMMAND $<TARGET_FILE:stratac> "${CMAKE_CURRENT_SOURCE_DIR}/samples/varargs.strata"
                -o "${STRATA_VARARGS_TEST_OBJECT}"
        DEPENDS stratac "${CMAKE_CURRENT_SOURCE_DIR}/samples/varargs.strata"
        COMMENT "stratac: AOT-compiling varargs.strata to a native object"
        VERBATIM
    )
    add_executable(aot_varargs_test samples/hosts/varargs_host.c "${STRATA_VARARGS_TEST_OBJECT}")
    set_target_properties(aot_varargs_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    add_test(NAME aot_varargs_test COMMAND aot_varargs_test)
    set_tests_properties(aot_varargs_test PROPERTIES PASS_REGULAR_EXPRESSION "entry\\(\\) = 63")

    set(STRATA_EXTERN_STRUCT_TEST_OBJECT "${CMAKE_CURRENT_BINARY_DIR}/extern_struct_test${CMAKE_C_OUTPUT_EXTENSION}")
    add_custom_command(
        OUTPUT "${STRATA_EXTERN_STRUCT_TEST_OBJECT}"
        COMMAND $<TARGET_FILE:stratac> "${CMAKE_CURRENT_SOURCE_DIR}/samples/extern_struct.strata"
                -o "${STRATA_EXTERN_STRUCT_TEST_OBJECT}"
        DEPENDS stratac "${CMAKE_CURRENT_SOURCE_DIR}/samples/extern_struct.strata"
        COMMENT "stratac: AOT-compiling extern_struct.strata to a native object"
        VERBATIM
    )
    add_executable(aot_extern_struct_test samples/hosts/extern_struct_host.c "${STRATA_EXTERN_STRUCT_TEST_OBJECT}")
    set_target_properties(aot_extern_struct_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    add_test(NAME aot_extern_struct_test COMMAND aot_extern_struct_test)
    set_tests_properties(aot_extern_struct_test PROPERTIES PASS_REGULAR_EXPRESSION "entry\\(\\) = 183")

    set(STRATA_EXTERN_LAYOUT_TEST_OBJECT "${CMAKE_CURRENT_BINARY_DIR}/extern_layout_test${CMAKE_C_OUTPUT_EXTENSION}")
    add_custom_command(
        OUTPUT "${STRATA_EXTERN_LAYOUT_TEST_OBJECT}"
        COMMAND $<TARGET_FILE:stratac> "${CMAKE_CURRENT_SOURCE_DIR}/samples/extern_layout.strata"
                -o "${STRATA_EXTERN_LAYOUT_TEST_OBJECT}"
        DEPENDS stratac "${CMAKE_CURRENT_SOURCE_DIR}/samples/extern_layout.strata"
        COMMENT "stratac: AOT-compiling extern_layout.strata to a native object"
        VERBATIM
    )
    add_executable(aot_extern_layout_test samples/hosts/extern_layout_host.c "${STRATA_EXTERN_LAYOUT_TEST_OBJECT}")
    set_target_properties(aot_extern_layout_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    add_test(NAME aot_extern_layout_test COMMAND aot_extern_layout_test)
    set_tests_properties(aot_extern_layout_test PROPERTIES PASS_REGULAR_EXPRESSION "entry\\(\\) = 385")

    set(STRATA_OPAQUE_STRUCT_TEST_OBJECT "${CMAKE_CURRENT_BINARY_DIR}/opaque_struct_test${CMAKE_C_OUTPUT_EXTENSION}")
    add_custom_command(
        OUTPUT "${STRATA_OPAQUE_STRUCT_TEST_OBJECT}"
        COMMAND $<TARGET_FILE:stratac> "${CMAKE_CURRENT_SOURCE_DIR}/samples/opaque_struct.strata"
                -o "${STRATA_OPAQUE_STRUCT_TEST_OBJECT}"
        DEPENDS stratac "${CMAKE_CURRENT_SOURCE_DIR}/samples/opaque_struct.strata"
        COMMENT "stratac: AOT-compiling opaque_struct.strata to a native object"
        VERBATIM
    )
    add_executable(aot_opaque_struct_test samples/hosts/opaque_struct_host.c "${STRATA_OPAQUE_STRUCT_TEST_OBJECT}")
    set_target_properties(aot_opaque_struct_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    add_test(NAME aot_opaque_struct_test COMMAND aot_opaque_struct_test)
    set_tests_properties(aot_opaque_struct_test PROPERTIES PASS_REGULAR_EXPRESSION "entry\\(\\) = 87")
endif()

set_target_properties(stratac jit_demo engine_demo box_demo simd_demo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
