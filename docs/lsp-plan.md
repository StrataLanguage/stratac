# Strata language server and Zed support: plan

Status: Phase 1 implemented (`strata-lsp`, covered by the `lsp_session` CTest). Phases 2 through 4 not started.

Goal: give Strata an LSP server and a Zed extension, so the editor shows errors as you type, highlights syntax, and eventually supports navigation and completion.

## Starting point

- The front end is reusable without LLVM. `ModuleLoaderLoadSource` takes an in-memory buffer, and `AnalyzeModule` ([src/Embed/Pipeline.c](../src/Embed/Pipeline.c)) runs resolve and overload checks.
- Diagnostics carry byte ranges and a file id ([src/Core/SourceLocation.h](../src/Core/SourceLocation.h)). `SourceManager` maps offsets to line and column.
- The parser recovers from errors, so partial files still produce an AST ([src/Parse/Parser.c](../src/Parse/Parser.c)).
- A Zed extension already exists at `/Users/emd22/Projects/strata-zed`, with a tree-sitter grammar and highlighting queries. Phase 2 builds on it.
- Sema stores an inferred type per expression node ([src/Sema/NodeTypes.h](../src/Sema/NodeTypes.h)), which makes hover types possible.
- Gaps: `IdentExpr` holds only a name and a use kind ([src/AST/AST.h](../src/AST/AST.h)). Identifiers don't link to their declarations, so go-to-definition and rename need new sema bookkeeping.

## Phase 1: diagnostics-only LSP (a few days)

Goal: Zed shows Strata errors and warnings as you type.

- **Target:** add a `strata-lsp` executable to [CMakeLists.txt](../CMakeLists.txt), next to `stratac`. It links the `strata` library, so it runs the real parser and sema in-process.
- **Transport:** JSON-RPC over stdio with `Content-Length` framing. Support `initialize`, `initialized`, `shutdown`, `exit`, `textDocument/didOpen`, `didChange` (full sync at first), `didSave` and `didClose`. A small JSON parser is enough.
- **Analysis:** on each change, debounce about 200 ms, then run `ModuleLoaderLoadSource` on the buffer and `AnalyzeModule`. Pass the same import directories `stratac` uses so `import` resolves.
- **Diagnostics:** convert each range with `SourceManagerLineCol` and send `textDocument/publishDiagnostics`. Use UTF-16 columns so non-ASCII text lines up. Clear a file's diagnostics when its errors go away.
- **Crash isolation:** run each analysis behind a guard. On a crash, keep the last good diagnostics and log the failure. If sema proves fragile on partial input, move analysis into a subprocess.
- **Tests:** add a CTest that drives a scripted LSP session over stdio and checks the diagnostics. Reuse the malformed inputs in [tests/fuzz](../tests/fuzz).

**Phase 1 as built:**

- Sources are in [src/Lsp](../src/Lsp): `Json` (reader and writer), `Uri`, `Transport` (Content-Length framing), `Server` (document state, dispatch, diagnostics), and `Main`. The target is `strata-lsp` in [CMakeLists.txt](../CMakeLists.txt).
- The test is [tests/lsp/SessionTest.py](../tests/lsp/SessionTest.py), registered as `lsp_session`. It checks positions, UTF-16 columns, `-I` imports, errors inside imported files, close handling, and recovery from malformed input.
- Usage: `strata-lsp [-I <dir>]...`. Editors can pass the same `-I` flags as `stratac`.
- Full CTest run: 492 of 492 pass.

**Where it differs from the plan:**

- **No debounce yet.** Each change is analyzed as it arrives. Analysis of small files is fast, but a debounce is still worth adding.
- **No crash isolation yet.** Analysis runs in-process, so a crash in sema ends the server. Moving analysis to a subprocess is still open.
- **Open buffers win over disk.** An import of a file that is open is read from its editor buffer (a loader overlay, `ModuleLoaderSetOverlay`). Editing or closing a file re-analyzes the open files that import it. The open file is loaded under its canonical path (`ModuleLoaderLoadBuffer`), so an import cycle back to it doesn't load it twice. Diagnostics for an open file come only from its own analysis: importers never publish over them or clear them.
- **Missing-import errors have no range.** The loader reports `cannot open module '…'` at the start of the importing file, not at the `import` line.

## Phase 2: wire the LSP into the existing Zed extension (a day or two)

The Zed extension already exists at `/Users/emd22/Projects/strata-zed` (id `strata`, remote `github.com/StrataLanguage/zed-plugin`). It has the language config, `highlights`, `brackets`, `indents`, `outline` and `overrides` queries, and a tree-sitter grammar at `grammars/tree-sitter-strata` with corpus tests. It has no language server wiring and no Rust crate. So this phase adds the server; it doesn't start a new extension.

- **Rust crate:** add `Cargo.toml` (a `cdylib` depending on `zed_extension_api`) and `src/lib.rs`. The extension implements `language_server_command`, which returns the path to `strata-lsp`. Look for it on `PATH`, and allow an override through Zed's binary path setting.
- **Manifest:** add a `[language_servers.strata-lsp]` table to `extension.toml` and bind it to the Strata language. Confirm the exact field names against Zed's current docs, since I haven't verified them.
- **Grammar changes:** `extension.toml` pins the grammar by `rev`. Push grammar changes to the remote and bump the `rev`. For local iteration, use the `file://` form described in that file's comments.
- **Grammar gap to check:** the corpus covers generic type declarations but has no expression call with explicit type arguments such as `f<int>(x)`. That's the case that must not parse as a comparison. Add a corpus test before relying on it.
- **Leave `grammars/strata/` alone:** it's the gitignored checkout Zed creates when it builds the grammar.
- **Dev install:** Zed's "Install Dev Extension" loads the folder from disk, so iteration doesn't need publishing.

## Phase 3: navigation and hover (one to two weeks)

- **Binding links:** add a declaration pointer to `IdentExpr` in [src/AST/AST.h](../src/AST/AST.h), set in `ResolveExpr` where scope lookup happens. Calls already carry their target (`CallExpr.resolvedDecl`), so they're the easy case.
- **Position lookup:** walk the AST to find the innermost node containing the cursor. Every request below uses this helper.
- **Requests:** `textDocument/definition`, `textDocument/hover` (declaration signature plus the type from `NodeTypes`) and `textDocument/documentSymbol`.
- **Generics:** definition requests resolve to the generic template in source, not to a monomorphized copy.

## Phase 4: completion, references and rename (later)

- **Scope snapshot:** record the names visible at each block so completion can list them.
- **Members:** completion after `.` uses the receiver's type from `NodeTypes`.
- **References and rename:** reverse the binding links from Phase 3.

## Open questions

1. Does the compiler keep any process-wide global state? One server handling several open files depends on the answer.
2. Should analysis run in-process or in a subprocess? In-process is faster. A subprocess keeps the server alive through sema crashes.
3. Confirm the split: the server lives in this repository, built alongside `stratac`, and the Zed extension stays in `strata-zed`. The server binary must be on the editor's `PATH` or set in its settings.
