# Task title

Phase 3 Fix3：新增 Web Console 安全退出按钮，并移除 manual_flight 常驻提示条

---

## 1. Background

这个任务为什么存在？

- 当前 Web Console 已经具备：
  - `Start`
  - `Pause`
  - `Stop`
  - `Reset`
- 但当前“退出 Web Console”仍然依赖板子本地终端侧退出路径，例如在板端按 `Ctrl+C`，浏览器端没有直接、安全、语义明确的退出入口。
- 由于这个系统的实际使用方式是：
  - 板子作为服务端
  - 电脑浏览器通过网线直连访问板子上的 Web Console
  - 板子负责实际执行 RD / Infer 工作流
  所以从浏览器端提供“关闭 Web 控制台服务”的能力是合理的。
- 同时，当前页面左侧控制区还有一条常驻黄条提示：
  - `manual_flight endpoints are visible but reserved for a future phase.`
  这条提示现在长期占空间、信息价值较低，而且 `manual_flight` 未实现这一事实已经体现在接口返回中，不需要再用一条常驻提示重复强调。
- 当前真实落点已经明确：
  - 前端黄条提示在 [web_console_assets.cpp](G:/Docker_windows_disk/PSIN_SAR_workflow/main/src/web_console_assets.cpp)
  - 命令 API 当前只有 [web_console_server.cpp](G:/Docker_windows_disk/PSIN_SAR_workflow/main/src/web_console_server.cpp) 中的：
    - `POST /api/command/start`
    - `POST /api/command/pause`
    - `POST /api/command/stop`
    - `POST /api/command/reset`

---

## 2. Goal

明确写出要实现的行为。

**必须是可验证的。**

- 在 Web Console 页面增加一个明确命名的安全退出按钮：
  - 名称使用 `Shutdown Web Console`
  - 不使用模糊的 `Exit`
- 该按钮不放在 `Run Control` 区域内部，而是放在页面右上角的全局操作位置。
- 该按钮视觉形态固定为：
  - 黄色圆形按钮
  - 以图标化方式表达关闭/退出 Web Console
  - 保持醒目，但不要做成红色危险主按钮
- 点击该按钮后，浏览器端先进行一次确认。
- 用户确认后，板子端执行“安全关闭 Web Console”的完整流程：
  - 若当前 workflow 正在运行，先请求停止
  - 等待 worker 安全退出
  - 持久化当前配置
  - 退出 Web Console 服务
- Web 服务关闭后，浏览器页面连接断开是预期行为，前端应给出简短提示，而不是静默卡死。
- 页面中当前那条 manual_flight 的常驻黄色提示条直接移除。

---

## 3. Out of scope

明确不做什么。

- 不新增“退出整个板上程序”的独立按钮
- 不把 `Stop` 改造成“顺带退出 Web 服务”
- 不实现 `manual_flight` 的真实飞行控制逻辑
- 不删除 `manual_flight` 的预留接口和按钮
- 不修改 RD / Infer 算法主链
- 不改 Web Console 的整体布局风格，只移除这条提示并补一个退出按钮
- 不引入鉴权、用户权限或多用户控制

---

## 4. Allowed files to modify

只列允许改的文件。

```text
main/src/web_console.cpp
main/src/web_console_server.cpp
main/src/web_console_controller.cpp
main/src/web_console_assets.cpp
main/include/workflow/web/web_console_server.hpp
main/include/workflow/web/web_console_controller.hpp
main/include/workflow/web/web_console.hpp
task/TASK_PHASE3_FIX3.md
```

---

## 5. Files/modules to avoid

写清楚不许动的范围。

```text
main/src/infer_workflow.cpp
main/src/rd_imaging_stream.cpp
main/src/infer_config.cpp
main/src/rd_config.cpp
main/src/web_console_protocol.cpp
main/include/workflow/shared/run_control.hpp
deps/**
ARCHITECTURE_TEMPLATE.md
CODEBASE_MAP_TEMPLATE.md
task/TASK_PHASE3.md
task/TASK_PHASE3_FIX1.md
task/TASK_PHASE3_FIX2.md
```

---

## 6. Chosen implementation direction

这项决策已经确定，不再二选一：

- 新按钮命名固定为：
  - `Shutdown Web Console`
- 语义固定为：
  - 关闭板子上的 Web 控制台服务
  - 不是只停 workflow
  - 也不是退出整个板上程序的通用入口
- 按钮位置固定为：
  - 页面右上角
  - 属于全局控制入口，不归类到 `Run Control` 局部按钮组
- 按钮视觉固定为：
  - 黄色圆形按钮
  - 可以使用电源/退出类图标
  - hover/active 状态要清楚，但不能喧宾夺主压过主流程按钮
- 协议层新增一个专用命令接口，推荐命名：
  - `POST /api/command/shutdown_web`
- 前端点击后必须先二次确认。
- 后端执行顺序固定为：
  1. 将控制状态切入“准备退出 Web Console”
  2. 若 workflow 还在运行，先请求 stop
  3. 等待 worker 安全退出
  4. 完成已有配置持久化逻辑
  5. 停止 Web server 主循环
  6. 结束 `workflow::web::Run(...)`
- 当前黄条提示直接移除，不替换成新的常驻块。
- `manual_flight` 的未实现状态继续通过现有命令返回 `not_implemented` 表达，而不是靠 UI 大块提示表达。

---

## 7. Functional requirements

列成可验收条目：

- [ ] 页面右上角新增 `Shutdown Web Console` 按钮
- [ ] 该按钮为黄色圆形全局操作按钮
- [ ] 点击该按钮时，浏览器先弹出确认框
- [ ] 用户取消确认时，不发送退出请求
- [ ] 用户确认后，前端请求 `POST /api/command/shutdown_web`
- [ ] 若 workflow 正在运行，后端先安全停止当前 workflow
- [ ] 若 workflow 未运行，后端直接进入关闭服务流程
- [ ] 关闭前继续沿用现有配置持久化逻辑
- [ ] Web 服务退出后，浏览器端能感知连接中断，并显示明确提示
- [ ] 页面中移除 `manual_flight endpoints are visible but reserved for a future phase.` 这条常驻提示
- [ ] `manual_flight` 的按钮和保留接口仍保留
- [ ] `manual_flight` 相关命令仍返回 `not_implemented`

---

## 8. Non-functional requirements

- [ ] 保持最小范围修改
- [ ] 不引入新依赖
- [ ] 不改动 RD / Infer 线程模型
- [ ] 不把“停任务”和“停 Web 服务”混成一个按钮
- [ ] 命名必须清晰，避免使用模糊 `Exit`
- [ ] 退出路径必须 review 友好，顺序清晰
- [ ] 前端连接断开提示必须简洁，不做复杂重连逻辑
- [ ] 按钮位置和形态必须稳定，不退化成普通矩形命令按钮

---

## 9. Interface expectations

给出希望的接口草案：

```cpp
class WebConsoleServer {
public:
    void Run();
    void Stop();
    void RequestShutdownFromApi();
};

class WebConsoleController {
public:
    std::string commandStop();
    std::string commandReset();
    std::string commandShutdownWeb();
};
```

协议建议：

```text
POST /api/command/shutdown_web
```

前端按钮行为建议：

```text
1. 点击页面右上角黄色圆形按钮
2. confirm("这会关闭板子上的 Web 控制台服务，当前页面将断开连接，并保存当前设置。是否继续？")
3. 确认后 POST /api/command/shutdown_web
4. 显示 "Web Console is shutting down..."
5. 等待 SSE / HTTP 连接断开
```

说明：

- 具体是让 `WebConsoleServer` 持有关闭标志，还是让 `web_console.cpp` 外层 run loop 感知“API 触发的 shutdown 请求”，实现方式可以调整。
- 但不允许把“API 收到 shutdown 后直接 `std::exit(...)`”作为实现。

---

## 10. File-level design

### `main/src/web_console_assets.cpp`

- 在页面右上角增加 `Shutdown Web Console` 按钮
- 按钮样式固定为黄色圆形图标按钮
- 点击时弹出二次确认
- 确认后调用新的 shutdown API
- 移除当前 `manual-note` 黄条及其相关 DOM
- 增加连接关闭时的简短提示逻辑

### `main/src/web_console_server.cpp`

- 增加新的 API 路由：
  - `POST /api/command/shutdown_web`
- 该接口不直接粗暴退出进程
- 需要把“请求关闭 Web Console”传递到外层关闭流程

### `main/src/web_console_controller.cpp`

- 增加 `commandShutdownWeb()` 或等价控制入口
- 负责完成：
  - stop 运行中的 workflow
  - 等待 worker 退出
  - 返回明确响应
- 不负责直接做 socket 层关闭

### `main/src/web_console.cpp`

- 需要让 `workflow::web::Run(...)` 支持“来自 Web API 的关闭请求”
- 继续复用现有退出路径中的：
  - `server->Stop()`
  - `controller->RequestWorkerStop()`
  - `controller->JoinWorker()`
  - 配置持久化

### `main/include/workflow/web/web_console_server.hpp`

- 增加最小必要的关闭请求接口或状态访问接口

### `main/include/workflow/web/web_console_controller.hpp`

- 增加最小必要的 shutdown 命令声明

---

## 11. Edge cases

要求 Codex 必须考虑这些：

- 用户点击 `Shutdown Web Console` 时，workflow 正处于 `Running`
- 用户点击 `Shutdown Web Console` 时，workflow 正处于 `Paused`
- 用户点击 `Shutdown Web Console` 时，workflow 已经是 `Stopping`
- 用户连续多次点击 shutdown 按钮
- 前端已经断开 SSE，但又点击 shutdown
- shutdown 请求发出后，浏览器比服务端更早断开连接
- 配置持久化失败时如何向终端日志报告
- 页面在服务即将关闭时，是否还能收到最后一条 log/state 事件

---

## 12. Validation

明确要求运行什么验证：

```text
1. g++ -fsyntax-only 覆盖：
   - main/src/web_console.cpp
   - main/src/web_console_server.cpp
   - main/src/web_console_controller.cpp
   - main/src/web_console_assets.cpp

2. 静态检查：
   - 确认新增 API 只有 shutdown_web，不复用 stop/reset 语义
   - 确认 manual-note 已移除
   - 确认现有 manual_flight not_implemented 返回仍保留

3. 板端/运行验证：
   - 启动 Web Console
   - 页面右上角能看到黄色圆形 `Shutdown Web Console` 按钮
   - 点击后先出现确认框
   - 取消时服务不退出
   - 确认后，若当前无任务运行，Web 服务正常关闭
   - 确认后，若当前任务在运行，先安全停任务，再关闭服务
   - 关闭后浏览器连接断开
   - 重启程序后，Web Console 仍可正常启动

4. UI 验证：
   - 左侧不再出现黄色 manual_flight 常驻提示条
   - 页面右上角黄色圆形 shutdown 按钮位置稳定
   - 新按钮不会挤压 `Run Control` 区域布局
```

---

## 13. Required response format before editing

要求 Codex 在动手前先输出：

1. 对任务的理解
2. 计划修改的文件
3. 不会修改的文件
4. 实现方案
5. 风险点
6. 验证方案

---

## 14. Required response format after editing

要求 Codex 改完后输出：

1. 修改了哪些文件
2. 每个文件做了什么改动
3. 为什么这样设计
4. 跑了哪些命令
5. 哪些验证通过 / 未通过
6. 剩余风险

---

## 15. Done when

写成客观验收标准：

- 页面右上角有黄色圆形 `Shutdown Web Console` 按钮
- 点击时先确认，再真正发起关闭请求
- 关闭请求会安全停止当前 worker 并复用现有退出持久化流程
- Web 服务能从浏览器端触发关闭
- 浏览器端能感知连接断开
- 左侧黄色 manual_flight 常驻提示条已经移除
- diff 范围只围绕 Web Console 按钮、退出控制与这条提示移除


## 16. md update

需要你随着代码更新而更新的文件：

- ARCHITECTURE_TEMPLATE.md
- CODEBASE_MAP_TEMPLATE.md
