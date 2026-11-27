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

function findInPath(exeName) {
  const pathEnv = process.env.PATH || process.env.Path || "";
  for (const rawDir of pathEnv.split(path.delimiter)) {
    const dir = rawDir.trim().replace(/^"(.*)"$/, "$1");
    if (!dir) continue;
    const candidate = path.join(dir, exeName);
    if (isExecutableFile(candidate)) return candidate;
  }
  return null;
}

function addCandidateDir(dirs, dir) {
  if (dir && !dirs.includes(dir)) dirs.push(dir);
}

function resolveVortexBinary(targetUri) {
  const config = vscode.workspace.getConfiguration("vortex");
  const configured = (config.get("serverPath", "vortex") || "").trim();
  const exeName = process.platform === "win32" ? "vortex.exe" : "vortex";

  if (configured && configured !== "vortex" && configured !== "vortex.exe") {
    return configured;
  }

  const candidateDirs = [];
  if (targetUri && targetUri.fsPath) {
    const fileDir = path.dirname(targetUri.fsPath);
    addCandidateDir(candidateDirs, fileDir);
    addCandidateDir(candidateDirs, path.join(path.dirname(fileDir), "vortex"));
    const ws = vscode.workspace.getWorkspaceFolder(targetUri);
    if (ws) {
      addCandidateDir(candidateDirs, ws.uri.fsPath);
      addCandidateDir(candidateDirs, path.join(path.dirname(ws.uri.fsPath), "vortex"));
    }
  }
  for (const ws of vscode.workspace.workspaceFolders || []) {
    addCandidateDir(candidateDirs, ws.uri.fsPath);
    addCandidateDir(candidateDirs, path.join(path.dirname(ws.uri.fsPath), "vortex"));
  }

  if (process.env.LOCALAPPDATA) {
    addCandidateDir(candidateDirs, path.join(process.env.LOCALAPPDATA, "vortex"));
  }
  const homeDir = process.env.USERPROFILE || process.env.HOME;
  if (homeDir) {
    addCandidateDir(candidateDirs, path.join(homeDir, ".vortex", "bin"));
  }
  if (process.platform !== "win32") {
    addCandidateDir(candidateDirs, "/usr/local/bin");
  }

  for (const dir of candidateDirs) {
    const candidate = path.join(dir, exeName);
    if (isExecutableFile(candidate)) return candidate;
  }

  return findInPath(exeName);
}

function resolveVortexBinaryForLsp(targetUri) {
  const resolved = resolveVortexBinary(targetUri);
  if (resolved) return resolved;
  const configured = (vscode.workspace.getConfiguration("vortex").get("serverPath", "vortex") || "").trim();
  return configured || (process.platform === "win32" ? "vortex.exe" : "vortex");
}

async function promptAndSaveVortexBinary() {
  const exeLabel = process.platform === "win32" ? "vortex.exe" : "vortex";
  const choice = await vscode.window.showErrorMessage(
    `Vortex executable (${exeLabel}) not found in PATH, workspace, or sibling directories.`,
    `Locate ${exeLabel}`
  );
  if (choice !== `Locate ${exeLabel}`) return null;

  const picked = await vscode.window.showOpenDialog({
    canSelectFiles: true,
    canSelectFolders: false,
    canSelectMany: false,
    openLabel: `Select ${exeLabel}`,
    filters: process.platform === "win32" ? { Executable: ["exe"] } : undefined
  });
  if (!picked || picked.length === 0) return null;

  const selectedPath = picked[0].fsPath;
  await vscode.workspace.getConfiguration("vortex").update(
    "serverPath",
    selectedPath,
    vscode.ConfigurationTarget.Global
  );
  return selectedPath;
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

async function prepareTargetVortexUri(targetUri) {
  const config = vscode.workspace.getConfiguration("vortex");
  const saveBeforeRun = config.get("saveBeforeRun", true);

  if (targetUri && targetUri.fsPath) {
    const openDoc = vscode.workspace.textDocuments.find(
      (d) => d.uri.fsPath === targetUri.fsPath
    );
    if (openDoc && saveBeforeRun && openDoc.isDirty) {
      await openDoc.save();
    }
    return targetUri;
  }

  const editor = vscode.window.activeTextEditor;
  if (!editor || (editor.document.languageId !== "vortex" && !editor.document.fileName.endsWith(".vx"))) {
    vscode.window.showWarningMessage("Open or select a Vortex (.vx) file first.");
    return null;
  }
  if (saveBeforeRun && editor.document.isDirty) {
    await editor.document.save();
  }
  if (editor.document.isUntitled) {
    vscode.window.showWarningMessage("Save the .vx file to disk before running Vortex.");
    return null;
  }
  return editor.document.uri;
}

async function runInVortexTerminal(args, uri) {
  const cwd = uri ? path.dirname(uri.fsPath) : (vscode.workspace.workspaceFolders?.[0]?.uri.fsPath || undefined);
  const bin = resolveVortexBinary(uri) || await promptAndSaveVortexBinary();
  if (!bin) return;
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

  const serverCmd = resolveVortexBinaryForLsp(vscode.window.activeTextEditor?.document?.uri);
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
    vscode.commands.registerCommand("vortex.runFile", async (uri) => {
      const target = await prepareTargetVortexUri(uri);
      if (!target) return;
      const mode = vscode.workspace.getConfiguration("vortex").get("defaultRunMode", "safe");
      await runInVortexTerminal(["run", ...modeFlags(mode), target.fsPath], target);
    }),

    vscode.commands.registerCommand("vortex.runFileRelease", async (uri) => {
      const target = await prepareTargetVortexUri(uri);
      if (!target) return;
      await runInVortexTerminal(["run", "--release", target.fsPath], target);
    }),

    ...["1", "2", "3"].map((level) =>
      vscode.commands.registerCommand(`vortex.runFileUltrasafe${level}`, async (uri) => {
        const target = await prepareTargetVortexUri(uri);
        if (!target) return;
        await runInVortexTerminal(["run", "--ultrasafe", level, target.fsPath], target);
      })
    ),

    vscode.commands.registerCommand("vortex.checkFileUltrasafe", async (uri) => {
      const target = await prepareTargetVortexUri(uri);
      if (!target) return;
      await runInVortexTerminal(["--ultrasafe", "3", "--check", target.fsPath], target);
    }),

    vscode.commands.registerCommand("vortex.runTests", async () => {
      const doc = vscode.window.activeTextEditor?.document;
      const ws = (doc && vscode.workspace.getWorkspaceFolder(doc.uri)) || vscode.workspace.workspaceFolders?.[0];
      const targetPath = ws ? ws.uri.fsPath : (doc ? path.dirname(doc.uri.fsPath) : ".");
      if (doc && doc.languageId === "vortex" && doc.isDirty) await doc.save();
      await runInVortexTerminal(["test", targetPath, "--ultrasafe", "2"], doc?.uri || ws?.uri);
    }),

    vscode.commands.registerCommand("vortex.buildStandalone", async (uri) => {
      const target = await prepareTargetVortexUri(uri);
      if (!target) return;
      const parsed = path.parse(target.fsPath);
      const outExt = process.platform === "win32" ? ".exe" : "";
      const outPath = path.join(parsed.dir, `${parsed.name}${outExt}`);
      await runInVortexTerminal(["build", "--release", target.fsPath, "-o", outPath], target);
    }),

    vscode.commands.registerCommand("vortex.openRepl", async () => {
      const ws = vscode.workspace.workspaceFolders?.[0];
      await runInVortexTerminal(["repl"], vscode.window.activeTextEditor?.document?.uri || ws?.uri);
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
