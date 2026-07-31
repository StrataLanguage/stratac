#include <strata/Codegen/LLDCApi.h>

#include <lld/Common/Driver.h>

LLD_HAS_DRIVER(coff)
LLD_HAS_DRIVER(elf)
LLD_HAS_DRIVER(macho)

#include <llvm/Support/raw_ostream.h>

#include <string>
#include <vector>

bool LLDLink(LLDTarget target, int argc, const char** argv, char** errorOut)
{
    std::vector<const char*> args(argv, argv + argc);

    std::string outStr;
    std::string errStr;

    llvm::raw_string_ostream outStream(outStr);
    llvm::raw_string_ostream errStream(errStr);

    std::vector<lld::DriverDef> drivers = {
        {lld::WinLink, &lld::coff::link},
        {lld::Gnu, &lld::elf::link},
        {lld::Darwin, &lld::macho::link},
    };

    lld::Result result = lld::lldMain(args, outStream, errStream, drivers);

    const bool success = (result.retCode == 0);

    if (!errorOut)
    {
        return success;
    }

    std::string combined = outStr + errStr;
    if (!combined.empty())
    {
        char* str = static_cast<char*>(malloc(combined.size() + 1));
        if (str)
        {
            memcpy(str, combined.c_str(), combined.size() + 1);
            *errorOut = str;
        }
        else
        {
            *errorOut = nullptr;
        }
    }
    else
    {
        *errorOut = nullptr;
    }

    return success;
}

void LLDFreeString(char* str)
{
    free(str);
}
