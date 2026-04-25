# ARCHITECTURE_TEMPLATE.md

> 记录当前仓库里真实存在的系统边界、模块边界、线程关系、配置边界和不可破坏规则。
> 若历史描述与代码冲突，以当前仓库实现为准。

---

## 1. System Goal

- 这是一个面向 `Linux + aarch64 + ZG330` 的 SAR 工作流工程。
- 系统提供 3 个实际运行模式：
  - `RD only`
  - `Inference only`
  - `Web Console`
- 典型数据链路是：
  - `echo.bin -> RD 成像 -> SAR 灰度 PNG -> patch 推理 -> 灰度恢复图 + 分割图 -> HDMI / PNG`
- `Web Console` 不是单独服务进程，而是 `psin_workflow` 内部的第三运行模式。

当前顶层入口在 [main.cpp](g:/Docker_windows_disk/PSIN_SAR_workflow/main/src/main.cpp)：

```text
main()
 -> PromptForMode()
 -> 1. RD only
 -> 2. Inference only
 -> 3. Web Console
 -> 0. Exit
```

---

## 2. Non-goals

当前工程明确不负责：

- 模型训练、量化、导出
- 重写 `deps/**`
- 自动把 `RD only` 和 `Inference only` 串成单次全流程
- 真正的 `manual_flight` 飞行控制逻辑
- 多作业并发执行
- 外部 Web 服务进程、WebSocket、鉴权、TLS

---

## 3. Runtime Modes

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
- Web 协议：
  - `HTTP + JSON + SSE`
- 职责：
  - 读取/修改内存态配置
  - 启动/暂停/停止/复位后台 workflow
  - 暴露状态、日志、输入源预览
  - 安全关闭 Web Console

---

## 4. Module Breakdown

| Module | File(s) | Responsibility |
|---|---|---|
| Main entry | `main/src/main.cpp` | 启动菜单、模式调度 |
| Shared app mode | `main/include/workflow/shared/app_mode.hpp` | CLI 模式枚举 |
| Shared config utils | `main/include/workflow/shared/config_utils.hpp`, `main/src/config_utils.cpp` | 简单 YAML 读取、字符串处理、布尔/整数解析、原子写文件 |
| Shared run control | `main/include/workflow/shared/run_control.hpp` | 协作式 `pause/resume/stop/reset` 与运行时快照发布 |
| RD config | `main/include/workflow/rd/rd_config.hpp`, `main/src/rd_config.cpp` | 读取/写回 `rd_imaging.yaml` |
| RD workflow | `main/include/workflow/rd/rd_workflow.hpp`, `main/src/rd_imaging_stream.cpp` | echo 收集、shape 校验、执行模式选择、成像、写 PNG、清理 scratch |
| Infer config | `main/include/workflow/infer/infer_config.hpp`, `main/src/infer_config.cpp` | 读取/写回 `infer_workflow.yaml` |
| Infer workflow | `main/include/workflow/infer/infer_workflow.hpp`, `main/src/infer_workflow.cpp` | SAR 收集、patch 切片、tensor 构建、推理、后处理、UI 合成、HDMI/PNG 输出 |
| Web config | `main/include/workflow/web/web_console_config.hpp`, `main/src/web_console_config.cpp`, `main/configs/web_console.yaml` | 读取/写回 Web 配置、`board_ip`、`flight.*` |
| Web controller | `main/include/workflow/web/web_console_controller.hpp`, `main/src/web_console_controller.cpp` | UI 选择、内存态配置、状态机、后台工作线程、stop/reset/shutdown 语义 |
| Web protocol | `main/include/workflow/web/web_console_protocol.hpp`, `main/src/web_console_protocol.cpp` | JSON DTO、Query/Flat JSON 解析、SSE payload |
| Web server | `main/include/workflow/web/web_console_server.hpp`, `main/src/web_console_server.cpp`, `main/src/web_console_assets.cpp` | HTTP 路由、静态资源、SSE 推送、输入图预览、`shutdown_web` 路由 |
| HDMI display adapter | `main/include/infer_workflow_hdmi_display.hpp` | RGB565 UDMA buffer 与 HDMI 输出适配 |

模块边界规则：

- `main.cpp` 只做模式分发，不塞 HTTP/算法细节。
- `web_console_server.cpp` 是唯一网络 I/O 承载点。
- `web_console_controller.cpp` 只做状态机、配置和后台线程调度，不直接做 socket 操作。
- RD / Infer 仍然保留单模块大入口，当前没有做彻底碎片化重构。

---

## 5. Main Data Flow

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
       -> auto(优先 memory_float32，超限则回退 scratch_double)
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
    -> SnakePatchSource(cfg.patch_size, cfg.stride)
    -> PatchTensorBuilder::build
    -> PatchInferenceRunner::forward
    -> restoreToGrayU8
    -> logitsToMaskBgr
    -> buildMiniMapContext
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

## 6. Configuration Boundaries

### 6.1 RD config

文件：
- [rd_imaging.yaml](g:/Docker_windows_disk/PSIN_SAR_workflow/main/configs/rd_imaging.yaml)

职责：
- echo 输入目录
- 输出目录
- scratch 目录
- tile 大小
- 内存限制
- 执行模式

支持的执行模式只有：
- `auto`
- `memory_float32`
- `scratch_double`

### 6.2 Infer config

文件：
- [infer_workflow.yaml](g:/Docker_windows_disk/PSIN_SAR_workflow/main/configs/infer_workflow.yaml)

职责：
- 设备 URL
- backend 相关开关
- SAR 输入目录
- patch 模式、`patch_size`、`stride`
- 输出模式与输出目录
- HDMI 显示尺寸

当前代码约束：
- `patch_size` 必须是 `512`
- `stride` 不是写死常量，必须为正数
- 当前仓库默认配置文件里 `stride: 128`

### 6.3 Web config

文件：
- [web_console.yaml](g:/Docker_windows_disk/PSIN_SAR_workflow/main/configs/web_console.yaml)

职责：
- `server.bind`
- `server.board_ip`
- `server.port`
- `server.sse_heartbeat_ms`
- `ui.title`
- `config.infer_path`
- `config.rd_path`
- `flight.*`

当前实现中，Web config 已支持读写回 YAML，不再只是只读配置。

---

## 7. Thread Model

当前线程模型以代码为准：

- Main thread
  - 运行 `main()`
  - 在 `Web Console` 模式下持有 `WebConsoleController` 和 `WebConsoleServer`
  - 负责整体启动/退出顺序和配置持久化
- Web thread
  - 运行 `WebConsoleServer::Run()`
  - 是唯一 HTTP/SSE 承载线程
- Workflow worker thread
  - 由 `WebConsoleController` 持有
  - 真正执行 `workflow::rd::Run(...)` 或 `workflow::infer::Run(...)`
- HDMI render thread
  - 只存在于 `Inference only + output.mode=hdmi`
  - 不并入 Web 线程

关键约束：

- Web 普通 HTTP 请求当前在 Web 线程内同步处理，没有每请求 detached 线程。
- `GET /events` 是实时状态主路径。
- `GET /api/state` 主要用于页面初始化和重拉。
- `stop` 是协作式停止；真正停下仍依赖 workflow 到达安全点。

---

## 8. Ownership And Lifetime

| Object | Owner | Lifetime |
|---|---|---|
| `infer::AppConfig` / `rd::AppConfig` | 各自 `Run(...)` 栈对象，或 `WebConsoleController` 内存态副本 | 单次模式运行 / 单次 Web 生命周期 |
| `WorkflowRunControl` | `WebConsoleController` | 单次后台作业生命周期 |
| `WebConsoleController` | `workflow::web::Run(...)` | 覆盖整个 Web Console 生命周期 |
| `WebConsoleServer` | `workflow::web::Run(...)` | 覆盖整个 HTTP/SSE 服务生命周期 |
| `Device` / `Session` / `NetworkView` | `workflow::infer::Run(...)` | 整次推理运行 |
| `IFrameSink` | `workflow::infer::Run(...)` | 整次推理运行 |
| `RGB565HDMIDisplay` | `HdmiFrameSink` | HDMI 输出生命周期 |
| RD scratch files | `workflow::rd::Run(...)` 每个 echo | 单 echo，除非 `keep_scratch=true` |

退出顺序以 [web_console.cpp](g:/Docker_windows_disk/PSIN_SAR_workflow/main/src/web_console.cpp) 为准：

```text
server->Stop()
controller->RequestWorkerStop()
join web_thread
controller->JoinWorker()
PersistControllerConfigs(...)
```

---

## 9. Error Handling

总体风格：

- 入口 `Run(...)` 返回整型状态码
- 内部广泛使用异常
- Web API 用固定 JSON：
  - `ok`
  - `code`
  - `message`

RD：
- 单个 echo 失败会记录错误并继续后续文件

Infer：
- 关键错误直接终止本次运行
- 含显式 `SIGSEGV -> _Exit(139)` 保护

Web：
- `manual_flight` 相关命令继续返回 `not_implemented`
- `shutdown_web` 通过 API 触发安全关闭 Web Console
- 后台异常通过 controller 收口为 `Error` 状态和 SSE/日志事件

---

## 10. Current UI / Control Facts

Web 前端当前真实行为：

- 设置面板默认隐藏，由右上角齿轮按钮展开/收起
- 右上角有黄色圆形 `Shutdown Web Console` 按钮
- 颜色、位置和交互都在 [web_console_assets.cpp](g:/Docker_windows_disk/PSIN_SAR_workflow/main/src/web_console_assets.cpp) 内嵌实现
- 已经移除历史上的 manual_flight 黄色常驻提示条
- `Reserved Endpoints` 区仍保留 `W/A/S/D` 按钮和接口

---

## 11. Invariants / Do-not-break Rules

最重要的不可破坏约束：

- 主程序一次只运行一个模式
- `Web Console` 下同一时刻只允许一个活跃后台 workflow
- `WebConsoleServer` 是唯一网络 I/O 承载点
- `WebConsoleController` 不能在持锁状态下做网络回调
- `RD only` 的执行模式只有 `auto / memory_float32 / scratch_double`
- `patch_size` 必须保持 `512`
- `manual_flight` 仍然是保留接口，不是已实现功能
- `shutdown_web` 必须走安全关闭路径，不能粗暴 `std::exit(...)`
- Web Console 退出前要持久化当前 infer/rd/web 配置

---

## 12. Known Hotspots

高风险文件与原因：

- [infer_workflow.cpp](g:/Docker_windows_disk/PSIN_SAR_workflow/main/src/infer_workflow.cpp)
  - 设备生命周期、推理顺序、HDMI 合成都在这里
- [rd_imaging_stream.cpp](g:/Docker_windows_disk/PSIN_SAR_workflow/main/src/rd_imaging_stream.cpp)
  - RD 管线、scratch 生命周期、tile 安全点都在这里
- [web_console_controller.cpp](g:/Docker_windows_disk/PSIN_SAR_workflow/main/src/web_console_controller.cpp)
  - Web 状态机、worker 生命周期、stop/reset/shutdown 都在这里
- [web_console_server.cpp](g:/Docker_windows_disk/PSIN_SAR_workflow/main/src/web_console_server.cpp)
  - HTTP 路由、SSE、socket 生命周期、shutdown 路径都在这里
- [config_utils.cpp](g:/Docker_windows_disk/PSIN_SAR_workflow/main/src/config_utils.cpp)
  - 仍然是“简化 YAML”读写，不是完整 YAML round-trip 引擎

---

## 13. Current Constraints Summary

- Web Console 不需要外网，只需要浏览器和板子网络可达
- 浏览器实际访问地址应使用 `server.board_ip:server.port`
- `server.bind` 只是监听地址
- 当前仓库已经支持把 infer/rd/web 配置写回 YAML
- 当前模板文档以后续变更应继续以代码为准，而不是以历史 task 文档为准

---

## 14. Phase 4 Manual Flight

- `manual_flight` 已接入真实执行链路，不再只是保留按钮和占位错误。
- 当前控制路径为：browser `keydown/keyup` -> `POST /api/manual/key` -> `WebConsoleController::commandManualKey(...)` -> `workflow::infer::SubmitManualFlightKey(...)` -> infer 内部 manual runtime -> latest-wins patch 推理 -> HDMI / PNG 输出。
- 当前线程职责为：Web server 只接收输入并转发；infer 内部 simulation thread 只推进位置、速度和请求中心点；infer worker thread 只消费最新 patch 请求并执行推理；HDMI render thread 仍然只负责显示。
- `manual_flight` 当前采用“图像平面 patch 中心点”模型，不涉及真实三维飞控。
- latest-wins 由 `request_sequence / consumed_sequence` 实现，明确避免历史 patch 排队。
- `flight.*` 已正式参与行为：`manual_step_px` / `boost_step_px` 对应速度上限参考值，`trigger_distance_px` 对应新 patch 触发阈值，`cache_grid_px` 对应路径采样 / 缓存网格参考值，`path_overlay` 控制 HDMI/UI 轨迹叠加，`control_bindings` 当前仍以 `W/A/S/D` 为主。
- Web 前端对 manual telemetry 额外使用现有 `GET /api/state` 做轻量轮询刷新，只在 `manual_flight + running/paused` 时启用，不新增新协议。
- Web 前端只有在 `/api/manual/key` 返回成功后才更新本地 `manualKeys`，避免浏览器本地按键状态与板端状态漂移。
