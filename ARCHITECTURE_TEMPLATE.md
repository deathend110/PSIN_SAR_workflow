# ARCHITECTURE_TEMPLATE.md

> 用于记录当前仓库里“真实运行的系统边界、模块边界、资源约束和不可破坏规则”。
> 当前内容已经从模板更新为本工程的实际架构说明。

---

## 1. System Goal

一句话目标：

- 这是一个面向 `Linux + aarch64 + ZG330` 的 SAR 工作流工程，负责把 `echo.bin` 做 RD 成像生成 SAR 图，再对 SAR 图做固定 patch 推理，最终输出“恢复图 + 分割图”到 HDMI 或 PNG。

当前入口形态：

- 主入口已经收口为 `main/src/main.cpp`
- 当前用户可见主程序名统一为 `psin_workflow`
- 启动后只允许选择一个模式运行：
  - `RD only`
  - `Inference only`
  - `Web Console`
  - `Exit`
- CLI 仍保留 `RD only / Inference only`
- `Web Console` 作为新的第三模式，以单进程 HTTP + JSON + SSE 方式暴露板端控制台

这样设计的原因：

- 当前 RD 成像和推理都对板端内存、设备状态和中间数据有明显资源压力
- 保持“单次只跑一个模式”更容易维持资源隔离和 review 可控性

---

## 2. Non-goals

当前工程明确不负责：

- 模型训练、量化、导出
- 改写第三方依赖 `deps/`
- 在 v1 中实现单进程自动串联 RD 与推理
- 真正的 `manual_flight` 飞行控制逻辑
- YAML 写回磁盘

---

## 3. Module Breakdown

| Module | File(s) | Responsibility |
|---|---|---|
| Main entry | `main/src/main.cpp` | 显示菜单并调度模式 |
| Shared config utils | `main/include/workflow/shared/*`, `main/src/config_utils.cpp` | 简单 YAML 读取、字符串清理、默认值解析、运行模式枚举 |
| Shared run control | `main/include/workflow/shared/run_control.hpp` | 协作式 `pause / resume / stop / reset` 请求与运行时快照发布 |
| RD config | `main/include/workflow/rd/rd_config.hpp`, `main/src/rd_config.cpp` | 解析 `configs/rd_imaging.yaml` |
| RD workflow | `main/include/workflow/rd/rd_workflow.hpp`, `main/src/rd_imaging_stream.cpp` | 收集 echo、校验、成像、写 SAR、管理 scratch |
| Infer config | `main/include/workflow/infer/infer_config.hpp`, `main/src/infer_config.cpp` | 解析 `configs/infer_workflow.yaml` |
| Infer workflow | `main/include/workflow/infer/infer_workflow.hpp`, `main/src/infer_workflow.cpp` | 收集 SAR、切 patch、构建 tensor、跑 session、后处理、合成工业风 UI 帧、输出 |
| Web console config | `main/include/workflow/web/web_console_config.hpp`, `main/src/web_console_config.cpp`, `main/configs/web_console.yaml` | 解析 Web 服务自身配置 |
| Web controller | `main/include/workflow/web/web_console_controller.hpp`, `main/src/web_console_controller.cpp` | 管理 UI 选择、内存配置、状态机、后台工作线程 |
| Web protocol | `main/include/workflow/web/web_console_protocol.hpp`, `main/src/web_console_protocol.cpp` | 固定 JSON DTO、简单 JSON/Query 解析、SSE payload |
| Web server | `main/include/workflow/web/web_console_server.hpp`, `main/src/web_console_server.cpp`, `main/src/web_console_assets.cpp` | HTTP 路由、静态资源、SSE 推送 |
| HDMI adapter | `main/include/infer_workflow_hdmi_display.hpp` | RGB565 UDMA buffer 与 HDMI 显示适配 |

模块边界原则：

- 只抽取真正重复的基础能力
- RD 与推理分别暴露单一顶层接口：
  - `workflow::rd::Run(const std::filesystem::path&)`
  - `workflow::infer::Run(const std::filesystem::path&)`
  - `workflow::web::Run(const std::filesystem::path&)`
- RD / Infer 同时保留 `Run(const AppConfig&, WorkflowRunControl*)` 重载，供 Web 控制层以内存配置启动后台任务
- 不为了“好看”把内部算法细碎拆成过多文件

---

## 4. Main Data Flow

### 4.1 Menu Dispatch

```text
main()
 -> PromptForMode()
 -> RD only        -> workflow::rd::Run("configs/rd_imaging.yaml")
 -> Inference only -> workflow::infer::Run("configs/infer_workflow.yaml")
 -> Web Console    -> workflow::web::Run("configs/web_console.yaml")
 -> Exit
```

### 4.2 RD only

```text
io/echo/*.bin
 -> collectEchoBins
 -> validate shape / file size
 -> choose execution mode
    -> memory_float32
    -> memory_double
    -> scratch_double
 -> output io/sar_img/*.png
 -> cleanup scratch
```

### 4.3 Inference only

```text
io/sar_img/*.png
 -> collectSarImages
 -> loadSarImageNorm(CV_32FC1, 0~1)
 -> SnakePatchSource
 -> PatchTensorBuilder
 -> session.forward
 -> restore gray + seg logits
 -> build mini-map / telemetry context
 -> compose final industrial UI frame
 -> HDMI or io/output/<stem>/patch_*.png
```

### 4.4 Web Console

```text
browser
 -> GET /, /app.js, /app.css
 -> GET /api/state, /api/settings, /api/sources
 -> POST /api/selection, /api/command/*, /api/settings
 -> GET /events (SSE primary path)
 -> WebConsoleServer thread
 -> WebConsoleController
 -> background worker thread
    -> workflow::rd::Run(AppConfig, WorkflowRunControl*)
    -> workflow::infer::Run(AppConfig, WorkflowRunControl*)
```

---

## 5. Data Shape / Format Rules

### Echo file

- 文件格式：`.bin`
- 前 8 字节：`int32 rows + int32 cols`，little-endian
- 后续数据：`rows * cols * 2 * float32`
- 语义：复数矩阵，实部与虚部交织

### SAR image

- 落盘格式：灰度 PNG
- 加载后类型：`CV_32FC1`
- 归一化范围：`0~1`

### Patch

- 类型：`CV_32FC1`
- 尺寸：固定 `512x512`
- 扫描方式：`auto_snake`
- 步长：`256`
- 边缘不足完整 patch 直接丢弃

### Model I/O

- input[0]：`FP32 [1,512,512,1]`
- output[0]：`FP32 [1,512,512,1]`
- output[1]：`FP32 [1,512,512,6]`

---

## 6. Thread Model

Fix1 tightened the Web Console side into four explicit runtime roles:

- Main thread:
  starts and stops Web mode, owns `WebConsoleController` and `WebConsoleServer`
- Web control thread:
  the only network I/O carrier; handles HTTP, SSE, state reads, and command writes
- Workflow worker thread:
  the only background algorithm thread; runs `workflow::rd::Run(...)` or `workflow::infer::Run(...)`
- HDMI render thread:
  only exists for `Inference + output.mode=hdmi`; it is not merged into the Web layer

Web-specific invariants after Fix1:

- Ordinary HTTP requests are handled synchronously inside the dedicated server thread; no per-request detached worker is used.
- `WebConsoleController` no longer performs network callbacks while holding `mutex_`.
- Real-time state is pushed primarily through `/events`; `GET /api/state` is retained for first-load bootstrap and reconnect fallback only.

当前线程模型分成三层：

- `workflow::rd::Run(...)`
  - 仍是单工作线程串行执行
  - 通过 `WorkflowRunControl` 在文件级 / tile 级安全点协作式响应 `pause / stop`
- `workflow::infer::Run(...)`
  - 保留 Phase 2 的“推理线程 + HDMI/UI 线程”结构
  - `pause / stop` 只在 patch 边界生效
- `workflow::web::Run(...)`
  - 主线程负责 HTTP accept / 路由
  - `WebConsoleController` 持有一个后台工作线程来运行阻塞式 RD/Infer 入口

当前没有项目级多任务调度器，也没有多作业并发执行；Web 模式下同一时刻只允许一个活跃工作流。

---

## 7. Ownership And Lifetime Rules

| Object | Owner | Lifetime |
|---|---|---|
| `AppConfig` | 各自 `Run(...)` 栈对象 | 覆盖一次模式执行 |
| `Device` | `workflow::infer::Run(...)` | 从 `Device::Open()` 到显式 `Device::Close()` |
| `Network` / `NetworkView` | `workflow::infer::Run(...)` | 覆盖 session 初始化和整次推理 |
| `Session` | `workflow::infer::Run(...)` | 覆盖所有 patch 推理 |
| `IFrameSink` | `workflow::infer::Run(...)` | 覆盖所有 SAR 图输出 |
| `RGB565HDMIDisplay` | `HdmiFrameSink` | 与 HDMI sink 同生命周期 |
| `WorkflowRunControl` | `WebConsoleController` | 覆盖单次 Web 启动的后台作业 |
| `WebConsoleController` | `workflow::web::Run(...)` | 覆盖整个 Web Console 生命周期 |
| `WebConsoleServer` | `workflow::web::Run(...)` | 由主线程持有、由独立 Web 线程承载，覆盖整个 HTTP / SSE 服务生命周期 |
| 输入 tensor | `PatchTensorBuilder::build()` 返回值 | 单 patch 生命周期 |
| 输出 tensor host copy | `PatchInferenceRunner::forward()` 返回值 | 单 patch 生命周期 |
| scratch files | `workflow::rd::Run(...)` 每个 echo 处理过程 | 正常或异常路径清理，除非 `keep_scratch=true` |

重要约束：

- `session.apply()` 必须在首个 patch 推理前完成
- `PatchInferenceRunner::forward()` 里 `waitForReady()` 与 `device.reset(1)` 的顺序不能随意改
- HDMI buffer 不能在设备关闭后继续使用

---

## 8. Error Handling Strategy

整体风格：

- 沿用现有工程的异常风格
- 入口函数 `Run(...)` 统一捕获异常并返回非零状态码

RD 模块：

- 单个 echo 失败时记录错误并继续后续文件
- 最终返回聚合状态

推理模块：

- 任意关键错误直接终止本次运行
- 常见失败点包括：
  - 配置错误
  - SAR 图片缺失
  - 模型 IO shape 不匹配
  - 设备打开失败
  - 输出超时

Web 控制台：

- API 层返回固定 JSON `ok/code/message`
- `manual_flight` 和 WASD 当前统一返回 `not_implemented`
- 后台线程异常通过 controller 收口成 `Error` 状态和 SSE `error` 事件

---

## 9. Performance Constraints

### RD side

- 重点是受限内存下稳定完成成像，而不是桌面端实时吞吐
- `rd.execution_mode` 必须继续支持：
  - `auto`
  - `memory_float32`
  - `memory_double`
  - `scratch_double`

### Inference side

热路径主要是：

- patch clone
- NHWC tensor copy
- `session.forward`
- host output copy
- argmax / colorize
- build mini-map / telemetry context
- compose final industrial UI frame
- HDMI 时的 `BGR -> BGR565`

本次重构目标不是额外性能优化，因此：

- 不改变模型接口
- 不改 patch 策略
- 不改输出语义

Web 模式下性能目标也不是高并发 HTTP，而是板端单用户、单作业、低成本控制和状态观测。

---

## 10. Invariants / Do-not-break Rules

Phase 1 UI invariants:
- `output.mode=hdmi` and `output.mode=png` must share the same composed UI frame.
- Telemetry only shows inference-stage metrics such as `FPS / infer ms / total ms`.
- The overlay must not fabricate or display RD imaging time in `Inference only`.

最关键的不可破坏约束：

- 主程序一次只运行一个模式
- 不在 v1 中自动串联 RD 与推理
- Web Console 只允许一个活跃后台工作流
- `RD only` 的输入输出路径保持：
  - `io/echo/*.bin`
  - `io/sar_img/*.png`
- `Inference only` 的 PNG 输出路径保持：
  - `io/output/<stem>/patch_*.png`
- patch 规则保持：
  - `512x512`
  - `stride=256`
  - `auto_snake`
- 边缘不足完整 patch 直接丢弃
- 第一阶段 HDMI UI 保持单线程后合成，不引入 UI/推理双线程
- HDMI 继续沿用现有 `infer_workflow_hdmi_display.hpp`
- `ManualFlightPatchSource` 只保留为预留扩展点，不接控制端

---

## 11. Extension Points

当前允许的扩展点：

- 在 `workflow/shared` 里继续增加真正复用的轻量工具
- 在 RD 模块内部继续抽取更清晰的子函数，但不改算法行为
- 在推理模块内部继续抽取 patch / postprocess / sink 边界
- 以后如果需要控制端接入，可优先接 `ManualFlightPatchSource`

当前不建议直接做的扩展：

- 未验证前就恢复单进程 RD+Inference 串联
- 在没有队列设计前引入 patch 并行
- 在没有重新审查设备生命周期前引入 UI/推理双线程
- 改动 `device.reset(1)`、session 生命周期或 HDMI buffer 语义
