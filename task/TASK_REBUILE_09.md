# TASK_REBUILE_09: 将 ManualFlight 运行时状态从隐式全局改为显式对象

## 0. Meta
- 阶段：中近期收口，作为 infer 编排层收窄前置。
- 优先级：P1。
- 板端约束关联：不改手动飞行行为，只改状态持有方式。
- 对应 review 建议：`PROJ_GPT_REVIEW.md` 的隐式全局状态建议；经主审查后前移为 infer 收口前置任务。
- 主责任域：ManualFlight 状态、生命周期、测试可见性。
- 依赖前置任务：建议在 `TASK_REBUILE_05` 提供 host/native 测试通道后尽早执行；本任务是 `TASK_REBUILE_06` 的前置。

## 1. Background
- 现在 `main/src/infer_workflow.cpp` 里有 `ManualFlightSharedState` 和函数级静态访问方式。
- 这能跑，但可测试性和生命周期透明度都一般。
- 这是当前 `infer_workflow.cpp` 里最适合先切出的窄问题之一，应早于整体编排层收窄执行。

## 2. Goal
- 把 ManualFlight 的运行时状态收敛到显式对象。
- 让配置、暂停、提交、快照都走明确生命周期。
- 使测试可以构造多个独立实例，而不是依赖单个静态全局。

## 3. Out of scope
- 不改手动飞行的用户语义。
- 不改 patch 路由规则。
- 不做线程模型大改。

## 4. Allowed files to modify
```text
task/TASK_REBUILE_09.md
main/src/infer_workflow.cpp
main/include/workflow/infer/infer_workflow.hpp
main/include/workflow/infer/manual_flight_runtime.hpp
main/src/manual_flight_runtime.cpp
main/tests/manual_flight_runtime_test.cpp
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
- [ ] ManualFlight 状态持有方式显式化。
- [ ] `reset / pause / submit / snapshot` 的生命周期更清楚。
- [ ] 测试可以构造独立运行时对象。
- [ ] 现有手动飞行行为保持不变。

## 7. Non-functional requirements
- [ ] 不引入裸 owning pointer。
- [ ] 线程安全边界要清楚。
- [ ] 不把状态继续散落到函数静态变量里。
- [ ] 这一步是阶段性收敛，不是最终大重写。

## 8. Implementation decomposition
- 主任务：先把 `ManualFlightSharedState` 包成显式 runtime 对象。
- 子任务 1：把函数静态状态迁入对象字段。
- 子任务 2：保留现有调用链，逐步替换访问方式。
- 子任务 3：补对象生命周期测试。
- 子任务 4：为 `TASK_REBUILE_06` 继续收窄编排层预留边界。
- 中近期：先完成对象化且不改外部行为。
- 后续：再消灭旧的函数级全局入口。

## 9. Edge cases
- 重置时不能丢失当前方向或路径信息。
- 暂停和恢复不能引入竞态。
- 多次创建 / 销毁不应互相污染。
- 异常路径之后状态要能回到干净态。

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
- [ ] ManualFlight 状态不再依赖隐式全局。
- [ ] 生命周期和测试可见性更清楚。
- [ ] 行为保持一致。

## 14. Follow-up
- `TASK_REBUILE_06` 默认以前置完成本任务为条件，再继续收窄 `infer_workflow.cpp`。
