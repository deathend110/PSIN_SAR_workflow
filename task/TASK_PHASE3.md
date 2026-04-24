# Task title

第三阶段：板端嵌入式 Web 控制台与工作流控制层

---

## 1. Background

这个任务为什么存在？

- 当前仓库入口仍然是 [main/src/main.cpp]( /g:/Docker_windows_disk/PSIN_SAR_workflow/main/src/main.cpp:49 ) 的命令行菜单，只支持：
  - `1. RD only`
  - `2. Inference only`
- 第二阶段已经把 `hdmi` 推理显示拆成“推理线程 + HDMI/UI 线程”，但控制入口仍然是本地终端，不适合板端长期运行和上位机浏览器控制。
- 浏览器不能直接连接“裸 TCP 自定义协议”，所以这里的“简单 TCP”在工程上必须落成浏览器可用的 HTTP 服务；实时状态推送采用同一 TCP 端口上的 SSE（Server-Sent Events），命令用 HTTP POST，避免额外实现 WebSocket。
- 当前配置来源已经明确：
  - 推理配置来自 [main/configs/infer_workflow.yaml]( /g:/Docker_windows_disk/PSIN_SAR_workflow/main/configs/infer_workflow.yaml:1 )
  - RD 配置来自 [main/configs/rd_imaging.yaml]( /g:/Docker_windows_disk/PSIN_SAR_workflow/main/configs/rd_imaging.yaml:1 )
  - 现有 `LoadConfig(...)` 只支持从 YAML 读配置，没有写回 YAML 的能力
- 当前 `workflow::rd::Run(...)` 和 `workflow::infer::Run(...)` 都是阻塞式入口，因此要支持 `start / pause / stop / reset`，必须增加一个新的“控制层 + 后台运行线程”，不能只加一个 HTTP server。

---

## 2. Goal

明确写出要实现的行为。

**必须是可验证的。**

- 在板端新增一个内嵌 Web 控制台模式，通过浏览器访问单端口 HTTP 服务。
- Web 控制台必须提供以下功能：
  - 选择工作流模式：`RD only` / `Inference only`
  - 选择 patch/飞行模式：`auto_snake` / `manual_flight`
  - `auto_snake` 模式下提供 `start / pause / stop / reset`
  - 选择输出模式：`hdmi` / `png`
  - 列出可选输入源并点击加载
  - 提供设置页，展示并编辑当前 YAML 派生出的 RD 参数、推理参数和预留飞行参数
  - 对当前未实现模块，暴露接口和 UI 占位，但返回明确 `not_implemented`
- 浏览器侧与板端的协议固定为：
  - HTTP GET 提供页面和静态资源
  - HTTP GET/POST 提供控制与设置接口
  - SSE 提供实时状态推送
- `main()` 必须新增 `Web Console` 入口，但原有 CLI 的 `RD only` 和 `Inference only` 不能被删掉。
- web控制台设计参考：`main\src\web_control_console_preview.jsx`
---

## 3. Out of scope

明确不做什么。

- 不实现真正的无人机手动飞行逻辑
- 不实现 WASD 对底层飞控或 patch 坐标的真实驱动
- 不在本阶段实现 YAML 写回磁盘；设置页先只修改内存中的“当前有效配置”
- 不引入外部前端框架、Node.js 构建链或独立 Web 服务进程
- 不实现鉴权、TLS、用户登录或多用户并发控制
- 不修改第二阶段 HDMI 线程模型的核心语义
- 不重做 RD / Infer 的算法逻辑，只为 Web 控制加入运行控制与状态上报钩子

---

## 4. Allowed files to modify

```text
main/src/main.cpp
main/src/infer_workflow.cpp
main/src/rd_imaging_stream.cpp
main/src/infer_config.cpp
main/src/rd_config.cpp
main/include/workflow/shared/app_mode.hpp
main/include/workflow/shared/config_utils.hpp
main/include/workflow/infer/infer_workflow.hpp
main/include/workflow/rd/rd_workflow.hpp
main/include/workflow/shared/run_control.hpp
main/include/workflow/web/web_console.hpp
main/include/workflow/web/web_console_config.hpp
main/include/workflow/web/web_console_protocol.hpp
main/include/workflow/web/web_console_controller.hpp
main/include/workflow/web/web_console_server.hpp
main/src/web_console.cpp
main/src/web_console_config.cpp
main/src/web_console_protocol.cpp
main/src/web_console_controller.cpp
main/src/web_console_server.cpp
main/src/web_console_assets.cpp
main/configs/web_console.yaml
task/TASK_PHASE3.md
```

---

## 5. Files/modules to avoid

```text
deps/**
main/include/infer_workflow_hdmi_display.hpp
ARCHITECTURE_TEMPLATE.md
CODEBASE_MAP_TEMPLATE.md
task/TASK_PHASE1.md
task/TASK_PHASE2.md
task/TASK_PHASE2_FIX1.md
```

---

## 6. Chosen implementation direction

这项决策已经确定，不再二选一：

- 浏览器兼容协议采用“单端口 HTTP + JSON + SSE”，不采用裸 TCP 自定义协议，不采用 WebSocket。
- Web 控制台作为新的运行模式接入 `main()`：
  - `1. RD only`
  - `2. Inference only`
  - `3. Web Console`
  - `0. Exit`
- Web 控制台本身采用“板端嵌入式单进程服务”，不启动独立 Node/Python 服务。
- 前端页面采用单页应用，但不引入 React/Vue：
  - HTML / CSS / JS 作为嵌入式静态资源
  - 由 `web_console_assets.cpp` 以内存字符串形式提供
  - 由 `web_console_server.cpp` 直接返回
- 第三阶段的板端职责拆成 4 层：
  - `WebConsoleServer`
    - 负责 socket accept、HTTP 解析、SSE 推送、静态资源返回、API 路由
  - `WebConsoleController`
    - 负责当前 UI 选择状态、工作流后台线程、命令分发、状态机
  - `WebConsoleProtocol`
    - 负责固定 JSON DTO、命令类型、状态快照、错误码、SSE event payload
  - `WorkflowRunControl`
    - 负责把 `pause / stop / reset / status publish` 钩子安全地传入 RD / Infer 运行流程
- Web 端与工作流端不直接互相调用算法代码，所有控制都经 `WebConsoleController` 中转。

---

## 7. File design

新建文件数量固定如下。

### 公开入口与配置

- `main/include/workflow/web/web_console.hpp`
  - `int Run(const std::filesystem::path& config_path);`
- `main/include/workflow/web/web_console_config.hpp`
- `main/src/web_console.cpp`
- `main/src/web_console_config.cpp`
- `main/configs/web_console.yaml`

### Web 控制台协议与控制器

- `main/include/workflow/web/web_console_protocol.hpp`
- `main/include/workflow/web/web_console_controller.hpp`
- `main/include/workflow/web/web_console_server.hpp`
- `main/src/web_console_protocol.cpp`
- `main/src/web_console_controller.cpp`
- `main/src/web_console_server.cpp`
- `main/src/web_console_assets.cpp`

### 共享运行控制

- `main/include/workflow/shared/run_control.hpp`

### 需要接入但不新建模块的文件

- `main/src/main.cpp`
- `main/include/workflow/shared/app_mode.hpp`
- `main/include/workflow/infer/infer_workflow.hpp`
- `main/include/workflow/rd/rd_workflow.hpp`
- `main/src/infer_workflow.cpp`
- `main/src/rd_imaging_stream.cpp`

---

## 8. Public interfaces and types

### 8.1 新增 AppMode

```cpp
namespace workflow {
    enum class AppMode {
        RdOnly = 1,
        InferOnly = 2,
        WebConsole = 3,
        Exit = 0,
    };
}
```

### 8.2 新增 WebConsoleConfig

`web_console.yaml` 只负责 Web 服务本身，不承载 RD/Infer 算法参数。

建议字段：

```text
server.bind = 0.0.0.0
server.port = 8080
server.sse_heartbeat_ms = 1000
ui.title = PSIN SAR Web Console
config.infer_path = configs/infer_workflow.yaml
config.rd_path = configs/rd_imaging.yaml
```

### 8.3 新增共享运行控制对象

```cpp
namespace workflow::shared {
    enum class ControlState {
        Idle,
        Starting,
        Running,
        Paused,
        Stopping,
        Finished,
        Error,
    };

    enum class SelectedWorkflow {
        RdOnly,
        InferOnly,
    };

    enum class SelectedPatchMode {
        AutoSnake,
        ManualFlight,
    };

    struct WorkflowSelection {
        SelectedWorkflow workflow;
        SelectedPatchMode patch_mode;
        std::string output_mode;   // hdmi / png
        std::string selected_source;
    };

    struct WorkflowRuntimeSnapshot {
        ControlState state;
        WorkflowSelection selection;
        std::string current_stage;
        std::string current_item;
        std::string last_error;
        int current_index;
        int total_count;
        double infer_ms;
        double total_ms;
        double fps;
    };

    class WorkflowRunControl {
    public:
        void requestPause();
        void requestResume();
        void requestStop();
        void requestReset();
        void waitIfPaused();
        bool shouldStop() const;
        void publish(const WorkflowRuntimeSnapshot& snapshot);
    };
}
```

约束：

- `pause / stop` 必须是协作式，不允许用线程强杀
- `reset` 是控制器级命令，不直接从外部线程破坏运行中的对象
- `publish(...)` 只能发布快照，不返回对内部状态的可写引用

### 8.4 新增运行入口重载

保留现有 `Run(config_path)` 兼容 CLI，同时新增供 Web 控制台调用的重载：

```cpp
namespace workflow::infer {
    int Run(const AppConfig& cfg,
            shared::WorkflowRunControl* control = nullptr);
}

namespace workflow::rd {
    int Run(const AppConfig& cfg,
            shared::WorkflowRunControl* control = nullptr);
}
```

要求：

- 原来的 `Run(config_path)` 变成兼容包装器：`LoadConfig(path)` 后调用新重载
- Web 控制台只调用 `AppConfig` 重载，不再通过临时 YAML 文件启动任务

---

## 9. Controller state machine

`WebConsoleController` 的状态机固定如下：

- `Idle`
  - 可修改模式、输出、底图、设置
  - 可执行 `start`
- `Starting`
  - 创建后台工作线程
  - 加载当前生效配置
  - 进入 `Running` 或 `Error`
- `Running`
  - `start` 按“resume”语义处理
  - 允许 `pause`
  - 允许 `stop`
- `Paused`
  - 允许 `start`（作为 resume）
  - 允许 `stop`
  - 不允许再次 `pause`
- `Stopping`
  - 等待后台线程协作退出
  - 退出后进入 `Idle`
- `Finished`
  - 允许 `reset`
  - 允许重新 `start`
- `Error`
  - 允许 `reset`
  - 允许重新 `start`

`reset` 的语义固定为：

- 若当前不在运行中：清空运行状态、恢复 controller 内部 UI 选择到最近一次应用成功的配置快照
- 若当前在运行中：先等价执行 `stop`，后台线程完全退出后再执行 reset

---

## 10. Workflow control semantics

### 10.1 Inference only

- `auto_snake` 模式下：
  - `pause` 在 patch 边界生效
  - `stop` 在 patch 边界生效
  - 每处理完一个 patch 发布一次 `WorkflowRuntimeSnapshot`
- `manual_flight` 模式下：
  - 接口、按钮和 WASD 事件通道全部存在
  - 后端统一返回 `not_implemented`
  - 不进入真实飞行或 patch 驱动逻辑

### 10.2 RD only

- `pause` / `stop` 采用协作式安全点：
  - 文件级边界
  - tile 级边界（如 `column_tile` / `row_tile` 主循环）
- 每处理完一个 echo 文件、每完成一个主要阶段，发布一次状态快照
- RD 不引入 HDMI UI 线程，Web 控制台只显示文字状态、进度和日志摘要

---

## 11. Web API design

固定接口如下。

### 页面与静态资源

- `GET /`
- `GET /app.js`
- `GET /app.css`

### 状态与事件

- `GET /api/state`
  - 返回当前控制台完整状态 JSON
- `GET /events`
  - SSE 长连接
  - 事件类型至少包含：
    - `state`
    - `log`
    - `error`

### 模式与控制

- `POST /api/selection`
  - 设置：
    - workflow mode
    - patch mode
    - output mode
    - selected source
- `POST /api/command/start`
- `POST /api/command/pause`
- `POST /api/command/stop`
- `POST /api/command/reset`

约束：

- `start` 在 `Paused` 状态下按 `resume` 处理
- `pause` 只对 `auto_snake` 有效
- `manual_flight` 下若收到 `pause / start / wasd`，返回 `not_implemented`

### 输入源与预览

- `GET /api/sources?workflow=infer`
  - 列出当前 `input.sar_img_dir` 下可选 SAR 图片
- `GET /api/sources?workflow=rd`
  - 列出当前 `rd.echo_dir` 下可选 echo 文件
- `GET /api/source/preview?id=...`
  - 仅对 inference 的图片源有效
  - RD 源无预览时返回明确提示，不返回 500

### 设置页

- `GET /api/settings`
  - 返回当前内存中的工作配置 DTO
- `POST /api/settings`
  - 更新当前内存中的设置 DTO
  - 不写回 YAML

### WASD 占位接口

- `POST /api/manual/key`
  - body: `key = w/a/s/d`, `action = down/up`
  - 当前阶段统一返回 `not_implemented`

---

## 12. Frontend behavior

前端页面固定为单页布局，最少包括 6 个区块：

- 模式选择区
  - `RD only` / `Inference only`
  - `auto_snake` / `manual_flight`
- 运行控制区
  - `start`
  - `pause`
  - `stop`
  - `reset`
- 输出模式区
  - `hdmi`
  - `png`
- 输入源区
  - 左侧列表
  - 右侧加载按钮
  - inference 模式下显示图片预览
- 设置页
  - 推理参数
  - RD 参数
  - 飞行参数占位
- 状态区
  - 当前状态
  - 当前模式
  - 当前文件/patch
  - 关键耗时
  - 最近错误
  - 简化日志流

前端技术决策：

- 使用原生 HTML/CSS/JS
- 不引入构建工具
- 页面首次加载通过 HTTP GET 获取
- 状态实时更新靠 SSE
- 所有按钮命令通过 `fetch POST` 发起

---

## 13. Settings mapping

设置页字段来源固定为当前 YAML 已有字段，不自行发明新参数。

### Inference settings

来源于 `infer_workflow.yaml` / `workflow::infer::AppConfig`

- `sys.device`
- `sys.run_backend`
- `sys.mmuMode`
- `sys.speedMode`
- `sys.compressFtmp`
- `sys.ocm_option`
- `sys.profile`
- `input.sar_img_dir`
- `input.sar_img_ext`
- `input.recursive`
- `pipeline.patch.mode`
- `pipeline.patch.patch_size`
- `pipeline.patch.stride`
- `pipeline.output_wait_ms`
- `display.width`
- `display.height`
- `display.fps`
- `output.mode`
- `output.dir`
- `output.overwrite`

### RD settings

来源于 `rd_imaging.yaml` / `workflow::rd::AppConfig`

- `rd.execution_mode`
- `rd.echo_dir`
- `rd.echo_ext`
- `rd.output_dir`
- `rd.scratch_dir`
- `rd.column_tile`
- `rd.row_tile`
- `rd.memory_limit_mb`
- `rd.prefer_memory_pipeline`
- `rd.keep_scratch`
- `rd.overwrite`

### Flight settings

- 当前仅提供占位 DTO 和前端表单
- 后端保存到内存，但不接入实际飞行逻辑
- 响应统一标记为 `reserved_for_future`

---

## 14. Validation

明确要求运行什么验证：

```text
1. g++ -fsyntax-only 覆盖新增 web_console*.cpp / *.hpp 以及被改动的 main.cpp / infer_workflow.cpp / rd_imaging_stream.cpp
2. 静态审查 main():
   - 确认新增 WebConsole 模式
   - 确认原有 RD only / Inference only CLI 保留
3. HTTP 基本验证:
   - 浏览器访问 / 返回 index 页面
   - /app.js 和 /app.css 能正确返回
4. API 验证:
   - /api/state 返回有效 JSON
   - /api/settings GET/POST 工作正常
   - /api/selection 工作正常
   - /api/command/start|pause|stop|reset 行为符合状态机
5. SSE 验证:
   - /events 能持续接收 state 更新
   - 后台任务运行时能看到状态变化
6. Workflow 验证:
   - Inference auto_snake: start / pause / stop / reset 生效
   - RD only: start / pause / stop / reset 在文件/ tile 安全点生效
   - manual_flight: UI 和接口存在，但返回 not_implemented
7. Source 验证:
   - inference 模式能列出图片并加载预览
   - rd 模式能列出 echo 文件
8. Regression:
   - 直接命令行模式 1 和 2 仍可运行
```

---

## 15. Required response format before editing

要求 Codex 在动手前先输出：

1. 对任务的理解
2. 计划修改的文件
3. 不会修改的文件
4. 实现方案
5. 风险点
6. 验证方案

---

## 16. Required response format after editing

要求 Codex 改完后输出：

1. 修改了哪些文件
2. 每个文件做了什么改动
3. 为什么这样设计
4. 跑了哪些命令
5. 哪些验证通过 / 未通过
6. 剩余风险

---

## 17. Done when

写成客观验收标准：

- `main()` 已新增 `Web Console` 模式入口
- 浏览器可通过单端口 HTTP 打开控制台页面
- 控制台能完成模式选择、输出选择、输入源选择和设置编辑
- `Inference only + auto_snake` 下的 `start / pause / stop / reset` 已可用
- `RD only` 下的 `start / pause / stop / reset` 已通过协作式安全点可用
- `manual_flight` 与 WASD 接口已存在且明确返回 `not_implemented`
- 前端实时状态更新通过 SSE 正常工作
- 原有 CLI 模式未被破坏
- diff 可 review，且 Web 控制台相关代码集中在新建文件中，只有入口接线和工作流控制钩子落到现有文件

---

## 18. md update
检查下面的md文件，若有代码仓库的更新内容与下面的md文件内容有关，伴随更新
- ARCHITECTURE_TEMPLATE.md
- CODEBASE_MAP_TEMPLATE.md
