# TASK_REBUILE_10: 隔离硬件 backend 与业务逻辑边界

## 0. Meta
- 阶段：长期重构，分阶段推进。
- 优先级：P1。
- 板端约束关联：保留 ZG330 设备访问顺序，不破坏板端运行。
- 对应 review 建议：`PROJ_GPT_REVIEW.md` 的 backend 隔离建议。
- 主责任域：推理 backend 抽象、Fake backend、显示适配层边界。
- 依赖前置任务：建议先完成 `TASK_REBUILE_05` 和 `TASK_REBUILE_06`，便于 host 测试和流程拆分。

## 1. Background
- 当前推理流程里硬件 backend 的调用和业务流程控制还耦得很紧。
- 这让 host 测试、假实现注入、以及后续替换 backend 都不够顺手。
- 这是一个长期任务，近期先引入最小接口和适配层，长期再迁移更多实现细节。

## 2. Goal
- 把 backend 访问从业务流程中隔离出来。
- 让 host 可以挂 fake backend 做测试。
- 把显示适配层也收口到更清楚的边界里。
- 保持 ZG330 路径的设备访问顺序与现有行为一致。

## 3. Out of scope
- 不改模型输出协议。
- 不改 RD / Web Console 逻辑。
- 不做整套设备抽象重写。

## 4. Allowed files to modify
```text
task/TASK_REBUILE_10.md
main/include/workflow/infer/infer_backend.hpp
main/include/infer_workflow_hdmi_display.hpp
main/src/infer_backend.cpp
main/src/infer_workflow.cpp
main/tests/infer_backend_regression_test.cpp
main/tests/infer_backend_fake_test.cpp
main/tests/hdmi_display_interface_regression_test.cpp
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
- [ ] backend 访问和业务流程分开。
- [ ] host fake backend 可用于测试。
- [ ] ZG330 backend 仍保持现有调用顺序。
- [ ] 业务逻辑不直接依赖具体硬件实现细节。
- [ ] 显示适配层的接口边界更清楚。

## 7. Non-functional requirements
- [ ] 接口要尽量窄。
- [ ] 不引入 shared ownership，除非确有必要。
- [ ] 不把设备细节泄露到业务层。
- [ ] 这是逐步隔离，不是一次性推倒重来。

## 8. Implementation decomposition
- 主任务：先定义最小 backend / display 适配边界，再把现有实现包进去。
- 子任务 1：把业务层依赖收束到接口而不是具体类。
- 子任务 2：补 fake backend，供 host 测试使用。
- 子任务 3：逐步把 ZG330 与显示细节留在适配实现里。
- 子任务 4：补回归测试，确认顺序和结果不变。
- 近期：先引入薄适配层。
- 长期：后续再把更多硬件交互从业务层移走。

## 9. Edge cases
- backend 初始化失败时应有明确错误。
- fake backend 不应假装成真实设备。
- 业务层不能拿到不该持有的设备句柄。
- 异常路径不能让设备访问顺序失控。

## 10. Validation
```text
g++ -std=c++17 -fsyntax-only main/src/infer_workflow.cpp main/src/infer_backend.cpp -Imain/include
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
- [ ] backend 与业务逻辑有明确边界。
- [ ] host fake backend 可测。
- [ ] ZG330 顺序不变。
- [ ] 显示适配层边界更清楚。

## 14. Follow-up
- HDMI 显示接口的类型与边界表达作为本任务的显示适配层子项处理，不再单独立 rebuild 文件。
- 后续如果要扩更多 backend 类型，再在这个接口层上继续加，不要回退到业务层直连硬件。
