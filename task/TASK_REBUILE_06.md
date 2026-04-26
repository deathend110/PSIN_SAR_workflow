# TASK_REBUILE_06: 在 ManualFlight 状态显式对象化后分阶段收窄 infer_workflow 编排层

## 0. Meta
- 阶段：长期重构，分 4 个子阶段推进。
- 优先级：P1。
- 板端约束关联：保留 ZG330 推理顺序、HDMI / PNG 输出语义、设备访问顺序，不改变外部结果。
- 对应 review 建议：`PROJ_GPT_REVIEW.md` 的大文件拆分建议；经主审查后收敛为“先状态对象化，再继续收窄编排层”。
- 主责任域：推理流程编排、patch 规划、UI 组装、输出 sink。
- 依赖前置任务：先完成 `TASK_REBUILE_09` 和 `TASK_REBUILE_07`，再进入本任务。
- 子阶段任务单：
  - `task/TASK_REBUILE_06A.md`
  - `task/TASK_REBUILE_06B.md`
  - `task/TASK_REBUILE_06C.md`
  - `task/TASK_REBUILE_06D.md`

## 1. Background
- `main/src/infer_workflow.cpp` 当前同时承担了 SAR 图收集、patch 规划、ManualFlight runtime 包装、UI 组装、输出链路和最终流程编排等多种职责。
- `TASK_REBUILE_09` 已先把 ManualFlight 运行态从隐式全局收成显式对象，这为进一步收窄 `infer_workflow.cpp` 提供了前置条件。
- 但 `infer_workflow.cpp` 里仍有两类高风险边界不能一次性大拆：
  - 设备访问顺序与 HDMI / PNG 输出语义
  - `auto_snake` / `manual_flight` 两条 patch 路径的运行行为
- 因此这条任务必须拆成多个小阶段，每一阶段只收一个主要职责边界，并要求阶段间可回退。

## 2. Goal
- 让 `infer_workflow.cpp` 逐步收敛成“以流程编排为主”的文件。
- 按风险从低到高依次抽离：
  - UI render 边界
  - patch planner 边界
  - output sink / HDMI worker 边界
  - 最终编排层收口
- 保持外部行为不变，不在本任务里顺手修改新的线程模型、硬件 backend 抽象或 Web 控制面行为。

## 3. Out of scope
- 不做全面目录重排。
- 不改模型输入输出协议。
- 不在本任务中引入新的公开 API 给 Web / main / RD。
- 不在本任务里直接处理 `auto_snake` pause/stop 灵敏度优化。
- 不在本任务里单独推进硬件 backend 抽象；那属于 `TASK_REBUILE_10`。

## 4. Allowed files to modify
```text
task/TASK_REBUILE_06.md
task/TASK_REBUILE_06A.md
task/TASK_REBUILE_06B.md
task/TASK_REBUILE_06C.md
task/TASK_REBUILE_06D.md
main/src/infer_workflow.cpp
main/include/workflow/infer/infer_workflow.hpp
main/include/workflow/infer/infer_workflow_internal.hpp
main/src/manual_flight_runtime.cpp
main/src/infer/patch_planner.cpp
main/src/infer/ui_render.cpp
main/src/infer/output_sink.cpp
main/tests/infer_workflow_regression_test.cpp
```

## 5. Files/modules to avoid
```text
main/src/main.cpp
main/src/web_console.cpp
main/src/web_console_server.cpp
main/src/rd_imaging_stream.cpp
deps/**
```

## 6. Functional requirements
- [ ] `infer_workflow.cpp` 的职责逐阶段向编排层收敛。
- [ ] UI render、patch planner、output sink 都有明确的独立边界。
- [ ] `auto_snake` 与 `manual_flight` 两条路径的行为保持不变。
- [ ] HDMI / PNG 输出语义、STOPPED 终态语义、设备访问顺序不变。
- [ ] 每个子阶段都有对应的最小回归验证。

## 7. Non-functional requirements
- [ ] 这是分阶段重构，不是一次性重写。
- [ ] 每个子阶段必须是可 review、可回退、低风险补丁。
- [ ] 不扩大公开接口到失控程度。
- [ ] 尽量保留现有命名风格和命名空间。
- [ ] 任何迁移都不能改变设备访问顺序或 NPU / HDMI 串行化语义。

## 8. Implementation decomposition
- 主任务：在状态边界明确之后，把 `infer_workflow.cpp` 收敛为流程编排层。

- 子阶段 06A：建立 infer 内部边界头并抽离 UI render
  - 目标：先把最纯、最稳定、最容易测的 UI 组装职责迁出。
  - 产物：`infer_workflow_internal.hpp`、`main/src/infer/ui_render.cpp`
  - 阶段门：UI helper 已迁出，`infer_workflow.cpp` 不再持有大段 UI 合成实现。

- 子阶段 06B：抽离 patch planner
  - 目标：把 `SnakePatchSource` 及其相关 patch 规划逻辑迁出。
  - 产物：`main/src/infer/patch_planner.cpp`
  - 阶段门：patch 规划可单独测试，蛇形顺序与 patch 计数行为不变。

- 子阶段 06C：抽离 output sink / HDMI worker
  - 目标：把 `LatestSnapshotMailbox`、`HdmiRenderWorker`、`processPatchToPng`、`processPatchToHdmi` 等输出链路迁出。
  - 产物：`main/src/infer/output_sink.cpp`
  - 阶段门：设备访问顺序、mailbox 行为、STOPPED 终帧语义不变。

- 子阶段 06D：最终编排层收口
  - 目标：在前 3 个边界稳定后，最后精简 `infer_workflow.cpp`，让其更接近“加载配置 -> 选择路径 -> 跑流程 -> 收尾”的编排层。
  - 阶段门：`infer_workflow.cpp` 以 orchestration 为主，不再承载大段实现细节。

## 9. Edge cases
- 手动飞行与自动 patch 的分支不能混乱。
- HDMI 与 PNG 输出语义不能互相污染。
- 迁移过程中不能让状态丢失。
- 任何拆分都不能改变设备访问顺序。
- STOPPED 终态帧、manual telemetry 和 mini-map 覆盖层行为必须保持一致。

## 10. Validation
```text
g++ -std=c++17 -fsyntax-only main/src/infer_workflow.cpp -Imain/include
g++ -std=c++17 -fsyntax-only main/src/manual_flight_runtime.cpp -Imain/include
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
- [ ] `TASK_REBUILE_06A`、`06B`、`06C`、`06D` 都有独立任务单。
- [ ] `infer_workflow.cpp` 的关键职责已按阶段向编排层收敛。
- [ ] 迁移后外部行为不变。
- [ ] 新边界都有最小测试覆盖。

## 14. Follow-up
- 本任务默认吸收原 HDMI 接口收口内容，作为 UI / 输出边界收窄时的子项处理，不再单独立 rebuild 文件。
- 不要在这一步追求“彻底重写”，先保证编排层边界清楚，再继续为 `TASK_REBUILE_10` 建立更稳定的 infer 边界。
