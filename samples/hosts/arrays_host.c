// arrays_host.c -- host side of arrays.strata.
//
//   stratac arrays.strata -o arrays.o
//   clang hosts/arrays_host.c arrays.o -o arrays.exe
//   ./arrays.exe; echo $?
#include <stdio.h>
#include <stdlib.h>

// Strata runtime: array/box/string heap allocation
void* strata_alloc(size_t n) { return malloc(n); }
void strata_free(void* p) { free(p); }
void strata_panic(const char* msg) { fprintf(stderr, "strata panic: %s\n", msg); abort(); }

// Strata-provided entry (defined in arrays.strata). The module declares a
// module-level global (testGlobalString), so the compiler gives every
// non-extern function a hidden leading context pointer -- invisible in the
// .strata source, but a real first parameter here.
extern int entry(void* ctx);
extern void* __strata_context_create(void);
extern void __strata_context_destroy(void*);

int main(void)
{
    void* ctx = __strata_context_create();
    int result = entry(ctx);
    printf("entry() = %d\n", result);
    __strata_context_destroy(ctx);
    return 0;
}
