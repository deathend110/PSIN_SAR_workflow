# ARCHITECTURE_TEMPLATE.md

> 本文档记录当前仓库 `main/` 目录里真实存在的系统边界、模块边界、线程关系、配置边界和不可破坏约束。  
> 若历史描述与当前代码冲突，以当前代码实现为准。

---

## 1. 系统目标

- 这是一个面向 `Linux + aarch64 + ZG330` 的 SAR 主机侧工作流工程。
- 程序提供 3 个真实运行模式：
  - `RD only`
  - `Inference only`
  - `Web Console`
- 典型数据链路为：
  - `echo.bin -> RD 成像 -> SAR 灰度 PNG -> patch 推理 -> 灰度恢复图 + 分割图 -> HDMI / PNG`
- `Web Console` 不是独立守护进程，而是主程序 `psin_workflow` 的第三种运行模式。

顶层入口在 `main/src/main.cpp`：

```text
main()
 -> PromptForMode()
 -> 1. RD only
 -> 2. Inference only
 -> 3. Web Console
 -> 0. Exit
```

---

## 2. 非目标

当前仓库明确不负责：

- 模型训练、量化和导出
- 重写 `deps/**` 或底层设备后端
- 将 `RD only` 与 `Inference only` 自动串成一个单次大流水线
- 多作业并发推理
- 独立外部 Web 服务进程、WebSocket、鉴权、TLS
- 真实三维飞控建模

`manual_flight` 当前已经实现为低自由度扫描游标模式，但它不是三维无人机飞控系统。

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
  - `infer.input.sar_img_dir` 下的 SAR PNG
- 输出：
  - `hdmi`
  - 或 `infer.output.dir/<sar_stem>/patch_*.png`

### 3.3 Web Console

- 入口：
  - `workflow::web::Run(const std::filesystem::path&)`
- 协议：
  - `HTTP + JSON + SSE`
- 职责：
  - 读取和修改内存态选择项
  - 读取和修改内存态设置项
  - 启动、暂停、停止、复位后台 workflow
  - 输出运行状态、事件流和图片预览
  - 安全关闭 Web Console
  - 退出时将 infer / rd / web 配置写回 YAML

---

## 4. 模块拆分

| 模块 | 文件 | 职责 |
|---|---|---|
| 主入口 | `main/src/main.cpp` | 菜单分发和模式选择 |
| Shared config utils | `main/include/workflow/shared/config_utils.hpp` `main/src/config_utils.cpp` | 简化 YAML 读取、文本解析、原子写文件 |
| Shared run control | `main/include/workflow/shared/run_control.hpp` | 协作式 `pause/resume/stop/reset` 和运行时快照发布 |
| RD config | `main/include/workflow/rd/rd_config.hpp` `main/src/rd_config.cpp` | 读取/写回 `rd_imaging.yaml` |
| RD workflow | `main/include/workflow/rd/rd_workflow.hpp` `main/src/rd_imaging_stream.cpp` | Echo 收集、shape 校验、执行模式选择、成像、写 PNG、清理 scratch |
| Infer config | `main/include/workflow/infer/infer_config.hpp` `main/src/infer_config.cpp` | 读取/写回 `infer_workflow.yaml` |
| Infer workflow | `main/include/workflow/infer/infer_workflow.hpp` `main/src/infer_workflow.cpp` | SAR 收集、patch 生成、tensor 构建、推理、后处理、UI 合成、HDMI/PNG 输出、manual_flight 运行时 |
| Web config | `main/include/workflow/web/web_console_config.hpp` `main/src/web_console_config.cpp` `main/configs/web_console.yaml` | 读取/写回 Web 配置、`board_ip`、`flight.*` |
| Web controller | `main/include/workflow/web/web_console_controller.hpp` `main/src/web_console_controller.cpp` | 选择项、设置项、状态机、后台 worker、manual key 转发、配置持久化入口 |
| Web protocol | `main/include/workflow/web/web_console_protocol.hpp` `main/src/web_console_protocol.cpp` | JSON DTO、Query/Flat JSON 解析、状态响应序列化 |
| Web server | `main/include/workflow/web/web_console_server.hpp` `main/src/web_console_server.cpp` `main/src/web_console_assets.cpp` | HTTP 路由、SSE、静态资源、预览图、`shutdown_web`、前端 UI |
| HDMI display adapter | `main/include/infer_workflow_hdmi_display.hpp` | RGB565 HDMI 输出适配 |

模块边界约束：

- `main.cpp` 只做模式分发，不进入 HTTP 或算法细节。
- `web_console_server.cpp` 是唯一网络 I/O 承载点。
- `web_console_controller.cpp` 负责 Web 控制平面的状态机和 worker 生命周期，不直接操作 socket。
- RD 和 Infer 仍然各自保留单个顶层 `Run(...)` 入口。

---

## 5. 主数据流

### 5.1 RD only

```text
rd_imaging.yaml
 -> LoadConfig
 -> collectEchoBins
 -> processOneEcho
    -> readEchoShapeAndValidate
    -> choose execution mode
       -> memory_float32
       -> scratch_double
       -> auto(优先 memory_float32，不满足内存约束则回退 scratch_double)
    -> writeNormalizedPng*
    -> cleanup scratch
```

### 5.2 Inference only

```text
infer_workflow.yaml
 -> LoadConfig
 -> collectSarImages
 -> Device::Open
 -> loadNetwork / validateNetworkIO / initSession / session.apply
 -> create sink (PNG or HDMI)
 -> for each SAR
    -> loadSarImageNorm
    -> patch source
       -> auto_snake: SnakePatchSource(cfg.patch_size, cfg.stride)
       -> manual_flight: ManualFlightRuntime
    -> PatchTensorBuilder::build
    -> PatchInferenceRunner::forward
    -> restoreToGrayU8
    -> logitsToMaskBgr
    -> buildMiniMapContext / applyManualTelemetry
    -> composeIndustrialUiFrame
    -> sink.write
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
 -> worker thread
    -> workflow::rd::Run(AppConfig, WorkflowRunControl*)
    -> workflow::infer::Run(AppConfig, WorkflowRunControl*)
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

当前支持的执行模式只有：

- `auto`
- `memory_float32`
- `scratch_double`

### 6.2 Infer config

文件：

- `main/configs/infer_workflow.yaml`

职责：

- 设备 URL
- backend 相关开关
- SAR 输入目录
- patch 模式、`patch_size`、`stride`
- 输出模式和输出目录
- HDMI 显示尺寸

当前代码约束：

- `patch_size` 必须是 `512`
- `stride` 不是写死常量，但必须为正整数
- 当前仓库默认配置是 `stride: 128`

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
  - 为兼容旧配置保留，但当前扫描游标模式不再依赖它触发 patch
- `flight.cache_grid_px`
- `flight.path_overlay`
- `flight.control_bindings`

当前实现中，Web config 已支持写回 YAML，不再是只读配置。

---

## 7. 线程模型

当前线程关系以代码实现为准：

- Main thread
  - 运行 `main()`
  - 在 Web Console 模式下创建 controller 和 server
  - 负责整体启动/退出顺序与配置持久化
- Web thread
  - 运行 `WebConsoleServer::Run()`
  - 是唯一 HTTP/SSE 服务线程
- Workflow worker thread
  - 由 `WebConsoleController` 持有
  - 真正执行 `workflow::rd::Run(...)` 或 `workflow::infer::Run(...)`
- HDMI render thread
  - 仅存在于 `output.mode=hdmi`
  - 负责从 `LatestSnapshotMailbox` 读取最新快照并显示
- Manual cursor runtime
  - 仅存在于 `patch.mode=manual_flight`
  - 位于 infer worker 主循环内部
  - 不再有独立 simulation thread
  - 推进节拍绑定到“当前 patch 完成 -> 计算下一合法中心 -> 提交下一 patch”

关键约束：

- Web 请求当前仍是单线程同步处理，没有每请求单独线程。
- `GET /events` 是状态流主路径。
- `GET /api/state` 用于首屏初始化、重连和 manual telemetry 读取。
- `stop` 是协作式停止，真正停下仍依赖 workflow 到达安全点。
- `manual_flight` 没有把控制逻辑塞进 HDMI 渲染线程，也没有把输入逻辑塞进 NPU forward 阻塞段。
- 当前 manual 模式不再使用独立 simulation thread，推进节拍直接绑定到 patch 完成。
- `ResetManualFlight()` 现在会把对外可见的 manual 中心点直接回到左上角第一个合法 patch 中心，而不是 `(0,0)` 占位值。

---

## 8. 所有权与生命周期

| 对象 | Owner | 生命周期 |
|---|---|---|
| `infer::AppConfig` / `rd::AppConfig` | 各自 `Run(...)` 栈对象，或 `WebConsoleController` 内存态副本 | 单次模式运行 / 单次 Web 生命周期 |
| `WorkflowRunControl` | `WebConsoleController` | 单次后台作业生命周期 |
| `WebConsoleController` | `workflow::web::Run(...)` | 整个 Web Console 生命周期 |
| `WebConsoleServer` | `workflow::web::Run(...)` | 整个 HTTP/SSE 生命周期 |
| `Device` / `Session` / `NetworkView` | `workflow::infer::Run(...)` | 整次推理运行 |
| `IFrameSink` | `workflow::infer::Run(...)` | 整次推理运行 |
| `RGB565HDMIDisplay` | `HdmiFrameSink` | HDMI 输出生命周期 |
| `ManualFlightRuntime` | `workflow::infer::Run(...)` | 单张 manual_flight SAR 运行期间 |
| RD scratch files | `workflow::rd::Run(...)` 每个 echo | 单 echo，除非 `keep_scratch=true` |

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
- 模块内部广泛使用异常
- Web API 使用固定 JSON 结果：
  - `ok`
  - `code`
  - `message`

RD：

- 单个 echo 失败会记录错误并继续处理后续文件

Infer：

- 关键初始化或设备错误会终止本次推理运行
- 仍保留显式 `SIGSEGV -> _Exit(139)` 保护

Web：

- `manual_flight` 相关接口已经实现，不再返回历史上的 `not_implemented`
- `shutdown_web` 会触发安全关闭路径
- 后台异常通过 controller 收口为 `Error` 状态，并经 `SSE / /api/state / 事件流` 暴露

---

## 10. 当前 UI / 控制事实

Web 前端当前真实行为：

- 设置面板默认隐藏，由齿轮按钮展开/收起
- 右上角有黄色圆形 `Shutdown Web Console` 按钮
- 历史上的 `manual_flight` 黄色常驻提示条已经删除
- `patch_mode` 已包含 `manual_flight`
- `Reserved Endpoints` 中的 `W/A/S/D` 和键盘输入会真正转发到 `/api/manual/key`
- 前端不再维护旧的“按住/松开” manual key 集合；键盘输入只发送方向切换命令
- 当前 manual telemetry 主要通过 `SSE` 状态事件暴露，`GET /api/state` 也可直接读取同一份 manual telemetry

---

## 11. 不可破坏约束

最重要的规则：

- 主程序一次只运行一个模式
- Web Console 下同一时刻只允许一个活跃后台 workflow
- `WebConsoleServer` 是唯一网络 I/O 承载点
- `WebConsoleController` 不应在持锁状态下做网络发送
- RD 执行模式只允许 `auto / memory_float32 / scratch_double`
- `patch_size` 必须保持 `512`
- `manual_flight` 使用 latest-wins 的“方向变更”语义，不对历史方向做 FIFO 排队；位置推进仍然按 patch 一步一步走
- `shutdown_web` 必须走安全关闭路径，不能直接粗暴 `std::exit(...)`
- Web Console 退出前要写回当前 infer / rd / web 配置

---

## 12. 高风险热点

- `main/src/infer_workflow.cpp`
  - 设备生命周期、推理顺序、manual runtime、HDMI 合成都集中在这里
- `main/src/rd_imaging_stream.cpp`
  - RD 管线、scratch 生命周期、tile 安全点都在这里
- `main/src/web_console_controller.cpp`
  - Web 状态机、worker 生命周期、stop/reset/shutdown/manual key 都在这里
- `main/src/web_console_server.cpp`
  - HTTP 路由、SSE、socket 生命周期、shutdown 路径都在这里
- `main/src/config_utils.cpp`
  - 仍然是简化 YAML 读写，不是完整 round-trip 引擎

---

## 13. 当前约束摘要

- Web Console 不需要外网，只需要浏览器和板子网络可达
- 浏览器实际访问地址应使用 `server.board_ip:server.port`
- `server.bind` 只是监听地址
- 当前仓库已经支持 infer / rd / web 配置写回 YAML
- 文档、任务单与注释若和代码冲突，以当前代码为准

---

## 14. Phase 4 Manual Flight

- `manual_flight` 已接入真实执行链路，不再只是预留按钮或占位错误
- 控制链路为：
  - `browser keydown/keyup -> POST /api/manual/key -> WebConsoleController::commandManualKey(...) -> workflow::infer::SubmitManualFlightKey(...) -> infer 内部 ManualFlightRuntime`
- `ManualFlightRuntime` 负责：
  - 位置推进
  - 速度推进
  - 路径采样
  - `requested_center` 更新
  - `request_sequence / consumed_sequence` latest-wins 调度
- infer worker 只消费最新 patch 请求，不保留历史队列
- HDMI render thread 仍只负责显示，不负责飞行控制
- `flight.*` 参数已经真正参与行为：
  - `manual_step_px`
  - `boost_step_px`
  - `trigger_distance_px`
    - 仅作为兼容配置项保留，不再驱动 manual patch 触发
  - `cache_grid_px`
  - `path_overlay`
  - `control_bindings`
- Web 前端会在 manual 运行态下补充轮询 `/api/state`，用于提升 telemetry 新鲜度
