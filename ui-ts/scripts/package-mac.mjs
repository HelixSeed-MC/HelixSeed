import path from "node:path";
import { packageFor, stageFor, uiRoot } from "./package-shared.mjs";

stageFor("darwin");

// Default to arm64 since Apple Silicon is the common case. Override with
// `PACKAGE_ARCH=x64` (or `universal`) for Intel/universal builds.
const arch = process.env.PACKAGE_ARCH ?? "arm64";

await packageFor({
  platform: "darwin",
  arch,
  icon: path.join(uiRoot, "build", "icon.icns"),
  gpuFilterName: "libgpu_filter.dylib",
  gpuFilterOpenclName: "libgpu_filter_opencl.dylib",
});
