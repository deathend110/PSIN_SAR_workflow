# Task title

Phase 4 Fix1：修正 manual_flight 的前端按键状态漂移、状态栏不实时与 patch 计数语义问题
---

## 1. Background

这个任务为什么存在？

- 第四阶段 `manual_flight` 主闭环已经接通：
  - Web `keydown/keyup`
  - `POST /api/manual/key`
  - `WebConsoleController::commandManualKey(...)`
  - infer 内部 manual runtime
  - latest-wins patch 推理
- 但当前实现经过 review 后，发现还有 3 个需要尽快收口的问题：

1. 前端按键本地状态可能和板端实际状态漂移  
   - [web_console_assets.cpp](G:/Docker_windows_disk/PSIN_SAR_workflow/main/src/web_console_assets.cpp) 的 `sendManualKey(...)` 先改本地 `manualKeys`，再发请求。
   - 如果 `/api/manual/key` 被后端拒绝，或者网络失败，本地集合已经变了，后续同一按键会被前端误判为“已按下”，从而吞掉新的 `keydown`。
   - 这会导致 manual 控制偶发“失灵”。

2. `Live Status` 的 manual telemetry 不是实时推进的  
   - infer 内部 simulation thread 持续更新位置、速度、路径，但 controller / SSE 只有在命令请求或推理快照到来时才推状态。
   - 结果是手动飞行中，如果还没跨过 `trigger_distance_px`，Web 状态区会长时间停留在旧值。

3. manual 模式下 patch 计数语义不清晰  
   - 当前 `patch_count` 在 manual 路径里是推理完成后才补写。
   - 但 UI 合成、日志和 `publishSnapshot(...)` 的部分链路在此之前就已经读取了它。
   - 这会让 `PATCH` 计数、`current_index/total_count` 和日志里总 patch 数的含义不一致。

这些问题目前不需要重做 manual 架构，只需要做一轮小范围修复即可。

---

## 2. Goal

明确写出要实现的行为。  
**必须是可验证的。**

- 修复 `manual_flight` 前端本地按键状态与板端状态的漂移问题。
- 保证 `/api/manual/key` 失败时，前端不会把本地按键集合留在错误状态。
- 让 `Live Status` 中的 manual telemetry 能以合理频率刷新，而不是只在推理完成时跳变。
- 统一 manual 模式下 `PATCH / current_index / total_count` 的语义，避免出现误导性的计数。
- 保持这轮修复为小 patch，不重做第四阶段线程设计。

---

## 3. Out of scope

明确不做什么。

- 不重做 `manual_flight` 线程模型
- 不把 simulation thread 改成 controller 线程或 HDMI 线程
- 不改 RD 主链
- 不引入新的网络协议
- 不做新的 UI 大改版
- 不实现真实无人机飞控
- 不做新的缓存系统重构
- 不做 `control_bindings` 动态映射增强

---

## 4. Allowed files to modify

只列允许改的文件。
```text
main/src/web_console_assets.cpp
main/src/web_console_controller.cpp
main/src/web_console_server.cpp
main/src/web_console_protocol.cpp
main/src/infer_workflow.cpp
main/include/workflow/web/web_console_protocol.hpp
task/TASK_PHASE4_FIX1.md
```

---

## 5. Files/modules to avoid

写清楚不许动的范围。
```text
main/include/workflow/shared/run_control.hpp
main/src/rd_imaging_stream.cpp
main/src/rd_config.cpp
main/include/workflow/rd/**
main/src/web_console.cpp
main/src/web_console_config.cpp
main/include/workflow/infer/infer_workflow.hpp
deps/**
cmake/**
ARCHITECTURE_TEMPLATE.md
CODEBASE_MAP_TEMPLATE.md
main/Host Computer Software.md
task/TASK_PHASE4.md
task/TASK_PHASE3_FIX1.md
task/TASK_PHASE3_FIX2.md
task/TASK_PHASE3_FIX3.md
task/TASK_PHASE3_FIX4.md
```

---

## 6. Chosen implementation direction

这项决策已经确定，不再二选一：

### 6.1 前端按键状态修复

- `sendManualKey(...)` 不允许“先改本地集合、后发请求”。
- 推荐改为：
  - 先发请求
  - 仅当响应 `ok=true` 时再更新 `app.manualKeys`
- 如果请求失败或响应 `ok=false`：
  - 本地集合保持原值
  - 记录日志

### 6.2 telemetry 刷新修复

- 不引入新协议。
- 在现有状态链路上补“合理频率的 manual 状态刷新”。
- 推荐方向：
  - infer 侧在 manual simulation 发生关键状态变化时，允许更高频地发布快照
  - 或 controller / Web 层为 manual 模式补一个轻量轮询刷新入口
- 优先选择最小改动、最容易 review 的方案。

### 6.3 patch 计数语义修复

- manual 模式下不强行伪装成“总 patch 数已知的扫描任务”。
- `current_index` 可以表示“已完成 manual patch 次数”。
- `total_count` 不应继续借用“自动扫描总 patch 数”语义。
- 如果总数未知，应明确为：
  - `0`
  - 或与当前值相同
  - 但 UI / 日志 /快照语义必须统一

---

## 7. Functional requirements

列成可验收条目：

- [ ] `/api/manual/key` 失败时，前端本地 `manualKeys` 不会错误更新
- [ ] 网络异常时，前端本地 `manualKeys` 不会卡死在错误状态
- [ ] 长按同一按键时，仍保留去重，不发送无意义风暴请求
- [ ] 松键后，本地和后端状态能正确回到未按下
- [ ] `manual_flight` 运行时，`Live Status` 中的位置/速度/按键状态能合理刷新
- [ ] 未跨过 `trigger_distance_px` 时，manual telemetry 也不会长时间完全静止
- [ ] manual 模式下 `PATCH` / `current_index` / `total_count` 语义一致
- [ ] 不再出现明显误导性的 `1/0`、`N/0` 或等价显示

---

## 8. Non-functional requirements

- [ ] 保持最小范围修复
- [ ] 不引入新依赖
- [ ] 不改变 Phase 4 已确定的线程职责边界
- [ ] 不把 latest-wins 改回队列模型
- [ ] diff 要小，便于 review

---

## 9. Interface expectations

给出希望的接口草案：

### 前端

```javascript
async function sendManualKey(key, action) {
  // 先请求后提交本地状态
}
```

### 状态接口

继续复用：

```text
GET /api/state
GET /events
```

说明：
- 本轮不新增 `/api/manual/state`
- 但允许增强现有 state 刷新节奏或现有字段语义

---

## 10. File-level design

### `main/src/web_console_assets.cpp`

- 修复 `sendManualKey(...)` 的本地状态更新顺序
- 保留按键去重，但去重依据必须和后端成功状态一致
- 保证 `blur / pointerup / keyup` 后释放逻辑仍然成立

### `main/src/web_console_controller.cpp`

- 如果 telemetry 刷新选择走 controller 链路，这里补最小必要协调
- 保持 `commandManualKey(...)` 的输入校验和转发职责

### `main/src/web_console_server.cpp`

- 如果 telemetry 刷新需要轻量状态接口调整，在这里做最小路由/响应补充
- 不新增复杂协议

### `main/src/web_console_protocol.cpp`

- 如需修正 `current_index / total_count` 的 JSON 含义或 manual 字段语义，在这里同步调整

### `main/src/infer_workflow.cpp`

- 修复 manual 模式的 patch 计数与快照发布时间点
- 如果 telemetry 刷新选择走 infer 快照链路，这里补最小的发布点
- 不重写 manual runtime

---

## 11. Edge cases

要求 Codex 必须考虑这些：

- manual 模式未运行时按键
- manual 模式刚停止时按键
- `/api/manual/key` 返回错误
- `/api/manual/key` 网络超时或 fetch 失败
- 浏览器 `keydown` 成功但 `keyup` 失败
- 浏览器失焦时批量释放按键
- manual 模式下长时间未触发新 patch，但用户仍持续移动
- manual 模式切回 `auto_snake`
- `Pause` 状态下 telemetry 与按键状态的显示

---

## 12. Validation

明确要求运行什么验证：

```text
1. g++ -fsyntax-only 覆盖：
   - main/src/web_console_assets.cpp
   - main/src/web_console_controller.cpp
   - main/src/web_console_server.cpp
   - main/src/web_console_protocol.cpp

2. 静态检查：
   - 确认 sendManualKey(...) 不再先写本地集合再发请求
   - 确认 manual telemetry 刷新不只依赖 patch 完成
   - 确认 manual 模式下 current_index / total_count 语义统一

3. 板端验证：
   - Start manual_flight
   - 长按 W
   - 观察 Live Status 的位置/速度/按键状态是否持续更新
   - 模拟 manual key 请求失败，确认前端不会进入“按键卡死”状态
   - 观察 PATCH / current_index / total_count 是否语义稳定
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

- 前端不再因为 manual key 请求失败而本地状态漂移
- manual telemetry 在飞行过程中能合理刷新
- manual 模式下计数语义不再误导
- 修复范围保持在小 patch 内
- diff 可 review


## 16. md update

需要你随着代码更新而更新的文件：
- ARCHITECTURE_TEMPLATE.md
- CODEBASE_MAP_TEMPLATE.md
- main/Host Computer Software.md
