# OpenCL prefilter backend

Parallel implementation of the `gpu_filter` ABI used by `scanner_native`.
Selectable from the UI ("GPU Backend" dropdown in the Performance panel) or
the scanner CLI (`--gpu-backend opencl`).

## Why a second backend

The CUDA backend in `../cuda/` is NVIDIA-only and requires a CUDA toolkit
build environment. The OpenCL backend works on:

- Windows / Linux with any vendor's OpenCL runtime (NVIDIA, AMD, Intel)
- macOS (uses Apple's built-in OpenCL framework — deprecated but still
  functional through at least macOS 14)
- Systems without the CUDA toolkit installed

## Build

### Windows

```bat
opencl\build_opencl.bat
```

Requires MSVC (Visual Studio Build Tools 2022). No OpenCL SDK install
needed — the runtime is loaded dynamically.

### Linux / macOS

```bash
./opencl/build_opencl.sh
```

Requires `g++` (or set `CXX`). No `-lOpenCL` link — OpenCL is loaded at
runtime via `dlopen("libOpenCL.so.1")` (Linux) or the Apple OpenCL
framework path (macOS).

## Runtime expectations

At runtime the library searches, in order:

1. `$OPENCL_LIBRARY` (env override)
2. Platform default (`OpenCL.dll`, `/System/.../OpenCL.framework/OpenCL`,
   `libOpenCL.so.1`, `libOpenCL.so`)

If none are found, `gpu_is_available()` returns 0 and `scanner_native`
reports an actionable error.

## ABI parity with CUDA

Exports the same symbols as `cuda/gpu_filter.dll`:

- `gpu_is_available`, `gpu_total_mem`
- `gpu_filter`, `gpu_filter_multi`, `gpu_filter_multi_checked`
- `gpu_filter_double_buffer_available` — returns 0 (sync only)

The async submit/collect entry points are intentionally not exported.
`scanner_native` checks for their presence and falls back to the sync
path when missing.

## Kernel parity

The kernel in `gpu_filter_opencl.cpp` (embedded as a string constant)
mirrors the scalar paths of `cuda/gpu_filter.cu`:

- Identical Java LCG and `next_int` rejection logic
- Identical `region_hit_exact` math for both linear and triangular spread
- Identical multi-constraint loop with `min_required`, quad-span
  detection and early termination

ILP-4 micro-tiling is left to the OpenCL driver's auto-vectorizer, so
raw throughput on NVIDIA cards will be lower than the CUDA backend on
the same hardware. Use CUDA when available; use OpenCL when not.
