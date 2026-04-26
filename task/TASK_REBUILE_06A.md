# TASK_REBUILE_06A: 建立 infer 内部边界头并抽离 UI render

## 0. Meta
- 阶段：`TASK_REBUILE_06` 子阶段 1。
- 优先级：P1。
- 板端约束关联：只抽离 UI 合成边界，不改变 NPU / HDMI / PNG 执行语义。
- 对应总任务：`task/TASK_REBUILE_06.md`
- 主责任域：`UiRenderContext`、mini-map、manual telemetry 覆盖层、工业 UI 合成。
- 依赖前置任务：`TASK_REBUILE_09` 已完成。

## 1. Background
- 当前 `infer_workflow.cpp` 里最适合先抽离的是 UI render 相关 helper，因为它们大多是纯逻辑或图像合成逻辑，不直接控制设备生命周期。
- `UiRenderContext`、`buildMiniMapContext()`、`applyManualTelemetry()`、`composeIndustrialUiFrame()` 已经是相对清晰的一组职责。
- 这一步同时需要补一个内部边界头，否则新 `.cpp` 仍然无法与 `infer_workflow.cpp` 解耦。

## 2. Goal
- 新建 `main/include/workflow/infer/infer_workflow_internal.hpp` 作为 infer 内部共享边界头。
- 把 UI render 相关数据结构和 helper 从 `infer_workflow.cpp` 抽到 `main/src/infer/ui_render.cpp`。
- 保持 HDMI / PNG 最终帧视觉语义不变，包括 `STOPPED` 状态 badge、manual telemetry、mini-map 覆盖层。

## 3. Out of scope
- 不抽 patch planner。
- 不抽 output sink / HDMI worker。
- 不改 `ManualFlightRuntimeState`。
- 不改 `auto_snake` / `manual_flight` 的流程控制点。

## 4. Allowed files to modify
```text
task/TASK_REBUILE_06A.md
main/src/infer_workflow.cpp
main/include/workflow/infer/infer_workflow_internal.hpp
main/src/infer/ui_render.cpp
main/tests/infer_workflow_regression_test.cpp
```

## 5. Files/modules to avoid
```text
main/src/manual_flight_runtime.cpp
main/src/web_console.cpp
main/src/web_console_server.cpp
main/src/rd_imaging_stream.cpp
deps/**
```

## 6. Functional requirements
- [ ] `UiRenderContext` 及相关内部结构已脱离 `infer_workflow.cpp`。
- [ ] `buildMiniMapContext()`、`applyManualTelemetry()`、`composeIndustrialUiFrame()` 已迁出。
- [ ] `STOPPED`、`PAUSED`、`EDGE HOLD`、`RUNNING` 的显示语义不变。
- [ ] HDMI / PNG 输出端看到的最终 frame 内容不因本次迁移改变。

## 7. Non-functional requirements
- [ ] 只建立内部边界，不新增面对外部模块的公开 API。
- [ ] 避免让 `infer_workflow_internal.hpp` 变成新的“大共享头”。
- [ ] 不顺手挪动输出链和设备锁逻辑。

## 8. Implementation decomposition
- 主任务：先建立 infer 内部边界头，再抽 UI render。
- 子任务 1：定义 `infer_workflow_internal.hpp` 的最小共享结构和函数声明。
- 子任务 2：迁出 UI render helper 到 `main/src/infer/ui_render.cpp`。
- 子任务 3：让 `infer_workflow.cpp` 改为调用新 helper。
- 子任务 4：补最小回归，验证状态 badge、mini-map 和 manual telemetry 语义未变。

## 9. Edge cases
- HDMI 停止态的红色 `STOPPED` badge 不能回退。
- manual telemetry 叠层不能丢字段。
- mini-map patch 标记和路径点数量显示不能回退。

## 10. Validation
```text
g++ -std=c++17 -fsyntax-only main/src/infer_workflow.cpp main/src/infer/ui_render.cpp -Imain/include
ctest --test-dir build/main-host --output-on-failure
```

## 11. Required response format before editing
1. 对任务的理解
2. 计划修改的文件
3. 不会修改的文件
4. 实现方案
5. 风险点
6. 验证方案

## 12. Required response format after editing
1. 修改了哪些文件
2. 每个文件做了什么改动
3. 为什么这样设计
4. 跑了哪些命令
5. 哪些验证通过 / 未通过
6. 剩余风险

## 13. Done when
- [ ] UI render 已迁出独立实现文件。
- [ ] `infer_workflow.cpp` 中不再承载大段 UI 组装实现。
- [ ] 新边界至少有最小回归验证。

## 14. Follow-up
- `06A` 完成后，才能继续进入 `TASK_REBUILE_06B` 的 patch planner 抽离。
