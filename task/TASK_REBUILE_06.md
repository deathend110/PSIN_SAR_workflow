# TASK_REBUILE_06: 在 ManualFlight 状态显式对象化后继续收窄 infer_workflow 编排层

## 0. Meta
- 阶段：长期重构，分阶段推进。
- 优先级：P1。
- 板端约束关联：保留 ZG330 推理顺序和 HDMI / PNG 输出语义，不改变结果。
- 对应 review 建议：`PROJ_GPT_REVIEW.md` 的大文件拆分建议；经主审查后收敛为“先状态对象化，再继续收窄编排层”。
- 主责任域：推理流程编排、patch 规划、UI 组装、输出 sink。
- 依赖前置任务：建议先完成 `TASK_REBUILE_09`，再继续对 `infer_workflow.cpp` 做编排层收窄。

## 1. Background
- `main/src/infer_workflow.cpp` 已经承担了过多职责，但当前最清晰的小切口其实是 `ManualFlightSharedState` 的显式对象化，而不是直接做整体大拆。
- 因此这条任务必须放在 `TASK_REBUILE_09` 之后执行，先让状态边界清楚，再继续收窄编排层。
- 目标不是“把大文件拆碎”，而是让 `infer_workflow.cpp` 更像编排层，逐步把局部职责迁出。

## 2. Goal
- 在 `TASK_REBUILE_09` 完成后，继续把 `infer_workflow.cpp` 收敛成以流程编排为主的文件。
- 把 patch 规划、UI 组装、输出 sink 等稳定职责逐步迁出。
- 保持外部行为不变，不在这条任务里顺手触碰新的线程或硬件边界。

## 3. Out of scope
- 不做全面目录重排。
- 不改模型输入输出协议。
- 不把所有 helper 一次性迁移完。
- 不在这条任务里单独做硬件 backend 抽象。

## 4. Allowed files to modify
```text
task/TASK_REBUILE_06.md
main/src/infer_workflow.cpp
main/include/workflow/infer/infer_workflow.hpp
main/include/workflow/infer/infer_workflow_internal.hpp
main/src/infer/patch_planner.cpp
main/src/infer/manual_flight_runtime.cpp
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
- [ ] `infer_workflow.cpp` 更接近流程编排层。
- [ ] patch 规划边界更清楚。
- [ ] UI 组装不再和流程控制继续膨胀在一起。
- [ ] 输出 sink 的职责边界更清楚。
- [ ] 外部运行结果保持一致。

## 7. Non-functional requirements
- [ ] 这是分阶段重构，不是一次性重写。
- [ ] 不扩大公开接口到失控程度。
- [ ] 尽量保留现有命名风格和命名空间。
- [ ] 任何迁移都不能改变设备访问顺序。

## 8. Implementation decomposition
- 主任务：在状态边界明确之后，把 `infer_workflow.cpp` 收敛为流程编排层。
- 子任务 1：先迁出最稳定、最容易测的 helper 和纯逻辑单元。
- 子任务 2：把 UI 组装与输出 sink 的实现边界进一步清楚化。
- 子任务 3：仅在状态显式对象化之后，再评估 patch planner / manual runtime 的迁移顺序。
- 子任务 4：补对应回归测试，保证迁移不改外部行为。
- 中后期：先做小步迁移，不做一次性大搬家。
- 长期：继续向“编排层 + 独立职责单元”推进。

## 9. Edge cases
- 手动飞行与自动 patch 的分支不能混乱。
- HDMI 与 PNG 输出语义不能互相污染。
- 迁移过程中不能让状态丢失。
- 任何拆分都不能改变设备访问顺序。

## 10. Validation
```text
g++ -std=c++17 -fsyntax-only main/src/infer_workflow.cpp -Imain/include
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
- [ ] `infer_workflow.cpp` 的关键职责已向编排层收敛。
- [ ] 迁移后外部行为不变。
- [ ] 新边界有最小测试覆盖。

## 14. Follow-up
- 本任务默认吸收原 HDMI 接口收口内容，作为 UI / 输出边界收窄时的子项处理，不再单独立 rebuild 文件。
- 不要在这一步追求“彻底重写”，先保证编排层边界清楚。
