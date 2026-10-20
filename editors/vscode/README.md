# Strata for VS Code

Syntax highlighting for `.strata` files, plus diagnostics from `strata-lsp` (the real compiler
front end: parse, imports and semantic analysis).

## Build the language server

`strata-lsp` is a target in the main CMake project (on by default, `-DSTRATA_BUILD_LSP=OFF` to skip).
It only links the front end, never LLVM:

```sh
cmake --build build/default --target strata-lsp     # -> build/default/bin/strata-lsp
```

Either put it on `PATH` or point the `strata.server.path` setting at it.

## Run the extension

```sh
cd editors/vscode
npm install
npm run compile
```

Then open `editors/vscode` in VS Code and press F5 (Extension Development Host), or package
and install it:

```sh
npx vsce package --allow-missing-repository
code --install-extension strata-0.1.0.vsix
```

## Settings

| Setting | Default | |
|---|---|---|
| `strata.server.path` | `""` | Path to `strata-lsp`; empty uses `PATH`. |
| `strata.server.enabled` | `true` | Turn the server off and keep highlighting only. |
| `strata.importDirs` | `[]` | Extra import search directories (like `stratac -I`). |
| `strata.trace.server` | `off` | Log LSP traffic to the output panel. |

`Strata: Restart Language Server` restarts it after rebuilding.

## Current limits

- Diagnostics only: no hover, go-to-definition or completion yet.
- Imports are read from disk, so unsaved edits in an imported file are not seen until saved.
- Errors inside an imported module are shown on line 1 of the importing file, linked to the
  real location.
