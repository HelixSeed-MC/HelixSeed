import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const uiRoot = path.resolve(here, "..");
const repoRoot = path.resolve(uiRoot, "..");
const stageRoot = path.join(uiRoot, "package-resources");

function copyFile(src, dst) {
  fs.mkdirSync(path.dirname(dst), { recursive: true });
  fs.copyFileSync(src, dst);
}

function copyDir(src, dst) {
  fs.rmSync(dst, { recursive: true, force: true });
  fs.cpSync(src, dst, { recursive: true });
}

fs.rmSync(stageRoot, { recursive: true, force: true });
fs.mkdirSync(stageRoot, { recursive: true });

copyFile(path.join(repoRoot, "native", "scanner_native.exe"), path.join(stageRoot, "native", "scanner_native.exe"));
copyFile(path.join(repoRoot, "native", "scanner_core.dll"), path.join(stageRoot, "native", "scanner_core.dll"));
copyFile(path.join(repoRoot, "gpu_filter.dll"), path.join(stageRoot, "gpu_filter.dll"));
copyFile(path.join(uiRoot, "assets", "HelixSeed.png"), path.join(stageRoot, "HelixSeed.png"));
copyFile(
  path.join(repoRoot, "cubiomes_12111_fork", "lib", "lib.dll"),
  path.join(stageRoot, "cubiomes_12111_fork", "lib", "lib.dll")
);
copyFile(
  path.join(repoRoot, "cubiomes_12111_fork", "src", "biomes.h"),
  path.join(stageRoot, "cubiomes_12111_fork", "src", "biomes.h")
);

copyDir(
  path.join(repoRoot, "GPULootSeedFinder", "target", "classes"),
  path.join(stageRoot, "GPULootSeedFinder", "target", "classes")
);
copyFile(
  path.join(repoRoot, "GPULootSeedFinder", "target", "runtime-classpath.txt"),
  path.join(stageRoot, "GPULootSeedFinder", "target", "runtime-classpath.txt")
);

console.log(`Staged HelixSeed resources in ${stageRoot}`);
