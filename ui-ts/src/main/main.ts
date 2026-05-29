import { app, BrowserWindow, ipcMain, shell } from "electron";
import { spawn, type ChildProcessWithoutNullStreams } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import type { AppContext, ScanExit, ScannerProgress, ScanStartRequest, ScanStartResult } from "../shared/contracts";

let mainWindow: BrowserWindow | null = null;
let docsWindow: BrowserWindow | null = null;
let activeProcess: ChildProcessWithoutNullStreams | null = null;
let stdoutRemainder = "";
let stderrRemainder = "";
let killTimer: NodeJS.Timeout | null = null;

const isWindows = process.platform === "win32";

function appRoot(): string {
  if (app.isPackaged) {
    return process.resourcesPath;
  }
  return path.resolve(__dirname, "..", "..", "..");
}

function scannerPath(root = appRoot()): string {
  return path.join(root, "native", isWindows ? "scanner_native.exe" : "scanner_native");
}

function cubiomesPath(root = appRoot()): string {
  return path.join(root, "cubiomes_26.1.2_fork", "lib", isWindows ? "lib.dll" : "lib.so");
}

function iconPath(root = appRoot()): string | undefined {
  const candidate = app.isPackaged
    ? path.join(root, "HelixSeed.png")
    : path.join(root, "ui-ts", "assets", "HelixSeed.png");
  return fs.existsSync(candidate) ? candidate : undefined;
}

function quoteArg(value: string): string {
  if (!/[ \t"]/u.test(value)) {
    return value;
  }
  return `"${value.replace(/\\/gu, "\\\\").replace(/"/gu, '\\"')}"`;
}

function commandLine(program: string, args: string[]): string {
  return [program, ...args].map(quoteArg).join(" ");
}

function send(channel: string, payload: unknown): void {
  if (!mainWindow || mainWindow.isDestroyed()) {
    return;
  }
  mainWindow.webContents.send(channel, payload);
}

function emitStdoutLine(line: string): void {
  const trimmed = line.trim();
  if (!trimmed) {
    return;
  }
  if (trimmed.startsWith("UI_PROGRESS ")) {
    const jsonText = trimmed.slice("UI_PROGRESS ".length).trim();
    try {
      send("scan:progress", JSON.parse(jsonText) as ScannerProgress);
    } catch {
      send("scan:line", `[ui] Bad progress payload: ${jsonText}`);
    }
    return;
  }
  if (trimmed.startsWith("SEED_FOUND ")) {
    send("scan:seed", trimmed.slice("SEED_FOUND ".length).trim());
    return;
  }
  send("scan:line", line);
}

function emitLines(kind: "stdout" | "stderr", chunk: Buffer): void {
  const text = chunk.toString("utf8");
  const current = kind === "stdout" ? stdoutRemainder + text : stderrRemainder + text;
  const lines = current.split(/\r?\n/u);
  const remainder = lines.pop() ?? "";
  if (kind === "stdout") {
    stdoutRemainder = remainder;
  } else {
    stderrRemainder = remainder;
  }
  for (const line of lines) {
    if (kind === "stdout") {
      emitStdoutLine(line);
    } else if (line.trim()) {
      send("scan:line", `[stderr] ${line}`);
    }
  }
}

function flushRemainders(): void {
  if (stdoutRemainder.trim()) {
    emitStdoutLine(stdoutRemainder);
  }
  if (stderrRemainder.trim()) {
    send("scan:line", `[stderr] ${stderrRemainder}`);
  }
  stdoutRemainder = "";
  stderrRemainder = "";
}

function createWindow(): void {
  const root = appRoot();
  mainWindow = new BrowserWindow({
    width: 1480,
    height: 940,
    minWidth: 1180,
    minHeight: 760,
    backgroundColor: "#050607",
    icon: iconPath(root),
    title: "HelixSeed",
    webPreferences: {
      preload: path.join(__dirname, "..", "preload", "preload.js"),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true
    }
  });

  if (process.env.VITE_DEV_SERVER_URL) {
    void mainWindow.loadURL(process.env.VITE_DEV_SERVER_URL);
  } else {
    void mainWindow.loadFile(path.join(__dirname, "..", "renderer", "index.html"));
  }

  mainWindow.on("closed", () => {
    mainWindow = null;
  });
}

function createDocsWindow(): void {
  if (docsWindow && !docsWindow.isDestroyed()) {
    if (docsWindow.isMinimized()) {
      docsWindow.restore();
    }
    docsWindow.focus();
    return;
  }

  const root = appRoot();
  docsWindow = new BrowserWindow({
    width: 980,
    height: 820,
    minWidth: 720,
    minHeight: 560,
    backgroundColor: "#080808",
    icon: iconPath(root),
    title: "HelixSeed Documentation",
    parent: mainWindow ?? undefined,
    webPreferences: {
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true
    }
  });

  docsWindow.on("closed", () => {
    docsWindow = null;
  });

  if (process.env.VITE_DEV_SERVER_URL) {
    void docsWindow.loadURL(new URL("guide.html", process.env.VITE_DEV_SERVER_URL).toString());
  } else {
    void docsWindow.loadFile(path.join(__dirname, "..", "renderer", "guide.html"));
  }
}

ipcMain.handle("app:get-context", (): AppContext => {
  const root = appRoot();
  const scanner = scannerPath(root);
  return {
    repoRoot: root,
    scannerPath: scanner,
    scannerExists: fs.existsSync(scanner),
    packaged: app.isPackaged
  };
});

ipcMain.handle("app:open-path", async (): Promise<void> => {
  // Always opens the application root, never a renderer-supplied path. shell.openPath
  // hands its target to the OS shell, which on Windows will *launch* executables/scripts.
  // Accepting an arbitrary path here would turn any renderer-side injection into RCE.
  await shell.openPath(appRoot());
});

ipcMain.handle("app:open-external", async (_event, url: string): Promise<void> => {
  if (!/^https?:\/\//iu.test(url)) {
    return;
  }
  await shell.openExternal(url);
});

ipcMain.handle("app:open-docs", (): void => {
  createDocsWindow();
});

ipcMain.handle("scan:start", async (_event, request: ScanStartRequest): Promise<ScanStartResult> => {
  if (activeProcess) {
    return { ok: false, error: "A scan is already running." };
  }

  const root = appRoot();
  const scanner = scannerPath(root);
  const cubiomes = cubiomesPath(root);
  if (!fs.existsSync(scanner)) {
    return { ok: false, error: `Native scanner not found: ${scanner}` };
  }
  if (!fs.existsSync(cubiomes)) {
    return { ok: false, error: `Native cubiomes library not found: ${cubiomes}` };
  }

  const queryDir = path.join(app.getPath("userData"), "queries");
  fs.mkdirSync(queryDir, { recursive: true });
  const queryPath = path.join(queryDir, "active-query.json");
  fs.writeFileSync(queryPath, JSON.stringify(request.query, null, 2), "utf8");

  const args = ["--query-file", queryPath, ...request.args, "--control-stdin", "--stream-seeds"];
  if (request.terminalMode) {
    args.push("--terminal-mode");
  }

  stdoutRemainder = "";
  stderrRemainder = "";
  const displayCommand = commandLine(scanner, args);
  send("scan:line", `$ ${displayCommand}`);

  activeProcess = spawn(scanner, args, {
    cwd: root,
    windowsHide: true,
    env: {
      ...process.env,
      SCANNER_CUBIOMES_DLL: cubiomes
    }
  });

  activeProcess.stdout.on("data", (chunk: Buffer) => emitLines("stdout", chunk));
  activeProcess.stderr.on("data", (chunk: Buffer) => emitLines("stderr", chunk));
  activeProcess.on("error", (error) => {
    send("scan:line", `[ui] Failed to start scanner: ${error.message}`);
  });
  activeProcess.on("exit", (code, signal) => {
    if (killTimer) {
      clearTimeout(killTimer);
      killTimer = null;
    }
    flushRemainders();
    const exitPayload: ScanExit = { code, signal };
    send("scan:exit", exitPayload);
    activeProcess = null;
  });

  return {
    ok: true,
    command: displayCommand,
    scannerPath: scanner,
    repoRoot: root
  };
});

const CONTROL_COMMANDS = new Set(["pause", "resume", "terminal_on", "terminal_off", "snapshot"]);

ipcMain.handle("scan:control", (_event, command: "pause" | "resume" | "terminal_on" | "terminal_off" | "snapshot"): boolean => {
  if (!activeProcess || activeProcess.killed) {
    return false;
  }
  // Never forward an unvalidated string to the child's stdin control channel.
  if (typeof command !== "string" || !CONTROL_COMMANDS.has(command)) {
    return false;
  }
  activeProcess.stdin.write(`${command}\n`);
  return true;
});

ipcMain.handle("scan:stop", (): boolean => {
  if (!activeProcess || activeProcess.killed) {
    return false;
  }
  activeProcess.kill();
  killTimer = setTimeout(() => {
    if (activeProcess && !activeProcess.killed) {
      activeProcess.kill("SIGKILL");
    }
  }, 3000);
  return true;
});

// Harden every web contents: a renderer (or anything injected into one) must never be
// able to navigate the IPC-privileged window away from our bundled content or spawn an
// uncontrolled child window. External http(s) links are handed to the system browser;
// all other navigations/window.open requests and any <webview> attach are denied.
app.on("web-contents-created", (_event, contents) => {
  contents.setWindowOpenHandler(({ url }) => {
    if (/^https?:\/\//iu.test(url)) {
      void shell.openExternal(url);
    }
    return { action: "deny" };
  });
  contents.on("will-navigate", (event, url) => {
    const devServer = process.env.VITE_DEV_SERVER_URL;
    if (devServer && url.startsWith(devServer)) {
      return;
    }
    event.preventDefault();
    if (/^https?:\/\//iu.test(url)) {
      void shell.openExternal(url);
    }
  });
  contents.on("will-attach-webview", (event) => {
    event.preventDefault();
  });
});

app.whenReady().then(() => {
  createWindow();
  app.on("activate", () => {
    if (BrowserWindow.getAllWindows().length === 0) {
      createWindow();
    }
  });
});

app.on("window-all-closed", () => {
  if (activeProcess && !activeProcess.killed) {
    activeProcess.kill();
  }
  if (process.platform !== "darwin") {
    app.quit();
  }
});
