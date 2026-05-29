import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const uiRoot = path.resolve(here, "..");
const repoRoot = path.resolve(uiRoot, "..");
const stageRoot = path.join(uiRoot, "package-resources");

// Target platform is normally inferred from the host (process.platform). The
// dist:mac and dist:linux scripts override via STAGE_PLATFORM so a Windows
// host can still produce a Mac/Linux staging directory provided the relevant
// native binaries have been pre-built.
const targetPlatform = process.env.STAGE_PLATFORM ?? process.platform;

function platformNames(platform) {
  switch (platform) {
    case "win32":
      return {
        scannerExe: "scanner_native.exe",
        scannerCore: "scanner_core.dll",
        gpuFilter: "gpu_filter.dll",
        gpuFilterOpencl: "gpu_filter_opencl.dll",
        cubiomesLib: "lib.dll",
      };
    case "darwin":
      return {
        scannerExe: "scanner_native",
        scannerCore: "scanner_core.dylib",
        gpuFilter: "libgpu_filter.dylib",
        gpuFilterOpencl: "libgpu_filter_opencl.dylib",
        cubiomesLib: "lib.dylib",
      };
    case "linux":
      return {
        scannerExe: "scanner_native",
        scannerCore: "scanner_core.so",
        gpuFilter: "libgpu_filter.so",
        gpuFilterOpencl: "libgpu_filter_opencl.so",
        cubiomesLib: "lib.so",
      };
    default:
      throw new Error(`Unsupported stage platform: ${platform}`);
  }
}

function copyFile(src, dst) {
  fs.mkdirSync(path.dirname(dst), { recursive: true });
  fs.copyFileSync(src, dst);
}

function copyOptional(src, dst, label) {
  if (fs.existsSync(src)) {
    copyFile(src, dst);
    return true;
  }
  console.log(`[stage] Skipped optional ${label}: ${src} not found`);
  return false;
}

function copyDir(src, dst) {
  fs.rmSync(dst, { recursive: true, force: true });
  fs.cpSync(src, dst, { recursive: true });
}

function firstExisting(paths) {
  return paths.find((candidate) => fs.existsSync(candidate)) ?? null;
}

const names = platformNames(targetPlatform);

fs.rmSync(stageRoot, { recursive: true, force: true });
fs.mkdirSync(stageRoot, { recursive: true });

const scannerSrc = path.join(repoRoot, "native", names.scannerExe);
const coreSrc = path.join(repoRoot, "native", names.scannerCore);
if (!fs.existsSync(scannerSrc)) {
  throw new Error(`Native scanner missing: ${scannerSrc}. Build native first.`);
}
if (!fs.existsSync(coreSrc)) {
  throw new Error(`Scanner core library missing: ${coreSrc}. Build native first.`);
}
copyFile(scannerSrc, path.join(stageRoot, "native", names.scannerExe));
copyFile(coreSrc, path.join(stageRoot, "native", names.scannerCore));

// CUDA backend (required for Windows historically; optional elsewhere since
// most Linux/macOS machines won't have a CUDA build available).
const cudaSrc = firstExisting([
  path.join(repoRoot, "cuda", names.gpuFilter),
  path.join(repoRoot, names.gpuFilter),
]);
if (targetPlatform === "win32") {
  if (!cudaSrc) {
    throw new Error("CUDA backend missing. Run cuda/build_cuda.bat first.");
  }
  copyFile(cudaSrc, path.join(stageRoot, names.gpuFilter));
} else {
  if (cudaSrc) {
    copyFile(cudaSrc, path.join(stageRoot, names.gpuFilter));
  } else {
    console.log(`[stage] Skipped optional CUDA backend: ${names.gpuFilter} not found`);
  }
}

// OpenCL backend — bundled when available so the runtime can pick either.
const openclSrc = firstExisting([
  path.join(repoRoot, "opencl", names.gpuFilterOpencl),
  path.join(repoRoot, names.gpuFilterOpencl),
]);
if (openclSrc) {
  copyFile(openclSrc, path.join(stageRoot, names.gpuFilterOpencl));
} else {
  console.log(`[stage] Skipped optional OpenCL backend: ${names.gpuFilterOpencl} not found`);
}

copyFile(path.join(uiRoot, "assets", "HelixSeed.png"), path.join(stageRoot, "HelixSeed.png"));

const cubiSrc = path.join(repoRoot, "cubiomes_26.1.2_fork", "lib", names.cubiomesLib);
if (!fs.existsSync(cubiSrc)) {
  throw new Error(`Cubiomes lib missing: ${cubiSrc}. Build cubiomes_26.1.2_fork first.`);
}
copyFile(cubiSrc, path.join(stageRoot, "cubiomes_26.1.2_fork", "lib", names.cubiomesLib));
copyFile(
  path.join(repoRoot, "cubiomes_26.1.2_fork", "src", "biomes.h"),
  path.join(stageRoot, "cubiomes_26.1.2_fork", "src", "biomes.h")
);

copyDir(
  path.join(repoRoot, "GPULootSeedFinder", "target", "classes"),
  path.join(stageRoot, "GPULootSeedFinder", "target", "classes")
);
copyFile(
  path.join(repoRoot, "GPULootSeedFinder", "target", "runtime-classpath.txt"),
  path.join(stageRoot, "GPULootSeedFinder", "target", "runtime-classpath.txt")
);

console.log(`Staged HelixSeed resources for ${targetPlatform} in ${stageRoot}`);
