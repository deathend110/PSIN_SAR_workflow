# CODEBASE_MAP_TEMPLATE.md

> 这是当前仓库的“代码地图”。
> 重点记录真实入口、主调用链、核心类职责、数据流路径、资源生命周期和高风险点。

---

## 1. Main Entrypoint

| Entrypoint | File | Responsibility |
|---|---|---|
| `main()` | `main/src/main.cpp` | 显示启动菜单并调度 `RD only / Inference only / Web Console / Exit` |
| `workflow::rd::Run(...)` | `main/src/rd_imaging_stream.cpp` | 完成 RD 成像流程 |
| `workflow::infer::Run(...)` | `main/src/infer_workflow.cpp` | 完成推理与显示/落盘流程 |
| `workflow::web::Run(...)` | `main/src/web_console.cpp` | 启动板端内嵌 Web 控制台 |

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
 -> workflow::web::Run("configs/web_console.yaml")
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
          -> buildMiniMapContext
          -> composeIndustrialUiFrame
          -> sink.write
 -> Device::Close
```

### 2.4 Web Console

Fix1 tightened this call chain:

- `workflow::web::Run(...)` owns controller and server on the main thread
- `WebConsoleServer::Run()` is carried by a dedicated Web thread
- ordinary HTTP requests are handled synchronously inside that server thread
- SSE is the primary real-time state path; `/api/state` is bootstrap / reconnect fallback
- the background workflow thread is still the only place that runs `workflow::rd::Run(...)` or `workflow::infer::Run(...)`

```text
workflow::web::Run
 -> LoadConfig(web_console.yaml)
 -> infer::LoadConfig + rd::LoadConfig
 -> WebConsoleController
 -> WebConsoleServer
 -> HTTP GET / POST / SSE
 -> controller state machine
 -> background worker thread
    -> workflow::rd::Run(AppConfig, WorkflowRunControl*)
    -> workflow::infer::Run(AppConfig, WorkflowRunControl*)
```

---

## 3. Core Types And Responsibilities

| Type | File | Responsibility |
|---|---|---|
| `workflow::AppMode` | `main/include/workflow/shared/app_mode.hpp` | 定义菜单模式枚举 |
| `workflow::shared::*` | `main/include/workflow/shared/config_utils.hpp`, `main/src/config_utils.cpp` | 简单 YAML 读取、字符串处理、默认值解析 |
| `WorkflowRunControl` | `main/include/workflow/shared/run_control.hpp` | 协作式控制请求与运行时快照发布 |
| `workflow::rd::AppConfig` | `main/include/workflow/rd/rd_config.hpp` | RD 阶段配置快照 |
| `workflow::infer::AppConfig` | `main/include/workflow/infer/infer_config.hpp` | 推理阶段配置快照 |
| `WebConsoleConfig` | `main/include/workflow/web/web_console_config.hpp` | Web 服务配置快照 |
| `WebConsoleController` | `main/src/web_console_controller.cpp` | 管理 UI 选择、内存配置、后台线程与状态机；锁内只改状态，锁外才分发事件 |
| `WebConsoleServer` | `main/src/web_console_server.cpp` | 唯一网络 I/O 承载点；在独立 Web 线程内处理 HTTP、SSE 和静态资源 |
| `WebConsoleProtocol` | `main/src/web_console_protocol.cpp` | DTO、序列化与简易 JSON/Query 解析 |
| `SnakePatchSource` | `main/src/infer_workflow.cpp` | 生成 `512x512`、`stride=256` 的蛇形 patch |
| `ManualFlightPatchSource` | `main/src/infer_workflow.cpp` | 预留的手动巡航 patch 接口，当前未接主流程 |
| `PatchTensorBuilder` | `main/src/infer_workflow.cpp` | 校验输入 shape/dtype，并把 patch 构造成 Host FP32 tensor |
| `PatchInferenceRunner` | `main/src/infer_workflow.cpp` | 封装 `session.forward -> waitForReady -> host copy -> device.reset(1)` |
| `MiniMapContext` | `main/src/infer_workflow.cpp` | 承载整张 SAR 缩略图和小地图红框映射 |
| `UiRenderContext` | `main/src/infer_workflow.cpp` | 承载模式、输出信息和最终 UI 合成需要的上下文 |
| `IFrameSink` | `main/src/infer_workflow.cpp` | 最终输出抽象接口 |
| `PngFrameSink` | `main/src/infer_workflow.cpp` | 写 `io/output/<stem>/patch_*.png` |
| `HdmiFrameSink` | `main/src/infer_workflow.cpp` | 把最终帧送到 HDMI |
| `RGB565HDMIDisplay` | `main/include/infer_workflow_hdmi_display.hpp` | 分配 UDMA buffer 并驱动板端显示 |
| `RadarConfig` | `main/src/rd_imaging_stream.cpp`, `main/src/infer_workflow.cpp` | RD 参数常量集合 |
| `PatchInfo` / `PatchPacket` | `main/src/infer_workflow.cpp` | patch 元数据与 patch 数据承载 |
| `RuntimeState` | `main/src/infer_workflow.cpp` | patch / frame 计数、FPS、infer/total ms 等推理阶段运行时上下文 |

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
 -> 灰度恢复图 + 彩色 mask + UI 元数据
 -> 完整 UI frame
 -> HDMI 或 PNG
 -> or 通过 Web Console 暴露运行状态 / 控制接口
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
| final frame | `CV_8UC3` | industrial UI frame shared by HDMI and PNG |
| HDMI buffer | RGB565 bytes | `width * height * 2` |

---

## 5. Thread / Resource Lifecycle

Fix1 tightened the Web side into four explicit roles:

- main thread: owns startup / shutdown sequencing for controller and server
- Web thread: the only HTTP / SSE carrier
- workflow worker thread: the only background `RD/Infer` executor
- HDMI thread: unchanged Phase 2 render path, still isolated from Web control

### 5.1 Thread model

当前线程模型分为：

- `workflow::rd::Run(...)`
  - 单工作线程
  - 通过 `WorkflowRunControl` 在文件 / tile 安全点协作式停顿
- `workflow::infer::Run(...)`
  - 推理线程 + HDMI/UI 线程
  - Web 控制只在 patch 边界注入 pause / stop
- `workflow::web::Run(...)`
  - HTTP 服务主线程
  - controller 持有一个后台工作线程来运行阻塞式 RD/Infer

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
|       |- build mini-map / telemetry context
|       |- compose industrial UI frame
|       `- device.reset(1)
`- Device::Close
```

### 5.4 Web lifecycle

```text
workflow::web::Run
 |- LoadConfig(web_console.yaml)
 |- WebConsoleController
 |   |- in-memory infer config
 |   |- in-memory rd config
 |   |- flight placeholder config
 |   |- WorkflowRunControl
 |   `- worker thread
 `- WebConsoleServer
     |- static asset routes
     |- API routes
     `- SSE clients
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
  - 是否只做菜单与调度，没有把控制层或 HTTP 细节塞回入口
- `main/src/rd_imaging_stream.cpp`
  - 是否只增加协作式安全点和状态发布，没有破坏 RD 行为与 scratch 生命周期
- `main/src/infer_workflow.cpp`
  - 是否只增加控制钩子和状态发布，没有破坏 patch 顺序、模型接口与设备生命周期
- `main/src/web_console_controller.cpp`
  - 状态机、后台线程和 reset/stop/join 顺序是否清晰
- `main/src/web_console_server.cpp`
  - HTTP 路由、SSE 客户端管理和单端口实现是否足够简单可 review
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
- `buildMiniMapContext`
- `composeIndustrialUiFrame`
- `HdmiFrameSink::write`

---

## 8. Known Risks

- `main/src/infer_workflow.cpp` 仍然较大，当前是“单入口收口 + 模块命名空间化 + 配置/共享能力抽取”，不是彻底碎片化重写
- `PatchInferenceRunner` 的 `waitForReady()` 与 `device.reset(1)` 顺序具有隐式设备约束
- 第一阶段 HDMI UI 已确定不采用 UI/推理双线程，后续若改变该决策必须重新审查生命周期
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
