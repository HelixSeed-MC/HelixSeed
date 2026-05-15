#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_SO="${SCRIPT_DIR}/libgpu_filter.so"
HOST_CXX="${CXX:-g++}"

if ! command -v nvcc >/dev/null 2>&1; then
  echo "[gpu_filter] nvcc not found." >&2
  exit 1
fi

pushd "${SCRIPT_DIR}" >/dev/null
nvcc \
  -std=c++17 \
  -O3 \
  -Xptxas -O3 \
  -lineinfo \
  -shared \
  -Xcompiler "-fPIC -O2 -pthread" \
  -ccbin "${HOST_CXX}" \
  -DGPU_FILTER_BUILD_DLL \
  gpu_filter.cu gpu_filter_wrapper.cpp \
  -o "${OUT_SO}"

cp -f "${OUT_SO}" "${SCRIPT_DIR}/../libgpu_filter.so"
popd >/dev/null

echo "[gpu_filter] Built ${OUT_SO}"
echo "[gpu_filter] Copied to project root: ../libgpu_filter.so"
