# Task title

修正 Phase 2 的 HDMI 渲染节拍、设备访问策略与验证缺口

---

## 1. Background

这个任务为什么存在？

- `TASK_PHASE2` 已经落地为“推理线程 + HDMI/UI 线程 + latest snapshot mailbox”的双线程实现。
- 当前实现已经基本符合“推理和 HDMI 输出解耦”的方向，但 review 发现还有 3 个关键问题没有收口：
  - HDMI/UI 线程没有真正按 `display_fps` 固定节拍输出，而是在新 snapshot 到来时可能立即再次写屏
  - `device_access_mutex` 目前保护了整段 `runner.forward(...)`，这虽然规避了设备并发风险，但会让 HDMI 线程在推理期间长期拿不到设备访问权
  - 缺少针对 mailbox、渲染线程节拍和退出路径的验证，当前无法证明行为真的符合第二阶段目标
- 相关上下文：
  - 目标实现文件是 `main/src/infer_workflow.cpp`
  - 当前设计文档是 `task/TASK_PHASE2.md`
  - review 依据是 `REVIEW_CHECKLIST.md`
- 触发场景：
  - `output.mode=hdmi` 路径下需要同时满足“线程已拆分”和“显示行为符合设计”
  - 当前 patch 已经到了“不是不能跑，而是不能放心合并”的状态
- 已知限制：
  - 不能改动 `PatchInferenceRunner::forward(...)` 的设备敏感顺序
  - 不能顺手把 `png` 路径改成异步
  - 不能因为修节拍而把线程模型再改回单线程

---

## 2. Goal

明确写出要实现的行为。

**必须是可验证的。**

- 修正 HDMI/UI 线程，使其真正按固定显示节拍工作，而不是“有新 snapshot 就立即多写一帧”。
- 明确并落实第二阶段的设备访问口径：
  - 保留 `device_access_mutex`
  - 接受“设备访问串行化下的非严格实时 HDMI”这一实现取舍
  - 文档和实现都必须明确：当前阶段追求的是“推理线程不再直接被 HDMI sleep 阻塞”，而不是“HDMI 与推理可完全并发访问同一 device”
- 补足最小可 review 的验证，至少能证明：
  - mailbox 行为正确
  - HDMI/UI 线程节拍正确
  - 退出顺序和异常路径正确

---

## 3. Out of scope

明确不做什么。

- 不移除 `device_access_mutex`
- 不尝试证明或引入 `FPAIDevice` 的无锁并发访问
- 不把 `png` 路径改成异步
- 不修改 `main/src/main.cpp`
- 不修改 `main/include/infer_workflow_hdmi_display.hpp`
- 不重构第一阶段 UI 布局、配色或文案
- 不引入新的并发框架或第三方依赖

---

## 4. Allowed files to modify

```text
main/src/infer_workflow.cpp
task/TASK_PHASE2_FIX1.md
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
task/TASK_PHASE2.md
```

---

## 6. Functional requirements

- [ ] `HdmiRenderWorker::run()` 必须改成固定节拍循环，而不是收到新 snapshot 后立即再次 `writeFrame(...)`
- [ ] 显示线程必须维护显式的“下一次显示时刻”，例如 `next_present_time`
- [ ] 新 snapshot 提前到达时，只更新当前 latest snapshot，不立即触发额外写屏
- [ ] 到达显示 deadline 时，才允许：
  - 调用 `composeIndustrialUiFrame(...)`
  - 调用 `HdmiFrameSink::write(...)`
- [ ] 在没有新 snapshot 的情况下，显示线程允许重复显示上一帧
- [ ] HDMI 线程的显示频率不得高于 `cfg.display_fps` 对应节拍
- [ ] `output.mode=png` 的同步写盘语义保持不变
- [ ] `PatchInferenceRunner::forward(...)` 的顺序保持不变：
  - `session_.forward(...)`
  - `output.waitForReady(...)`
  - `output.to(HostDevice::MemRegion())`
  - `device_.reset(1)`
- [ ] 实现中必须明确注释：当前第二阶段接受“设备访问串行化下的非严格实时 HDMI”，这是有意为之，不是遗漏

---

## 7. Non-functional requirements

- [ ] 保持最小修改范围，只修 review 指出的 3 类问题
- [ ] 不扩大公共接口
- [ ] 不新增第三方依赖
- [ ] 不引入裸 owning pointer
- [ ] 线程同步必须保持可审查，不能把时序逻辑写成隐式 side effect
- [ ] 注释要解释“为什么这样做”，尤其是：
  - 为什么 HDMI 线程按 deadline loop 跑
  - 为什么设备访问继续串行化
  - 为什么这不等于完全实时渲染

---

## 8. Interface expectations

给出希望的接口草案：

```cpp
class HdmiRenderWorker {
public:
    void start();
    void requestStop();
    void join();

private:
    void run();              // deadline-driven render loop
    void writeFrame(...);    // device access stays serialized here
};
```

接口约束：

- `LatestSnapshotMailbox` 保持 latest-state mailbox 语义，不改成 FIFO
- `PatchInferenceRunner` 不改
- `PngFrameSink` 不改
- `HdmiFrameSink` 继续只做最终写屏，不再负责节拍控制

---

## 9. Edge cases

- 新 snapshot 高频到达时，显示线程不得写出高于 `display_fps` 的突发帧
- 没有 snapshot 更新时，显示线程不能 busy-spin
- 首帧占位逻辑必须保持当前实现语义，不要顺手改成“首帧前不显示”
- 若 HDMI 线程因为设备锁被推理暂时阻塞，这应表现为“错过一部分刷新时机”，而不是死锁或异常退出
- 退出时：
  - `markInputClosed()` 后线程能安全收尾
  - `join()` 仍然发生在 `Device::Close(device)` 之前
- 若渲染线程抛异常，主线程必须感知并停止后续 patch 推理

---

## 10. Validation

明确要求运行什么验证：

```text
1. g++ -fsyntax-only "main/src/infer_workflow.cpp" "-Imain/include" "-Ideps/modelzoo_utils/include" "-Ideps/thirdparty/include" "-Ideps/thirdparty/include/Eigen-3.4.0" "-Ideps/thirdparty/include/Eigen-3.4.0/Eigen"
2. 静态审查 HdmiRenderWorker::run():
   - 确认不再因为 NewSnapshot 而立即额外 writeFrame
   - 确认存在显式显示 deadline
3. 静态审查设备访问:
   - 确认 display_.show(...) 与 runner.forward(...) / device.reset(1) 仍然通过同一 device lock 串行化
   - 确认实现注释已明确这是当前阶段接受的 tradeoff
4. 行为验证:
   - 验证 HDMI 线程空闲时重复上一帧而不是 busy-spin
   - 验证 snapshot 高频更新时显示输出频率仍受 display_fps 约束
5. 生命周期验证:
   - 验证 requestStop / markInputClosed / join / Device::Close(device) 顺序仍然正确
6. 若仓库缺少自动化测试基础设施:
   - 至少补一段面向 reviewer 的验证说明，写清如何人工检查节拍、退出顺序和日志语义
```

建议的最小人工验证口径：

- `output.mode=hdmi` 下观察运行日志与屏幕行为：
  - 高频 patch 完成时，屏幕刷新不应出现“每来一个 snapshot 就额外突发写一帧”的现象
  - 在没有新 snapshot 的短时间窗口内，HDMI 应重复上一帧而不是 busy-spin 或黑屏
- 人工核对收尾顺序：
  - patch 循环结束后先触发 `markInputClosed()`
  - HDMI/UI 线程 `join()` 完成后才允许 `Device::Close(device)`
- 人工核对 tradeoff 文案：
  - 代码注释需明确当前阶段接受“设备访问串行化下的非严格实时 HDMI”
  - reviewer 不应把“显示可能错过一部分 deadline”误判为遗漏

---

## 11. Required response format before editing

要求 Codex 在动手前先输出：

1. 对任务的理解
2. 计划修改的文件
3. 不会修改的文件
4. 实现方案
5. 风险点
6. 验证方案

---

## 12. Required response format after editing

要求 Codex 改完后输出：

1. 修改了哪些文件
2. 每个文件做了什么改动
3. 为什么这样设计
4. 跑了哪些命令
5. 哪些验证通过 / 未通过
6. 剩余风险

---

## 13. Done when

写成客观验收标准：

- HDMI/UI 线程已经是固定节拍渲染，而不是 snapshot 触发式突发写屏
- `display_fps` 对 HDMI 写屏频率真正生效
- latest mailbox 语义保持不变
- `png` 路径行为保持不变
- 设备访问仍被明确串行化，且实现/文档承认这是当前阶段 tradeoff
- 退出顺序仍然满足 `join()` 先于 `Device::Close(device)`
- 至少有静态验证和最小行为验证覆盖这次修复目标
- diff 可 review，且范围没有扩散

---

## 14. md update

无需同步更新其它 md 文件。
