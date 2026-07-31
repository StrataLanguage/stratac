#include <strata/Codegen/LLDCApi.h>

#include <lld/Common/Driver.h>

LLD_HAS_DRIVER(coff)
LLD_HAS_DRIVER(elf)
LLD_HAS_DRIVER(macho)

#include <llvm/Support/raw_ostream.h>

#include <string>
#include <vector>

char* LLDLink(int argc, const char** argv)
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

    char* errBuf = nullptr;
    std::string combined = outStr + errStr;

    if (!combined.empty())
    {
        errBuf = static_cast<char*>(malloc(combined.size() + 1));
        if (errBuf)
        {
            memcpy(errBuf, combined.c_str(), combined.size() + 1);
        }
    }

    return errBuf;
}

void LLDFreeString(char* str)
{
    free(str);
}
