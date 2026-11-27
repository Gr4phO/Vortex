const fs = require("fs");
const path = require("path");
const vscode = require("vscode");
const { LanguageClient, TransportKind } = require("vscode-languageclient/node");

let client = null;
let vortexTerminal = null;

function isExecutableFile(p) {
  try {
    return fs.existsSync(p) && fs.statSync(p).isFile();
  } catch {
    return false;
  }
}

function resolveVortexBinary(targetUri) {
  const config = vscode.workspace.getConfiguration("vortex");
  const configured = (config.get("serverPath", "vortex") || "").trim();
  const exeName = process.platform === "win32" ? "vortex.exe" : "vortex";

  if (configured && configured !== "vortex" && configured !== "vortex.exe") {
    return configured;
  }

  const folders = [];
  if (targetUri) {
    const ws = vscode.workspace.getWorkspaceFolder(targetUri);
    if (ws) folders.push(ws.uri.fsPath);
  }
  for (const ws of vscode.workspace.workspaceFolders || []) {
    if (!folders.includes(ws.uri.fsPath)) folders.push(ws.uri.fsPath);
  }

  for (const dir of folders) {
    const candidate = path.join(dir, exeName);
    if (isExecutableFile(candidate)) return candidate;
  }

  return configured || exeName;
}

function isPowerShell(terminal) {
  const shellPath =
    (terminal.creationOptions && terminal.creationOptions.shellPath) ||
    vscode.env.shell ||
    "";
  const lower = shellPath.toLowerCase();
  if (lower.includes("pwsh") || lower.includes("powershell")) return true;
  if (lower.includes("cmd.exe") || lower.includes("bash") || lower.includes("zsh") || lower.includes("sh.exe")) {
    return false;
  }
  return process.platform === "win32";
}

function quoteArg(arg, ps) {
  if (ps) {
    return `'${String(arg).replace(/'/g, "''")}'`;
  }
  return `"${String(arg).replace(/"/g, '\\"')}"`;
}

function getOrCreateTerminal(cwd) {
  if (vortexTerminal) {
    const alive = vscode.window.terminals.some((t) => t === vortexTerminal);
    if (alive) return vortexTerminal;
    vortexTerminal = null;
  }
  vortexTerminal = vscode.window.createTerminal({
    name: "Vortex",
    cwd: cwd || undefined
  });
  return vortexTerminal;
}

async function prepareActiveVortexFile(requireSaved = true) {
  const editor = vscode.window.activeTextEditor;
  if (!editor || editor.document.languageId !== "vortex") {
    vscode.window.showWarningMessage("Open a Vortex (.vx) file first.");
    return null;
  }
  const config = vscode.workspace.getConfiguration("vortex");
  if (config.get("saveBeforeRun", true) && editor.document.isDirty) {
    await editor.document.save();
  }
  if (requireSaved && editor.document.isUntitled) {
    vscode.window.showWarningMessage("Save the .vx file to disk before running Vortex.");
    return null;
  }
  return editor.document;
}

function runInVortexTerminal(args, uri) {
  const cwd = uri ? path.dirname(uri.fsPath) : (vscode.workspace.workspaceFolders?.[0]?.uri.fsPath || undefined);
  const bin = resolveVortexBinary(uri);
  const terminal = getOrCreateTerminal(cwd);
  const ps = isPowerShell(terminal);
  const config = vscode.workspace.getConfiguration("vortex");

  terminal.show(false);
  if (config.get("clearTerminalBeforeRun", false)) {
    terminal.sendText(ps ? "Clear-Host" : (process.platform === "win32" ? "cls" : "clear"), true);
  }

  const binPart = quoteArg(bin, ps);
  const argParts = args.map((a) => quoteArg(a, ps)).join(" ");
  const cmdLine = ps
    ? `& ${binPart}${argParts ? " " + argParts : ""}`
    : `${binPart}${argParts ? " " + argParts : ""}`;

  terminal.sendText(cmdLine, true);
}

function modeFlags(mode) {
  switch (mode) {
    case "release": return ["--release"];
    case "ultrasafe1": return ["--ultrasafe", "1"];
    case "ultrasafe2": return ["--ultrasafe", "2"];
    case "ultrasafe3": return ["--ultrasafe", "3"];
    default: return ["--safe"];
  }
}

async function startLanguageServer(context) {
  const config = vscode.workspace.getConfiguration("vortex");
  if (!config.get("enableLsp", true)) return;

  const serverCmd = resolveVortexBinary(vscode.window.activeTextEditor?.document?.uri);
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

  try {
    await client.start();
  } catch (err) {
    console.warn("[Vortex] Failed to start LSP server:", err && err.message ? err.message : err);
  }
}

function activate(context) {
  context.subscriptions.push(
    vscode.window.onDidCloseTerminal((t) => {
      if (t === vortexTerminal) vortexTerminal = null;
    })
  );

  context.subscriptions.push(
    vscode.commands.registerCommand("vortex.runFile", async () => {
      const doc = await prepareActiveVortexFile(true);
      if (!doc) return;
      const mode = vscode.workspace.getConfiguration("vortex").get("defaultRunMode", "safe");
      runInVortexTerminal(["run", ...modeFlags(mode), doc.uri.fsPath], doc.uri);
    }),

    vscode.commands.registerCommand("vortex.runFileRelease", async () => {
      const doc = await prepareActiveVortexFile(true);
      if (!doc) return;
      runInVortexTerminal(["run", "--release", doc.uri.fsPath], doc.uri);
    }),

    vscode.commands.registerCommand("vortex.checkFileUltrasafe", async () => {
      const doc = await prepareActiveVortexFile(true);
      if (!doc) return;
      runInVortexTerminal(["--ultrasafe", "3", "--check", doc.uri.fsPath], doc.uri);
    }),

    vscode.commands.registerCommand("vortex.runTests", async () => {
      const doc = vscode.window.activeTextEditor?.document;
      const ws = (doc && vscode.workspace.getWorkspaceFolder(doc.uri)) || vscode.workspace.workspaceFolders?.[0];
      const targetPath = ws ? ws.uri.fsPath : (doc ? path.dirname(doc.uri.fsPath) : ".");
      if (doc && doc.languageId === "vortex" && doc.isDirty) await doc.save();
      runInVortexTerminal(["test", targetPath, "--ultrasafe", "2"], doc?.uri || ws?.uri);
    }),

    vscode.commands.registerCommand("vortex.buildStandalone", async () => {
      const doc = await prepareActiveVortexFile(true);
      if (!doc) return;
      const parsed = path.parse(doc.uri.fsPath);
      const outExt = process.platform === "win32" ? ".exe" : "";
      const outPath = path.join(parsed.dir, `${parsed.name}${outExt}`);
      runInVortexTerminal(["build", "--release", doc.uri.fsPath, "-o", outPath], doc.uri);
    }),

    vscode.commands.registerCommand("vortex.openRepl", () => {
      const ws = vscode.workspace.workspaceFolders?.[0];
      runInVortexTerminal(["repl"], vscode.window.activeTextEditor?.document?.uri || ws?.uri);
    }),

    vscode.commands.registerCommand("vortex.restartLsp", async () => {
      if (client) {
        try {
          await client.stop();
        } catch {}
        client = null;
      }
      await startLanguageServer(context);
      vscode.window.showInformationMessage("Vortex Language Server restarted.");
    })
  );

  startLanguageServer(context);
  context.subscriptions.push({
    dispose: () => {
      if (client) return client.stop();
      return undefined;
    }
  });
}

function deactivate() {
  if (!client) return undefined;
  return client.stop();
}

module.exports = { activate, deactivate };
