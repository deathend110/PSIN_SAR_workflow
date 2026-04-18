#!/bin/bash
# 一键编译所有示例工程

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PACKAGE_ROOT="$(dirname "$SCRIPT_DIR")"

BACKEND="${1:-ZG}"  # 设定ZG后端

echo "编译所有示例工程 (Backend: $BACKEND)"

cd "$PACKAGE_ROOT/examples"

for example_dir in $(find . -name "CMakeLists.txt" -exec dirname {} \;); do
    echo "========================================"
    echo "编译: $example_dir"
    echo "========================================"
    
    cd "$PACKAGE_ROOT/examples/$example_dir"
    
    cmake -S . -B "build/$BACKEND" -DTARGET_CHIP="$BACKEND"
    cmake --build "build/$BACKEND" -j$(nproc)
    
    echo "✓ 完成: $example_dir"
    echo
done

echo "所有示例编译完成!"
