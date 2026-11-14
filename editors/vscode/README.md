# Vortex VS Code Extension

Provides syntax highlighting, real-time diagnostics, go-to-definition, hover documentation, and completion for `.vx` files via the built-in `vortex lsp` language server.

## Development / Usage

1. Ensure `vortex` (or `vortex.exe`) is in your `PATH`, or configure `"vortex.serverPath"` in VS Code Settings.
2. Install Node client dependency:
   ```bash
   cd editors/vscode
   npm install
   ```
3. Open `editors/vscode` in VS Code and press `F5` to launch the Extension Development Host, or package with `vsce package`.
