#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LLDTargetElf = 0, // Linux, FreeBSD
    LLDTargetMacho,   // MacOS
    LLDTargetCoff     // Windows
} LLDTarget;

/**
 * @brief Links object files into an executable or shared library.
 *
 * @param target    LLD_TARGET_ELF or LLD_TARGET_COFF
 * @param argc      Number of command-line style arguments
 * @param argv      Array of argument strings (e.g., {"lld", "main.o", "-o", "main.exe"})
 * @param error_out Pointer to string buffer that will receive stdout/stderr if non-NULL.
 *                  Must be freed with lld_free_string().
 * @return          true if linking succeeded, false on error.
 */
bool LLDLink(LLDTarget target, int argc, const char** argv, char** errorOut);

/**
 * @brief Frees error strings allocated by lld_link.
 */
void LLDFreeString(char* str);


#ifdef __cplusplus
}
#endif
