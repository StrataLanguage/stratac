// strata-lsp: a Language Server Protocol server over stdin and stdout.
//
//   strata-lsp [-I <dir>]...
//
// Editors start the server and talk to it with framed JSON-RPC messages. Diagnostics come from the same
// parser and semantic analysis that stratac runs. Protocol output goes to stdout only; logs go to stderr.

#include "Lsp/Server.h"
#include "Lsp/Transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static void PrintUsage(FILE* out)
{
    fprintf(out, "usage: strata-lsp [-I <dir>]...\n");
    fprintf(out, "  -I <dir>  search <dir> for imports, in the order given\n");
}

int main(int argc, char** argv)
{
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    LspServer* server = LspServerCreate();

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-I") == 0 && i + 1 < argc)
        {
            LspServerAddImportDir(server, argv[++i]);
        }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            PrintUsage(stdout);
            LspServerDestroy(server);
            return 0;
        }
        else
        {
            fprintf(stderr, "strata-lsp: unknown argument '%s'\n", argv[i]);
            PrintUsage(stderr);
            LspServerDestroy(server);
            return 2;
        }
    }

    bool running = true;
    while (running)
    {
        size_t len = 0;
        char* body = LspReadMessage(stdin, &len);
        if (!body)
        {
            break; // end of input: the client went away without an exit message
        }

        running = LspServerHandleMessage(server, body, len);
        free(body);
    }

    int code = LspServerExitCode(server);
    LspServerDestroy(server);
    return code;
}
