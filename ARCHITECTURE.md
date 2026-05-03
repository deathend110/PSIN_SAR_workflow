# ARCHITECTURE.md

> 本文档记录当前仓库 `main/` 目录的真实系统边界、模块关系、线程模型、配置边界和不可破坏约束。
> 若历史描述与当前代码冲突，以当前代码实现为准。

---

## 1. 系统目标

- 这是一个面向 `Linux + aarch64 + ZG330` 板端部署的 PSIN SAR 主机侧工作流程序。
- 程序覆盖三类运行能力：
  - `RD only`：把 `echo.bin` 成像为 SAR 灰度 PNG
  - `Inference only`：把 SAR PNG 切成 patch，执行模型推理，输出 HDMI 或 PNG
  - `Web Console`：通过嵌入式 HTTP + JSON + SSE 控制 RD / Infer 工作流
- 典型数据链路为：
  - `echo.bin -> RD 成像 -> SAR PNG -> patch 推理 -> 恢复图 / 分割图 / UI 合成 -> HDMI 或 PNG`
- `Web Console` 不是独立守护进程，而是主程序 `psin_workflow` 的一个运行模式。

顶层入口在 `main/src/main.cpp`：

```text
main()
 -> while (true)
    -> PromptForMode()
    -> 1. RD only
    -> 2. Inference only
    -> 3. Web Console
    -> 0. Exit
```

当前实现语义：

- `RD only` 和 `Inference only` 被选中后直接执行并返回进程退出码。
- `Web Console` 返回 `0` 后不会直接退出进程，而是回到菜单循环，允许再次选择模式。
- `Exit` 才是显式退出主程序的路径。

---

## 2. 非目标

当前仓库明确不负责：

- 模型训练、量化、导出
- 重写 `deps/**` 或底层设备后端
- 把 `RD only` 与 `Inference only` 自动串成单次大流水线
- 多作业并发推理
- 独立外部 Web 服务、WebSocket、认证、TLS
- 三维飞控或无人机控制系统

`manual_flight` 当前只是 `manual_flight` patch 游标模式，不是物理飞控系统。

---

## 3. 运行模式

### 3.1 RD only

- 入口：
  - `workflow::rd::Run(const std::filesystem::path&)`
  - `workflow::rd::Run(const AppConfig&, WorkflowRunControl*)`
- 输入：
  - `rd.echo_dir` 下的 `.bin`
- 输出：
  - `rd.output_dir` 下的 SAR 灰度 PNG

### 3.2 Inference only

- 入口：
  - `workflow::infer::Run(const std::filesystem::path&)`
  - `workflow::infer::Run(const AppConfig&, WorkflowRunControl*)`
- 输入：
  - `infer.input.sar_img_dir` 下的 SAR 图像
- patch 模式：
  - `auto_snake`
  - `manual_flight`
  - `debug_raster`
- 输出：
  - `hdmi`
  - 或 `infer.output.dir/<sar_stem>/patch_*.png`
  - `debug_raster` 下还会额外输出 `debug_<sar_stem>/restore` 和 `mask_class`

### 3.3 Web Console

- 入口：
  - `workflow::web::Run(const std::filesystem::path&)`
- 协议：
  - `HTTP + JSON + SSE`
- 职责：
  - 读取和修改内存态 selection / settings
  - 启动、暂停、停止、复位后台 RD / Infer workflow
  - 暴露状态、事件流和图像预览
  - 转发 `manual_flight` 键控输入
  - 在退出前把 infer / rd / web 配置写回 runtime YAML

---

## 4. 模块拆分

| 模块 | 文件 | 职责 |
|---|---|---|
| 主入口 | `main/src/main.cpp` | 菜单循环与模式分发 |
| Shared config utils | `main/include/workflow/shared/config_utils.hpp` `main/src/config_utils.cpp` | 简化 YAML 读写、文本解析、原子写文件 |
| Shared run control | `main/include/workflow/shared/run_control.hpp` | 协作式 pause / resume / stop / reset 与状态快照发布 |
| RD config | `main/include/workflow/rd/rd_config.hpp` `main/src/rd_config.cpp` | 读写 `rd_imaging.yaml` |
| RD workflow | `main/include/workflow/rd/rd_workflow.hpp` `main/src/rd_imaging_stream.cpp` | Echo 扫描、管线选择、RD 成像、PNG 输出、scratch 生命周期 |
| Infer config | `main/include/workflow/infer/infer_config.hpp` `main/src/infer_config.cpp` | 读写 `infer_workflow.yaml` |
| Infer workflow orchestrator | `main/include/workflow/infer/infer_workflow.hpp` `main/src/infer_workflow.cpp` | 采集 SAR 图、打开设备、加载网络、创建 sink、驱动 patch 流程、发布快照 |
| Infer manual runtime | `main/include/workflow/infer/manual_flight_runtime.hpp` `main/src/manual_flight_runtime.cpp` | `manual_flight` 游标状态机、方向切换、edge hold、latest-wins 调度 |
| Infer patch planner | `main/include/workflow/infer/infer_workflow_internal.hpp` `main/src/infer/patch_planner.cpp` | `SnakePatchSource` 与 `DebugRasterPatchSource` |
| Infer output / HDMI | `main/include/workflow/infer/infer_workflow_internal.hpp` `main/src/infer/output_sink.cpp` `main/include/infer_workflow_hdmi_display.hpp` | Tensor 构建、forward 封装、PNG/HDMI sink、快照邮箱、HDMI 渲染线程 |
| Infer UI render | `main/include/workflow/infer/infer_workflow_internal.hpp` `main/src/infer/ui_render.cpp` | 小地图、manual telemetry 注入、工业 UI 合成 |
| Web config | `main/include/workflow/web/web_console_config.hpp` `main/src/web_console_config.cpp` | 读写 `web_console.yaml` |
| Web controller | `main/include/workflow/web/web_console_controller.hpp` `main/src/web_console_controller.cpp` | selection / settings、worker 生命周期、manual key 转发、状态机 |
| Web protocol | `main/include/workflow/web/web_console_protocol.hpp` `main/src/web_console_protocol.cpp` | HTTP 请求解析、JSON DTO、状态序列化、路由枚举 |
| Web server | `main/include/workflow/web/web_console_server.hpp` `main/src/web_console_server.cpp` `main/src/web_console_assets.cpp` | HTTP 路由、SSE、静态资源、预览接口、shutdown 路径 |

模块边界约束：

- `main.cpp` 只做菜单与模式分发，不进入 HTTP 或算法细节。
- `infer_workflow.cpp` 负责 Infer 总编排，但已不再独占所有 infer 细节实现。
- `manual_flight` 核心状态机在 `manual_flight_runtime.cpp`，不是混在 Web 层。
- `web_console_controller.cpp` 负责控制面状态与 worker 生命周期，不直接持有 socket 逻辑。
- `web_console_server.cpp` 是唯一网络 I/O 承载点。

---

## 5. 主数据流

### 5.1 RD only

```text
rd_imaging.yaml
 -> LoadConfig
 -> collectEchoBins
 -> for each echo
    -> processOneEcho
       -> readEchoShapeAndValidate
       -> choose execution mode
          -> memory_float32
          -> memory pipeline
          -> scratch_double
       -> write SAR PNG
       -> cleanup scratch unless keep_scratch=true
```

### 5.2 Inference only

```text
infer_workflow.yaml
 -> LoadConfig
 -> collectSarImages
 -> Device::Open
 -> loadNetwork / validateNetworkIO / initSession / session.apply
 -> create sink
    -> PngFrameSink
    -> or HdmiFrameSink + LatestSnapshotMailbox + HdmiRenderWorker
 -> for each SAR
    -> loadSarImageNorm
    -> choose patch source
       -> auto_snake: SnakePatchSource
       -> manual_flight: ManualFlightRuntime
       -> debug_raster: DebugRasterPatchSource
    -> PatchTensorBuilder::build
    -> PatchInferenceRunner::forward
    -> restoreToGrayU8 / logitsToMaskBgr
    -> applyManualTelemetry if needed
    -> composeIndustrialUiFrame
    -> sink.write or mailbox.publish
 -> Device::Close
```

### 5.3 Web Console

```text
browser
 -> GET /, /app.js, /app.css
 -> GET /api/state, /api/settings, /api/sources, /api/source/preview
 -> POST /api/selection, /api/settings
 -> POST /api/command/start|pause|stop|reset|shutdown_web
 -> POST /api/manual/key
 -> GET /events
 -> WebConsoleServer
 -> WebConsoleController
 -> background workflow worker
    -> workflow::rd::Run(AppConfig, WorkflowRunControl*)
    -> workflow::infer::Run(AppConfig, WorkflowRunControl*)
 -> server stop + worker stop + config persist
 -> workflow::web::Run(...) returns
 -> main() menu loop resumes
```

---

## 6. 配置边界

### 6.1 RD config

文件：

- `main/configs/rd_imaging.yaml`

职责：

- echo 输入目录
- 输出目录
- scratch 目录
- tile 大小
- 内存上限
- 执行模式

当前支持执行模式：

- `auto`
- `memory_float32`
- `scratch_double`

说明：

- `auto` 下代码会在 `memory_float32`、memory pipeline、`scratch_double` 之间按估算内存选择。

### 6.2 Infer config

文件：

- `main/configs/infer_workflow.yaml`

职责：

- 设备 URL 与 backend 选项
- SAR 输入目录 / 扩展名 / 是否递归
- patch 模式、`patch_size`、`stride`
- `debug_raster` 的 X/Y stride
- 模型 `json/raw`
- 输出模式与输出目录
- HDMI 尺寸与 FPS

当前代码约束：

- `patch_size` 必须是 `512`
- `stride` 必须为正整数，且不超过 `512`
- 板级预算校验要求 `stride >= 64`
- `debug_raster` 只能与 `output_mode=png` 组合
- `manual_flight` 要求恰好选择一个 SAR 图像

### 6.3 Web config

文件：

- `main/configs/web_console.yaml`

职责：

- `server.bind`
- `server.board_ip`
- `server.port`
- `server.sse_heartbeat_ms`
- `ui.title`
- `config.infer_path`
- `config.rd_path`
- `flight.manual_step_px`
- `flight.boost_step_px`
- `flight.trigger_distance_px`
- `flight.cache_grid_px`
- `flight.path_overlay`
- `flight.control_bindings`

说明：

- Web config 支持写回 runtime YAML。
- infer / rd config 也会在 Web Console 退出时一起持久化。

---

## 7. 线程模型

当前线程关系以代码实现为准：

- Main thread
  - 运行 `main()`
  - 在 Web 模式下负责创建 controller / server
  - 负责 Web 退出收尾与配置持久化
- Web thread
  - 运行 `WebConsoleServer::Run()`
  - 单线程处理 HTTP 请求与 SSE 心跳/事件刷新
- Workflow worker thread
  - 由 `WebConsoleController` 持有
  - 真实执行 `workflow::rd::Run(...)` 或 `workflow::infer::Run(...)`
- HDMI render thread
  - 仅在 `infer.output.mode=hdmi` 时存在
  - 从 `LatestSnapshotMailbox` 读取最新快照并刷新 HDMI
- Manual cursor runtime
  - 没有独立线程
  - `manual_flight` 推进节拍绑定在 infer worker patch 循环内

关键约束：

- Web Server 不是 per-request 多线程模型。
- `stop` 是协作式停止，依赖 RD / Infer 到达安全点。
- `manual_flight` 输入不会直接插入 HDMI 渲染线程，也不会打断一次正在执行的 forward。

---

## 8. 所有权与生命周期

| 对象 | Owner | 生命周期 |
|---|---|---|
| `infer::AppConfig` / `rd::AppConfig` | 各自 `Run(...)` 栈对象，或 `WebConsoleController` 内存态副本 | 单次运行 / 单次 Web 生命周期 |
| `WorkflowRunControl` | `WebConsoleController` | 单次后台 workflow 生命周期 |
| `WebConsoleController` | `workflow::web::Run(...)` | 整个 Web Console 生命周期 |
| `WebConsoleServer` | `workflow::web::Run(...)` | 整个 HTTP/SSE 生命周期 |
| `Device` / `Session` / `NetworkView` | `workflow::infer::Run(...)` | 单次 infer 运行 |
| `IFrameSink` | `workflow::infer::Run(...)` | 单次 infer 运行 |
| `LatestSnapshotMailbox` | `workflow::infer::Run(...)` | 单次 HDMI infer 运行 |
| `HdmiRenderWorker` | `workflow::infer::Run(...)` | 单次 HDMI infer 运行 |
| `ManualFlightRuntimeState` | 当前活动的 `ManualFlightRuntime` + 进程级 coordinator 引用 | 单张 manual SAR 运行期内活跃 |
| RD scratch files | `workflow::rd::Run(...)` 每个 echo | 单个 echo，除非 `keep_scratch=true` |

Web Console 退出顺序以 `main/src/web_console.cpp` 为准：

```text
server->Stop()
controller->RequestWorkerStop()
join web_thread
controller->JoinWorker()
PersistControllerConfigs(...)
```

---

## 9. 错误处理

总体风格：

- 顶层 `Run(...)` 返回整型状态码
- 模块内部大量使用异常
- Web API 使用固定 JSON 结果：
  - `ok`
  - `code`
  - `message`

RD：

- 单个 echo 失败会记录错误并继续处理后续文件

Infer：

- 初始化、设备、模型或 session 错误会终止本次 infer 运行
- 保留显式 `SIGSEGV -> _Exit(139)` 保护

Web：

- HTTP 请求解析区分 `400 / 408 / 413 / 500`
- `shutdown_web` 触发安全停机路径
- `shutdown_web` 当前会结束 `workflow::web::Run(...)`，但主程序随后回到菜单，而不是直接退出整个进程

---

## 10. 当前 UI / 控制事实

- 前端通过 `/events` 订阅状态流，`/api/state` 主要用于首屏初始化与补读状态。
- `/api/source/preview` 只支持 inference 图像预览。
- `patch_mode` 当前包含：
  - `auto_snake`
  - `manual_flight`
  - `debug_raster`
- `manual_flight` 的键控接口是 `/api/manual/key`。
- `keyup` 在当前 cursor 语义下不会触发停止，只会被忽略。
- Web 设置修改只更新内存态，真正落盘发生在 Web Console 退出时。

---

## 11. 不可破坏约束

- 主程序一次只运行一个工作模式。
- Web Console 下同一时刻只允许一个活动后台 workflow。
- `WebConsoleServer` 是唯一网络 I/O 承载点。
- `WebConsoleController` 不应在持锁状态下执行网络发送。
- RD 执行模式外部接口只暴露 `auto / memory_float32 / scratch_double`。
- `patch_size` 必须保持 `512`。
- `manual_flight` 采用 latest-wins 的方向切换语义，不维护历史 FIFO 队列。
- `shutdown_web` 必须走安全关闭路径，不能直接 `std::exit(...)`。
- Web Console 退出前必须写回 infer / rd / web runtime 配置。

---

## 12. 高风险热点

- `main/src/infer_workflow.cpp`
  - Infer 顶层编排、设备生命周期、patch 模式选择、快照发布
- `main/src/manual_flight_runtime.cpp`
  - manual cursor 状态机、边界处理、暂停/恢复、方向调度
- `main/src/infer/output_sink.cpp`
  - Tensor 构建、forward、PNG 输出、HDMI 快照和渲染线程交互
- `main/src/rd_imaging_stream.cpp`
  - RD 管线选择、scratch 生命周期、安全点
- `main/src/web_console_controller.cpp`
  - Web 状态机、worker 生命周期、selection/settings 约束、manual key 转发
- `main/src/web_console_server.cpp`
  - HTTP 路由、SSE、shutdown 路径、socket 生命周期

---

## 13. 当前约束摘要

- 实际代码已经支持 infer / rd / web 三份配置的 runtime 写回。
- `main()` 当前是菜单循环；Web 退出后返回菜单，不是直接退出进程。
- infer 已拆成 orchestrator、manual runtime、patch planner、UI render、output sink 多个实现文件。
- `manual_flight` 已真实接入 infer 主链与 Web 控制链，但它是 patch 游标语义，不是实时飞控。
- 文档、任务单或注释若与源码冲突，以当前源码为准。
