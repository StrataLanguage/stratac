#include "Codegen/LLVMModuleBuilder.h"
#include "Codegen/CodegenBackend.h"
#include "Codegen/LLVMCApi.h"
#include "Core/Diagnostics.h"

#include <stdlib.h>
#include <string.h>

CodegenResult GenerateLlvmIr(const Module* mod)
{
    CodegenResult res = {0};

    res.moduleName = mod ? mod->name : NULL;

    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);

    BuiltModule bm = BuildLlvmModule(mod, &diag, scratch_arena(), false, NULL);

    if (DiagHasErrors(&diag))
    {
        Sb sb;
        SbInit(&sb);
        SbPuts(&sb, "; LLVM back-end errors:\n");

        for (size_t i = 0; i < diag.m_count; i++)
        {
            SbPrintf(&sb, ";   %s\n", diag.m_diagnostics[i].message);
        }

        /* The caller free()s `output`, so it must be heap-owned — NOT an
           arena/scratch pointer (a free() on scratch memory corrupts the
           heap and crashes at teardown). */
        res.output = DupString(SbFinish(&sb, scratch_arena()));
        res.ok = false;

        DiagnosticEngineFree(&diag);
        BuiltModuleDispose(&bm);

        return res;
    }

    char* ir = LLVMPrintModuleToString(bm.mod);
    res.output = DupString(ir);
    res.ok = true;

    LLVMDisposeMessage(ir);
    DiagnosticEngineFree(&diag);
    BuiltModuleDispose(&bm);

    return res;
}
