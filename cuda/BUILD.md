# Native CUDA Backend Build

## Linux

Prerequisites:

- NVIDIA CUDA toolkit with `nvcc`
- `g++`

Build from repository root:

```bash
./cuda/build_linux.sh
```

Expected outputs:

- `cuda/libgpu_filter.so`
- `libgpu_filter.so` (copied to project root)

If the library is elsewhere:

```bash
export GPU_FILTER_DLL=/path/to/libgpu_filter.so
```

## Windows

Build from repository root:

```powershell
cmd /c cuda\build_cuda.bat
```

Expected outputs:

- `cuda\gpu_filter.dll`
- `gpu_filter.dll`

## Notes

- Kernel uses integer-only math.
- `RegionTerm` and `ConstraintDesc` arrays are staged in constant memory.
- Python side calls `gpu_filter` / `gpu_filter_multi` via `ctypes`.
