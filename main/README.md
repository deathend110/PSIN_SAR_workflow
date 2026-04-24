# main/README.md

`main/` 是当前仓库里真正可编译、可运行的主工程目录，包含第 0 阶段的完整 SAR 工作流：

```text
echo.bin -> RD 成像 -> SAR PNG -> 512x512 patch 蛇形扫描 -> 模型推理 -> 恢复图/分割图后处理 -> HDMI 或 PNG
```

当前工程面向 `Linux + aarch64 + ZG330`，`main/CMakeLists.txt` 已明确禁止 Windows 编译此目录。

## 1. 当前目录分布

```text
main/
  CMakeLists.txt
  build_full_workflow.sh
  compile_combine_bf16.toml
  Host Computer Software.md
  README.md
  .icraft/
    logs/
  build/
    ZG/
      full_workflow
      rd_imaging_stream
  configs/
    full_workflow.yaml
    rd_imaging.yaml
  imodel/
    ZG/
      bf16/
      int8/
  include/
    full_workflow_hdmi_display.hpp
  io/
    echo/
    sar_img/
    output/
  log/
    cmake_configure.log
    cmake_build.log
  src/
    full_workflow.cpp
    rd_imaging_stream.cpp
    hdmi_ui_preview_1080_p_industrial.jsx
```

各目录作用：

- `src/`
  - `rd_imaging_stream.cpp`：独立 RD 成像程序，负责 `echo.bin -> SAR PNG`
  - `full_workflow.cpp`：读取 SAR PNG、切 patch、调用模型推理、输出 HDMI/PNG
  - `hdmi_ui_preview_1080_p_industrial.jsx`：HDMI UI 预览稿，不参与当前 CMake 构建

- `configs/`
  - `rd_imaging.yaml`：RD 成像阶段配置
  - `full_workflow.yaml`：推理与显示阶段配置

- `include/`
  - `full_workflow_hdmi_display.hpp`：板端 RGB565 HDMI 显示适配层

- `imodel/`
  - 保存当前板端模型文件，`full_workflow` 默认读取 `imodel/ZG/bf16/`

- `io/`
  - `echo/`：输入回波 `.bin`
  - `sar_img/`：RD 成像输出的 SAR PNG，也可直接放入已有 SAR 图跳过第 1 阶段
  - `output/`：`output.mode=png` 时保存并排调试图

- `build/`
  - 当前工作区里已经存在 `build/ZG/` 构建产物

- `log/`
  - 保存 CMake configure / build 日志

- `.icraft/logs/`
  - `debug.dump_backend_log=true` 时生成后端部署/内存优化日志

## 2. 当前可执行程序

编译后会生成两个程序：

```text
build/ZG/rd_imaging_stream
build/ZG/full_workflow
```

职责划分：

1. `rd_imaging_stream`
   - 读取 `io/echo/*.bin`
   - 根据内存预算在 `memory_float32` / `memory_double` / `scratch_double` 之间切换
   - 输出 `io/sar_img/*.png`

2. `full_workflow`
   - 读取 `io/sar_img/*.png`
   - 按 `512x512`、`stride=256` 做自动蛇形 patch 扫描
   - 将 patch 转为 `NHWC [1,512,512,1] FP32`
   - 调用 icraft session 推理
   - 输出恢复图和分割 mask 的并排结果
   - 按配置写 HDMI 或 PNG

主调用关系：

```text
rd_imaging_stream
 -> io/echo/*.bin
 -> io/sar_img/*.png

full_workflow
 -> io/sar_img/*.png
 -> patch infer
 -> HDMI / io/output/**/*.png
```

## 3. 构建

推荐在 `main/` 目录下构建：

```bash
cd main
chmod +x build_full_workflow.sh
./build_full_workflow.sh
```

日志位置：

```text
log/cmake_configure.log
log/cmake_build.log
```

手动构建方式：

```bash
cd main
mkdir -p log
cmake -S . -B build/ZG -DCMAKE_BUILD_TYPE=Release 2>&1 | tee log/cmake_configure.log
cmake --build build/ZG -j$(nproc) 2>&1 | tee log/cmake_build.log
```

说明：

- 当前 `CMakeLists.txt` 会直接指定 `aarch64-linux-gnu-gcc/g++`
- 工程会链接 `../../deps` 下的头文件和静态库
- `WIN32` 会直接 `FATAL_ERROR`

## 4. 运行方式

### 4.1 第 1 阶段：echo -> SAR

当 `io/echo/` 中有回波文件、`io/sar_img/` 还没有结果时，先运行：

```bash
cd main
./build/ZG/rd_imaging_stream configs/rd_imaging.yaml
```

输入示例：

```text
io/echo/1_hh_amp_echo_1bit.bin
io/echo/1_hv_amp_echo_1bit.bin
```

输出示例：

```text
io/sar_img/1_hh_amp_echo_1bit.png
io/sar_img/1_hv_amp_echo_1bit.png
```

### 4.2 第 2 阶段：SAR -> 推理 -> 输出

当 `io/sar_img/` 中已经有 SAR 图片时，运行：

```bash
cd main
./build/ZG/full_workflow configs/full_workflow.yaml
```

内部流程：

```text
1. 遍历 io/sar_img 下的 PNG
2. 读取为灰度图并归一化到 0~1
3. 用 512x512、stride=256 做 auto_snake 扫描
4. 构造 [1,512,512,1] FP32 tensor
5. 调用 session.forward
6. output[0] 转恢复灰度图
7. output[1] 对 6 类 logits 做 argmax，并映射为 RGB mask
8. 左右并排合成 frame
9. 根据 output.mode 输出到 HDMI 或 PNG
```

## 5. 关键配置

### 5.1 `configs/rd_imaging.yaml`

主要字段：

- `rd.execution_mode`
  - `auto`
  - `memory_float32`
  - `scratch_double`
- `rd.echo_dir`
- `rd.output_dir`
- `rd.scratch_dir`
- `rd.column_tile`
- `rd.row_tile`
- `rd.memory_limit_mb`
- `rd.keep_scratch`

默认输入输出关系：

```text
echo_dir    = ./io/echo
output_dir  = ./io/sar_img
scratch_dir = ./io/rd_scratch
```

### 5.2 `configs/full_workflow.yaml`

主要字段：

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
input[0]  = [1,512,512,1]   FP32
output[0] = [1,512,512,1]   FP32 restore
output[1] = [1,512,512,6]   FP32 seg logits
```

## 6. 输出模式

### 6.1 HDMI

```yaml
output:
  mode: hdmi
```

当前 HDMI 画面是左右并排：

```text
左侧：恢复灰度图
右侧：分割 mask RGB 图
```

显示参数来自：

```yaml
display:
  width: 1920
  height: 1080
  fps: 0
```

### 6.2 PNG 调试输出

```yaml
output:
  mode: png
  dir: ./io/output
  overwrite: true
```

输出示例：

```text
io/output/1_hh_amp_echo_1bit/patch_000000.png
io/output/1_hh_amp_echo_1bit/patch_000001.png
```

每张图仍然是“左恢复图 + 右分割图”的并排格式。

## 7. 当前工作区里的辅助文件

- `compile_combine_bf16.toml`
  - 模型编译/部署相关配置文件，不是运行时入口

- `Host Computer Software.md`
  - 上位机/控制端设计草稿，属于后续扩展方向

- `src/hdmi_ui_preview_1080_p_industrial.jsx`
  - HDMI 工业风 UI 预览稿，当前没有被 `CMakeLists.txt` 编译，也没有接入 `full_workflow`

## 8. 已知限制

- `main/` 当前只支持 Linux/ZG330，不维护 Windows 构建路径
- patch 尺寸实际上固定为 `512x512`
- 当前只实现 `pipeline.patch.mode=auto_snake`
- `ManualFlightPatchSource` 虽已在代码中预留，但还没接控制端
- `full_workflow.cpp` 里保留了部分本地成像辅助函数，但当前主入口并不走这条路径
- 当前整体是单主线程串行流程，没有显式多线程 pipeline

## 9. 常见检查项

### 找不到 echo 文件

```bash
ls io/echo
```

### 找不到 SAR PNG

```bash
ls io/sar_img
```

### 找不到模型 json/raw

```bash
ls ./imodel/ZG/bf16/
```

### HDMI 没有画面

检查：

```yaml
output:
  mode: hdmi
```

并确认板卡、HDMI 线、显示器分辨率与 `display.width/height` 匹配。

### 想先离线调试，不接 HDMI

改为：

```yaml
output:
  mode: png
```

然后查看：

```text
io/output/
```
