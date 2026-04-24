# BUGFIX_1

> 本文档依据 BUGFIX_TEMPLATE.md 生成。  
> 当前阶段为分析阶段：不改代码，只做复述、复现、根因候选排序与修复计划。

---

## 1. Bug title

- Web Console 多处稳定性问题：切图相关崩溃、重复启动后模型加载阶段退出、UI 状态栏布局错位、频繁点击后进程退出。

---

## 2. Symptom

- Bug 1：pause 状态下无法切图，且执行切图时可能终止程序。
- Bug 2：重复运行后点击 start，在加载模型阶段突然退出。
- Bug 3：刚启动 Web Console，选择图片 1 和 2 正常，直接选图片 3 会退出。
- Bug 4：保持默认选项，直接启动 HDMI 推理 1 号图片，有时在日志到达以下阶段时直接退出：
  - [info] npu: 0x40000000
  - [info] [stage] load network json/raw
- Bug 5：右侧 Live status 状态栏留白与字符串长度不匹配，位置不对齐。
- Bug 6：多次点击 Web 控制台后程序直接退出。

频率与条件（基于当前信息）：

- Bug 1：高概率复现，和 pause 场景强相关。
- Bug 2/4：间歇复现，多次 start/stop 后更易出现。
- Bug 3：对特定图片触发，说明和输入源或预览路径相关。
- Bug 5：稳定复现，属于前端布局问题。
- Bug 6：高频交互后触发，疑似连接/资源/异常传播问题。

---

## 3. Minimal reproduction

### 3.1 Bug 1（pause 切图）

- 输入条件：infer 输入目录下至少 3 张 SAR 图。
- 触发步骤：
  1. 进入 Web Console。
  2. 点击 Start。
  3. 点击 Pause。
  4. 在 Input Source Loader 点击另一张图。
- 预期行为：允许切换目标图，或至少稳定返回可读错误，不退出进程。
- 实际行为：当前逻辑拒绝切图，且用户观察到会出现终止。

### 3.2 Bug 2/4（重复运行后模型加载退出）

- 输入条件：模型 json/raw 路径正确，设备可用。
- 触发步骤：
  1. Start -> Stop（或运行结束）循环多次。
  2. 再次点击 Start。
- 预期行为：正常进入推理流程。
- 实际行为：有时在 load network json/raw 阶段直接退出。

### 3.3 Bug 3（特定图片触发退出）

- 输入条件：至少有图片 1/2/3，且 3 可被前端列出。
- 触发步骤：
  1. 刚启动 Web Console，不先运行。
  2. 点击图片 3。
- 预期行为：切换选中项并正常预览。
- 实际行为：直接退出。

### 3.4 Bug 5（Live status 布局错位）

- 输入条件：Web Console 正常打开。
- 触发步骤：观察右侧状态栏，在状态值较长时更明显。
- 预期行为：键值列对齐、留白和内容长度匹配。
- 实际行为：留白与文本长度不匹配，位置偏移。

### 3.5 Bug 6（多次点击后退出）

- 输入条件：Web Console 正常打开。
- 触发步骤：频繁点击页面按钮或重复打开/操作控制台。
- 预期行为：服务持续稳定。
- 实际行为：进程退出。

---

## 4. Suspected root causes

按“可能性 + 影响面”排序：

1. 推理路径安装 SIGSEGV 处理后直接 _Exit(139)，把底层崩溃放大为整进程退出。
   - 证据点：main/src/infer_workflow.cpp 中存在 std::signal(SIGSEGV, ...) 与 std::_Exit(139)。
   - 对应 Bug：2、4，且可放大 6。

2. 控制器状态机禁止运行态（含 paused）修改 selection，导致 pause 切图与产品预期冲突。
   - 证据点：applySelection 在 isRunningState 下直接返回 invalid_state；isRunningState 包含 Paused。
   - 对应 Bug：1。

3. 前端点击 source 会同时触发 selection 提交和 preview 请求；在未运行场景也会触发。
   - 证据点：source click -> pushSelection -> renderPreview -> /api/source/preview。
   - 对应 Bug：1、3。

4. 预览接口 readFileBinary 为整文件读入内存后再返回完整响应，存在大图/异常文件导致内存或异常放大风险。
   - 证据点：web_console_server.cpp 中 readFileBinary 与 /api/source/preview 路径。
   - 对应 Bug：3、6（资源压力下）。

5. SSE 客户端与事件队列在高频交互时可能造成资源压力，错误上抛后触发 Web 线程退出，进而使 web_console 模式结束。
   - 证据点：EventSource /events、sse_clients_、pending_events_、web_console.cpp 中 server->Run 异常会令 g_web_console_stop = true。
   - 对应 Bug：6。

6. Live status 使用固定网格列和右对齐策略，未对长字符串做约束或截断，导致视觉对齐问题。
   - 证据点：web_console_assets.cpp 的 .kv-grid 样式（grid-template-columns: 1fr auto）。
   - 对应 Bug：5。

7. selected_source 在 applySelection 时缺少强语义校验，问题延后到 start 时暴露。
   - 证据点：selected_source 直接赋值，start 时覆盖 infer_cfg.sar_img_dir / rd_cfg.echo_dir。
   - 对应 Bug：1、3。

---

## 5. Constraints

- 不允许顺手重构其他模块。
- 不允许扩大公共接口。
- 不允许改变线程模型（除非证明是最小必要改动）。
- 不允许通过关闭校验或静默吞错来“修复”。
- 优先做最小补丁，先保证稳定性，再考虑体验增强。

---

## 6. Required workflow for Codex

必须按顺序执行：

1. 复述 6 个 bug 与触发条件。
2. 给出最小复现步骤（覆盖 1/2/3/4/5/6）。
3. 列出根因候选并排序（含证据点）。
4. 写 failing test（至少覆盖 pause 切图、重复 start、特定图片预览）。
5. 做最小修复。
6. 跑回归验证与压力验证。
7. 输出影响面与残余风险。

> 当前文档仅完成 1~3，未进入代码修复。

---

## 7. Required output before editing

1. 你如何理解这个 bug

- 这是 Web Console 侧“状态机策略、预览链路、异常处理策略、连接/资源管理、前端布局”叠加导致的稳定性问题；核心是“交互预期与运行约束不一致 + 崩溃后硬退出”。

2. 最小复现路径

- 已在第 3 节给出，覆盖 6 个问题。

3. 你怀疑的根因列表（按概率排序）

- 已在第 4 节给出 1~7 条。

4. 计划修改的文件

- main/src/web_console_controller.cpp
- main/src/web_console_assets.cpp
- main/src/web_console_server.cpp
- main/src/infer_workflow.cpp

5. 测试方案

- 回归用例 A：未运行前切图（1/2/3 图片），验证不退出。
- 回归用例 B：start -> pause -> 切图 -> start，验证行为符合需求（可切换或稳定拒绝且不退出）。
- 回归用例 C：start/stop 循环 30+ 次，验证模型加载阶段稳定。
- 回归用例 D：默认 HDMI 启动图片 1，重复执行，验证不会在 load network json/raw 退出。
- 回归用例 E：高频点击按钮与重复打开页面，验证服务不退出、SSE 连接受控。
- 回归用例 F：Live status 长字符串场景，验证布局对齐。

---

## 8. Required output after editing

1. failing test 是什么

- 本阶段未编写（仅分析）。

2. 最终根因是什么

- 本阶段尚未定稿，需要修复与验证后确认主根因。

3. 改了哪些文件

- 本阶段未改代码文件。

4. 修复逻辑是什么

- 本阶段未进入修复。

5. 为什么这是最小修复

- 本阶段未进入修复。

6. 回归验证结果

- 本阶段未执行回归。

7. 仍然未覆盖的风险

- 第三方后端内部稳定性仍需通过压力测试确认。
- 大文件预览与异常文件输入边界仍需专项验证。

---

## 9. Done when

- 6 个问题均可稳定复现且已被回归用例覆盖。
- 修复后不再出现“切图导致退出”与“模型加载阶段突然退出”。
- Live status 布局与字符串长度匹配。
- 高频点击/长时运行下 Web Console 不退出。
- 变更范围可审计、无接口扩散、无新明显风险。