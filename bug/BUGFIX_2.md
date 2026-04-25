# BUGFIX_2

> 本文档依据 `BUGFIX_TEMPLATE.md` 生成。  
> 当前阶段为静态排查阶段：不修改代码，只做复现路径、根因候选排序与修复计划整理。

---

## 0. 本次问题范围

本轮只分析两个新问题：

- Bug 1：选择过推理图片后，无法切换到 `RD` 模式。
- Bug 2：`stop` 有时响应很慢，尤其在推理跑到半张图或中途 patch 阶段时明显；刚开始推理时点 `stop` 又比较快。

它们都位于 Web Console 控制路径，不属于 RD / Infer 核心算法正确性问题。

---

## 1. Bug title

- Web Console 模式切换与停止控制问题：Infer 图片选中后无法切到 RD，以及 stop 仅在 patch 安全点生效导致中途停止响应慢。

---

## 2. Symptom

### Bug 1：Infer 图片选中后无法切换到 RD

- 现象：
  - 在 `Inference only` 模式下选中过某张推理图片后，再点击 `RD only` 按钮，模式切换失败。
- 发生频率：
  - 高概率稳定复现。
- 发生条件：
  - 当前 `selected_source` 是一张 inference 图片路径。
  - 然后直接点击 workflow 按钮切到 `rd`。
- 用户体感：
  - 界面像是“点不动”或“切不过去”，通常不会给出足够直观的解释。
  - 下面的终端会提示：selection: selected_source is not available for the current workflow.

### Bug 2：stop 有时失灵或响应慢

- 现象：
  - 推理刚开始时点击 `stop`，通常较快停止。
  - 推理跑到一半、尤其处于 patch 中段时点击 `stop`，往往要等较久才真正停下。
- 发生频率：
  - 稳定存在，但延迟长短取决于当前 patch 处理耗时。
- 发生条件：
  - `Inference only`
  - `auto_snake`
  - 当前正在处理某个 patch
- 用户体感：
  - 不是完全失灵，而是“按钮按了，状态会变成 stopping，但 worker 很久才真正退出”。

---

## 3. Minimal reproduction

### 3.1 Bug 1：无法从 infer 切到 RD

- 输入条件：
  - `infer.input.sar_img_dir` 下有可选图片。
  - `rd.echo_dir` 下也有可选 echo 文件。
- 触发步骤：
  1. 打开 Web Console。
  2. 保持 `Inference only`。
  3. 在左侧 source 列表里点击任意一张 inference 图片。
  4. 再点击 `RD only`。
- 预期行为：
  - workflow 切换到 `rd`。
  - source 列表切到 RD 文件列表。
  - 若当前已选 source 不再适用，应自动清空或切换到 RD 默认 source。
- 实际行为：
  - workflow 切换请求被拒绝，页面停留在 infer 侧。

### 3.2 Bug 2：stop 响应慢

- 输入条件：
  - `Inference only`
  - `auto_snake`
  - 至少一张较大的 SAR 图可切成多个 patch
- 触发步骤：
  1. 点击 `Start`。
  2. 等运行进入 patch 循环中段。
  3. 点击 `Stop`。
- 预期行为：
  - 很快停止，或至少在用户可接受时间内停止。
- 实际行为：
  - 控制器很快返回 `Workflow stopped.` 或页面很快显示 `stopping`，
  - 但 worker 线程实际要等当前 patch 处理完整结束后才真正退出。

---

## 4. Suspected root causes

以下按“概率 + 证据强度”排序。

### 4.1 Bug 1 主根因：workflow 切换时前端带上了旧的 infer `selected_source`

证据链：

- 前端点击 workflow 按钮时，`renderAll()` 内部逻辑是：
  - 先设置 `app.state.workflow_mode=value`
  - 再直接 `await pushSelection()`
  - 最后才 `await reloadSources()`
  - 见 `main/src/web_console_assets.cpp:324` 到 `main/src/web_console_assets.cpp:327`
- `pushSelection()` 会把当前 `app.selectedSource` 一并发给后端：
  - `selected_source: app.selectedSource || ""`
  - 见 `main/src/web_console_assets.cpp:362` 到 `main/src/web_console_assets.cpp:368`
- 也就是说：
  - 当用户从 infer 切到 rd 时，请求体里往往仍然带着一条“推理图片路径”。
- 后端 `applySelection()` 会在 workflow 已经变成 `rd` 的前提下校验 source：
  - 如果 `next_selection.workflow == RdOnly`，就用 `listRdSourcesLocked()` 校验
  - 若 `selected_source` 不在 RD source 列表内，则返回：
    - `"selected_source is not available for the current workflow."`
  - 见 `main/src/web_console_controller.cpp:278` 到 `main/src/web_console_controller.cpp:286`

结论：

- Bug 1 不是 RD 模式按钮本身失效。
- 是“前端切 workflow 时提交顺序不对”，导致 controller 把旧的 infer 图片路径当成 RD 非法输入而拒绝整次切换。

### 4.2 Bug 1 次根因：前端没有把“workflow 切换”和“source 重装载”拆成两个阶段

证据链：

- 当前前端顺序是：
  - `pushSelection()` 先发
  - `reloadSources()` 后做
  - 见 `main/src/web_console_assets.cpp:324` 到 `main/src/web_console_assets.cpp:327`
- 这与正确顺序相反。

更合理的顺序应是：

1. 先只切换 workflow。
2. 再根据新的 workflow 拉新的 sources。
3. 再选择一个 RD 默认 source 或清空 source。
4. 最后同步新的 `selected_source`。

结论：

- Bug 1 的根本是“source 生命周期跟 workflow 生命周期绑错顺序了”。

### 4.3 Bug 2 主根因：stop 是协作式停止，只在 patch 边界检查

证据链：

- 推理主循环在 `main/src/infer_workflow.cpp:2157` 进入 patch 循环。
- `stop` 检查逻辑只发生在每个 patch 开始处：
  - `control->waitIfPaused();`
  - `if (control->shouldStop()) { ... break; }`
  - 见 `main/src/infer_workflow.cpp:2159` 到 `main/src/infer_workflow.cpp:2167`
- 一旦已经进入 `processPatchToPng(...)` 或 `processPatchToHdmi(...)`，当前 patch 会完整跑完：
  - `tensor_builder.build`
  - `runner.forward`
  - `waitForReady`
  - host copy
  - postprocess
  - UI compose
  - sink.write / mailbox.publish
  - 见 `main/src/infer_workflow.cpp:1901` 到 `main/src/infer_workflow.cpp:2015`

结论：

- Bug 2 不是传统意义上的“stop 失灵”。
- 它是当前设计明确选择了“patch 边界协作式停止”，所以停止延迟上限就是“当前 patch 剩余耗时”。

### 4.4 Bug 2 放大因素：`commandStop()` 是同步 join，返回时间直接绑定 worker 真正退出时间

证据链：

- `commandStop()` 在 controller 中：
  - `run_control_->requestStop()`
  - 然后把 `worker_` 移出
  - 接着直接 `worker_to_join.join()`
  - 见 `main/src/web_console_controller.cpp:641` 到 `main/src/web_console_controller.cpp:674`
- 这意味着 HTTP `/api/command/stop` 的响应时间，不是“命令已发出”的时间，而是“后台线程完全退出”的时间。

结论：

- 当 patch 很重时，用户会感觉“stop 按钮卡住”。
- 其实不是没收到 stop，而是 controller 故意同步等待 worker 收尾。

### 4.5 Bug 2 次级放大因素：HDMI 路径下 patch 处理本身比刚开始更慢

证据链：

- `processPatchToHdmi(...)` 除了推理，还包含：
  - `device_access_mutex` 串行锁
  - `runner.forward(...)`
  - restore / mask 后处理
  - mailbox publish
  - render worker 状态检查
  - 见 `main/src/infer_workflow.cpp:1951` 到 `main/src/infer_workflow.cpp:2015`
- `PatchInferenceRunner::forward()` 内部也会等待输出 ready，再做 host copy，再 `device_.reset(1)`：
  - 见 `main/src/infer_workflow.cpp:833` 到 `main/src/infer_workflow.cpp:844`

结论：

- 越靠后面的 patch，尤其在 HDMI 模式或设备繁忙时，单 patch 耗时越容易拉长。
- stop 延迟因此更明显。

### 4.6 低优先级候选：前端对 stop 的 UI 反馈不足，放大了“失灵”体感

证据链：

- 前端 `command` 按钮点击后只是：
  - `postJson(path,{})`
  - `logLine`
  - `await refreshState()`
  - 见 `main/src/web_console_assets.cpp:346` 到 `main/src/web_console_assets.cpp:356`
- 没有专门提示：
  - “stop 只在 patch 边界生效”
  - “当前正在等待 worker 收尾”

结论：

- 这不是主根因，但会把协作式 stop 的正常延迟，放大成用户理解中的“stop 失灵”。

---

## 5. Constraints

- 不允许顺手重构整个 Web Console 前端状态管理。
- 不允许改变 RD / Infer 的线程模型。
- 不允许为了 stop 变快而引入线程强杀。
- 不允许破坏 `TASK_PHASE3_FIX1` 明确的“协作式 stop / safe point”设计。
- 修复必须优先最小范围：
  - Bug 1 先修 workflow/source 切换顺序
  - Bug 2 先澄清 stop 语义，再考虑是否在 patch 内增加更细粒度 safe point

---

## 6. Required workflow for Codex

后续如果进入代码修复，建议按下面顺序推进：

1. 先复现 Bug 1：
   - infer 选图
   - 点 `RD only`
   - 看 `/api/selection` 返回内容
2. 再复现 Bug 2：
   - patch 开始前 stop
   - patch 中途 stop
   - 比较停止延迟
3. 写 failing test / 回归脚本：
   - workflow 切换时旧 `selected_source` 不应阻断切换
   - stop 请求发出后状态应立即可见为 `stopping`
   - worker 退出延迟要能被明确测量
4. 做最小修复：
   - Bug 1：拆分 workflow 切换和 source 刷新顺序
   - Bug 2：若要优化，只能在不破坏线程模型的前提下加更细粒度 safe point 或改 stop API 返回语义
5. 跑回归：
   - infer / rd 模式切换
   - start/pause/stop/reset
   - HDMI / PNG 两条路径

---

## 7. Required output before editing

### 7.1 你如何理解这两个 bug

- Bug 1 是一个前端状态同步顺序错误，不是 RD 模式本身不可切换。
- Bug 2 主要是当前 stop 设计语义带来的延迟问题，不是简单的线程池失控或命令丢失。

### 7.2 最小复现路径

- 已在第 3 节分别给出。

### 7.3 你怀疑的根因列表（按概率排序）

1. workflow 切换时前端带着旧 infer `selected_source` 一起提交，导致 controller 以 RD source 规则校验失败。
2. workflow/source 切换顺序设计不对，应该先切 workflow，再换 source。
3. stop 只在 patch 边界检查，当前 patch 跑完前不会真正停。
4. `commandStop()` 同步 `join()`，把 worker 收尾耗时直接暴露成 HTTP stop 响应时间。
5. HDMI 路径和重 patch 路径放大了 stop 的等待时间。
6. 前端缺少 stop 延迟语义提示，放大“失灵”感受。

### 7.4 计划修改的文件（若进入代码修复）

- `main/src/web_console_assets.cpp`
- `main/src/web_console_controller.cpp`
- 可能涉及 `main/src/infer_workflow.cpp`

说明：

- Bug 1 大概率只改前端 JS 就能修干净，也可能需要 controller 放宽部分过渡态校验。
- Bug 2 若只做“交互认知修正”，可能只改前端和 controller 文案。
- 若要真正缩短 stop 延迟，则必须评估 `infer_workflow.cpp` 是否能增加 patch 内安全点，但这已经比普通 UI 修复更敏感。

### 7.5 测试方案

- 回归 A：infer 选图后，切到 RD，确认模式能切换且 source 列表变成 echo 列表。
- 回归 B：RD 切回 infer，再选图，再切 RD，循环多次验证不回退。
- 回归 C：推理刚开始时 stop，记录从点击到完全停止的时间。
- 回归 D：推理中段 stop，记录从点击到完全停止的时间。
- 回归 E：比较 PNG 与 HDMI 路径下的 stop 延迟差异。
- 回归 F：确认 stop 过程中页面状态能及时显示 `stopping`，避免误判为按钮失灵。

---

## 8. Required output after editing

> 当前阶段尚未进入代码修复，以下内容留作后续修复完成后填写。

### 8.1 failing test 是什么

- 当前未编写。

### 8.2 最终根因是什么

- 当前静态分析结论：
  - Bug 1 主根因是 workflow/source 提交顺序错误。
  - Bug 2 主根因是 stop 只在 patch 边界生效，且 controller 同步等待 worker 退出。

### 8.3 改了哪些文件

- 当前阶段未改代码文件。

### 8.4 修复逻辑是什么

- 当前阶段未进入修复。

### 8.5 为什么这是最小修复

- 当前阶段未进入修复。

### 8.6 回归验证结果

- 当前阶段未执行。

### 8.7 仍然未覆盖的风险

- 如果要把 stop 明显提速，可能需要改动 `infer_workflow.cpp` 的 patch 内 safe point，这会触及热路径和设备交互顺序，风险高于普通 Web 控制台修复。
- Bug 2 在用户体验层面也可能需要“stop requested / waiting for safe point”的明确提示，否则即使逻辑正确，仍容易被感知成失灵。

---

## 9. Done when

- Bug 1：
  - infer 选图后可稳定切换到 RD。
  - workflow 切换时不会被旧 source 阻断。
- Bug 2：
  - stop 的行为和语义被明确验证。
  - 若不改变线程模型，则至少要保证：
    - 状态能立即进入 `stopping`
    - 用户知道当前在等待 patch 安全点
    - 延迟范围可解释、可测量、可回归
- 修复范围可 review，未扩散到无关模块。
