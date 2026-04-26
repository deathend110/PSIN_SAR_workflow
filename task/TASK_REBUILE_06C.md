# TASK_REBUILE_06C: 抽离 output sink 与 HDMI worker 边界

## 0. Meta
- 阶段：`TASK_REBUILE_06` 子阶段 3。
- 优先级：P1。
- 板端约束关联：这是 `TASK_REBUILE_06` 中最敏感的子阶段，必须保持 device lock 顺序、mailbox 语义、HDMI / PNG 输出语义不变。
- 对应总任务：`task/TASK_REBUILE_06.md`
- 主责任域：`LatestSnapshotMailbox`、`HdmiRenderWorker`、`processPatchToPng()`、`processPatchToHdmi()`。
- 依赖前置任务：建议先完成 `TASK_REBUILE_06A` 和 `TASK_REBUILE_06B`。

## 1. Background
- 输出链是当前 `infer_workflow.cpp` 中风险最高的职责边界，因为它同时涉及：
  - HDMI 渲染线程
  - mailbox latest-wins 语义
  - device access mutex
  - `STOPPED` 终态帧逻辑
- 这部分必须单独成为一个子阶段，不能和 UI render、patch planner 一起大搬家。

## 2. Goal
- 把 `LatestSnapshotMailbox`、`HdmiRenderWorker`、`processPatchToPng()`、`processPatchToHdmi()` 迁到 `main/src/infer/output_sink.cpp`。
- 让 `infer_workflow.cpp` 只保留调用和收尾逻辑。
- 保持 HDMI / PNG 输出语义、device 锁顺序、STOPPED 终帧语义不变。

## 3. Out of scope
- 不改 NPU / HDMI 串行化策略。
- 不重做 render worker 的线程模型。
- 不改变 mailbox 的 latest-wins 语义。
- 不处理新的性能优化。

## 4. Allowed files to modify
```text
task/TASK_REBUILE_06C.md
main/src/infer_workflow.cpp
main/include/workflow/infer/infer_workflow_internal.hpp
main/src/infer/output_sink.cpp
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
- [ ] output sink 相关实现已迁出 `infer_workflow.cpp`。
- [ ] HDMI worker 生命周期行为不变。
- [ ] mailbox latest-wins 语义不变。
- [ ] `STOPPED` 终帧、waiting frame、current frame 语义不变。
- [ ] PNG 输出命名和落盘行为不变。

## 7. Non-functional requirements
- [ ] 不能改变 `device_access_mutex` 的使用顺序。
- [ ] 不能引入新的线程模型或新的跨线程共享策略。
- [ ] 不顺手调整 HDMI 帧率控制或 mailbox 设计。

## 8. Implementation decomposition
- 主任务：把输出链从大编排文件中独立出来。
- 子任务 1：在内部边界头中声明 output sink 所需最小共享结构。
- 子任务 2：迁出 mailbox 和 HDMI worker 实现。
- 子任务 3：迁出 `processPatchToPng()` / `processPatchToHdmi()`。
- 子任务 4：补最小回归，验证 device 锁顺序和 STOPPED 终帧语义未变。

## 9. Edge cases
- 运行中 stop 后的终态红色 `STOPPED` 画面不能回退。
- 没有有效快照时的 placeholder / empty frame 退化路径不能回退。
- HDMI worker 异常抛出路径不能被吞掉。
- PNG 与 HDMI 输出路径不能互相污染。

## 10. Validation
```text
g++ -std=c++17 -fsyntax-only main/src/infer_workflow.cpp main/src/infer/output_sink.cpp -Imain/include
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
- [ ] output sink / HDMI worker 已迁出独立实现文件。
- [ ] HDMI / PNG 输出语义、device 锁顺序、STOPPED 终帧语义不变。
- [ ] 新边界有最小回归验证。

## 14. Follow-up
- `06C` 完成后，才能进入 `TASK_REBUILE_06D` 的最终编排层收口。
