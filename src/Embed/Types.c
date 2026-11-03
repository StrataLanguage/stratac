/* Public type-metadata API: compile type metadata without code generation, and free it. */
#include "Embed/EmbedInternal.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* Parses and checks `mod`, then serializes its components. The loader/source cleanup stays
       with the caller. */
    static int TypeMetadataFromModule(Module* mod, DiagnosticEngine* diag, Arena* arena, const SourceManager* sources,
                                      size_t sourceCount, void** outBytes, size_t* outSize, const char** errOut)
    {
        AnalyzeModule(mod, diag, arena);

        if (DiagHasErrors(diag) || !mod)
        {
            char* diagText = DiagFormat(diag, sources, sourceCount, arena);
            SetErrOut(errOut, diagText, "compilation failed");
            return 0;
        }

        uint8_t* bytes = NULL;
        size_t size = 0;
        TypeMetadataBuildFromModule(mod, &bytes, &size);

        *outBytes = bytes;
        *outSize = size;

        return 1;
    }

    int strataCompileTypeMetadata(StrataCompiler* c, const char* path, void** outBytes, size_t* outSize,
                                  const char** errOut)
    {
        if (errOut)
        {
            *errOut = NULL;
        }

        if (!outBytes || !outSize)
        {
            SetErrOut(errOut, "null output", "");
            return 0;
        }

        *outBytes = NULL;
        *outSize = 0;

        if (!c || !path)
        {
            SetErrOut(errOut, "null compiler or path", "");
            return 0;
        }

        Arena arena;
        DiagnosticEngine diag;
        ModuleLoader loader;
        InitModuleLoader(&arena, &diag, &loader, c);

        Module* mod = ModuleLoaderLoad(&loader, path);
        int ok
            = TypeMetadataFromModule(mod, &diag, &arena, loader.sources, loader.sourceCount, outBytes, outSize, errOut);

        TeardownCompile(mod, &loader, &diag, &arena);

        return ok;
    }

    int strataCompileTypeMetadataString(StrataCompiler* c, const char* source, const char* moduleName, void** outBytes,
                                        size_t* outSize, const char** errOut)
    {
        if (errOut)
        {
            *errOut = NULL;
        }

        if (!outBytes || !outSize)
        {
            SetErrOut(errOut, "null output", "");
            return 0;
        }

        *outBytes = NULL;
        *outSize = 0;

        if (!c || !source)
        {
            SetErrOut(errOut, "null compiler or source", "");
            return 0;
        }

        const char* name = moduleName ? moduleName : "strata_module";
        size_t sourceLen = strlen(source);

        if (c->importResolver)
        {
            Arena arena;
            DiagnosticEngine diag;
            ModuleLoader loader;
            InitModuleLoader(&arena, &diag, &loader, c);

            Module* mod = ModuleLoaderLoadSource(&loader, name, source, sourceLen);
            int ok = TypeMetadataFromModule(mod, &diag, &arena, loader.sources, loader.sourceCount, outBytes, outSize,
                                            errOut);

            TeardownCompile(mod, &loader, &diag, &arena);

            return ok;
        }

        Arena arena;
        arena_init(&arena, 0);

        SourceManager src;
        SourceManagerInit(&src);
        SourceManagerSetSource(&src, source, sourceLen, name);

        DiagnosticEngine diag;
        DiagnosticEngineInit(&diag);

        Lexer lex;
        LexerInit(&lex, src.m_text, src.m_textLen, &diag, 0);

        Parser parser;
        ParserInit(&parser, &lex, &diag, &arena, name);

        Module* mod = ParserParseModule(&parser);

        if (mod && mod->imports.count > 0)
        {
            DiagErrorFmt(&diag, SRC_INVALID,
                         "imports are not supported when compiling from a string; use strataCompileTypeMetadata or "
                         "strataSetImportResolver");
        }

        int ok = TypeMetadataFromModule(mod, &diag, &arena, &src, 1, outBytes, outSize, errOut);

        AstDispose((Node*)mod);
        DiagnosticEngineFree(&diag);
        SourceManagerFree(&src);
        arena_free(&arena);

        return ok;
    }

    void strataFreeTypeMetadata(void* bytes)
    {
        free(bytes);
    }

#ifdef __cplusplus
}
#endif
