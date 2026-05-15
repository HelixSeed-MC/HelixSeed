import { packager } from "@electron/packager";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const uiRoot = path.resolve(here, "..");
const repoRoot = path.resolve(uiRoot, "..");

await packager({
  dir: uiRoot,
  name: "HelixSeed",
  platform: "win32",
  arch: "x64",
  out: path.join(repoRoot, "dist-ts"),
  overwrite: true,
  icon: path.join(uiRoot, "build", "icon.ico"),
  asar: true,
  prune: true,
  appBundleId: "local.helixseed.ui",
  executableName: "HelixSeed",
  extraResource: [
    path.join(uiRoot, "package-resources", "native"),
    path.join(uiRoot, "package-resources", "gpu_filter.dll"),
    path.join(uiRoot, "package-resources", "cubiomes_12111_fork"),
    path.join(uiRoot, "package-resources", "GPULootSeedFinder"),
    path.join(uiRoot, "package-resources", "HelixSeed.png")
  ],
  ignore: [
    /^\/assets($|\/)/,
    /^\/build($|\/)/,
    /^\/node_modules($|\/)/,
    /^\/package-resources($|\/)/,
    /^\/scripts($|\/)/,
    /^\/src($|\/)/,
    /^\/index\.html$/,
    /^\/package-lock\.json$/,
    /^\/tsconfig.*\.json$/,
    /^\/vite\.config\.ts$/
  ]
});
