# Vortex Systems Language (.vx) for VS Code

Official VS Code extension for the **Vortex** systems programming language (`x86-64` AVX2 & `ARM64` NEON JIT, 2 MiB paged arenas, zero-GC).

## Features

- **▶ Run in Terminal Button**: One-click **Run Vortex File in Terminal** button in the top-right editor toolbar (just like Python), plus a dropdown menu for:
  - `Run Vortex File (--release)`
  - `Check File (--ultrasafe 3 --check)`
  - `Run Native Tests (vortex test)`
  - `Build Standalone Executable`
  - `Open Interactive Vortex REPL`
- **Native Language Server (`vortex lsp`)**: Real-time diagnostics, go-to-definition, hover info, and auto-completion.
- **Full Syntax Highlighting**: Supports `match`, `enum`, `struct`, `parallel_for`, `unsafe`, `bool`/`u8..u64`/`usize`/`isize`/`char`, array literals, `0b`/`0o`/`0x`/`_`/`1e-5` numbers, `'a'` char literals, and `//` + `/* ... */` comments.

## Setup

1. Make sure `vortex` (or `vortex.exe`) is in your `PATH`, in your workspace root, or set `"vortex.serverPath"` in VS Code Settings.
2. Package or install:
   ```sh
   cd editors/vscode
   npm install
   npx @vscode/vsce package
   code --install-extension vortex-systems-1.1.6.vsix
   ```
