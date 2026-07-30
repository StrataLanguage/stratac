cmake_minimum_required(VERSION 3.20)

project(strata LANGUAGES C VERSION 0.1.0 DESCRIPTION "Strata scripting language")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(STRATA_ENABLE_LLVM "Link the in-process LLVM C API back-end" ON)
set(LLVM_C_DIR "" CACHE PATH
    "Directory of the x64 LLVM distribution (lib/LLVM-C.*, bin/LLVM-C.dll)")

set(STRATA_SOURCES
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
    src/Codegen/CodegenBackend.c
    src/Embed.c
)

add_library(strata STATIC ${STRATA_SOURCES})
target_include_directories(strata PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(STRATA_ENABLE_LLVM)
    find_library(LLVM_C_LIB
        NAMES LLVM-C LLVM-C.lib
        PATHS ${LLVM_C_DIR}/lib
        NO_DEFAULT_PATH REQUIRED)
    target_sources(strata PRIVATE
        src/Codegen/LLVMCBackend.c
        src/Codegen/LLVMModuleBuilder.c
        src/Codegen/LLVMAot.c
        src/Codegen/LLVMJit.c)
    target_include_directories(strata PRIVATE ${LLVM_C_DIR}/include)
    target_compile_definitions(strata PUBLIC STRATA_ENABLE_LLVM)
    target_link_libraries(strata PUBLIC ${LLVM_C_LIB})
    set(STRATA_LLVM_DLL "${LLVM_C_DIR}/bin/LLVM-C.dll" CACHE FILEPATH "Path to LLVM-C.dll")
endif()

function(strata_stage_dll target)
    if(STRATA_ENABLE_LLVM AND EXISTS "${STRATA_LLVM_DLL}")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${STRATA_LLVM_DLL}" "$<TARGET_FILE_DIR:${target}>")
    endif()
endfunction()

add_executable(stratac src/stratac/main.c)
target_link_libraries(stratac PRIVATE strata)
strata_stage_dll(stratac)

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

add_executable(strata_tests
    tests/unit/test_main.c
    tests/unit/LexerTests.c
    tests/unit/ParserTests.c
    tests/unit/CodegenTests.c
    tests/unit/EmbedTests.c
    tests/unit/SampleTests.c
    tests/unit/JitTests.c
    tests/unit/StructTests.c
    tests/unit/OverloadTests.c
    tests/unit/ControlFlowTests.c
)
target_link_libraries(strata_tests PRIVATE strata)
target_compile_definitions(strata_tests PRIVATE
    STRATA_SAMPLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/samples")
strata_stage_dll(strata_tests)
add_test(NAME strata_tests COMMAND strata_tests)

set_target_properties(stratac strata_tests jit_demo engine_demo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
