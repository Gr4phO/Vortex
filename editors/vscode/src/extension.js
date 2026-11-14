const vscode = require("vscode");
const { LanguageClient, TransportKind } = require("vscode-languageclient/node");

let client = null;

function activate(context) {
  const config = vscode.workspace.getConfiguration("vortex");
  const serverCmd = config.get("serverPath", "vortex") || "vortex";

  const serverOptions = {
    run: { command: serverCmd, args: ["lsp"], transport: TransportKind.stdio },
    debug: { command: serverCmd, args: ["lsp"], transport: TransportKind.stdio }
  };

  const clientOptions = {
    documentSelector: [{ scheme: "file", language: "vortex" }],
    synchronize: {
      fileEvents: vscode.workspace.createFileSystemWatcher("**/*.vx")
    }
  };

  client = new LanguageClient(
    "vortexLanguageServer",
    "Vortex Language Server",
    serverOptions,
    clientOptions
  );

  client.start();
  context.subscriptions.push({
    dispose: () => {
      if (client) return client.stop();
    }
  });
}

function deactivate() {
  if (!client) return undefined;
  return client.stop();
}

module.exports = { activate, deactivate };
