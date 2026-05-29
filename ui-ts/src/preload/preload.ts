import { contextBridge, ipcRenderer } from "electron";
import type { AppContext, ScanExit, ScannerProgress, ScanStartRequest, ScanStartResult } from "../shared/contracts";

const api = {
  getContext: (): Promise<AppContext> => ipcRenderer.invoke("app:get-context"),
  openPath: (): Promise<void> => ipcRenderer.invoke("app:open-path"),
  openExternal: (url: string): Promise<void> => ipcRenderer.invoke("app:open-external", url),
  openDocs: (): Promise<void> => ipcRenderer.invoke("app:open-docs"),
  startScan: (request: ScanStartRequest): Promise<ScanStartResult> => ipcRenderer.invoke("scan:start", request),
  stopScan: (): Promise<boolean> => ipcRenderer.invoke("scan:stop"),
  sendControl: (command: "pause" | "resume" | "terminal_on" | "terminal_off" | "snapshot"): Promise<boolean> =>
    ipcRenderer.invoke("scan:control", command),
  onProgress: (callback: (payload: ScannerProgress) => void): (() => void) => {
    const listener = (_event: Electron.IpcRendererEvent, payload: ScannerProgress) => callback(payload);
    ipcRenderer.on("scan:progress", listener);
    return () => ipcRenderer.removeListener("scan:progress", listener);
  },
  onLine: (callback: (line: string) => void): (() => void) => {
    const listener = (_event: Electron.IpcRendererEvent, line: string) => callback(line);
    ipcRenderer.on("scan:line", listener);
    return () => ipcRenderer.removeListener("scan:line", listener);
  },
  onSeed: (callback: (seed: string) => void): (() => void) => {
    const listener = (_event: Electron.IpcRendererEvent, seed: string) => callback(seed);
    ipcRenderer.on("scan:seed", listener);
    return () => ipcRenderer.removeListener("scan:seed", listener);
  },
  onExit: (callback: (payload: ScanExit) => void): (() => void) => {
    const listener = (_event: Electron.IpcRendererEvent, payload: ScanExit) => callback(payload);
    ipcRenderer.on("scan:exit", listener);
    return () => ipcRenderer.removeListener("scan:exit", listener);
  }
};

contextBridge.exposeInMainWorld("helixSeed", api);

export type HelixSeedApi = typeof api;
