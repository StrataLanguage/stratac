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

option(STRATA_SHARED "Build Strata as a shared library (DLL)" OFF)
option(STRATA_ENABLE_LLVM "Build the LLVM IR, AOT, and LLVM JIT backends" ON)
option(STRATA_ENABLE_TCC "Build the vendored TinyCC runtime backend" ON)
option(STRATA_STATIC_RUNTIME "Optimize a static, self-contained Strata runtime" OFF)

set(STRATA_COMMON_SOURCES
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
    src/Codegen/CBackend.c
    src/Import/ModuleLoader.c
    src/Embed.c
)

if(STRATA_ENABLE_TCC)
    list(APPEND STRATA_COMMON_SOURCES src/Codegen/TccJit.c)
endif()

set(STRATA_LLVM_SOURCES
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
    STRATA_HAS_TCC=$<BOOL:${STRATA_ENABLE_TCC}>
)

if(STRATA_ENABLE_TCC)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "The embedded TinyCC backend currently requires a 64-bit target")
    endif()

    set(TCC_PREDEFS_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/tinycc/tccdefs_.h")
    add_custom_command(
        OUTPUT "${TCC_PREDEFS_HEADER}"
        COMMAND ${CMAKE_COMMAND}
                -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/third_party/tinycc/include/tccdefs.h
                -DOUTPUT=${TCC_PREDEFS_HEADER}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateTccPredefs.cmake
        DEPENDS
            third_party/tinycc/include/tccdefs.h
            cmake/GenerateTccPredefs.cmake
        VERBATIM
    )

    add_library(strata_tinycc STATIC
        third_party/tinycc/libtcc.c
        "${TCC_PREDEFS_HEADER}"
    )
    set_target_properties(strata_tinycc PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS ON
        POSITION_INDEPENDENT_CODE ON
    )
    target_include_directories(strata_tinycc PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/generated/tinycc"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tinycc"
    )
    target_include_directories(strata PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tinycc"
    )
    target_compile_definitions(strata_tinycc PRIVATE
        ONE_SOURCE=1
        CONFIG_TCC_STATIC=1
    )

    if(WIN32)
        target_compile_definitions(strata_tinycc PRIVATE
            TCC_TARGET_X86_64=1
            TCC_TARGET_PE=1
        )
    elseif(APPLE)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
            target_compile_definitions(strata_tinycc PRIVATE TCC_TARGET_ARM64=1)
        else()
            target_compile_definitions(strata_tinycc PRIVATE TCC_TARGET_X86_64=1)
        endif()
        target_compile_definitions(strata_tinycc PRIVATE TCC_TARGET_MACHO=1)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
        target_compile_definitions(strata_tinycc PRIVATE TCC_TARGET_X86_64=1)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
        target_compile_definitions(strata_tinycc PRIVATE TCC_TARGET_ARM64=1)
    else()
        message(FATAL_ERROR "Unsupported TinyCC target: ${CMAKE_SYSTEM_PROCESSOR}")
    endif()

    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(strata_tinycc PRIVATE
            -Wno-error
            -Wno-implicit-fallthrough
            -Wno-missing-field-initializers
            -Wno-pointer-sign
            -Wno-sign-compare
            -Wno-unused-function
            -Wno-unused-parameter
        )
    endif()

    if(UNIX AND NOT APPLE)
        target_link_libraries(strata_tinycc PUBLIC m)
    endif()
    target_link_libraries(strata PRIVATE strata_tinycc)
endif()

if(STRATA_ENABLE_LLVM)
    find_package(LLVM REQUIRED CONFIG)

    # If there is no custom path provided, use the path pulled by find_package
    if(LLVM_C_DIR STREQUAL "")
        get_filename_component(LLVM_C_DIR "${LLVM_DIR}/../../.." ABSOLUTE)
    endif()

    if(APPLE)
        set(LLVM_C_LIB "${LLVM_C_DIR}/lib/libLLVM-C.dylib")
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
        set(STRATA_LLVM_DLL "${LLVM_C_DIR}/lib/libLLVM-C.dylib" CACHE FILEPATH "Path to LLVM-C")
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

add_executable(jit_demo samples/hosts/jit_demo_host.c)
target_link_libraries(jit_demo PRIVATE strata)
target_compile_definitions(jit_demo PRIVATE
    STRATA_SAMPLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/samples")
strata_stage_dll(jit_demo)

add_executable(engine_demo samples/hosts/engine_demo_host.c)
target_link_libraries(engine_demo PRIVATE strata)
target_compile_definitions(engine_demo PRIVATE
    USE_JIT
    STRATA_SAMPLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/samples")
strata_stage_dll(engine_demo)

enable_testing()

if(NOT STRATA_SHARED)
set(STRATA_TEST_SOURCES
    tests/unit/test_main.c
    tests/unit/LexerTests.c
    tests/unit/ParserTests.c
    tests/unit/CBackendTests.c
)
if(STRATA_ENABLE_TCC)
    list(APPEND STRATA_TEST_SOURCES tests/unit/TccJitTests.c)
endif()
if(STRATA_ENABLE_LLVM)
    list(APPEND STRATA_TEST_SOURCES
        tests/unit/CodegenTests.c
        tests/unit/EmbedTests.c
        tests/unit/SampleTests.c
        tests/unit/JitTests.c
        tests/unit/StructTests.c
        tests/unit/OverloadTests.c
        tests/unit/ControlFlowTests.c
        tests/unit/ImportTests.c
        tests/unit/OperatorTests.c
    )
endif()
add_executable(strata_tests ${STRATA_TEST_SOURCES})
target_link_libraries(strata_tests PRIVATE strata)
target_include_directories(strata_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit)
target_compile_definitions(strata_tests PRIVATE
    STRATA_SAMPLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/samples")
if(EXISTS "${STRATA_LLVM_DLL}")
    add_custom_command(TARGET strata_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${STRATA_LLVM_DLL}" "$<TARGET_FILE_DIR:strata_tests>")
endif()
add_test(NAME strata_tests COMMAND strata_tests)

set_target_properties(stratac strata_tests jit_demo engine_demo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
