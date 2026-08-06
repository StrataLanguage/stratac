#include "../third_party/tinycc/libtcc.h"
#include "../third_party/tinycc/tcc.h"

#include <stdio.h>
#include <stdlib.h>

// Define a struct matching the 128-bit vector layout
typedef struct
{
    float v[4];
} float32x4_t;

// Define a function pointer matching: float32x4_t f(float)
typedef float (*func_f)(float);

int main(void)
{
    TCCState* s = tcc_new();
    if (!s)
    {
        fprintf(stderr, "Failed to create TCC state\n");
        return 1;
    }

    tcc_set_output_type(s, TCC_OUTPUT_EXE);
    tcc_set_options(s, "-nostdlib -nostdinc");

    const char* code = "typedef struct { float v[4]; } float32x4_t;\n"
                       "float f(float a) {\n"
                       "    float32x4_t r;\n"
                       "    __asm__(\"dup %0.4s, %1.s[0]\" : \"=w\"(r) : \"w\"(a));\n"
                       "    return r.v[2];\n"
                       "}\n"
                       "int main() { return (int)f(30.0f); }\n";

    if (tcc_compile_string(s, code) < 0)
    {
        fprintf(stderr, "Compilation failed\n");
        tcc_delete(s);
        return 1;
    }

    const char* outputFilename = "out";
    if (tcc_output_file(s, outputFilename) < 0)
    {
        fprintf(stderr, "Failed to write output file\n");
        tcc_delete(s);
        return 1;
    }

    // if (tcc_relocate(s) < 0)
    // {
    //     fprintf(stderr, "Failed to relocate code in memory\n");
    //     tcc_delete(s);
    //     return 1;
    // }

    // func_f f = (func_f)tcc_get_symbol(s, "f");
    // if (!f)
    // {
    //     fprintf(stderr, "Symbol 'f' not found\n");
    //     tcc_delete(s);
    //     return 1;
    // }

    // float result = f(3.14f);

    // printf("Result vector: [%.2f]\n", result);

    // FILE* fout = fopen("final.bin", "wb");
    // fwrite(fout);

    // Cleanup memory
    tcc_delete(s);
    return 0;
}
