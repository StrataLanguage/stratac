#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Links object files into an executable or shared library.
 *
 * @param argc      Number of command-line style arguments
 * @param argv      Array of argument strings (e.g., {"lld", "main.o", "-o", "main.exe"})
 * @returns         Error message on error, `NULL` on success
 */
char* LLDLink(int argc, const char** argv);

/**
 * @brief Frees error strings allocated by lld_link.
 */
void LLDFreeString(char* str);


#ifdef __cplusplus
}
#endif
