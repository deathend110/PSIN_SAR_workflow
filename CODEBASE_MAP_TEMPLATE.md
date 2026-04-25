# CODEBASE_MAP_TEMPLATE.md

> 这是当前仓库的代码地图。
> 重点记录真实入口、主调用链、核心类型职责、数据流路径、线程/资源生命周期和高风险点。
> 若历史信息与当前实现冲突，以当前代码为准。

---

## 1. Main Entrypoints

| Entrypoint | File | Responsibility |
|---|---|---|
| `main()` | `main/src/main.cpp` | 显示菜单并调度 `RD only / Inference only / Web Console / Exit` |
| `workflow::rd::Run(const std::filesystem::path&)` | `main/src/rd_imaging_stream.cpp` | 读取 `rd_imaging.yaml` 并执行 RD 成像 |
| `workflow::infer::Run(const std::filesystem::path&)` | `main/src/infer_workflow.cpp` | 读取 `infer_workflow.yaml` 并执行推理输出 |
| `workflow::web::Run(const std::filesystem::path&)` | `main/src/web_console.cpp` | 启动板端 Web Console 模式 |

当前主程序菜单在 [main.cpp](g:/Docker_windows_disk/PSIN_SAR_workflow/main/src/main.cpp)：

```text
1. RD only
2. Inference only
3. Web Console
0. Exit
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
workflow::rd::Run(config_path)
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
workflow::infer::Run(config_path)
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
    -> SnakePatchSource(cfg.patch_size, cfg.stride)
    -> while next(packet)
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

```text
workflow::web::Run(config_path)
 -> LoadConfig(web_console.yaml)
 -> infer::LoadConfig + rd::LoadConfig
 -> WebConsoleController
 -> apply persisted flight settings into controller
 -> WebConsoleServer
 -> spawn dedicated web_thread
    -> WebConsoleServer::Run()
 -> browser HTTP / SSE
 -> controller state machine
 -> background workflow worker thread
    -> workflow::rd::Run(AppConfig, WorkflowRunControl*)
    -> workflow::infer::Run(AppConfig, WorkflowRunControl*)
 -> shutdown path
    -> server->Stop()
    -> controller->RequestWorkerStop()
    -> join web_thread
    -> controller->JoinWorker()
    -> PersistControllerConfigs(...)
```

---

## 3. Core Types And Responsibilities

| Type | File | Responsibility |
|---|---|---|
| `workflow::AppMode` | `main/include/workflow/shared/app_mode.hpp` | 菜单模式枚举 |
| `workflow::shared::WorkflowRunControl` | `main/include/workflow/shared/run_control.hpp` | 协作式 `pause/resume/stop/reset` 与运行快照发布 |
| `workflow::shared::WorkflowRuntimeSnapshot` | `main/include/workflow/shared/run_control.hpp` | Web 状态显示的统一快照对象 |
| `workflow::rd::AppConfig` | `main/include/workflow/rd/rd_config.hpp` | RD 配置快照，支持读写 YAML |
| `workflow::infer::AppConfig` | `main/include/workflow/infer/infer_config.hpp` | Infer 配置快照，支持读写 YAML |
| `workflow::web::WebConsoleConfig` | `main/include/workflow/web/web_console_config.hpp` | Web 配置快照，包含 `bind_address`、`board_ip`、`flight_settings` |
| `workflow::web::FlightSettings` | `main/include/workflow/web/web_console_protocol.hpp` | Web 侧预留飞行参数 |
| `workflow::web::WebConsoleController` | `main/src/web_console_controller.cpp` | 选择状态、内存配置、后台线程、状态机、stop/reset/shutdown 语义 |
| `workflow::web::WebConsoleServer` | `main/src/web_console_server.cpp` | 唯一网络 I/O 承载点；HTTP、SSE、静态资源、preview 路由 |
| `workflow::web::WebConsoleProtocol` | `main/src/web_console_protocol.cpp` | JSON/Query 解析与序列化 |
| `workflow::infer::SnakePatchSource` | `main/src/infer_workflow.cpp` | 蛇形 patch 生成器，按配置 stride 前进 |
| `workflow::infer::ManualFlightPatchSource` | `main/src/infer_workflow.cpp` | 预留但未接主流程的手动 patch 源 |
| `workflow::infer::PatchTensorBuilder` | `main/src/infer_workflow.cpp` | 构建 `FP32 [1,512,512,1]` 输入 tensor |
| `workflow::infer::PatchInferenceRunner` | `main/src/infer_workflow.cpp` | 封装 `forward -> waitForReady -> host copy -> device.reset(1)` |
| `workflow::infer::MiniMapContext` | `main/src/infer_workflow.cpp` | 小地图上下文 |
| `workflow::infer::UiRenderContext` | `main/src/infer_workflow.cpp` | 最终工业风 UI 合成上下文 |
| `workflow::infer::IFrameSink` | `main/src/infer_workflow.cpp` | 输出抽象接口 |
| `workflow::infer::PngFrameSink` | `main/src/infer_workflow.cpp` | PNG 落盘 |
| `workflow::infer::HdmiFrameSink` | `main/src/infer_workflow.cpp` | HDMI 输出 |
| `RGB565HDMIDisplay` | `main/include/infer_workflow_hdmi_display.hpp` | 板端 RGB565 HDMI 显示适配 |

---

## 4. Data Flow Path

### 4.1 End-to-end physical data path

```text
echo.bin
 -> RD 成像
 -> SAR 灰度 PNG
 -> 归一化为 CV_32FC1
 -> 512x512 patch
 -> FP32 NHWC tensor
 -> session.forward
 -> 灰度恢复输出 + 6 类 logits
 -> 灰度恢复图 + 彩色 mask + UI 元数据
 -> 最终合成 frame
 -> HDMI 或 PNG
```

### 4.2 Web control path

```text
browser
 -> WebConsoleServer
 -> WebConsoleController
 -> WorkflowRunControl
 -> infer/rd worker thread safe point
```

### 4.3 Web state path

```text
workflow thread / controller state change
 -> WorkflowRunControl::publish(...)
 -> WebConsoleController::onWorkflowSnapshot
 -> WebConsoleServer SSE queue
 -> /events
 -> browser
```

### 4.4 Data format map

| Stage | Type | Shape / Rule |
|---|---|---|
| echo payload | `float32 interleaved complex` | `rows x cols x 2` |
| RD matrix | `CV_64FC2` or `CV_32FC2` | complex matrix |
| SAR file | grayscale PNG | `H x W` |
| loaded SAR | `CV_32FC1` | normalized to `0~1` |
| patch | `CV_32FC1` | fixed `512 x 512` |
| model input | `FP32` | `1 x 512 x 512 x 1` |
| restore output | `FP32` | `1 x 512 x 512 x 1` |
| seg logits | `FP32` | `1 x 512 x 512 x 6` |
| final UI frame | `CV_8UC3` | HDMI/PNG 共用 |
| HDMI buffer | RGB565 bytes | `width * height * 2` |

---

## 5. Thread / Resource Lifecycle

### 5.1 Threads

- RD only
  - 单工作线程
- Inference only
  - 推理线程
  - HDMI 模式下还有一个渲染线程
- Web Console
  - main thread
  - dedicated web_thread
  - background workflow worker thread
  - HDMI 模式下额外 render thread

### 5.2 Inference lifecycle

```text
LoadConfig
 -> Device::Open
 -> loadNetwork / initSession / session.apply
 -> create sink
 -> for each SAR
    -> loadSarImageNorm
    -> create SnakePatchSource
    -> for each patch
       -> build input tensor
       -> forward / waitForReady
       -> copy outputs
       -> composeIndustrialUiFrame
       -> sink.write
 -> Device::Close
```

### 5.3 RD lifecycle

```text
LoadConfig
 -> collectEchoBins
 -> for each echo
    -> validate
    -> create scratch_root
    -> run selected pipeline
    -> write SAR PNG
    -> cleanup scratch unless keep_scratch=true
```

### 5.4 Web lifecycle

```text
LoadConfig(web_console.yaml)
 -> build controller
 -> apply flight settings into controller
 -> build server
 -> run web thread
 -> browser interactions
 -> stop/shutdown_web/Ctrl+C
 -> stop server
 -> stop worker
 -> join threads
 -> persist infer/rd/web configs
```

---

## 6. Files That Matter Most In Review

- [main.cpp](g:/Docker_windows_disk/PSIN_SAR_workflow/main/src/main.cpp)
  - 是否仍只负责菜单调度
- [rd_imaging_stream.cpp](g:/Docker_windows_disk/PSIN_SAR_workflow/main/src/rd_imaging_stream.cpp)
  - RD 行为、安全点、scratch 生命周期是否被破坏
- [infer_workflow.cpp](g:/Docker_windows_disk/PSIN_SAR_workflow/main/src/infer_workflow.cpp)
  - patch 顺序、设备生命周期、HDMI 路径是否被破坏
- [web_console_controller.cpp](g:/Docker_windows_disk/PSIN_SAR_workflow/main/src/web_console_controller.cpp)
  - worker 生命周期、状态机、stop/reset/shutdown 是否清晰
- [web_console_server.cpp](g:/Docker_windows_disk/PSIN_SAR_workflow/main/src/web_console_server.cpp)
  - HTTP/SSE 路由、socket 生命周期、`shutdown_web` 是否安全
- [web_console_assets.cpp](g:/Docker_windows_disk/PSIN_SAR_workflow/main/src/web_console_assets.cpp)
  - 前端按钮、设置面板、断连提示是否与后端语义一致
- [web_console_config.cpp](g:/Docker_windows_disk/PSIN_SAR_workflow/main/src/web_console_config.cpp)
  - `board_ip`、`flight.*`、配置读写闭环是否一致

---

## 7. Known Hotspots

### RD side

- `processOneEcho`
- `runMemoryFloat32Pipeline`
- `runRangeCompression*`
- `runAzimuthFft`
- `runFusedRcmcAzimuthCompressionAndMagnitude`

### Inference side

- `SnakePatchSource::next`
- `PatchTensorBuilder::build`
- `PatchInferenceRunner::forward`
- `restoreToGrayU8`
- `logitsToMaskBgr`
- `buildMiniMapContext`
- `composeIndustrialUiFrame`
- `HdmiFrameSink::write`

### Web side

- `WebConsoleController::applySelection`
- `WebConsoleController::commandStop`
- `WebConsoleController::commandReset`
- `WebConsoleController::commandShutdownWeb`
- `WebConsoleServer::handleClient`
- `WebConsoleServer::handleSseClient`
- `WebConsoleServer::Stop`

---

## 8. Known Risks

- [infer_workflow.cpp](g:/Docker_windows_disk/PSIN_SAR_workflow/main/src/infer_workflow.cpp) 仍然很大，当前是“命名空间内收口”，不是彻底模块化拆分
- `patch_size` 虽来自 YAML，但当前实现仍只接受 `512`
- `stride` 已可配置，但很多旧描述容易把它写成固定 `256`
- `manual_flight` 在 UI、配置和协议里都保留了占位，但主流程仍未实现
- `config_utils.cpp` 仍然是简化 YAML 读写，不会保留原始注释/排版
- `shutdown_web` 会关闭 Web Console 本身，前端断连是预期行为

---

## 9. Current File Layout

```text
main/include/workflow/shared/
  app_mode.hpp
  config_utils.hpp
  run_control.hpp

main/include/workflow/rd/
  rd_config.hpp
  rd_workflow.hpp

main/include/workflow/infer/
  infer_config.hpp
  infer_workflow.hpp

main/include/workflow/web/
  web_console.hpp
  web_console_config.hpp
  web_console_controller.hpp
  web_console_protocol.hpp
  web_console_server.hpp

main/src/
  main.cpp
  config_utils.cpp
  rd_config.cpp
  rd_imaging_stream.cpp
  infer_config.cpp
  infer_workflow.cpp
  web_console.cpp
  web_console_config.cpp
  web_console_controller.cpp
  web_console_protocol.cpp
  web_console_server.cpp
  web_console_assets.cpp

main/configs/
  rd_imaging.yaml
  infer_workflow.yaml
  web_console.yaml
```

---

## 10. Review Notes

当前仓库文档化时需要特别注意这些事实：

- YAML 写回已经实现，不再是“只读配置”
- Web Console 现在有 `board_ip` 概念
- Web UI 默认隐藏设置面板
- Web UI 右上角已经有黄色圆形 shutdown 按钮
- 黄色 manual_flight 常驻提示条已经删除
- `stop` 与 `shutdown_web` 是两个不同语义

---

## 11. Phase 4 Manual Flight Map

### New control chain

```text
browser keydown/keyup
 -> POST /api/manual/key
 -> WebConsoleServer::handleClient
 -> WebConsoleController::commandManualKey
 -> workflow::infer::SubmitManualFlightKey
 -> ManualFlightRuntime shared state
```

### New infer chain

```text
workflow::infer::Run(..., manual_flight)
 -> loadSarImageNorm
 -> ManualFlightRuntime
    -> simulation thread updates position / velocity / requested_center
    -> request_sequence / consumed_sequence implements latest-wins
 -> waitNextPatch(packet)
 -> PatchTensorBuilder::build
 -> PatchInferenceRunner::forward
 -> applyManualTelemetry
 -> composeIndustrialUiFrame
 -> HDMI / PNG output
```

### New core types

- `workflow::infer::ManualFlightSettings`: infer 侧 manual 参数快照。
- `workflow::infer::ManualFlightTelemetry`: infer 向 Web / UI 暴露的位置、速度、请求中心点与按键状态。
- `workflow::infer::ManualFlightRuntime`: infer 内部 manual 运行态，负责位置推进、轨迹、latest-wins patch 请求。

### Web-visible state additions

- `manual.active`
- `manual.paused`
- `manual.position_x / manual.position_y`
- `manual.velocity_x / manual.velocity_y`
- `manual.requested_center_x / manual.requested_center_y`
- `manual.last_inferred_center_x / manual.last_inferred_center_y`
- `manual.path_points`
- `manual.active_keys`

### Phase 4 Fix1 adjustments

- 前端 `sendManualKey(...)` 现在采用“请求成功后再提交本地按键集合”的顺序。
- 前端在 manual 活跃时会对现有 `/api/state` 做低频轮询，以补足 SSE 只在命令/patch 事件时更新的空窗。
- manual 模式下 `patch_count` 会在进入 `processPatchToPng / processPatchToHdmi` 前先写成当前已处理计数，保证 UI、日志和 snapshot 语义一致。
