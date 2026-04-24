# `main/` Overview

`main/` 是当前仓库里真正参与构建和运行的主工程目录，面向 `Linux + aarch64 + ZG330`。

当前工作流已经收口为一个单入口程序：

```text
echo.bin -> RD 成像 -> SAR PNG -> 512x512 patch 扫描 -> 模型推理 -> 恢复图 / 分割图 -> HDMI 或 PNG
```

注意：

- `main/CMakeLists.txt` 明确禁止在 Windows 上直接配置构建。
- 当前版本是“单入口 + 单次只跑一个模式”。
- v1 不支持在同一次进程里自动串联 `RD + Inference`。

## 1. Directory Map

```text
main/
  CMakeLists.txt
  README.md
  build_main.sh
  compile_combine_bf16.toml
  Host Computer Software.md
  .icraft/
  build/
    ZG/
  configs/
    infer_workflow.yaml
    rd_imaging.yaml
  imodel/
    ZG/
      bf16/
      int8/
  include/
    infer_workflow_hdmi_display.hpp
    workflow/
      infer/
      rd/
      shared/
  io/
    echo/
    output/
    sar_img/
  log/
  src/
    config_utils.cpp
    infer_workflow.cpp
    infer_config.cpp
    main.cpp
    rd_config.cpp
    rd_imaging_stream.cpp
    hdmi_ui_preview_1080_p_industrial.jsx
```

## 2. Current Build Output

当前只构建一个主程序：

```text
build/ZG/psin_workflow
```

这个可执行程序启动后会显示菜单：

```text
1. RD only
2. Inference only
0. Exit
```

模式说明：

- `RD only`
  - 读取 `configs/rd_imaging.yaml`
  - 扫描 `io/echo/*.bin`
  - 输出 `io/sar_img/*.png`
- `Inference only`
  - 读取 `configs/infer_workflow.yaml`
  - 扫描 `io/sar_img/*.png`
  - 输出到 HDMI 或 `io/output/**/*.png`
- `Exit`
  - 不做任何处理，直接退出

## 3. Module Layout

### `src/main.cpp`

单入口主程序，只负责：

- 显示启动菜单
- 解析用户选择
- 调用 `workflow::rd::Run(...)` 或 `workflow::infer::Run(...)`

### `include/workflow/shared` + `src/config_utils.cpp`

公共基础能力，目前只抽取真正重复的部分：

- 运行模式枚举 `AppMode`
- 简单 YAML 文本读取
- 字符串清理、布尔值解析、默认值读取

### `include/workflow/rd` + `src/rd_config.cpp` + `src/rd_imaging_stream.cpp`

RD 成像模块：

- `rd_config.*`
  - 解析 `configs/rd_imaging.yaml`
- `rd_imaging_stream.cpp`
  - 收集 echo 文件
  - 校验输入格式
  - 根据内存预算选择执行模式
  - 执行 RD pipeline
  - 写出 SAR PNG
  - 管理 scratch 生命周期

### `include/workflow/infer` + `src/infer_config.cpp` + `src/infer_workflow.cpp`

推理模块：

- `infer_config.*`
  - 解析 `configs/infer_workflow.yaml`
- `infer_workflow.cpp`
  - 收集 SAR PNG
  - 生成蛇形 patch
  - 构建 `NHWC FP32` 输入 tensor
  - 初始化 session / device
  - 执行推理
  - 后处理恢复图与分割图
  - 输出到 HDMI 或 PNG

### `include/infer_workflow_hdmi_display.hpp`

HDMI 适配层，负责：

- 分配 RGB565 UDMA buffer
- 将图像数据送到板端显示硬件

## 4. Runtime Data Flow

### RD only

```text
io/echo/*.bin
 -> workflow::rd::Run()
 -> memory_float32 / memory_double / scratch_double
 -> io/sar_img/*.png
```

### Inference only

```text
io/sar_img/*.png
 -> workflow::infer::Run()
 -> SnakePatchSource
 -> PatchTensorBuilder
 -> PatchInferenceRunner
 -> restore gray + seg mask
 -> HDMI or io/output/<stem>/patch_*.png
```

## 5. Build

推荐在 `main/` 目录下构建：

```bash
cd main
chmod +x build_main.sh
./build_main.sh
```

手动构建方式：

```bash
cd main
mkdir -p log
cmake -S . -B build/ZG -DCMAKE_BUILD_TYPE=Release 2>&1 | tee log/cmake_configure.log
cmake --build build/ZG -j$(nproc) 2>&1 | tee log/cmake_build.log
```

说明：

- `CMakeLists.txt` 当前固定使用 `aarch64-linux-gnu-gcc/g++`
- 依赖从 `../../deps` 和系统里的 icraft 包解析
- Windows 侧配置会直接 `FATAL_ERROR`

## 6. Run

必须在 `main/` 目录下启动，原因是菜单内置使用相对配置路径：

```bash
cd main
./build/ZG/psin_workflow
```

程序启动后，根据菜单选择模式：

- 选 `1`
  - 执行 `configs/rd_imaging.yaml`
- 选 `2`
  - 执行 `configs/infer_workflow.yaml`
- 选 `0`
  - 直接退出

## 7. Config Files

### `configs/rd_imaging.yaml`

关键字段：

- `rd.execution_mode`
- `rd.echo_dir`
- `rd.output_dir`
- `rd.scratch_dir`
- `rd.memory_limit_mb`
- `rd.keep_scratch`
- `rd.column_tile`
- `rd.row_tile`

默认目录关系：

```text
echo_dir    = ./io/echo
output_dir  = ./io/sar_img
scratch_dir = ./io/rd_scratch
```

### `configs/infer_workflow.yaml`

关键字段：

- `sys.device`
- `sys.run_backend`
- `sys.profile`
- `pipeline.patch.mode`
- `pipeline.patch.patch_size`
- `pipeline.patch.stride`
- `pipeline.icore.json`
- `pipeline.icore.raw`
- `pipeline.output_wait_ms`
- `display.width`
- `display.height`
- `display.fps`
- `output.mode`
- `output.dir`
- `output.overwrite`
- `debug.dump_backend_log`

当前模型接口约束：

```text
input[0]  = [1,512,512,1] FP32
output[0] = [1,512,512,1] FP32
output[1] = [1,512,512,6] FP32
```

## 8. Important Behavior Invariants

- `Inference only` 只读取 SAR PNG，不在 v1 中自动先跑 RD。
- patch 规则保持不变：
  - `patch_size=512`
  - `stride=256`
  - `auto_snake`
  - 边缘不足完整 patch 直接丢弃
- `output.mode=png` 时，输出路径保持为 `io/output/<stem>/patch_*.png`
- `waitForReady()` 与 `device.reset(1)` 的顺序不能随意改
- `ManualFlightPatchSource` 仍然只是预留扩展点，当前不接控制端

## 9. Files Present But Not In Current Main Path

- `compile_combine_bf16.toml`
  - 模型编译或部署相关配置，不是运行入口
- `Host Computer Software.md`
  - 后续控制端扩展草案
- `src/hdmi_ui_preview_1080_p_industrial.jsx`
  - HDMI UI 预览稿，当前不参与 CMake 构建，也不接入主程序

## 10. Quick Checks

检查 echo 输入：

```bash
ls io/echo
```

检查 SAR PNG：

```bash
ls io/sar_img
```

检查模型文件：

```bash
ls ./imodel/ZG/bf16/
```

离线调试时，如不接 HDMI，可改为：

```yaml
output:
  mode: png
```

然后查看：

```text
io/output/
```
