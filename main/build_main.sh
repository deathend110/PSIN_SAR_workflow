#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

TARGET_NAME="psin_workflow"
LOG_DIR="${SCRIPT_DIR}/log"
BUILD_DIR="${SCRIPT_DIR}/build/ZG"
TOOLCHAIN_FILE="${SCRIPT_DIR}/../cmake/toolchains/zg330-aarch64.cmake"
mkdir -p "${LOG_DIR}"

CONFIGURE_LOG="${LOG_DIR}/cmake_configure.log"
BUILD_LOG="${LOG_DIR}/cmake_build.log"
OUTPUT_BIN="${BUILD_DIR}/${TARGET_NAME}"

if [[ "$(uname -s)" != "Linux" ]]; then
  echo "[error] build_main.sh is intended for Linux build hosts." >&2
  echo "[error] Current host: $(uname -s)" >&2
  exit 2
fi

if command -v nproc >/dev/null 2>&1; then
  JOBS="$(nproc)"
elif command -v getconf >/dev/null 2>&1; then
  JOBS="$(getconf _NPROCESSORS_ONLN)"
else
  JOBS=1
fi

{
  echo "[configure] $(date '+%F %T')"
  echo "[workdir] ${SCRIPT_DIR}"
  echo "[build_dir] ${BUILD_DIR}"
  echo "[target] ${TARGET_NAME}"
  echo "[toolchain] ${TOOLCHAIN_FILE}"
  cmake -S . -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DPSIN_BUILD_PROFILE=zg330 \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}"
} 2>&1 | tee "${CONFIGURE_LOG}"

{
  echo "[build] $(date '+%F %T')"
  echo "[jobs] ${JOBS}"
  cmake --build "${BUILD_DIR}" --target "${TARGET_NAME}" -j"${JOBS}"
} 2>&1 | tee "${BUILD_LOG}"

echo "Build logs:"
echo "  ${CONFIGURE_LOG}"
echo "  ${BUILD_LOG}"
echo "Expected output:"
echo "  ${OUTPUT_BIN}"
