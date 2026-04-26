# TASK_REBUILE_06B: 抽离 patch planner 边界

## 0. Meta
- 阶段：`TASK_REBUILE_06` 子阶段 2。
- 优先级：P1。
- 板端约束关联：只抽 patch 规划逻辑，不改变 patch 顺序、patch 数量、蛇形路径语义。
- 对应总任务：`task/TASK_REBUILE_06.md`
- 主责任域：`SnakePatchSource`、patch 枚举、snake 顺序。
- 依赖前置任务：建议先完成 `TASK_REBUILE_06A`。

## 1. Background
- `SnakePatchSource` 目前逻辑独立、无设备访问依赖，是第二个最适合迁出的边界。
- 这一步的价值在于把 `auto_snake` 的 patch 枚举职责从大编排文件中拿出去，降低后续理解和测试成本。
- 但它必须保持 patch 顺序、rows/cols/total、蛇形方向全部不变，否则会直接改变推理覆盖路径。

## 2. Goal
- 把 `SnakePatchSource` 及其最小依赖迁到 `main/src/infer/patch_planner.cpp`。
- 让 `infer_workflow.cpp` 改成使用显式 planner 边界，而不是内嵌 patch 枚举实现。
- 不改变 `auto_snake` 的 patch 顺序和 patch count 行为。

## 3. Out of scope
- 不改 manual runtime。
- 不改 HDMI / PNG 输出链。
- 不优化 `pause/stop` 灵敏度。
- 不重做 patch 数据结构协议。

## 4. Allowed files to modify
```text
task/TASK_REBUILE_06B.md
main/src/infer_workflow.cpp
main/include/workflow/infer/infer_workflow_internal.hpp
main/src/infer/patch_planner.cpp
main/tests/infer_workflow_regression_test.cpp
```

## 5. Files/modules to avoid
```text
main/src/manual_flight_runtime.cpp
main/src/infer/output_sink.cpp
main/src/web_console.cpp
main/src/web_console_server.cpp
deps/**
```

## 6. Functional requirements
- [ ] `SnakePatchSource` 已迁出 `infer_workflow.cpp`。
- [ ] rows / cols / total count 计算行为不变。
- [ ] 奇偶行蛇形顺序不变。
- [ ] patch `x/y/grid_row/grid_col/right_to_left` 语义不变。

## 7. Non-functional requirements
- [ ] planner 边界应尽量纯，不引入设备、UI、输出链依赖。
- [ ] 不把 patch planner 和 ManualFlight runtime 混在一起。
- [ ] 不扩大外部公开接口。

## 8. Implementation decomposition
- 主任务：把自动 patch 规划迁出为独立实现单元。
- 子任务 1：在内部边界头中为 planner 暴露最小声明。
- 子任务 2：迁出 `SnakePatchSource` 的实现。
- 子任务 3：让 `infer_workflow.cpp` 改用新 planner。
- 子任务 4：补最小回归，验证 patch count 与蛇形顺序未变。

## 9. Edge cases
- 小于 `patch_size` 的图片跳过逻辑不能改变。
- stride 边界不能导致 rows / cols 计算回退。
- 最后一列和奇数行反向顺序不能错位。

## 10. Validation
```text
g++ -std=c++17 -fsyntax-only main/src/infer_workflow.cpp main/src/infer/patch_planner.cpp -Imain/include
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
- [ ] patch planner 已迁出独立实现文件。
- [ ] `auto_snake` patch 顺序和 patch count 行为不变。
- [ ] 新边界有最小回归验证。

## 14. Follow-up
- `06B` 完成后，才能进入 `TASK_REBUILE_06C` 的 output sink / HDMI worker 抽离。
