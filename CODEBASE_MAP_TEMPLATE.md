# CODEBASE_MAP_TEMPLATE.md

> 这是当前仓库 `main/` 目录的代码地图。  
> 重点记录真实入口、主调用链、核心类型职责、数据流、线程/资源生命周期和高风险点。  
> 若历史信息与当前实现冲突，以当前代码为准。

---

## 1. 主入口

| 入口 | 文件 | 职责 |
|---|---|---|
| `main()` | `main/src/main.cpp` | 显示菜单并调度 `RD only / Inference only / Web Console / Exit` |
| `workflow::rd::Run(const std::filesystem::path&)` | `main/src/rd_imaging_stream.cpp` | 读取 `rd_imaging.yaml` 并执行 RD 成像 |
| `workflow::infer::Run(const std::filesystem::path&)` | `main/src/infer_workflow.cpp` | 读取 `infer_workflow.yaml` 并执行推理 |
| `workflow::web::Run(const std::filesystem::path&)` | `main/src/web_console.cpp` | 启动板端 Web Console 模式 |

菜单定义在 `main/src/main.cpp`：

```text
1. RD only
2. Inference only
3. Web Console
0. Exit
```

---

## 2. 主调用链

### 2.1 菜单分发

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
 -> patch source
    -> auto_snake: SnakePatchSource
    -> manual_flight: ManualFlightRuntime
 -> loop patch
    -> PatchTensorBuilder::build
    -> PatchInferenceRunner::forward
    -> restoreToGrayU8
    -> logitsToMaskBgr
    -> buildMiniMapContext / applyManualTelemetry
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

## 3. 核心类型与职责

| 类型 | 文件 | 职责 |
|---|---|---|
| `workflow::shared::WorkflowRunControl` | `main/include/workflow/shared/run_control.hpp` | 协作式 `pause/resume/stop/reset` 和快照发布 |
| `workflow::shared::WorkflowRuntimeSnapshot` | `main/include/workflow/shared/run_control.hpp` | Web 状态栏与事件流统一快照 |
| `workflow::shared::WorkflowSelection` | `main/include/workflow/shared/run_control.hpp` | 当前 workflow / patch_mode / output_mode / source 选择 |
| `workflow::rd::AppConfig` | `main/include/workflow/rd/rd_config.hpp` | RD 配置快照，可读写 YAML |
| `workflow::infer::AppConfig` | `main/include/workflow/infer/infer_config.hpp` | Infer 配置快照，可读写 YAML |
| `workflow::web::WebConsoleConfig` | `main/include/workflow/web/web_console_config.hpp` | Web 配置快照，含 `bind_address`、`board_ip`、`flight_settings` |
| `workflow::web::FlightSettings` | `main/include/workflow/web/web_console_protocol.hpp` | Web 侧 manual_flight 参数集合 |
| `workflow::web::ManualFlightTelemetry` | `main/include/workflow/web/web_console_protocol.hpp` | Web 状态响应里的 manual 遥测 |
| `workflow::web::WebConsoleController` | `main/src/web_console_controller.cpp` | 选择项、设置项、worker 生命周期、manual key 转发、状态机 |
| `workflow::web::WebConsoleServer` | `main/src/web_console_server.cpp` | 唯一网络 I/O 承载点；HTTP、SSE、静态资源、preview、shutdown |
| `workflow::infer::SnakePatchSource` | `main/src/infer_workflow.cpp` | `auto_snake` 模式下的蛇形 patch 生成器 |
| `workflow::infer::ManualFlightSettings` | `main/include/workflow/infer/infer_workflow.hpp` | infer 侧 manual 参数快照 |
| `workflow::infer::ManualFlightTelemetry` | `main/include/workflow/infer/infer_workflow.hpp` | infer 侧 manual 遥测 |
| `workflow::infer::ManualFlightRuntime` | `main/src/infer_workflow.cpp` | manual_flight 运行时；左上角起点、方向切换、按 patch 完成推进、edge hold、latest-wins 方向更新 |
| `workflow::infer::PatchTensorBuilder` | `main/src/infer_workflow.cpp` | 构建 `FP32 [1,512,512,1]` 输入 tensor |
| `workflow::infer::PatchInferenceRunner` | `main/src/infer_workflow.cpp` | 封装 `forward -> waitForReady -> host copy -> device.reset(1)` |
| `workflow::infer::MiniMapContext` | `main/src/infer_workflow.cpp` | 小地图上下文，含 path overlay |
| `workflow::infer::UiRenderContext` | `main/src/infer_workflow.cpp` | 工业 UI 合成上下文 |
| `workflow::infer::LatestSnapshotMailbox` | `main/src/infer_workflow.cpp` | HDMI 路径下的最新帧邮箱 |
| `workflow::infer::HdmiRenderWorker` | `main/src/infer_workflow.cpp` | HDMI 渲染线程 worker |
| `workflow::infer::IFrameSink` | `main/src/infer_workflow.cpp` | 输出抽象 |
| `workflow::infer::PngFrameSink` | `main/src/infer_workflow.cpp` | PNG 落盘 |
| `workflow::infer::HdmiFrameSink` | `main/src/infer_workflow.cpp` | HDMI 输出 |

---

## 4. 数据流路径

### 4.1 端到端物理数据流

```text
echo.bin
 -> RD 成像
 -> SAR 灰度 PNG
 -> 归一化 CV_32FC1
 -> 512x512 patch
 -> FP32 NHWC tensor
 -> session.forward
 -> 灰度恢复输出 + 6 类 logits
 -> 灰度恢复图 + 彩色 mask + UI 元数据
 -> 最终合成 frame
 -> HDMI 或 PNG
```

### 4.2 Web 控制流

```text
browser
 -> WebConsoleServer
 -> WebConsoleController
 -> WorkflowRunControl
 -> infer / rd worker thread safe point
```

### 4.3 Web 状态流

```text
workflow thread / controller state change
 -> WorkflowRunControl::publish(...)
 -> WebConsoleController::onWorkflowSnapshot
 -> WebConsoleServer SSE queue
 -> /events
 -> browser
```

### 4.4 Manual flight 输入流

```text
browser keydown/keyup or on-screen buttons
 -> POST /api/manual/key
 -> WebConsoleServer::handleClient
 -> WebConsoleController::commandManualKey
 -> workflow::infer::SubmitManualFlightKey
 -> ManualFlightRuntime shared state
 -> latest-wins patch request
```

### 4.5 数据格式图

| 阶段 | 类型 | 形状 / 规则 |
|---|---|---|
| echo payload | `float32 interleaved complex` | `rows x cols x 2` |
| RD matrix | `CV_64FC2` 或 `CV_32FC2` | complex matrix |
| SAR file | grayscale PNG | `H x W` |
| loaded SAR | `CV_32FC1` | 归一化到 `0~1` |
| patch | `CV_32FC1` | 固定 `512 x 512` |
| model input | `FP32` | `1 x 512 x 512 x 1` |
| restore output | `FP32` | `1 x 512 x 512 x 1` |
| seg logits | `FP32` | `1 x 512 x 512 x 6` |
| final UI frame | `CV_8UC3` | HDMI / PNG 共用 |
| HDMI buffer | RGB565 bytes | `width * height * 2` |

---

## 5. 线程 / 资源生命周期

### 5.1 线程

- RD only
  - 单工作线程
- Inference only
  - 推理主线程
  - `output.mode=hdmi` 时额外有 HDMI render thread
  - `patch.mode=manual_flight` 时不再额外起 simulation thread；manual 推进由推理主循环完成
- Web Console
  - main thread
  - dedicated web_thread
  - background workflow worker thread
  - HDMI 模式下仍可能再有 render thread
  - manual_flight 下也不再额外起 simulation thread

### 5.2 Inference 生命周期

```text
LoadConfig
 -> Device::Open
 -> loadNetwork / initSession / session.apply
 -> create sink
 -> for each SAR
    -> loadSarImageNorm
    -> create patch source
    -> loop patch
       -> build input tensor
       -> forward / waitForReady
       -> copy outputs
       -> composeIndustrialUiFrame
       -> sink.write
 -> Device::Close
```

### 5.3 RD 生命周期

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

### 5.4 Web 生命周期

```text
LoadConfig(web_console.yaml)
 -> build controller
 -> apply flight settings
 -> build server
 -> run web thread
 -> browser interactions
 -> stop / shutdown_web / Ctrl+C
 -> stop server
 -> stop worker
 -> join threads
 -> persist infer / rd / web configs
```

---

## 6. 需要优先阅读的文件

- `main/src/main.cpp`
  - 是否仍只负责菜单调度
- `main/src/rd_imaging_stream.cpp`
  - RD 行为、安全点、scratch 生命周期是否被破坏
- `main/src/infer_workflow.cpp`
  - patch 顺序、设备生命周期、manual runtime、HDMI 路径是否一致
- `main/src/web_console_controller.cpp`
  - worker 生命周期、状态机、manual key、stop/reset/shutdown 语义
- `main/src/web_console_server.cpp`
  - HTTP/SSE 路由、socket 生命周期、`shutdown_web`
- `main/src/web_console_assets.cpp`
  - 前端状态管理、manual keys、本地轮询与后端语义是否一致
- `main/src/web_console_config.cpp`
  - `board_ip`、`flight.*`、配置读写是否闭环

---

## 7. 高风险热点

### RD 侧

- `processOneEcho`
- `runMemoryFloat32Pipeline`
- `runRangeCompression*`
- `runAzimuthFft`
- `runFusedRcmcAzimuthCompressionAndMagnitude`

### Infer 侧

- `SnakePatchSource::next`
- `ManualFlightRuntime`
- `PatchTensorBuilder::build`
- `PatchInferenceRunner::forward`
- `restoreToGrayU8`
- `logitsToMaskBgr`
- `applyManualTelemetry`
- `composeIndustrialUiFrame`
- `HdmiFrameSink::write`

### Web 侧

- `WebConsoleController::applySelection`
- `WebConsoleController::commandStart`
- `WebConsoleController::commandStop`
- `WebConsoleController::commandReset`
- `WebConsoleController::commandManualKey`
- `WebConsoleController::commandShutdownWeb`
- `WebConsoleServer::handleClient`
- `WebConsoleServer::handleSseClient`
- `WebConsoleServer::Stop`

---

## 8. 当前已知风险

- `main/src/infer_workflow.cpp` 仍然很大，manual runtime、推理主链和 UI 合成都在一个翻译单元中
- `patch_size` 虽来自 YAML，但当前实现仍只接受 `512`
- `config_utils.cpp` 仍是简化 YAML 读写器，不保留原始注释和排版
- Web Server 仍是单线程同步请求处理，慢请求会影响其他请求与 SSE 心跳
- `manual_flight` 已实现，但它依赖 latest-wins 和协作式停止，不能按“强实时飞控”理解
- `stop` 在推理中仍依赖安全点，不等于立即抢占中断

---

## 9. 当前文件布局

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

## 10. 代码阅读结论

阅读 `main/` 目录后的当前结论：

- YAML 写回已经真实存在，不再是只读配置
- Web Console 已有 `board_ip` 概念，终端启动时会打印实际访问地址
- Web UI 默认隐藏设置面板，并带右上角黄色圆形 shutdown 按钮
- `manual_flight` 已不再是预留壳，已经接入 infer 主链、Web 控制链和状态链
- manual telemetry 当前通过 `SSE + 低频 /api/state` 混合更新
- `stop` 与 `shutdown_web` 是两个不同语义

---

## 11. Phase 4 Manual Flight 地图

### 新控制链

```text
browser keydown(W/A/S/D)
 -> POST /api/manual/key
 -> WebConsoleServer::handleClient
 -> WebConsoleController::commandManualKey
 -> workflow::infer::SubmitManualFlightKey
 -> ManualFlightRuntime pending_direction
```

### 新 infer 链

```text
workflow::infer::Run(..., manual_flight)
 -> loadSarImageNorm
 -> ManualFlightRuntime
    -> initializeSharedState() 将起点放在左上角第一个合法 patch 中心，默认方向 right
 -> waitNextPatch(packet)
 -> PatchTensorBuilder::build
 -> PatchInferenceRunner::forward
 -> markInferenceCommitted(packet)
    -> 更新 last_inferred_center / patch_count
    -> 读取 pending_direction
    -> 计算下一合法中心；若撞边则 edge_blocked=true，等待新方向
    -> request_sequence / consumed_sequence 只对“最新方向”生效，不做历史 FIFO
 -> applyManualTelemetry
 -> composeIndustrialUiFrame
 -> HDMI / PNG output
```

### 新核心对象

- `workflow::infer::ManualFlightSettings`
- `workflow::infer::ManualFlightTelemetry`
- `workflow::infer::ManualFlightRuntime`

### Web 可见状态增量

- `manual.configured`
- `manual.active`
- `manual.paused`
- `manual.edge_blocked`
- `manual.position_x / manual.position_y`
- `manual.last_inferred_center_x / manual.last_inferred_center_y`
- `manual.path_points`
- `manual.patch_count`
- `manual.current_direction`
- `manual.pending_direction`

### Phase 4 Fix2 / Reset 语义补充

- manual 已从旧的“自由飞行 + 惯性 + trigger_distance”语义收敛为扫描游标模式
- `W/A/S/D` 只切换方向，不再依赖 keyup 释放
- `ResetManualFlight()` 现在会把 `current_center / requested_center / last_inferred_center` 直接回到左上角第一个合法 patch 中心
- `commandReset()` 在同一次状态发布里就会把新的 manual telemetry 暴露给 `SSE / /api/state`
