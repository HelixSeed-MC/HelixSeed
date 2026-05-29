#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build_linux"
LIB_DIR="${SCRIPT_DIR}/lib"

mkdir -p "${BUILD_DIR}" "${LIB_DIR}"

cmake -S "${SCRIPT_DIR}/src" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}" --config Release -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

if [[ -f "${BUILD_DIR}/libcubiomes.so" ]]; then
  cp -f "${BUILD_DIR}/libcubiomes.so" "${LIB_DIR}/lib.so"
fi

echo "[cubiomes] Built Linux shared library in ${BUILD_DIR}"
