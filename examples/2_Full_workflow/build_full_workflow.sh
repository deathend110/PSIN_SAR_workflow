#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

LOG_DIR="${SCRIPT_DIR}/log"
BUILD_DIR="${SCRIPT_DIR}/build/ZG"
mkdir -p "${LOG_DIR}"

CONFIGURE_LOG="${LOG_DIR}/cmake_configure.log"
BUILD_LOG="${LOG_DIR}/cmake_build.log"

{
  echo "[configure] $(date '+%F %T')"
  echo "[workdir] ${SCRIPT_DIR}"
  cmake -S . -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
} 2>&1 | tee "${CONFIGURE_LOG}"

{
  echo "[build] $(date '+%F %T')"
  cmake --build "${BUILD_DIR}" -j"$(nproc)"
} 2>&1 | tee "${BUILD_LOG}"

echo "Build logs:"
echo "  ${CONFIGURE_LOG}"
echo "  ${BUILD_LOG}"
