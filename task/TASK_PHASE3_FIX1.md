# Task title

收紧 Phase 3 的 Web Console 线程边界、控制边界与协议边界

---

## 1. Background

这个任务为什么存在？

- `TASK_PHASE3` 的原版设计已经实现，当前仓库里真实新增了这些 Web 相关模块：
  - `main/src/web_console.cpp`
  - `main/src/web_console_config.cpp`
  - `main/src/web_console_controller.cpp`
  - `main/src/web_console_protocol.cpp`
  - `main/src/web_console_server.cpp`
  - `main/src/web_console_assets.cpp`
  - `main/include/workflow/web/*.hpp`
  - `main/include/workflow/shared/run_control.hpp`
- `ARCHITECTURE_TEMPLATE.md` 和 `CODEBASE_MAP_TEMPLATE.md` 已经把当前 Web Console 结构记录为：
  - `workflow::web::Run(...)`
  - `WebConsoleController`
  - `WebConsoleServer`
  - `WorkflowRunControl`
  - 一个后台工作线程去运行阻塞式 `workflow::rd::Run(AppConfig, WorkflowRunControl*)` 或 `workflow::infer::Run(AppConfig, WorkflowRunControl*)`
- 当前实现已经完成“浏览器可访问的 HTTP + JSON + SSE 控制台”，但根据我们刚刚确认的新思路，Web Console 还需要进一步收紧：
  - Web 控制必须是单独的轻量线程/线程组，不能并入推理线程
  - Web 控制也不能并入 HDMI 渲染线程
  - Web 线程只能读共享状态快照、发控制命令，不能直接参与推理/显示执行链
  - 当前 `web_console_server.cpp` 采用 `acceptLoop + 每连接 detached 线程` 的实现，这对 v1 虽然能跑，但线程边界还不够收敛
  - 当前 `WebConsoleController` 在持锁状态下会直接通过 callback 推送事件，这会把 controller 锁和网络推送路径耦合在一起

---

## 2. Goal

明确写出要实现的行为。

**必须是可验证的。**

- 把第三阶段 Web Console 的设计收紧为明确的 4 类运行角色：
  - 主线程
  - Web 控制线程
  - 工作流线程
  - HDMI 渲染线程
- 明确 Web 控制线程的职责边界：
  - 只处理 HTTP / SSE / API 路由 / 状态读取 / 命令写入
  - 不直接调用推理热路径
  - 不直接调用 HDMI 写屏
  - 不直接访问 `Device / Session / FPAIDevice / LatestSnapshotMailbox`
- 明确控制链路：
  - 浏览器命令 -> `WebConsoleServer` -> `WebConsoleController` -> `WorkflowRunControl`
  - 推理 / RD / HDMI 只发布状态快照，不反向操作 Web 层
- 将“轮询查看状态”的设计正式收紧为：
  - `GET /api/state` 只用于页面初始化或兜底重拉
  - 实时状态以 `SSE /events` 为主
  - 不把高频轮询作为主路径

---

## 3. Out of scope

明确不做什么。

- 不重做 `TASK_PHASE3` 的整体功能范围
- 不删除已存在的 `HTTP + JSON + SSE` 协议
- 不把 Web Console 改成 WebSocket
- 不把 `manual_flight` 从占位接口升级成真实功能
- 不重做第二阶段 HDMI 线程模型
- 不把 Web 控制线程改成直接读写算法内部对象
- 不引入新的前端框架或外部 Web 服务进程

---

## 4. Allowed files to modify

```text
main/src/main.cpp
main/src/web_console.cpp
main/src/web_console_controller.cpp
main/src/web_console_protocol.cpp
main/src/web_console_server.cpp
main/src/infer_workflow.cpp
main/src/rd_imaging_stream.cpp
main/include/workflow/shared/run_control.hpp
main/include/workflow/web/web_console.hpp
main/include/workflow/web/web_console_controller.hpp
main/include/workflow/web/web_console_protocol.hpp
main/include/workflow/web/web_console_server.hpp
task/TASK_PHASE3_FIX1.md
```

---

## 5. Files/modules to avoid

```text
deps/**
main/include/infer_workflow_hdmi_display.hpp
main/src/web_console_assets.cpp
main/configs/web_console.yaml
ARCHITECTURE_TEMPLATE.md
CODEBASE_MAP_TEMPLATE.md
task/TASK_PHASE3.md
```

---

## 6. Chosen implementation direction

这项决策已经确定，不再二选一：

- Web Console 线程关系固定为：
  - 主线程：启动/停止 Web 模式，持有 controller 和 server
  - Web 控制线程：处理网络 I/O、HTTP 请求、SSE 推送
  - 工作流线程：执行 `RD only` 或 `Inference only`
  - HDMI 渲染线程：仅在 `Inference + output.mode=hdmi` 时存在
- Web 控制线程独立存在，不并到：
  - `workflow::infer::Run(...)` 的推理线程
  - `HdmiRenderWorker`
- 当前实现中的“每连接 detached 线程”方案不再作为推荐终态。Fix1 的目标设计改为：
  - 一个长期运行的 `WebConsoleServer` 线程
  - 该线程负责 accept、解析请求、返回响应、维护 SSE 客户端
  - 非流式普通请求在这个线程内同步处理
  - SSE 客户端仍通过 server 内部列表管理，但不再为每个普通连接额外脱离出 detached 工作线程
- `WebConsoleController` 继续持有唯一后台工作线程，但职责收紧为：
  - 管理状态机
  - 管理内存配置
  - 创建/停止/join 单个工作流线程
  - 不直接做网络 socket 读写
- `WorkflowRunControl` 继续作为唯一控制桥：
  - `pause / resume / stop / reset`
  - 最新运行时快照发布
  - Web 层只能通过它和 controller 间接控制工作流
- `WorkflowRunControl` 继续只承载“协作式控制”和“运行快照”，不扩展成通用消息总线。

---

## 7. Tightened thread model

### 7.1 Main thread

职责固定为：

- 进入 `workflow::web::Run(...)`
- 创建：
  - `WebConsoleController`
  - `WebConsoleServer`
- 启动 Web 控制线程
- 在退出时按顺序：
  - 停止 server
  - 请求 controller 停止后台工作线程
  - join Web 控制线程
  - join 工作流线程

主线程不做：

- HTTP accept
- 算法执行
- HDMI 显示

### 7.2 Web control thread

职责固定为：

- 监听 socket
- 解析 HTTP
- 返回静态资源
- 处理 `/api/*`
- 维护 `/events` SSE 客户端
- 读取只读状态快照

明确禁止：

- 直接调用 `workflow::infer::Run(...)`
- 直接调用 `workflow::rd::Run(...)`
- 直接调用 `HdmiFrameSink::write(...)`
- 直接调用 `PatchInferenceRunner::forward(...)`
- 直接访问 `Device / Session / FPAIDevice`

### 7.3 Workflow worker thread

职责固定为：

- 在 controller 的调度下运行一个阻塞式工作流：
  - `workflow::infer::Run(AppConfig, WorkflowRunControl*)`
  - 或 `workflow::rd::Run(AppConfig, WorkflowRunControl*)`
- 在安全点协作式检查：
  - `pause`
  - `stop`
  - `reset`
- 发布 `WorkflowRuntimeSnapshot`

### 7.4 HDMI render thread

职责保持不变：

- 只读取最新 snapshot
- 只做固定节拍 UI 渲染与 HDMI 输出
- 不处理 Web 请求
- 不处理浏览器命令

---

## 8. Tightened shared-state design

共享状态收紧为两层：

### 8.1 Controller-owned authoritative state

由 `WebConsoleController` 持有：

- 当前 UI 选择
- 当前内存配置
- 当前运行状态机
- 当前后台线程句柄
- `WorkflowRunControl`

这部分只有 controller 可写。

### 8.2 Read-only runtime snapshots

供 Web 控制线程读取并返回给浏览器：

- `WorkflowRuntimeSnapshot`
  - 当前 workflow
  - 当前 stage
  - 当前 item
  - 当前 index / total
  - `infer_ms / total_ms / fps`
  - `last_error`
- 如后续需要 HDMI 单独状态，新增：
  - `DisplayRuntimeSnapshot`

Fix1 的明确要求：

- Web 线程只能读取快照
- Web 线程不能直接读算法内部对象
- `WorkflowRunControl::publish(...)` 仍然是工作流到 controller 的唯一快照发布入口

---

## 9. Tightened command flow

命令流严格固定为：

```text
browser
 -> HTTP POST /api/command/*
 -> WebConsoleServer
 -> WebConsoleController
 -> WorkflowRunControl
 -> workflow thread safe point
```

状态流严格固定为：

```text
workflow thread / hdmi render thread
 -> WorkflowRunControl::publish(...)
 -> WebConsoleController
 -> cached runtime snapshot
 -> WebConsoleServer SSE broadcast
 -> browser
```

禁止的反向路径：

- workflow thread -> direct socket write
- infer thread -> direct SSE push
- HDMI thread -> direct controller mutation

---

## 10. Protocol tightening

第三阶段协议保留 `HTTP + JSON + SSE`，但 Fix1 要把使用口径写死：

- `GET /api/state`
  - 只用于首次加载或主动刷新
- `GET /events`
  - 作为实时状态主路径
- `POST /api/selection`
- `POST /api/settings`
- `POST /api/command/start`
- `POST /api/command/pause`
- `POST /api/command/stop`
- `POST /api/command/reset`
- `POST /api/manual/key`

Fix1 约束：

- 前端不以高频 `GET /api/state` 轮询作为主更新路径
- SSE 必须是主路径
- 若 SSE 断开，前端允许退回低频补偿轮询，但这只作为故障兜底，不写成主设计

---

## 11. Controller locking rules

这是 Fix1 需要明确写死的实现约束：

- `WebConsoleController` 不允许在持有 `mutex_` 的情况下执行网络发送
- event callback 可以保留，但必须改成：
  - 在锁内复制 callback 和要发送的 payload
  - 解锁后再调用 callback
- `commandStop()` / `commandReset()` / 析构路径中的 `join()` 顺序必须清楚，避免：
  - 持锁 join
  - 回调重入
  - worker 线程退出时再次争抢 controller 锁导致死锁

---

## 12. File-level design tightening

### `main/src/web_console_server.cpp`

收紧要求：

- 作为唯一 Web 控制线程实现点
- 不再把普通 HTTP client 处理拆成 detached 线程
- SSE client 管理继续留在这里
- 不持有工作流对象，只持有 controller 引用

### `main/src/web_console_controller.cpp`

收紧要求：

- 只做状态机、配置管理、后台工作线程调度
- 所有 event callback 出锁后调用
- `start / pause / stop / reset` 的合法性判断全部在这里完成

### `main/include/workflow/shared/run_control.hpp`

收紧要求：

- 继续作为工作流控制桥
- 不新增网络、JSON、socket 相关职责
- 如要扩展，只允许扩展快照字段与控制标志

### `main/src/infer_workflow.cpp`

收紧要求：

- 继续只接收 `WorkflowRunControl*`
- 继续只在 patch 边界检查 pause/stop
- 不新增 Web 协议或 server 相关逻辑

### `main/src/rd_imaging_stream.cpp`

收紧要求：

- 继续只在文件 / tile 安全点检查控制标志
- 不新增 Web 协议或 server 相关逻辑

---

## 13. Functional requirements

- [ ] Web 控制线程必须独立存在，不并入推理线程和 HDMI 渲染线程
- [ ] Web 控制线程只读快照、只写命令，不直接参与算法执行
- [ ] `WebConsoleServer` 必须成为唯一网络 I/O 承载点
- [ ] 普通 HTTP 请求不得再依赖 detached 线程处理
- [ ] `WebConsoleController` 仍然只允许一个后台工作流线程
- [ ] SSE 必须作为实时状态主路径
- [ ] `GET /api/state` 只作为初始化和兜底重拉
- [ ] `manual_flight` 和 WASD 继续保留接口，但统一返回 `not_implemented`
- [ ] 原有 CLI `RD only / Inference only / Web Console` 入口保持不变

---

## 14. Non-functional requirements

- [ ] 保持最小范围修改，只收紧 Web Console 设计，不重做算法
- [ ] 不新增第三方依赖
- [ ] 不扩大公共接口到无关模块
- [ ] 锁边界必须清晰，禁止在 controller 锁内做 socket/network 回调
- [ ] 线程退出顺序必须 review 友好
- [ ] 文档中的线程模型、代码中的真实线程模型、SSE 实现方式三者必须一致

---

## 15. Validation

明确要求运行什么验证：

```text
1. g++ -fsyntax-only 覆盖:
   - main/src/web_console.cpp
   - main/src/web_console_controller.cpp
   - main/src/web_console_protocol.cpp
   - main/src/web_console_server.cpp
   - main/src/main.cpp
   - main/src/infer_workflow.cpp
   - main/src/rd_imaging_stream.cpp
2. 静态审查:
   - 确认 Web 线程不直接调用 infer/rd 算法入口
   - 确认 Web 线程不直接访问 Device / Session / FPAIDevice
   - 确认普通 HTTP 请求不再走 detached 线程
   - 确认 controller 回调在出锁后调用
3. 行为验证:
   - 浏览器首次加载能通过 /api/state 拿到完整状态
   - /events 能持续收到状态更新
   - 关闭 SSE 后再连，控制台仍可恢复状态显示
4. 控制验证:
   - start / pause / stop / reset 经由 controller 生效
   - infer 路径 pause/stop 仍只在 patch 边界生效
   - rd 路径 pause/stop 仍只在文件/tile 边界生效
5. 生命周期验证:
   - Web 模式退出时，server 线程、worker 线程、HDMI 线程都能正确收尾
```

---

## 16. Required response format before editing

要求 Codex 在动手前先输出：

1. 对任务的理解
2. 计划修改的文件
3. 不会修改的文件
4. 实现方案
5. 风险点
6. 验证方案

---

## 17. Required response format after editing

要求 Codex 改完后输出：

1. 修改了哪些文件
2. 每个文件做了什么改动
3. 为什么这样设计
4. 跑了哪些命令
5. 哪些验证通过 / 未通过
6. 剩余风险

---

## 18. Done when

写成客观验收标准：

- Web 控制线程已经被明确为独立轻量线程
- 它既不并入推理线程，也不并入 HDMI 渲染线程
- Web 线程只处理 HTTP/SSE/状态读取/命令写入
- 控制命令统一经 `WebConsoleController -> WorkflowRunControl`
- 实时状态统一经 SSE 推送，不再把高频轮询作为主设计
- 普通 HTTP 请求不再依赖 detached 线程
- controller 的锁边界与网络回调边界已经解耦
- diff 可 review，且修改范围只围绕 Web Console 收紧

---

## 19. md update
检查下面的md文件，若有代码仓库的更新内容与下面的md文件内容有关，伴随更新
- ARCHITECTURE_TEMPLATE.md
- CODEBASE_MAP_TEMPLATE.md
