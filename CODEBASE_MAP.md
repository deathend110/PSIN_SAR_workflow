# CODEBASE_MAP.md

> 这是当前仓库 `main/` 目录的代码地图。
> 重点记录真实入口、主调用链、核心类型职责、数据流、线程/资源生命周期和高风险点。
> 若旧文档与当前实现冲突，以当前代码为准。

---

## 1. 主入口

| 入口 | 文件 | 职责 |
|---|---|---|
| `main()` | `main/src/main.cpp` | 显示菜单并调度 `RD only / Inference only / Web Console / Exit` |
| `workflow::rd::Run(const std::filesystem::path&)` | `main/src/rd_imaging_stream.cpp` | 读取 `rd_imaging.yaml` 并执行 RD 成像 |
| `workflow::infer::Run(const std::filesystem::path&)` | `main/src/infer_workflow.cpp` | 读取 `infer_workflow.yaml` 并执行 patch 推理 |
| `workflow::web::Run(const std::filesystem::path&)` | `main/src/web_console.cpp` | 启动嵌入式 Web Console |

顶层菜单定义在 `main/src/main.cpp`：

```text
1. RD only
2. Inference only
3. Web Console
0. Exit
```

当前实现补充：

- `main()` 是外层菜单循环，不是一次性 `switch -> return`。
- `RD only` 与 `Inference only` 仍然是单次执行后直接返回退出码。
- `Web Console` 成功返回后会回到菜单循环。

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
       -> write SAR PNG
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
    -> PngFrameSink
    -> or HdmiFrameSink + LatestSnapshotMailbox + HdmiRenderWorker
 -> choose patch source per SAR
    -> auto_snake: SnakePatchSource
    -> manual_flight: ManualFlightRuntime
    -> debug_raster: DebugRasterPatchSource
 -> loop patch
    -> PatchTensorBuilder::build
    -> PatchInferenceRunner::forward
    -> restoreToGrayU8
    -> logitsToMaskBgr
    -> applyManualTelemetry if needed
    -> composeIndustrialUiFrame
    -> sink.write or mailbox.publish
 -> Device::Close
```

### 2.4 Web Console

```text
workflow::web::Run(config_path)
 -> LoadConfig(web_console.yaml)
 -> infer::LoadConfig + rd::LoadConfig
 -> WebConsoleController
 -> ApplyInitialFlightSettings
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
 -> workflow::web::Run(...) returns
 -> main() menu loop resumes
```

---

## 3. 核心类型与职责

| 类型 | 文件 | 职责 |
|---|---|---|
| `workflow::shared::WorkflowRunControl` | `main/include/workflow/shared/run_control.hpp` | 协作式 `pause / resume / stop / reset` 与快照发布 |
| `workflow::shared::WorkflowRuntimeSnapshot` | `main/include/workflow/shared/run_control.hpp` | RD / Infer / Web 共用的运行态快照 |
| `workflow::shared::WorkflowSelection` | `main/include/workflow/shared/run_control.hpp` | 当前 workflow / patch_mode / output_mode / source 选择 |
| `workflow::rd::AppConfig` | `main/include/workflow/rd/rd_config.hpp` | RD 配置快照，可读写 YAML |
| `workflow::infer::AppConfig` | `main/include/workflow/infer/infer_config.hpp` | Infer 配置快照，可读写 YAML |
| `workflow::web::WebConsoleConfig` | `main/include/workflow/web/web_console_config.hpp` | Web 配置快照，含 `bind_address`、`board_ip`、`flight_settings` |
| `workflow::web::FlightSettings` | `main/include/workflow/web/web_console_protocol.hpp` | Web 侧 `manual_flight` 参数集合 |
| `workflow::web::ManualFlightTelemetry` | `main/include/workflow/web/web_console_protocol.hpp` | Web API 暴露的 manual 遥测 |
| `workflow::web::WebConsoleController` | `main/src/web_console_controller.cpp` | 控制面状态机、worker 生命周期、selection/settings、manual key 转发 |
| `workflow::web::WebConsoleServer` | `main/src/web_console_server.cpp` | HTTP/SSE 服务、静态资源、preview、shutdown_web 路由 |
| `workflow::infer::ManualFlightRuntimeState` | `main/src/manual_flight_runtime.cpp` | `manual_flight` 游标状态机，含 pause / edge hold / latest-wins |
| `workflow::infer::SnakePatchSource` | `main/src/infer/patch_planner.cpp` | `auto_snake` 蛇形 patch 生成器 |
| `workflow::infer::DebugRasterPatchSource` | `main/src/infer/patch_planner.cpp` | `debug_raster` 栅格 patch 生成器 |
| `workflow::infer::PatchTensorBuilder` | `main/src/infer/output_sink.cpp` | 构建 `FP32 [1,512,512,1]` 输入 tensor |
| `workflow::infer::PatchInferenceRunner` | `main/src/infer/output_sink.cpp` | 封装 `forward -> waitForReady -> host copy -> device.reset(1)` |
| `workflow::infer::MiniMapContext` | `main/include/workflow/infer/infer_workflow_internal.hpp` | 小地图与路径叠加上下文 |
| `workflow::infer::UiRenderContext` | `main/include/workflow/infer/infer_workflow_internal.hpp` | 工业 UI 合成上下文 |
| `workflow::infer::InferenceSnapshot` | `main/include/workflow/infer/infer_workflow_internal.hpp` | HDMI 路径的最新推理快照 |
| `workflow::infer::IFrameSink` | `main/include/workflow/infer/infer_workflow_internal.hpp` | 输出抽象接口 |
| `workflow::infer::PngFrameSink` | `main/src/infer/output_sink.cpp` | PNG 输出与 debug patch 落盘 |
| `workflow::infer::HdmiFrameSink` | `main/src/infer/output_sink.cpp` | HDMI 输出适配 |
| `workflow::infer::LatestSnapshotMailbox` | `main/src/infer/output_sink.cpp` | HDMI 最新快照邮箱 |
| `workflow::infer::HdmiRenderWorker` | `main/src/infer/output_sink.cpp` | HDMI 渲染线程 worker |

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
 -> 恢复输出 + 6 类 logits
 -> 灰度恢复图 + 彩色 mask + UI 元数据
 -> 最终 frame
 -> HDMI 或 PNG
```

### 4.2 Web 控制流

```text
browser
 -> WebConsoleServer
 -> WebConsoleController
 -> WorkflowRunControl
 -> infer / rd worker safe point
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
browser keydown / keyup or on-screen button
 -> POST /api/manual/key
 -> WebConsoleController::commandManualKey
 -> workflow::infer::SubmitManualFlightKey
 -> ManualFlightRuntimeState
 -> latest-wins next patch request
```

### 4.5 数据格式图

| 阶段 | 类型 | 形状 / 规则 |
|---|---|---|
| echo payload | `float32 interleaved complex` | `rows x cols x 2` |
| RD matrix | `CV_64FC2` 或 `CV_32FC2` | complex matrix |
| SAR file | grayscale image | `H x W` |
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
  - `patch.mode=manual_flight` 时没有独立 simulation thread
- Web Console
  - main thread
  - dedicated web_thread
  - background workflow worker thread
  - HDMI 模式下可能再有 render thread

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
       -> composeIndustrialUiFrame or mailbox.publish
 -> join HDMI render worker if any
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
 -> browser interaction
 -> stop / shutdown_web / Ctrl+C
 -> stop server
 -> stop worker
 -> join threads
 -> persist infer / rd / web configs
 -> workflow::web::Run(...) returns
 -> main() menu loop resumes
```

---

## 6. 需要优先阅读的文件

- `main/src/main.cpp`
  - 确认当前菜单循环语义
- `main/src/rd_imaging_stream.cpp`
  - RD 行为、安全点、scratch 生命周期
- `main/src/infer_workflow.cpp`
  - Infer 顶层编排、设备生命周期、patch 模式选择
- `main/src/manual_flight_runtime.cpp`
  - manual cursor 调度和状态机
- `main/src/infer/patch_planner.cpp`
  - `auto_snake` 与 `debug_raster` patch 生成规则
- `main/src/infer/output_sink.cpp`
  - tensor 构建、forward、PNG / HDMI 输出路径
- `main/src/infer/ui_render.cpp`
  - manual telemetry 与工业 UI 合成
- `main/src/web_console_controller.cpp`
  - worker 生命周期、selection/settings、manual key、stop/reset/shutdown 语义
- `main/src/web_console_server.cpp`
  - HTTP/SSE 路由、preview、shutdown_web
- `main/src/web_console_config.cpp`
  - `board_ip`、`flight.*`、配置读写闭环

---

## 7. 高风险热点

### RD 侧

- `processOneEcho`
- `runMemoryFloat32Pipeline`
- `runRangeCompressionToMemory`
- `runRangeCompression`
- `runAzimuthFft`
- `runFusedRcmcAzimuthCompressionAndMagnitude`

### Infer 侧

- `ManualFlightRuntimeState::waitNextCenter`
- `ManualFlightRuntimeState::markInferenceCommitted`
- `SnakePatchSource::next`
- `DebugRasterPatchSource::next`
- `PatchTensorBuilder::build`
- `PatchInferenceRunner::forward`
- `processPatchToPng`
- `processPatchToHdmi`
- `applyManualTelemetry`
- `composeIndustrialUiFrame`
- `HdmiRenderWorker::run`

### Web 侧

- `WebConsoleController::applySelection`
- `WebConsoleController::applySettings`
- `WebConsoleController::commandStart`
- `WebConsoleController::commandStop`
- `WebConsoleController::commandReset`
- `WebConsoleController::commandShutdownWeb`
- `WebConsoleController::commandManualKey`
- `WebConsoleController::workerMain`
- `WebConsoleServer::handleClient`
- `WebConsoleServer::handleSseClient`
- `WebConsoleServer::Stop`

---

## 8. 当前已知约束

- `patch_size` 虽然来自 YAML，但当前实现只接受 `512`
- `debug_raster` 必须使用 `output_mode=png`
- `manual_flight` 只支持单张 SAR 输入
- Web Server 仍是单线程同步请求处理，慢请求会影响其他请求与心跳节奏
- `manual_flight` 已真实实现，但它是协作式 patch 游标模式，不应按“强实时飞控”理解
- `stop` 依赖 safe point，不等于强制中断
- Web 设置修改是内存态生效，退出 Web 时持久化到 runtime YAML

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
  infer_workflow_internal.hpp
  manual_flight_runtime.hpp

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
  manual_flight_runtime.cpp
  web_console.cpp
  web_console_config.cpp
  web_console_protocol.cpp
  web_console_controller.cpp
  web_console_server.cpp
  web_console_assets.cpp

main/src/infer/
  patch_planner.cpp
  ui_render.cpp
  output_sink.cpp

main/configs/
  rd_imaging.yaml
  infer_workflow.yaml
  web_console.yaml
```

---

## 10. 代码阅读结论

- `main()` 当前是循环菜单，Web Console 退出后回到菜单。
- Infer 真实实现已经分层，不再集中在单个 `infer_workflow.cpp` 中。
- `manual_flight` 真实接入 infer 主链、Web 控制链和状态链。
- Web Console 退出时会把 infer / rd / web 三份 runtime 配置写回。
- `shutdown_web` 的结果是关闭当前 Web Console 模式并返回主菜单，而不是直接结束整个进程。
