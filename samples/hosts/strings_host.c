// strings_host.c -- host side of strings.strata.
//
//   stratac strings.strata -o strings.o
//   clang hosts/strings_host.c strings.o -o strings.exe
//   ./strings.exe; echo $?
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Strata runtime: box/string heap allocation
void* strata_alloc(unsigned long n) { return malloc((size_t)n); }
void strata_free(void* p) { free(p); }
void strata_panic(const char* msg) { fprintf(stderr, "strata panic: %s\n", msg); abort(); }

// Strata-provided entry (defined in strings.o). The module declares
// module-level globals (globalString, boxedString), so the compiler gives
// every non-extern function a hidden leading context pointer -- invisible
// in the .strata source, but a real first parameter here.
extern int run(void* ctx);
extern void* __strata_context_create(void);
extern void __strata_context_destroy(void*);

int main(void)
{
    void* ctx = __strata_context_create();
    int result = run(ctx);
    printf("run() = %d\n", result);
    __strata_context_destroy(ctx);
    return 0;
}
