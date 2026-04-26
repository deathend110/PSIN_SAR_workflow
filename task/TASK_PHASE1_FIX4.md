# Task title

Add HDMI stopped terminal state and red STOPPED badge

---

## 1. Background

- 当前 HDMI 渲染线程在 [main/src/infer_workflow.cpp](</g:/Docker_windows_disk/PSIN_SAR_workflow/main/src/infer_workflow.cpp>) 的 `HdmiRenderWorker::run()` 中，只在启动时构造一张 `WAITING` 占位帧。
- 当前 UI 顶部右侧状态 badge 在 `composeIndustrialUiFrame(...)` 中走的是固定颜色路径：
  - 绿色圆点：`success_color`
  - 绿色文字：`running_text_color`
  - 绘制调用：`drawBadge(..., ui_context.status_label, running_text_color, true)`
- 也就是说，虽然 `UiRenderContext::status_label` 可以是 `RUNNING / PAUSED / EDGE HOLD / WAITING`，但状态 badge 的视觉风格并没有和状态语义真正绑定。
- 当前 HDMI 退出路径在 `HdmiRenderWorker::run()` 中遇到以下唤醒原因会直接 `return`：
  - `LatestSnapshotMailbox::WakeReason::StopRequested`
  - `LatestSnapshotMailbox::WakeReason::InputClosed`
- 这导致推理结束时 HDMI 不会主动再输出一张“终止态界面”，屏幕只会停留在上一张运行中帧，右上角通常仍是绿色 `RUNNING`。

---

## 2. Goal

在不改推理主链路和线程模型的前提下，实现以下行为：

1. HDMI 推理终止时，额外输出一张终止态 UI 帧。
2. 终止态 UI 复用当前工业风布局，不另起一套全新页面结构。
3. 终止态帧右上角状态 badge 从当前绿色 `RUNNING` 切换为红色 `STOPPED`。
4. 如果 HDMI 在尚未产生任何有效推理快照前就结束，也必须能显示一张 `STOPPED` 终止态占位界面。

---

## 3. Out of scope

- 不修改 `PatchTensorBuilder`、`PatchInferenceRunner`、`SnakePatchSource`、`ManualFlightRuntime` 的推理逻辑
- 不修改 Web Console、RD workflow、配置解析逻辑
- 不新增线程、队列、锁或新的异步状态机
- 不为 PNG 输出额外生成 `STOPPED` 终止图
- 不重做整套 HDMI UI 布局，只允许在现有工业风 UI 上做终止态补充
- 不顺手重构 `UiRenderContext` / `RuntimeState` / `InferenceSnapshot` 的公共语义，除非终止态表达必须做最小字段补充

---

## 4. Allowed files to modify

```text
main/src/infer_workflow.cpp
task/TASK_PHASE1_FIX4.md
```

---

## 5. Files/modules to avoid

```text
main/src/main.cpp
main/src/web_console.cpp
main/src/web_console_controller.cpp
main/src/web_console_server.cpp
main/src/rd_imaging_stream.cpp
main/src/infer_config.cpp
main/include/**
deps/**
main/CMakeLists.txt
main/build_main.sh
```

---

## 6. Chosen implementation direction

本修复锁定为“局部 UI 状态补点”，不做大改。

### 6.1 状态 badge 去硬编码

在 `main/src/infer_workflow.cpp` 内部引入一个局部状态样式解析 helper，替换 `composeIndustrialUiFrame(...)` 中对状态 badge 的固定绿色写法。

推荐方向：

- 新增类似 `resolveStatusBadgeStyle(const std::string& label)` 的局部 helper
- 输出内容至少包含：
  - 状态文字颜色
  - 状态圆点颜色
- 颜色语义要求：
  - `RUNNING` 继续保持现有绿色风格
  - `STOPPED` 使用红色风格
  - 其余状态如 `WAITING / PAUSED / EDGE HOLD` 默认保持当前兼容行为，除非实现中证明需要最小区分

这样可以把“状态文案”和“状态视觉”真正绑定，同时把 diff 控制在 UI 合成层。

### 6.2 HDMI 渲染线程在退出前补写终止帧

在 `HdmiRenderWorker::run()` 中，不再在 `StopRequested / InputClosed` 时直接返回，而是先构造并写出一张终止态帧，然后再退出线程。

推荐行为：

1. 优先复用当前已持有的最新画面上下文：
   - `current_state`
   - `current_snapshot`
   - `current_frame` 对应的 `restore_bgr / mask_bgr / ui_context`
2. 在退出前构造一个 `stopped_ui_context`：
   - `status_label = "STOPPED"`
   - 其它字段尽量沿用当前快照，避免布局和内容突变
3. 重新调用 `composeIndustrialUiFrame(...)` 生成一张最终 HDMI 终止态帧
4. 通过 `writeFrame(...)` 再写出一次，然后结束线程

### 6.3 无有效快照时的退化策略

如果 HDMI 还没有收到任何推理快照就结束：

- 继续复用现有占位路径
- 基于 `WAITING` 占位上下文改出一张 `STOPPED` 占位帧
- 不要求伪造恢复图或分割图内容

这能保证“无论是否实际跑过 patch，终止时 HDMI 都有明确终止态”。

---

## 7. Functional requirements

- [ ] `output.mode=hdmi` 启动时，仍先显示当前占位界面，默认状态为 `WAITING`
- [ ] HDMI 正常推理过程中，右上角状态 badge 的 `RUNNING` 继续保持绿色
- [ ] HDMI 推理终止时，渲染线程在退出前必须额外写出一张终止态帧
- [ ] 终止态帧右上角状态文字必须为 `STOPPED`
- [ ] 终止态帧的状态 badge 必须为红色文本，并带红色状态圆点
- [ ] 终止态帧继续使用当前工业风 UI 布局，不额外引入新页面框架
- [ ] 若已有最新推理结果，终止态帧应复用最后一次有效快照的 restore/mask/telemetry 主体内容
- [ ] 若尚无有效推理结果，终止态帧应退化为占位界面 + 红色 `STOPPED`
- [ ] `output.mode=png` 的现有落盘语义保持不变
- [ ] 不能改变 `forward() -> waitForReady() -> host copy -> device.reset(1)` 的设备敏感顺序

---

## 8. Non-functional requirements

- [ ] 修改范围限制在 `infer_workflow.cpp` 内部
- [ ] 不引入新依赖
- [ ] 不新增线程或改变 `LatestSnapshotMailbox` 的 latest-wins 语义
- [ ] 不修改 `IFrameSink` 对外接口
- [ ] 不改变 HDMI 与 NPU 访问共用 `device_access_mutex` 的串行化策略
- [ ] diff 应保持 review 友好，优先局部 helper + 局部退出逻辑补点

---

## 9. Interface expectations

允许的最小接口调整：

```cpp
struct StatusBadgeStyle
{
    cv::Scalar dot_color;
    cv::Scalar text_color;
};

StatusBadgeStyle resolveStatusBadgeStyle(const std::string& status_label);
```

说明：

- `StatusBadgeStyle` 建议仅定义在 `main/src/infer_workflow.cpp` 的匿名命名空间内
- 不要把它提升到公共头文件
- `UiRenderContext` 默认只保留 `status_label` 也可以，只要状态颜色能在 UI 合成阶段解析出来
- 若实现者认为新增 `UiRenderContext` 内部颜色字段更稳，也必须证明它比字符串解析更小、更清晰

---

## 10. Edge cases

- 正常跑完全部 patch 后结束：必须显示 `STOPPED`
- 用户 stop 导致推理提前终止：必须显示 `STOPPED`
- HDMI 刚启动、尚未有任何快照就终止：必须显示 `STOPPED` 占位界面
- `WAITING`、`PAUSED`、`EDGE HOLD` 不能因为这次修复被误渲染成 `STOPPED`
- 终止态补帧不能引入额外死锁，尤其不能破坏 `device_access_mutex` 当前使用方式
- 终止态补帧必须发生在 `HdmiRenderWorker` 线程退出前，不能依赖主线程在 join 之后再补发
- 终止态帧只要求“最后写一次”，不要求维持新的循环显示状态机

---

## 11. Validation

实现后至少执行以下验证：

```text
1. 静态代码核对：
   - composeIndustrialUiFrame(...) 不再把状态 badge 固定写死为绿色
   - HdmiRenderWorker::run() 在 InputClosed / StopRequested 路径上会先补写 STOPPED 帧

2. 语法检查：
   g++ -std=c++17 -fsyntax-only "main/src/infer_workflow.cpp" "-Imain/include" "-Ideps/modelzoo_utils/include" "-Ideps/thirdparty/include" "-Ideps/thirdparty/include/Eigen-3.4.0" "-Ideps/thirdparty/include/Eigen-3.4.0/Eigen"

3. 行为核对：
   - HDMI 启动初始仍显示 WAITING
   - 正常运行阶段右上角仍显示绿色 RUNNING
   - 结束时最终画面变为红色 STOPPED
   - PNG 输出路径和行为无变化
```

如果当前环境无法做真实 HDMI 板端验证，必须明确说明这一点，但静态代码路径和语法检查仍要做。

---

## 12. Required response format before editing

1. 对任务的理解
2. 计划修改的文件
3. 不会修改的文件
4. 实现方案
5. 风险点
6. 验证方案

---

## 13. Required response format after editing

1. 修改了哪些文件
2. 每个文件做了什么改动
3. 为什么这样设计
4. 跑了哪些命令
5. 哪些验证通过 / 未通过
6. 剩余风险

---

## 14. Done when

- HDMI 终止时可见明确的终止态 UI
- 右上角绿色 `RUNNING` 在终止态下切换为红色 `STOPPED`
- 正常运行态 UI 不受影响
- PNG 路径和推理主链路语义不变
- 线程模型、锁模型、设备访问顺序未被改坏
- diff 范围集中、易于 review
