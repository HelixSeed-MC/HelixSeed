import path from "node:path";
import { packageFor, stageFor, uiRoot } from "./package-shared.mjs";

stageFor("linux");

const arch = process.env.PACKAGE_ARCH ?? "x64";

await packageFor({
  platform: "linux",
  arch,
  icon: path.join(uiRoot, "build", "icon.png"),
  gpuFilterName: "libgpu_filter.so",
  gpuFilterOpenclName: "libgpu_filter_opencl.so",
});
