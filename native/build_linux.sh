#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
CXX="${CXX:-g++}"
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
COMMON_FLAGS=(-std=c++20 -O3 -DNDEBUG -Wall -Wextra -fPIC)
LINK_FLAGS=(-ldl -lpthread)

if ! command -v "${CXX}" >/dev/null 2>&1; then
  echo "[scanner_core] Missing compiler: ${CXX}" >&2
  exit 1
fi

if [[ -x "${REPO_ROOT}/cubiomes_26.1.2_fork/build_linux.sh" ]]; then
  "${REPO_ROOT}/cubiomes_26.1.2_fork/build_linux.sh"
fi

pushd "${SCRIPT_DIR}" >/dev/null

"${CXX}" "${COMMON_FLAGS[@]}" -DSCANNER_CORE_BUILD_DLL -c scanner_core.cpp -o scanner_core.o
"${CXX}" -shared -o scanner_core.so scanner_core.o "${LINK_FLAGS[@]}"
"${CXX}" "${COMMON_FLAGS[@]}" scanner_native.cpp -L. -Wl,-rpath,'$ORIGIN' -Wl,--enable-new-dtags -l:scanner_core.so -o scanner_native "${LINK_FLAGS[@]}"
"${CXX}" "${COMMON_FLAGS[@]}" scanner_backend.cpp -o scanner_backend

rm -f scanner_core.o
popd >/dev/null

echo "[scanner_core] Built native/scanner_core.so"
echo "[scanner_core] Built native/scanner_native"
echo "[scanner_core] Built native/scanner_backend"
