# BUGFIX_1

> 本文档依据 `BUGFIX_TEMPLATE.md` 生成。  
> 当前阶段只做静态排查与修复方案整理，不修改 C++ 代码。

---

## 0. Web 控制台当前实现梳理

### 0.1 Web 控制台做了什么

- 菜单入口在 `main/src/main.cpp:56`，选择 `3` 会进入 `workflow::web::Run("configs/web_console.yaml")`。
- `workflow::web::Run(...)` 在 `main/src/web_console.cpp:29` 中完成：
  - 读取 `web_console.yaml`
  - 进一步加载 `infer_workflow.yaml` 和 `rd_imaging.yaml`
  - 创建 `WebConsoleController`
  - 创建 `WebConsoleServer`
  - 启动独立 `web_thread` 跑 `server->Run()`
- `WebConsoleController` 在 `main/src/web_console_controller.cpp:99` 中持有：
  - 当前 UI 选择
  - 当前内存配置快照
  - `WorkflowRunControl`
  - 唯一后台 `worker_` 线程
- `WebConsoleServer` 在 `main/src/web_console_server.cpp:225` 中负责：
  - HTTP 路由
  - `/events` SSE 长连接
  - `/api/source/preview` 图片预览
  - 所有 socket 写出
- 前端页面由 `main/src/web_console_assets.cpp` 以内嵌 HTML/CSS/JS 提供：
  - `renderSources()` 在点击图片时先 `pushSelection()`，再 `renderPreview()`，见 `main/src/web_console_assets.cpp:268`
  - `renderPreview()` 直接设置 `img.src=/api/source/preview?...`，见 `main/src/web_console_assets.cpp:285`
  - `reloadSources()` 首次加载后会自动选中首图并立即触发预览，见 `main/src/web_console_assets.cpp:377`
  - `connectEvents()` 通过 `EventSource("/events")` 接收 `state/log/error`，见 `main/src/web_console_assets.cpp:388`
- 推理工作流在 `main/src/infer_workflow.cpp:2026` 中运行，并在多个阶段调用 `publishSnapshot(...)`：
  - `load config`
  - `collect SAR images`
  - `load network`
  - `sar loaded`
  - `patch processed`
- RD 工作流在 `main/src/rd_imaging_stream.cpp:1294` 中运行，也通过 `WorkflowRunControl` 发布快照。

### 0.2 与 TASK_PHASE3 / TASK_PHASE3_FIX1 的对应关系

- `TASK_PHASE3` 要求 Web 控制台提供：
  - 模式选择
  - patch 模式选择
  - 输出模式选择
  - 输入源加载
  - 设置页
  - `start/pause/stop/reset`
  - SSE 实时状态
- `TASK_PHASE3_FIX1` 进一步收紧了线程边界：
  - 主线程只负责生命周期
  - Web 线程只负责 HTTP/SSE
  - 后台 worker 线程只负责 RD/Infer
  - HDMI 线程只负责显示
- 当前代码在结构上基本落到了这个设计，但稳定性和交互边界仍有明显缺口，尤其是：
  - socket 写路径的进程级安全性
  - 预览请求的取消/重发行为
  - `Paused` 状态下的 selection 政策
  - 前端状态栏布局策略

---

## 1. Bug title

- Web Console 稳定性问题集合：切图导致退出、重复运行后在“load network”阶段退出、特定图片预览触发退出、Live Status 布局错位、频繁点击后直接退出。

---

## 2. Symptom

- Bug 1：`pause` 状态下无法切图；执行切图操作时，进程可能直接退出；未运行前切图也可能直接退出。
- Bug 2：多次重复运行后再次点击 `Start`，会在“加载模型”阶段突然退出。
- Bug 3：刚进入 Web Console，图片 1 和 2 可正常选择，但直接选择图片 3 可能触发退出。
- Bug 4：默认选项下直接启动 `HDMI + inference + 1号图片`，有时日志停在：
  - `[info] npu: 0x40000000`
  - `[info] [stage] load network json/raw`
  - 随后整个程序直接退出。
- Bug 5：右侧 `Live Status` 的留白和实际字符串长度不匹配，键值位置不对齐。
- Bug 6：频繁点击 Web 控制台页面或重复打开/交互后，程序会直接退出。

补充观察：

- 终端直接走模式 `2. Inference only` 基本正常，问题集中出现在 Web 控制台路径。
- 因此本组问题优先怀疑 Web 层交互、网络写出、SSE/预览链路，而不是核心推理算法本身。

---

## 3. Minimal reproduction

### 3.1 Bug 1：`pause` 状态切图

- 输入条件：`infer.input.sar_img_dir` 下至少有 3 张可预览图片。
- 触发步骤：
  1. 进入 Web Console。
  2. 点击 `Start`。
  3. 点击 `Pause`。
  4. 在 `Input Source Loader` 中点击另一张图片。
- 预期行为：
  - 要么允许切换并把新的 `selected_source` 作为下次运行输入。
  - 要么明确拒绝，但必须稳定返回错误，不能退出进程。
- 实际行为：
  - 当前实现会先在控制器层拒绝 selection 更新；
  - 同时前端仍会继续触发预览；
  - 用户可观察到进程直接退出。

### 3.2 Bug 2：重复运行后再次 Start

- 输入条件：模型配置正确，板端设备可正常打开。
- 触发步骤：
  1. Web Console 中执行多轮 `Start -> Stop`，或 `Start -> 跑完 -> Start`。
  2. 再次点击 `Start`。
- 预期行为：重新开始推理。
- 实际行为：有时在“load network”阶段前后直接退出。

### 3.3 Bug 3：启动后直接切到图片 3

- 输入条件：启动后 source 列表中至少有图片 1、2、3。
- 触发步骤：
  1. 进入 Web Console。
  2. 页面完成初始加载。
  3. 不启动推理，直接点击图片 3。
- 预期行为：切换选中项并刷新预览。
- 实际行为：有时整个程序直接退出。

### 3.4 Bug 4：默认 HDMI 推理时在 load network 阶段退出

- 输入条件：默认设置，输出模式为 `hdmi`，选择 1 号图片。
- 触发步骤：
  1. 进入 Web Console。
  2. 直接点击 `Start`。
- 预期行为：进入 `load network -> init session -> session apply -> patch loop`。
- 实际行为：日志停在 `load network json/raw` 附近即退出。

### 3.5 Bug 5：Live Status 布局错位

- 输入条件：Web Console 正常打开。
- 触发步骤：
  1. 打开页面。
  2. 观察右侧 `Live Status`。
  3. 当 `CURRENT_ITEM`、`LAST_ERROR` 等值变长时更明显。
- 预期行为：键列和值列对齐稳定。
- 实际行为：值列宽度随内容跳动，留白与文本长度不匹配。

### 3.6 Bug 6：频繁点击后退出

- 输入条件：Web Console 已打开，浏览器会持续建立 `/events` 和图片预览请求。
- 触发步骤：
  1. 频繁点击 source、模式按钮、控制按钮，或刷新页面。
  2. 让浏览器频繁取消旧请求、建立新请求。
- 预期行为：最多返回错误或丢弃旧连接，不应退出进程。
- 实际行为：程序直接退出。

---

## 4. Suspected root causes

以下按“概率 + 解释力”排序。

### 4.1 主根因：Web socket 写路径未处理 `SIGPIPE`

证据：

- `main/src/web_console_server.cpp:38` 的 `sendAll()` 直接调用：
  - `::send(fd, cursor, remaining, 0);`
- 代码中没有任何：
  - `SIGPIPE` 忽略
  - `MSG_NOSIGNAL`
  - `SO_NOSIGPIPE`
  - `sigaction(SIGPIPE, ...)`
- 也就是说，只要浏览器取消请求、关闭 SSE、刷新页面、替换图片预览，服务端仍向旧 socket 写数据，就可能收到 `SIGPIPE`，整进程被系统打死。

它能解释的现象：

- Bug 6：频繁点击/刷新后退出。
- Bug 3：启动后直接点图片 3 时退出。
  - 页面首次加载后，`reloadSources()` 会自动设置首图并触发 `renderPreview()`，见 `main/src/web_console_assets.cpp:377`。
  - 用户快速切到图片 3 时，浏览器会取消上一张预览请求，再发新的 `/api/source/preview`。
  - 旧请求若仍在服务端写响应，最容易触发 `SIGPIPE`。
- Bug 1：切图时退出。
  - 即使 selection 被 controller 拒绝，前端仍会继续 `renderPreview()`，见 `main/src/web_console_assets.cpp:274` 和 `main/src/web_console_assets.cpp:285`。
  - 这会制造额外的预览取消/重发。
- Bug 2 / Bug 4：看起来像“load network”阶段退出。
  - 实际上 `infer_workflow` 在真正 `loadNetwork(...)` 之前，已经先调用了一次 `publishSnapshot(..., "load network", ...)`，见 `main/src/infer_workflow.cpp:2058` 到 `main/src/infer_workflow.cpp:2060`。
  - 如果这次状态更新通过 SSE 发给已经断开的浏览器连接，进程会死在“发布 load network 状态”这一刻，外部观感就像“模型加载阶段突然退出”。

结论：

- 这是当前最强的统一解释。
- 它比“模型加载本身不稳定”更能解释为什么纯 CLI 模式 2 正常，而 Web 模式异常。

### 4.2 次根因：`Paused` 被视为运行态，selection 被控制器硬拒绝

证据：

- `main/src/web_console_controller.cpp:214` 的 `applySelection()` 一进入就判断：
  - `if (isRunningState(runtime_snapshot_.state)) return invalid_state`
- `main/src/web_console_controller.cpp:799` 的 `isRunningState(...)` 把以下状态都算作运行态：
  - `Starting`
  - `Running`
  - `Paused`
  - `Stopping`

它能解释的现象：

- Bug 1 中“pause 状态下无法切图”是当前实现的确定行为，不是偶发。
- 这与用户预期冲突，因为 `Paused` 通常被理解为“允许调整下一步输入，但不继续执行”。

结论：

- Bug 1 不是单纯的 crash 问题，还包含明确的产品/状态机策略错误。
- 即使修掉崩溃，`Paused` 下仍会因为 selection 被拒而表现为“无法切图”。

### 4.3 放大因素：前端切图逻辑会在 selection 失败后继续刷新预览

证据：

- `main/src/web_console_assets.cpp:274` 到 `main/src/web_console_assets.cpp:278`：
  - 点击 source 后先 `await pushSelection()`
  - 然后无条件 `renderSources(); renderPreview();`
- `pushSelection()` 只打印日志，不根据 `response.ok` 阻断后续流程，见 `main/src/web_console_assets.cpp:355` 到 `main/src/web_console_assets.cpp:363`。

它能解释的现象：

- Bug 1 中，selection 明明被 controller 拒绝，但浏览器仍会发新的图片预览请求。
- 这把“功能不允许”和“网络写崩溃”耦合到一起，造成用户感知更糟。

结论：

- 这是 Bug 1 的直接放大器。
- 它不一定单独导致退出，但会显著提高触发主根因 4.1 的概率。

### 4.4 次级候选：预览接口整文件读入再整体返回，容易放大取消请求窗口

证据：

- `/api/source/preview` 在 `main/src/web_console_server.cpp:398` 进入。
- 命中后会调用 `readFileBinary(...)`，见 `main/src/web_console_server.cpp:418`。
- `readFileBinary(...)` 在 `main/src/web_console_server.cpp:168` 中直接把整个文件读进 `std::string`。

它能解释的现象：

- 图片越大，服务端准备和发送响应的窗口越长。
- 浏览器越容易在这段时间内取消旧请求，从而更容易撞上 4.1 的 `SIGPIPE`。
- 这也能解释为什么“图片 3”可能比 1、2 更容易出问题，如果它尺寸更大或读盘更慢。

结论：

- 这更像 4.1 的触发增强器，而不是单独主根因。

### 4.5 次级候选：`infer_workflow` 安装了进程级 `SIGSEGV` 处理并直接 `_Exit(139)`

证据：

- `main/src/infer_workflow.cpp:2034`：
  - `std::signal(SIGSEGV, handleSegfault);`
- `handleSegfault` 在 `main/src/infer_workflow.cpp:65` 附近最终调用：
  - `std::_Exit(139);`

它能解释的现象：

- 如果设备驱动、网络加载、session 初始化中真的触发了底层段错误，Web 控制台完全没有机会通过 controller 或 SSE 汇报错误，进程会立即退出。

但它不能单独解释：

- 未启动推理前仅切图就退出。
- 频繁点击页面按钮就退出。

结论：

- 这是“推理期突然退出”的重要兜底风险，但不是解释本组 6 个问题的第一主因。

### 4.6 明确的 UI 根因：Live Status 使用弹性网格，未固定键列和值列策略

证据：

- `main/src/web_console_assets.cpp:161`：
  - `.kv-grid{display:grid;grid-template-columns:1fr auto;...}`
- 状态值节点只做了：
  - `value.style.textAlign="right";`
  - 见 `main/src/web_console_assets.cpp:260`
- 没有：
  - 固定 label 列宽
  - `minmax(...)`
  - 长文本截断
  - 溢出换行策略

它能解释的现象：

- Bug 5 的“留白空间和实际字符串长度不匹配，位置也不匹配”是当前 CSS 策略的直接结果。

结论：

- 这是一个确定性前端布局问题，不需要设备复现就能确认。

### 4.7 低优先级候选：preview 路径缺少“必须属于当前 source 列表”的约束

证据：

- `main/src/web_console_controller.cpp:203` 的 `resolveInferPreviewPath()` 只检查：
  - 文件存在
  - 扩展名是图片
- 没有检查：
  - 该路径是否来自当前 `listInferSourcesLocked()`
  - 是否属于当前配置目录

影响：

- 这更偏向路径约束和接口完整性问题。
- 它本身不是当前 6 个问题的最强解释，但说明 source 选择链路校验较弱。

---

## 5. Constraints

- 不允许顺手重构 RD / Infer 主算法。
- 不允许扩大公有接口到无关模块。
- 不允许为了“修复”而关闭核心校验。
- 优先最小补丁，先修进程稳定性，再修交互语义和 UI 布局。
- 修复时应遵守 `TASK_PHASE3_FIX1` 已经确定的线程边界，不把 Web 层重新耦合进推理/HDMI 热路径。

---

## 6. Required workflow for Codex

按 `BUGFIX_TEMPLATE.md`，后续正式修复建议按下面顺序推进：

1. 先复现：
   - 切图取消预览
   - 重复 `Start/Stop`
   - 页面刷新/反复打开
2. 优先验证主根因：
   - 断开浏览器连接后继续向 `/events` 或 `/api/source/preview` 写响应，确认是否触发进程退出
3. 写失败用例：
   - 预览取消不应终止进程
   - 断开的 SSE 客户端不应终止进程
   - `Paused` 状态下切图应符合明确设计
4. 做最小修复：
   - 先修 socket 写安全性
   - 再修 `Paused` 下 selection 策略
   - 再修前端预览与状态栏布局
5. 做回归：
   - CLI 模式 1/2 不回退
   - Web 模式多轮 `start/pause/stop/reset` 稳定

---

## 7. Required output before editing

### 7.1 你如何理解这组 bug

- 这不是 6 个彼此独立的小问题，而是一个“Web 控制台稳定性主问题 + 交互策略问题 + 一个确定的 UI 布局问题”的组合。
- 主问题更像网络写路径没有做好断连保护，导致浏览器取消请求或关闭连接时，服务端被 `SIGPIPE` 直接打死。
- `pause` 下不能切图则是控制器状态机策略与用户预期不一致。

### 7.2 最小复现路径

- 已在第 3 节分别给出 6 条最小复现。

### 7.3 怀疑的根因列表（按概率排序）

1. `send()` 未处理 `SIGPIPE`，导致断连时整进程退出。
2. `Paused` 被当成运行态，`applySelection()` 硬拒绝切图。
3. 前端 selection 失败后仍然继续发预览请求，放大崩溃概率。
4. 预览接口整文件读入、整包发送，进一步放大请求取消窗口。
5. `infer_workflow` 的 `SIGSEGV -> _Exit(139)` 放大底层异常。
6. `Live Status` 网格布局策略不正确。

### 7.4 计划修改的文件（若进入代码修复）

- `main/src/web_console_server.cpp`
- `main/src/web_console_assets.cpp`
- `main/src/web_console_controller.cpp`
- `main/src/infer_workflow.cpp`

说明：

- 如果只修主根因 4.1，最小补丁大概率集中在 `web_console_server.cpp`。
- 如果同时修 Bug 1 的交互语义，则还要改 `web_console_controller.cpp` 和前端 JS。
- Bug 5 则只需要改 `web_console_assets.cpp` 的 CSS / DOM 策略。

### 7.5 测试方案

- 回归 A：页面首次加载后快速切换图片 1/2/3，不应退出。
- 回归 B：`Start -> Pause -> 切图 -> Start`，行为应稳定且符合设计。
- 回归 C：`Start/Stop` 循环 30 次以上，不应在 `load network` 阶段随机退出。
- 回归 D：反复刷新页面并保持推理运行，SSE 断开/重连不应影响进程存活。
- 回归 E：默认 `HDMI + 1号图` 连续运行多次，不应停在 `load network` 阶段直接退出。
- 回归 F：`CURRENT_ITEM`、`LAST_ERROR` 为长字符串时，`Live Status` 布局保持稳定。

---

## 8. Required output after editing

> 当前阶段尚未进入代码修改，以下内容留作后续修复完成后填写。

### 8.1 failing test 是什么

- 当前未编写。

### 8.2 最终根因是什么

- 当前静态分析结论：
  - 第一主根因高度怀疑是 `SIGPIPE` 未处理。
  - Bug 1 还叠加了 `Paused` 状态机策略错误。
  - Bug 5 是确定的前端布局问题。

### 8.3 改了哪些文件

- 当前阶段未改代码文件。

### 8.4 修复逻辑是什么

- 当前阶段未进入修复。

### 8.5 为什么这是最小修复

- 当前阶段未进入修复。

### 8.6 回归验证结果

- 当前阶段未执行。

### 8.7 仍然未覆盖的风险

- 板端设备 / 驱动 / 后端库如果确有真实段错误，仍需要板端复现与日志确认。
- “图片 3 更容易触发”这一点，仍需结合图片大小、磁盘速度和浏览器取消时序做验证。

---

## 9. Done when

- 可以稳定复现并确认：
  - 浏览器取消预览请求不会让进程退出
  - SSE 客户端断开不会让进程退出
  - `Paused` 下切图行为符合明确设计
  - `load network` 阶段不再出现 Web 模式独有的随机退出
  - `Live Status` 键值布局稳定
- 修复后 CLI `Inference only` 行为不被破坏。
- 修复范围保持在 Web 控制台相关模块内，可 review。
