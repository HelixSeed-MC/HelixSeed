import path from "node:path";
import { packageFor, uiRoot } from "./package-shared.mjs";
import "./stage-resources.mjs";

await packageFor({
  platform: "win32",
  arch: "x64",
  icon: path.join(uiRoot, "build", "icon.ico"),
  gpuFilterName: "gpu_filter.dll",
  gpuFilterOpenclName: "gpu_filter_opencl.dll",
});
