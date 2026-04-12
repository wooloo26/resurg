import * as path from "path";
import * as fs from "fs";
import * as vscode from "vscode";
import {
  LanguageClient,
  LanguageClientOptions,
  ServerOptions,
  TransportKind,
} from "vscode-languageclient/node";

let client: LanguageClient | undefined;

/**
 * Find the Resurg repo root.
 *
 * Search order:
 *  1. Extension source tree (<repo>/tools/rsg-lsp/editors/vscode)
 *  2. Workspace folder (when the repo itself is the workspace)
 *  3. realpath of extensionPath (junction target resolution)
 */
function findRepoRoot(extensionPath: string): string | undefined {
  // Extension lives at <repo>/tools/rsg-lsp/editors/vscode
  const fromExt = path.resolve(extensionPath, "..", "..", "..", "..");
  if (fs.existsSync(path.join(fromExt, "build")) && fs.existsSync(path.join(fromExt, "std"))) {
    return fromExt;
  }

  // Try resolving the real path (handles junctions/symlinks).
  try {
    const real = fs.realpathSync(extensionPath);
    if (real !== extensionPath) {
      const fromReal = path.resolve(real, "..", "..", "..", "..");
      if (fs.existsSync(path.join(fromReal, "build")) && fs.existsSync(path.join(fromReal, "std"))) {
        return fromReal;
      }
    }
  } catch {
    // Ignore resolve errors.
  }

  // Workspace folder might be the repo root.
  const folders = vscode.workspace.workspaceFolders;
  if (folders) {
    for (const folder of folders) {
      const ws = folder.uri.fsPath;
      if (fs.existsSync(path.join(ws, "build")) && fs.existsSync(path.join(ws, "std"))) {
        return ws;
      }
    }
  }

  return undefined;
}

/**
 * Discover the rsg-lsp binary path.
 *
 * Search order:
 *  1. resurg.lspPath setting
 *  2. build/rsg-lsp(.exe) next to the repo root (dev mode)
 *  3. rsg-lsp on PATH
 */
function findLspBinary(extensionPath: string): string {
  const config = vscode.workspace.getConfiguration("resurg");
  const explicit = config.get<string>("lspPath");
  if (explicit && explicit.length > 0) {
    return explicit;
  }

  const exe = process.platform === "win32" ? "rsg-lsp.exe" : "rsg-lsp";

  // Dev mode: resolve from source tree.
  const repo = findRepoRoot(extensionPath);
  if (repo) {
    const devBin = path.join(repo, "build", exe);
    if (fs.existsSync(devBin)) {
      return devBin;
    }
  }

  // Fallback: expect on PATH.
  return exe;
}

/**
 * Discover the std library path.
 *
 * Search order:
 *  1. resurg.stdPath setting
 *  2. std/ next to the repo root (dev mode)
 */
function findStdPath(extensionPath: string): string | undefined {
  const config = vscode.workspace.getConfiguration("resurg");
  const explicit = config.get<string>("stdPath");
  if (explicit && explicit.length > 0) {
    return explicit;
  }

  const repo = findRepoRoot(extensionPath);
  if (repo) {
    const devStd = path.join(repo, "std");
    if (fs.existsSync(devStd)) {
      return devStd;
    }
  }

  return undefined;
}

export function activate(context: vscode.ExtensionContext): void {
  const command = findLspBinary(context.extensionPath);
  const args: string[] = [];

  const stdPath = findStdPath(context.extensionPath);
  if (stdPath) {
    args.push(`--std-path=${stdPath}`);
  }

  const outputChannel = vscode.window.createOutputChannel("Resurg LSP");
  outputChannel.appendLine(`rsg-lsp binary: ${command}`);
  outputChannel.appendLine(`std path: ${stdPath ?? "(not found)"}`);

  const serverOptions: ServerOptions = {
    run: { command, args, transport: TransportKind.stdio },
    debug: { command, args, transport: TransportKind.stdio },
  };

  const clientOptions: LanguageClientOptions = {
    documentSelector: [{ scheme: "file", language: "resurg" }],
    synchronize: {
      fileEvents: vscode.workspace.createFileSystemWatcher("**/*.rsg"),
    },
    outputChannel,
  };

  client = new LanguageClient(
    "resurg-lsp",
    "Resurg Language Server",
    serverOptions,
    clientOptions
  );

  client.start().catch((err) => {
    const msg = err instanceof Error ? err.message : String(err);
    outputChannel.appendLine(`Failed to start rsg-lsp: ${msg}`);
    vscode.window.showErrorMessage(
      `Resurg LSP failed to start: ${msg}. Check Output > "Resurg LSP" for details.`
    );
  });

  context.subscriptions.push({
    dispose: () => {
      if (client) {
        client.stop();
      }
    },
  });
}

export function deactivate(): Thenable<void> | undefined {
  if (client) {
    return client.stop();
  }
  return undefined;
}
