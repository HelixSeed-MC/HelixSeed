// Common packager helpers shared by package-{win,mac,linux}.mjs.
//
// Each platform script chooses a target, picks the right native filenames,
// and calls packageFor(...) with that context. Keeping the per-platform
// scripts thin makes it easy to add a new target later.

import fs from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { packager } from "@electron/packager";

const here = path.dirname(fileURLToPath(import.meta.url));
export const uiRoot = path.resolve(here, "..");
export const repoRoot = path.resolve(uiRoot, "..");

const sharedIgnore = [
  /^\/assets($|\/)/,
  /^\/build($|\/)/,
  /^\/node_modules($|\/)/,
  /^\/package-resources($|\/)/,
  /^\/scripts($|\/)/,
  /^\/src($|\/)/,
  /^\/index\.html$/,
  /^\/package-lock\.json$/,
  /^\/tsconfig.*\.json$/,
  /^\/vite\.config\.ts$/,
];

export function stageFor(platform) {
  // Invoke staging in a child process with STAGE_PLATFORM set, so we don't
  // need cross-env in npm scripts.
  const res = spawnSync(process.execPath, [path.join(uiRoot, "scripts", "stage-resources.mjs")], {
    stdio: "inherit",
    env: { ...process.env, STAGE_PLATFORM: platform },
  });
  if (res.status !== 0) {
    process.exit(res.status ?? 1);
  }
}

export async function packageFor({ platform, arch, icon, gpuFilterName, gpuFilterOpenclName }) {
  const extraResource = [
    path.join(uiRoot, "package-resources", "native"),
    path.join(uiRoot, "package-resources", "cubiomes_26.1.2_fork"),
    path.join(uiRoot, "package-resources", "GPULootSeedFinder"),
    path.join(uiRoot, "package-resources", "HelixSeed.png"),
  ];

  const cudaStaged = path.join(uiRoot, "package-resources", gpuFilterName);
  if (fs.existsSync(cudaStaged)) {
    extraResource.push(cudaStaged);
  } else if (platform === "win32") {
    // Win packaging has historically required CUDA; bail loudly.
    throw new Error(`Missing staged ${gpuFilterName}. Build cuda/build_cuda.bat then re-stage.`);
  }

  const openclStaged = path.join(uiRoot, "package-resources", gpuFilterOpenclName);
  if (fs.existsSync(openclStaged)) {
    extraResource.push(openclStaged);
  }

  const packagerOptions = {
    dir: uiRoot,
    name: "HelixSeed",
    platform,
    arch,
    out: path.join(repoRoot, "dist-ts"),
    overwrite: true,
    asar: true,
    prune: true,
    appBundleId: "local.helixseed.ui",
    executableName: "HelixSeed",
    extraResource,
    ignore: sharedIgnore,
  };
  if (icon && fs.existsSync(icon)) {
    packagerOptions.icon = icon;
  }
  return packager(packagerOptions);
}
