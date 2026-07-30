cmake_minimum_required(VERSION 3.20)

project(strata LANGUAGES CXX VERSION 0.1.0 DESCRIPTION "Strata scripting language")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(STRATA_ENABLE_LLVM "Link the in-process LLVM C API back-end" ON)
set(LLVM_C_DIR "C:/Users/andre/llvm-x64" CACHE PATH
    "Directory of the x64 LLVM distribution (must contain lib/LLVM-C.* and bin/LLVM-C.dll)")

# ---------------------------------------------------------------------------
# Strata core library
# ---------------------------------------------------------------------------
set(STRATA_SOURCES
    src/Core/SourceManager.cpp
    src/Core/Diagnostics.cpp
    src/Lex/Token.cpp
    src/Lex/Lexer.cpp
    src/Parse/Parser.cpp
    src/Sema/ResolveOverloads.cpp
    src/Codegen/ASTDump.cpp
    src/Codegen/CodegenBackend.cpp
    src/Codegen/IRTextBackend.cpp
    src/Embed.cpp
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
        src/Codegen/LLVMCBackend.cpp
        src/Codegen/LLVMModuleBuilder.cpp
        src/Codegen/LLVMAot.cpp
        src/Codegen/LLVMJit.cpp)
    target_include_directories(strata PRIVATE ${LLVM_C_DIR}/include)
    target_compile_definitions(strata PUBLIC STRATA_ENABLE_LLVM)
    target_link_libraries(strata PUBLIC ${LLVM_C_LIB})
    set(STRATA_LLVM_DLL "${LLVM_C_DIR}/bin/LLVM-C.dll" CACHE FILEPATH "Path to LLVM-C.dll")
endif()

# ---------------------------------------------------------------------------
# Copy the LLVM runtime DLL next to executables so they can run from the build
# tree (Windows resolves the DLL relative to the .exe).
# ---------------------------------------------------------------------------
function(strata_stage_dll target)
    if(STRATA_ENABLE_LLVM AND EXISTS "${STRATA_LLVM_DLL}")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${STRATA_LLVM_DLL}" "$<TARGET_FILE_DIR:${target}>")
        message(STATUS "Will stage LLVM-C.dll for ${target}")
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Compiler driver
# ---------------------------------------------------------------------------
add_executable(stratac src/stratac/main.cpp)
target_link_libraries(stratac PRIVATE strata)
strata_stage_dll(stratac)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()

add_executable(strata_tests
    tests/unit/test_main.cpp
    tests/unit/LexerTests.cpp
    tests/unit/ParserTests.cpp
    tests/unit/CodegenTests.cpp
    tests/unit/EmbedTests.cpp
    tests/unit/SampleTests.cpp
    tests/unit/JitTests.cpp
    tests/unit/StructTests.cpp
    tests/unit/OverloadTests.cpp
)
target_link_libraries(strata_tests PRIVATE strata)
target_compile_definitions(strata_tests PRIVATE
    STRATA_SAMPLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/samples")
strata_stage_dll(strata_tests)
add_test(NAME strata_tests COMMAND strata_tests)

set_target_properties(stratac strata_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
