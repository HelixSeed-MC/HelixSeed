#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
CXX="${CXX:-g++}"

case "$(uname -s)" in
  Darwin)
    OUT="${SCRIPT_DIR}/libgpu_filter_opencl.dylib"
    EXTRA_LDFLAGS=()
    ;;
  *)
    OUT="${SCRIPT_DIR}/libgpu_filter_opencl.so"
    EXTRA_LDFLAGS=()
    ;;
esac

if ! command -v "${CXX}" >/dev/null 2>&1; then
  echo "[gpu_filter_opencl] Missing compiler: ${CXX}" >&2
  exit 1
fi

pushd "${SCRIPT_DIR}" >/dev/null

# OpenCL is loaded dynamically at runtime, so we deliberately do NOT link
# against -lOpenCL. The shared library has no link-time dependency on any
# OpenCL SDK; if the user has no OpenCL ICD, gpu_is_available() returns 0.
"${CXX}" -std=c++17 -O3 -fPIC -shared -DGPU_FILTER_BUILD_DLL \
  gpu_filter_opencl.cpp \
  -ldl -lpthread \
  "${EXTRA_LDFLAGS[@]}" \
  -o "${OUT}"

cp -f "${OUT}" "${REPO_ROOT}/$(basename "${OUT}")"
popd >/dev/null

echo "[gpu_filter_opencl] Built ${OUT}"
echo "[gpu_filter_opencl] Copied to project root: $(basename "${OUT}")"
