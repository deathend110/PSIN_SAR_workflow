#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build/ZG"
LOG_DIR="${SCRIPT_DIR}/log"
BUILD_TYPE="${BUILD_TYPE:-Release}"
DEMO_BUILD_TESTS="${DEMO_BUILD_TESTS:-ON}"
CMAKE_GENERATOR="${CMAKE_GENERATOR:-}"
TOOLCHAIN_FILE="${TOOLCHAIN_FILE:-${SCRIPT_DIR}/../cmake/toolchains/zg330-aarch64.cmake}"

usage() {
    cat <<'EOF'
Usage:
  ./build_demo.sh [clean] [--debug] [--release] [--no-tests]

Options:
  clean       Remove demo/build before configuring
  --debug     Configure with CMAKE_BUILD_TYPE=Debug
  --release   Configure with CMAKE_BUILD_TYPE=Release (default)
  --no-tests  Configure with DEMO_BUILD_TESTS=OFF

Environment overrides:
  BUILD_TYPE=<Debug|Release>
  DEMO_BUILD_TESTS=<ON|OFF>
  CMAKE_GENERATOR=<generator>
  TOOLCHAIN_FILE=<path-to-toolchain.cmake>
EOF
}

CLEAN_FIRST=0

for arg in "$@"; do
    case "${arg}" in
        clean)
            CLEAN_FIRST=1
            ;;
        --debug)
            BUILD_TYPE="Debug"
            ;;
        --release)
            BUILD_TYPE="Release"
            ;;
        --no-tests)
            DEMO_BUILD_TESTS="OFF"
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "[build_demo] Unknown argument: ${arg}" >&2
            usage
            exit 1
            ;;
    esac
done

if [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* || "$(uname -s)" == CYGWIN* ]]; then
    echo "[build_demo] This demo is intended for the Linux/ZG330 toolchain, not Windows shells." >&2
    exit 1
fi

if [[ ! -f "${TOOLCHAIN_FILE}" ]]; then
    echo "[build_demo] Toolchain file not found: ${TOOLCHAIN_FILE}" >&2
    exit 1
fi

if [[ "${CLEAN_FIRST}" -eq 1 ]]; then
    echo "[build_demo] Removing ${BUILD_DIR}"
    rm -rf "${BUILD_DIR}"
fi

mkdir -p "${BUILD_DIR}"
mkdir -p "${LOG_DIR}"

CONFIGURE_LOG="${LOG_DIR}/configure.log"
BUILD_LOG="${LOG_DIR}/build.log"
TEST_LOG="${LOG_DIR}/test.log"

CONFIGURE_CMD=(
    cmake
    -S "${SCRIPT_DIR}"
    -B "${BUILD_DIR}"
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
    -DDEMO_BUILD_TESTS="${DEMO_BUILD_TESTS}"
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}"
)

if [[ -n "${CMAKE_GENERATOR}" ]]; then
    CONFIGURE_CMD+=(-G "${CMAKE_GENERATOR}")
fi

echo "[build_demo] Configuring demo"
{
    echo "[configure] $(date '+%F %T')"
    echo "[workdir] ${SCRIPT_DIR}"
    echo "[build_dir] ${BUILD_DIR}"
    echo "[toolchain] ${TOOLCHAIN_FILE}"
    "${CONFIGURE_CMD[@]}"
} 2>&1 | tee "${CONFIGURE_LOG}"

echo "[build_demo] Building demo_simple_infer"
{
    echo "[build] $(date '+%F %T')"
    echo "[build_dir] ${BUILD_DIR}"
    cmake --build "${BUILD_DIR}" --target demo_simple_infer -j"$(nproc)"
} 2>&1 | tee "${BUILD_LOG}"

if [[ "${DEMO_BUILD_TESTS}" == "ON" ]]; then
    echo "[build_demo] Running host-side tests"
    ctest --test-dir "${BUILD_DIR}" --output-on-failure 2>&1 | tee "${TEST_LOG}"
fi

echo "[build_demo] Done"
echo "[build_demo] Binary: ${BUILD_DIR}/demo_simple_infer"
echo "[build_demo] Logs:"
echo "  - ${CONFIGURE_LOG}"
echo "  - ${BUILD_LOG}"
if [[ "${DEMO_BUILD_TESTS}" == "ON" ]]; then
    echo "  - ${TEST_LOG}"
fi
