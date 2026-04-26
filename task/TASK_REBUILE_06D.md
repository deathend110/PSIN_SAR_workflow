# TASK_REBUILE_06D: 最终编排层收口 infer_workflow.cpp

## 0. Meta
- 阶段：`TASK_REBUILE_06` 子阶段 4。
- 优先级：P1。
- 板端约束关联：在前 3 个边界稳定后，最后只做 orchestration 收口，不改变外部行为。
- 对应总任务：`task/TASK_REBUILE_06.md`
- 主责任域：`infer_workflow.cpp` 顶层流程、阶段日志、路径选择、收尾逻辑。
- 依赖前置任务：建议先完成 `TASK_REBUILE_06A`、`06B`、`06C`。

## 1. Background
- 当前 `infer_workflow.cpp` 即使把 UI render、patch planner、output sink 迁走，仍需要最后一步收口，才能真正变成“编排层”。
- 如果没有 `06D`，前面 3 个阶段可能只是“文件拆开了”，但顶层流程仍然夹杂过多实现细节。
- 这一步必须放在最后，确保前置边界已经稳定，不在最终收口阶段再引入新的跨模块移动风险。

## 2. Goal
- 让 `infer_workflow.cpp` 主要保留：
  - 配置加载与阶段发布
  - SAR 图遍历
  - patch mode 分支选择
  - 顶层 patch 循环
  - stop / finish / error 收尾
- 清理前 3 个子阶段留下的过渡代码，让 orchestration 边界最终成型。

## 3. Out of scope
- 不再新增新的职责拆分主题。
- 不改 Web / main / RD 侧接口。
- 不在这一步优化 `auto_snake` pause/stop 灵敏度。
- 不处理硬件 backend 隔离；那属于 `TASK_REBUILE_10`。

## 4. Allowed files to modify
```text
task/TASK_REBUILE_06D.md
main/src/infer_workflow.cpp
main/include/workflow/infer/infer_workflow.hpp
main/include/workflow/infer/infer_workflow_internal.hpp
main/tests/infer_workflow_regression_test.cpp
```

## 5. Files/modules to avoid
```text
main/src/web_console.cpp
main/src/web_console_server.cpp
main/src/rd_imaging_stream.cpp
deps/**
```

## 6. Functional requirements
- [ ] `infer_workflow.cpp` 以 orchestration 为主，而不是 helper / worker 大杂烩。
- [ ] 顶层阶段日志、snapshot 发布、stop / finish / error 收尾语义不变。
- [ ] `auto_snake` 和 `manual_flight` 的顶层路径选择行为不变。

## 7. Non-functional requirements
- [ ] 不为“更好看”做额外无关重排。
- [ ] 不扩大公开接口。
- [ ] 不破坏前 3 个子阶段的清晰边界。

## 8. Implementation decomposition
- 主任务：在前 3 个子阶段完成后，对 `infer_workflow.cpp` 做最终编排层收口。
- 子任务 1：清理过渡 helper 和遗留内联实现。
- 子任务 2：压缩顶层流程到“阶段 -> 路径 -> 执行 -> 收尾”的可读结构。
- 子任务 3：补最小回归，验证外部行为没有因为收尾整理而改变。

## 9. Edge cases
- stop / finish / error 三种收尾路径不能混淆。
- manual reset 触发条件不能回退。
- HDMI / PNG 模式切换行为不能改变。

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
- [ ] `infer_workflow.cpp` 最终呈现为编排层。
- [ ] stop / finish / error 收尾语义不变。
- [ ] 顶层流程清晰且有最小回归验证。

## 14. Follow-up
- `06D` 完成后，`TASK_REBUILE_10` 才有更稳的 infer 边界可依赖。
