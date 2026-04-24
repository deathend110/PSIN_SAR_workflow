# CODEBASE_MAP_TEMPLATE.md

> 这是当前仓库的“代码地图”。
> 重点记录真实入口、主调用链、核心类职责、数据流路径、资源生命周期和高风险点。

---

## 1. Main Entrypoint

| Entrypoint | File | Responsibility |
|---|---|---|
| `main()` | `main/src/main.cpp` | 显示启动菜单并调度 `RD only / Inference only / Exit` |
| `workflow::rd::Run(...)` | `main/src/rd_imaging_stream.cpp` | 完成 RD 成像流程 |
| `workflow::infer::Run(...)` | `main/src/infer_workflow.cpp` | 完成推理与显示/落盘流程 |

当前构建产物：

```text
main/build/ZG/psin_workflow
```

---

## 2. Main Call Chains

### 2.1 Menu dispatch

```text
main
 -> PromptForMode
 -> workflow::rd::Run("configs/rd_imaging.yaml")
 -> workflow::infer::Run("configs/infer_workflow.yaml")
 -> Exit
```

### 2.2 RD only

```text
workflow::rd::Run
 -> LoadConfig
 -> create_directories(output_dir, scratch_dir)
 -> collectEchoBins
 -> for each echo
    -> processOneEcho
       -> readEchoShapeAndValidate
       -> choose execution mode
          -> runMemoryFloat32Pipeline
          -> runRangeCompressionToMemory + runMemoryPipeline
          -> runRangeCompression + runAzimuthFft + runFusedRcmcAzimuthCompressionAndMagnitude
       -> writeNormalizedPng / writeNormalizedPngFromComplex
       -> cleanup scratch
```

### 2.3 Inference only

```text
workflow::infer::Run
 -> LoadConfig
 -> collectSarImages
 -> Device::Open
 -> loadNetwork
 -> network.view(0)
 -> validateNetworkIO
 -> PatchTensorBuilder
 -> initSession
 -> session.apply
 -> emitBackendLogIfRequested
 -> create sink
    -> HdmiFrameSink or PngFrameSink
 -> PatchInferenceRunner
 -> for each SAR
    -> loadSarImageNorm
    -> SnakePatchSource
    -> while next(packet)
       -> processPatch
          -> PatchTensorBuilder::build
          -> PatchInferenceRunner::forward
          -> restoreToGrayU8
          -> logitsToMaskBgr
          -> composeSideBySide
          -> sink.write
 -> Device::Close
```

---

## 3. Core Types And Responsibilities

| Type | File | Responsibility |
|---|---|---|
| `workflow::AppMode` | `main/include/workflow/shared/app_mode.hpp` | 定义菜单模式枚举 |
| `workflow::shared::*` | `main/include/workflow/shared/config_utils.hpp`, `main/src/config_utils.cpp` | 简单 YAML 读取、字符串处理、默认值解析 |
| `workflow::rd::AppConfig` | `main/include/workflow/rd/rd_config.hpp` | RD 阶段配置快照 |
| `workflow::infer::AppConfig` | `main/include/workflow/infer/infer_config.hpp` | 推理阶段配置快照 |
| `SnakePatchSource` | `main/src/infer_workflow.cpp` | 生成 `512x512`、`stride=256` 的蛇形 patch |
| `ManualFlightPatchSource` | `main/src/infer_workflow.cpp` | 预留的手动巡航 patch 接口，当前未接主流程 |
| `PatchTensorBuilder` | `main/src/infer_workflow.cpp` | 校验输入 shape/dtype，并把 patch 构造成 Host FP32 tensor |
| `PatchInferenceRunner` | `main/src/infer_workflow.cpp` | 封装 `session.forward -> waitForReady -> host copy -> device.reset(1)` |
| `IFrameSink` | `main/src/infer_workflow.cpp` | 最终输出抽象接口 |
| `PngFrameSink` | `main/src/infer_workflow.cpp` | 写 `io/output/<stem>/patch_*.png` |
| `HdmiFrameSink` | `main/src/infer_workflow.cpp` | 把最终帧送到 HDMI |
| `RGB565HDMIDisplay` | `main/include/infer_workflow_hdmi_display.hpp` | 分配 UDMA buffer 并驱动板端显示 |
| `RadarConfig` | `main/src/rd_imaging_stream.cpp`, `main/src/infer_workflow.cpp` | RD 参数常量集合 |
| `PatchInfo` / `PatchPacket` | `main/src/infer_workflow.cpp` | patch 元数据与 patch 数据承载 |
| `RuntimeState` | `main/src/infer_workflow.cpp` | 日志与统计上下文 |

---

## 4. Data Flow Path

### 4.1 End-to-end

```text
echo.bin
 -> RD 成像
 -> SAR grayscale PNG
 -> SAR 归一化
 -> 512x512 patch
 -> FP32 NHWC tensor
 -> session.forward
 -> restore output + seg logits
 -> 灰度恢复图 + 彩色 mask
 -> 左右并排 frame
 -> HDMI 或 PNG
```

### 4.2 Data format map

| Stage | Type | Shape / Rule |
|---|---|---|
| echo payload | `float32 interleaved complex` | `rows x cols x 2` |
| RD memory matrix | `CV_64FC2` or `CV_32FC2` | complex matrix |
| SAR file | grayscale PNG | `H x W` |
| loaded SAR | `CV_32FC1` | normalized to `0~1` |
| patch | `CV_32FC1` | fixed `512 x 512` |
| model input | `FP32` | `1 x 512 x 512 x 1` |
| restore output | `FP32` | `1 x 512 x 512 x 1` |
| seg logits | `FP32` | `1 x 512 x 512 x 6` |
| final frame | `CV_8UC3` | side-by-side output frame |
| HDMI buffer | RGB565 bytes | `width * height * 2` |

---

## 5. Thread / Resource Lifecycle

### 5.1 Thread model

当前两个模式都运行在单主线程：

- 没有项目级 patch worker
- 没有 RD / infer 并行流水线
- 没有显式锁、条件变量或任务队列

### 5.2 Inference lifecycle

```text
workflow::infer::Run
 |- LoadConfig
 |- Device::Open
 |- loadNetwork / network.view
 |- initSession
 |- session.apply
 |- create sink
 |   |- PngFrameSink
 |   `- HdmiFrameSink
 |       `- RGB565HDMIDisplay
 |- for each SAR
 |   |- loadSarImageNorm
 |   |- SnakePatchSource
 |   `- for each patch
 |       |- build input tensor
 |       |- session.forward
 |       |- waitForReady
 |       |- host copy outputs
 |       `- device.reset(1)
 `- Device::Close
```

### 5.3 RD lifecycle

```text
workflow::rd::Run
 |- LoadConfig
 |- collectEchoBins
 `- for each echo
    |- validate shape and size
    |- create scratch_root
    |- run selected RD pipeline
    |- write SAR PNG
    `- cleanup scratch unless keep_scratch=true
```

---

## 6. Files That Matter Most In Review

- `main/src/main.cpp`
  - 是否只做菜单与调度，没有把业务重新塞回入口
- `main/src/rd_imaging_stream.cpp`
  - 是否保持 RD 行为、内存模式选择与 scratch 生命周期不变
- `main/src/infer_workflow.cpp`
  - 是否保持 patch 顺序、模型接口、输出语义与设备生命周期不变
- `main/src/config_utils.cpp`
  - 是否只承担轻量共享工具，没有引入过度公共化
- `main/CMakeLists.txt`
  - 是否仍然只构建单一主程序，且没有改变 Linux/ZG 约束

---

## 7. Known Hotspots

### RD side

- `runRangeCompression*`
- `runAzimuthFft`
- `runFusedRcmcAzimuthCompressionAndMagnitude`
- `runMemoryFloat32Pipeline`

### Inference side

- `SnakePatchSource::next`
- `PatchTensorBuilder::build`
- `PatchInferenceRunner::forward`
- `logitsToMaskBgr`
- `composeSideBySide`
- `HdmiFrameSink::write`

---

## 8. Known Risks

- `main/src/infer_workflow.cpp` 仍然较大，当前是“单入口收口 + 模块命名空间化 + 配置/共享能力抽取”，不是彻底碎片化重写
- `PatchInferenceRunner` 的 `waitForReady()` 与 `device.reset(1)` 顺序具有隐式设备约束
- `pipeline.patch.patch_size` 虽来自 YAML，但当前实现仍只接受 `512`
- `ManualFlightPatchSource` 仍未接控制端，后续接线时需要重新校对文档
- 本机是 Windows，无法通过 `main/CMakeLists.txt` 做完整 configure/build 验证

---

## 9. Recent Change Log

### 2026-04-24

- changed:
  - 将 `main/` 从“双独立入口”整理为“单入口菜单 + 两个模块顶层 `Run(...)` 接口”
  - 新增 `workflow/shared`、`workflow/rd`、`workflow/infer` 头文件边界
  - 把配置解析与轻量公共工具从两个大 `.cpp` 中抽出
- reason:
  - 降低入口复杂度，保留资源隔离，同时把后续扩展点放到更清晰的模块边界上
- validation:
  - 新增/修改的轻量模块通过 `g++ -fsyntax-only`
  - `rd_imaging_stream.cpp` 通过 `g++ -fsyntax-only`
  - 完整 CMake 构建因 Windows 平台保护无法在当前机器执行
