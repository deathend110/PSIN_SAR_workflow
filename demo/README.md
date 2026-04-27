# Demo Simple Infer

`demo/` 是一个独立于 `main/` 源码结构的小程序，只保留最小离线推理能力：

- 读取极简 YAML 配置
- 遍历输入目录中的图片
- 跳过非 `512x512` 图片
- 对合法图片执行一次模型推理
- 为每张图片输出 `restore.png` 与 `mask_class.png`

## 非目标

- 不复用 `main/src/**`
- 不包含 Web、HDMI、manual flight、patch planner
- 不生成彩色 mask 可视化

## 输入要求

- 支持扩展名：`.png`、`.jpg`、`.jpeg`、`.bmp`
- 图片按灰度方式读取
- 只有 `512x512` 图像会进入推理
- 不可读或尺寸不匹配的图片会打印 `skipped: ...` 并跳过

## 配置示例

`demo/configs/demo_infer.yaml`

```yaml
demo:
  input_dir: ./io/demo_input
  output_dir: ./io/demo_output
  json_path: ./models/model.json
  raw_path: ./models/model.raw
```

## 构建

推荐直接使用脚本：

```bash
cd demo
./build_demo.sh
```

说明：

- 不一定要带 `clean`
- `./build_demo.sh` 适合正常增量编译
- `./build_demo.sh clean` 适合你改过 `CMakeLists.txt`、切换工具链、怀疑旧缓存干扰，或者想彻底重配时使用

常用变体：

```bash
./build_demo.sh clean
./build_demo.sh --debug
./build_demo.sh clean --debug
./build_demo.sh --no-tests
```

参数作用：

- `clean`
  - 删除 `demo/build/ZG`
  - 让下一次 `cmake` 从干净目录重新配置
- `--debug`
  - 设置 `CMAKE_BUILD_TYPE=Debug`
  - 方便调试和看更完整的符号信息
- `--release`
  - 设置 `CMAKE_BUILD_TYPE=Release`
  - 这是默认值，适合正常交叉编译产出
- `--no-tests`
  - 设置 `DEMO_BUILD_TESTS=OFF`
  - 只编译 `demo_simple_infer`，不跑 host 侧测试
- `-h` / `--help`
  - 打印脚本帮助信息

如果你想手动执行 `cmake`，也可以：

```bash
cmake -S demo -B demo/build/ZG \
  -DCMAKE_BUILD_TYPE=Release \
  -DDEMO_BUILD_TESTS=ON \
  -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchains/zg330-aarch64.cmake
cmake --build demo/build/ZG --target demo_simple_infer -j$(nproc)
```

说明：

- `build_demo.sh` 只面向 Linux/ZG330 工具链环境
- 默认使用 `../cmake/toolchains/zg330-aarch64.cmake` 交叉编译
- 默认构建 `Release`
- 默认会在编译完成后执行 host 侧测试
- 可通过环境变量覆盖：

```bash
BUILD_TYPE=Debug DEMO_BUILD_TESTS=OFF ./build_demo.sh clean
TOOLCHAIN_FILE=/path/to/your-toolchain.cmake ./build_demo.sh clean
```

## 运行

```bash
./build/ZG/demo_simple_infer ./configs/demo_infer.yaml
```

## 输出结构

```text
output_dir/
  sample_a/
    restore.png
    mask_class.png
  sample_b/
    restore.png
    mask_class.png
```

- `restore.png`：单通道 `uint8` 灰度图
- `mask_class.png`：单通道 `uint8`，像素值直接为类别编号 `1~6`

## 快速自测

```bash
ctest --test-dir ./build/ZG/ --output-on-failure
```
