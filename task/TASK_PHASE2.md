# Task title

第二阶段：HDMI UI 渲染与模型推理解耦为双线程

---

## 1. Background

这个任务为什么存在？

- 第二阶段开始前，`main/src/infer_workflow.cpp` 的主链路是单线程串行：`processPatch(...)` 内部依次执行 `runner.forward(...) -> 后处理 -> composeIndustrialUiFrame(...) -> sink.write(...)`。
- 第二阶段开始前，`HdmiFrameSink::write(...)` 会执行 `cv::cvtColor(..., COLOR_BGR2BGR565)`、`display_.show(...)` 和按 `fps` 的 sleep，这会直接阻塞下一次 patch 推理。
- 你当前确认的第二阶段思路不是“每个 patch 结果都排队显示”的普通 FIFO 生产者消费者，而是：
  - 线程 1 负责推理，持续产出“当前最新推理结果”
  - 线程 2 负责按显示节拍读取最新结果、挂 UI 参数、合成 HDMI 最终帧并输出
- 当前 HDMI 显示链路通过 `HdmiFrameSink -> RGB565HDMIDisplay<FPAIDevice>` 访问 `FPAIDevice`，而推理链路中的 `PatchInferenceRunner::forward(...)` 会执行 `device_.reset(1)`。如果显示线程和推理线程同时碰同一底层 device/driver，存在并发访问风险。
- 第二阶段目标是提升 `hdmi` 模式下的推理吞吐，同时保持第一阶段 UI 语义，不扩大为整套输出框架重构。

---

## 2. Goal

明确写出要实现的行为。

**必须是可验证的。**

- 在 `output.mode=hdmi` 下，把“模型推理/后处理”和“HDMI UI 帧渲染/显示”拆成两个线程。
- 推理线程只负责：
  - patch 提取
  - `runner.forward(...)`
  - 后处理得到 `restore_gray` / `restore_bgr` / `mask_bgr`
  - 产出一份“最新推理结果快照”
- HDMI/UI 线程只负责：
  - 按固定显示节拍运行
  - 读取“当前最新推理结果快照”
  - 挂载当前状态参数
  - 调用 `composeIndustrialUiFrame(...)`
  - 把最终帧送给 `HdmiFrameSink::write(...)`
- HDMI 线程采用 latest-state / mailbox 模型，而不是 FIFO 队列模型：
  - 只保留最新一份完整结果
  - 旧结果允许被覆盖
  - 不要求每个 patch 都显示一次
- `output.mode=png` 在第二阶段保持当前同步写盘语义，不纳入本阶段双线程改造。
- `PatchInferenceRunner::forward(...)` 的设备敏感顺序保持不变：`session_.forward(...) -> output.waitForReady(...) -> output.to(host) -> device_.reset(1)`。

---

## 3. Out of scope

明确不做什么。

- 不把 `png` 输出也改成异步线程
- 不把 HDMI 设计成“每个 patch 都必须显示一次”的 FIFO 消费模型
- 不新增第三个线程去单独做 UI 合成
- 不修改 `main/src/main.cpp` 的菜单语义
- 不修改 patch 规则、模型输入输出格式、输出目录语义
- 不修改 `classColorBgr(...)` 和第一阶段 UI 配色语义
- 不引入 `deps/modelzoo_utils` 的 actor / queue 框架
- 不修改 `ARCHITECTURE_TEMPLATE.md`、`CODEBASE_MAP_TEMPLATE.md`
- 本次任务文档只允许落到 `task/TASK_PHASE2.md`

---

## 4. Allowed files to modify

```text
main/src/infer_workflow.cpp
task/TASK_PHASE2.md
```

---

## 5. Files/modules to avoid

```text
main/src/main.cpp
main/src/rd_imaging_stream.cpp
main/include/**
deps/**
ARCHITECTURE_TEMPLATE.md
CODEBASE_MAP_TEMPLATE.md
```

---

## 6. Chosen implementation direction

这项决策已经确定，不再二选一：

- 第二阶段采用“两线程 + 最新状态槽位”的实现，不采用通用 FIFO 输出队列。
- 线程角色固定为：
  - 推理线程：生产最新推理结果快照
  - HDMI/UI 线程：消费最新状态并渲染显示
- 推理线程不直接调用 `composeIndustrialUiFrame(...)`，也不直接调用 `HdmiFrameSink::write(...)`。
- 推理线程产出的不是最终 UI 帧，而是“足够渲染 UI 的最新快照”，建议至少包含：
  - `RuntimeState` 的冻结副本
  - `cv::Mat restore_bgr`
  - `cv::Mat mask_bgr`
- HDMI/UI 线程在自己的循环中负责：
  - 等待新快照或等待下一显示时刻
  - 从 latest mailbox 读取最新快照
  - 调用 `composeIndustrialUiFrame(...)`
  - 调用 `sink.write(...)`
- latest mailbox 采用单生产者 / 单消费者模型，深度固定为 `1`：
  - 生产者写入新快照时可以覆盖旧快照
  - 消费者永远只取当前最新完成快照
  - 不做 backlog 积压
- mailbox 中的数据必须是完整自包含对象，禁止把以下对象跨线程借引用：
  - `Tensor`
  - `Session`
  - `Device`
  - `FPAIDevice`
  - `UiRenderContext&`
- `cv::Mat` 快照进入 mailbox 后，生产者不得再次修改其底层数据。
- 因为 `HdmiFrameSink` 也访问 `FPAIDevice`，第二阶段默认实现方向必须优先保证“设备访问串行化”。采用以下约束：
  - 推理线程执行 `runner.forward(...)` 和 `device_.reset(1)` 时，HDMI 线程不得同时调用 `display_.show(...)`
  - 如果现有底层设备没有明确线程安全保证，则必须在模块内引入一个“device access mutex”，把推理设备访问和 HDMI 设备访问都包在同一把锁下
  - 这把锁只保护底层 device/FPAIDevice 访问，不保护 `cv::Mat` 渲染逻辑
- 线程退出顺序固定为：
  1. 推理线程结束 patch 循环
  2. 设置输入完成标志
  3. 唤醒 HDMI/UI 线程
  4. HDMI/UI 线程消费最后一个可显示快照后退出
  5. `join()` HDMI/UI 线程
  6. `Device::Close(device)`
- 异常路径要求：
  - 任一线程抛异常都必须写入共享异常状态
  - 主线程必须能观察到该异常并中止流程
  - 不允许 HDMI/UI 线程在 `Device::Close(device)` 之后继续存活

---

## 7. Functional requirements

- [ ] `output.mode=hdmi` 下，推理线程不再直接调用 `composeIndustrialUiFrame(...)`
- [ ] `output.mode=hdmi` 下，推理线程不再直接调用 `HdmiFrameSink::write(...)`
- [ ] HDMI/UI 线程必须按显示节拍持续运行，即使短时间内没有新 patch 结果，也允许重复显示上一帧
- [ ] 推理线程每完成一个 patch，都会尝试发布一份新的“最新结果快照”
- [ ] 若 HDMI/UI 线程来不及消费，中间旧 patch 结果允许被覆盖
- [ ] HDMI 最终画面始终以“最近一次完成推理的 patch 结果”为准
- [ ] `output.mode=png` 下，仍然保持当前同步 `patch_*.png` 写盘语义
- [ ] `png` 的文件路径和命名继续保持 `io/output/<stem>/patch_*.png`
- [ ] `PatchInferenceRunner` 接口不改
- [ ] `session.apply()`、`forward()`、`waitForReady()`、`device.reset(1)` 顺序不改
- [ ] UI 内容仍然复用第一阶段的 `composeIndustrialUiFrame(...)`，不改展示语义
- [ ] HDMI/UI 线程允许跳帧显示，但不允许读到半写入快照或悬空数据

---

## 8. Non-functional requirements

- [ ] 改动集中在 `infer_workflow.cpp` 内部，不扩大公共接口
- [ ] 不新增第三方依赖
- [ ] 不引入裸 owning pointer
- [ ] 线程同步仅允许使用 `std::mutex`、`std::condition_variable`、明确状态位和明确所有权
- [ ] device/FPAIDevice 访问同步策略必须写清楚，不能靠“经验上应该没事”
- [ ] mailbox 语义必须有注释，明确它不是 FIFO 队列
- [ ] diff 必须 review 友好，不顺手重构 UI 样式或其它无关逻辑

---

## 9. Interface expectations

给出希望的接口草案：

```cpp
struct InferenceSnapshot {
    RuntimeState state;
    UiRenderContext ui_context;
    cv::Mat restore_bgr;
    cv::Mat mask_bgr;
};

class LatestSnapshotMailbox {
public:
    enum class WakeReason { Timeout, NewSnapshot, InputClosed, StopRequested };
    void publish(InferenceSnapshot&& snapshot);
    bool loadLatest(InferenceSnapshot& snapshot, std::uint64_t& sequence);
    WakeReason waitForChangeOrStop(std::uint64_t known_sequence,
                                   std::chrono::microseconds timeout);
    void markInputClosed();
    void requestStop();
};

class HdmiRenderWorker {
public:
    HdmiRenderWorker(IFrameSink& sink,
                     LatestSnapshotMailbox& mailbox,
                     const UiRenderContext& placeholder_ui_context,
                     int display_width,
                     int display_height,
                     int display_fps,
                     std::mutex& device_access_mutex);
    void start();
    void requestStop();
    void join();
};
```

接口约束：

- `IFrameSink` 不改
- `PngFrameSink` 不改
- `PatchInferenceRunner` 不改
- mailbox / render worker 仅放在 `infer_workflow.cpp` 内部
- `UiRenderContext` 可以按值或安全共享只读方式传给 HDMI/UI 线程，但不能悬空

---

## 10. Edge cases

Locked choice for this phase:
- HDMI/UI 线程启动后先显示占位帧，再在收到首个有效 snapshot 后切换到真实推理结果。

- HDMI/UI 线程重复显示上一帧时，不能错误累加 frame 计数或 patch 计数
- 推理线程连续快速发布新快照时，mailbox 不能暴露半写入 `cv::Mat`
- 首个 patch 还未完成期间，HDMI/UI 线程继续按显示节拍重复占位帧，而不是阻塞等待首帧
- 若推理先结束，HDMI/UI 线程允许把最后一个有效结果保持到退出，不要求清屏
- 若 HDMI/UI 线程报错，主线程必须停止后续 patch 推理并安全收尾
- 若 `sar_norm.cols < patch_size` 或 `sar_norm.rows < patch_size`，继续沿用当前 skip 逻辑，不创建无意义渲染状态
- device 锁的粒度不能扩大到整段 UI 渲染，否则会抵消双线程收益

---

## 11. Validation

明确要求运行什么验证：

```text
1. g++ -fsyntax-only "main/src/infer_workflow.cpp" "-Imain/include" "-Ideps/modelzoo_utils/include" "-Ideps/thirdparty/include" "-Ideps/thirdparty/include/Eigen-3.4.0" "-Ideps/thirdparty/include/Eigen-3.4.0/Eigen"
2. 静态审查调用链，确认 HDMI 路径中:
   - 推理线程不再直调 composeIndustrialUiFrame(...)
   - 推理线程不再直调 HdmiFrameSink::write(...)
3. output.mode=hdmi:
   - 验证显示线程按 fps 运行
   - 验证推理线程不再被 HDMI sleep 直接阻塞
   - 验证允许跳帧但不会卡死/崩溃
4. 设备并发:
   - 验证 display_.show(...) 与 device.reset(1) 不会无保护并发访问同一 device
5. output.mode=png:
   - 验证 patch_*.png 语义保持不变
   - 验证 png 仍然同步输出
6. 退出路径:
   - 验证 HDMI/UI 线程 join 发生在 Device::Close(device) 之前
   - 验证异常路径不会留下悬挂线程
```

---

## 12. Required response format before editing

要求 Codex 在动手前先输出：

1. 对任务的理解
2. 计划修改的文件
3. 不会修改的文件
4. 实现方案
5. 风险点
6. 验证方案

---

## 13. Required response format after editing

要求 Codex 改完后输出：

1. 修改了哪些文件
2. 每个文件做了什么改动
3. 为什么这样设计
4. 跑了哪些命令
5. 哪些验证通过 / 未通过
6. 剩余风险

---

## 14. Done when

写成客观验收标准：

- `output.mode=hdmi` 已拆成“推理线程 + HDMI/UI 线程”
- HDMI 线程消费的是 latest snapshot，而不是 FIFO 全量 patch 队列
- 推理线程不再被 HDMI sleep / display 写寄存器直接阻塞
- `output.mode=png` 行为保持现状，不被第二阶段顺手改掉
- `PatchInferenceRunner` 和设备 reset 顺序未改
- `display_.show(...)` 与推理设备访问存在明确同步策略
- 线程退出顺序正确，`join()` 先于 `Device::Close(device)`
- diff 可 review，且改动没有扩散到无关模块

---

## 15. Review acceptance criteria

实现完成后，review 必须按下面 6 点输出：

1. 这次 patch 是否符合任务目标
2. 是否有范围失控
3. 是否有线程 / 生命周期 / 越界风险
4. 测试是否充分
5. 是否建议合并
6. 若不建议，最关键的 3 个问题是什么

Reviewer 默认结论标准：

- 若实现仍然是“推理线程直接合成 UI 并直接写 HDMI”，判定“不符合任务目标”
- 若 HDMI 线程仍按 FIFO 消费每个 patch，判定“设计不符合第二阶段思路”
- 若 `display_.show(...)` 与 `device.reset(1)` 可能并发访问同一 device 且无明确保护，判定“高风险线程/生命周期问题，不建议合并”
- 若 `png` 被顺手改成异步或行为发生变化，判定“范围失控”
