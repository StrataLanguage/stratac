if(NOT DEFINED BINARY)
    message(FATAL_ERROR "ReportBinarySize.cmake requires BINARY")
endif()

file(SIZE "${BINARY}" binary_size)
message(STATUS "Stripped binary: ${BINARY}")
message(STATUS "File size: ${binary_size} bytes")
