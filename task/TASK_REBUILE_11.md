# TASK_REBUILE_11: Web 配置编辑合法性校验与板端资源护栏

## 0. Meta
- 阶段：重建期，近期落地。
- 优先级：P0。
- 板端约束关联：强；直接保护 700MB 内存 + 1GHz CPU + ZG330 受限板端。
- 对应 review 建议：主审查新增任务，用于补齐 Web 配置编辑缺少资源护栏这一现实缺口。
- 主责任域：Web 配置编辑、参数语义校验、板端预算保护。
- 依赖前置任务：建议在 `TASK_REBUILE_02` 与 `TASK_REBUILE_03` 之后执行，复用 HTTP 边界和本地配置写回路径。

## 1. Background
- 当前 `main/src/web_console_controller.cpp` 会直接接收 Web 侧传入的 `display.width/height`、`display.fps`、`patch_size`、`stride`、`rd.memory_limit_mb` 等参数，并把它们写入内存态，随后还可能持久化。
- 对 700MB 内存 + 1GHz CPU + ZG330 这类受限板端，这意味着用户可以把参数改到明显超预算或超支持范围，然后把问题推迟到运行时才暴露。
- 这条任务的目标不是做自动调参，而是在 Web 层尽早拒绝明显非法或明显不合理的配置。

## 2. Goal
- 给 Web 配置编辑补上参数语义校验和板端资源护栏。
- 在参数进入内存态和持久化前，拒绝当前不支持或明显超预算的输入。
- 保持现有 Web 协议和页面结构不变，只增强校验和错误反馈。

## 3. Out of scope
- 不改 Web Console 页面结构。
- 不重写配置系统。
- 不把这条任务扩成完整资源调度器。
- 不改 `infer_workflow.cpp`、`rd_imaging_stream.cpp` 的业务执行链路。

## 4. Allowed files to modify
```text
task/TASK_REBUILE_11.md
main/include/workflow/web/web_console_controller.hpp
main/include/workflow/web/web_console_protocol.hpp
main/src/web_console_controller.cpp
main/src/web_console_protocol.cpp
main/src/infer_config.cpp
main/src/rd_config.cpp
main/tests/web_console_settings_validation_test.cpp
main/README.md
```

## 5. Files/modules to avoid
```text
main/src/main.cpp
main/src/web_console.cpp
main/src/web_console_server.cpp
main/src/infer_workflow.cpp
main/src/rd_imaging_stream.cpp
deps/**
```

## 6. Functional requirements
- [ ] `patch_size` 只能接受当前支持值。
- [ ] `stride`、`display.width/height/fps` 有明确合法范围。
- [ ] `rd.memory_limit_mb` 有明确上限和下限。
- [ ] 明显超板端预算的配置在 Web 层直接拒绝，而不是等运行时失败。
- [ ] 非法参数不会写入内存态，也不会被持久化。

## 7. Non-functional requirements
- [ ] 不引入新依赖。
- [ ] 不改变现有 Web 页面结构和交互入口。
- [ ] 校验规则必须和当前板端受限环境一致，不能写成过宽占位逻辑。
- [ ] 错误信息应明确，但不把内部异常细节原样暴露给浏览器。

## 8. Implementation decomposition
- 主任务：给 Web 配置编辑加参数语义校验和板端资源护栏。
- 子任务 1：梳理当前 infer / rd / web 已经支持和明确不支持的参数范围。
- 子任务 2：在 `web_console_controller.cpp` 的设置更新路径上补参数校验。
- 子任务 3：统一错误返回文本，让前端知道是“非法值”还是“超板端预算”。
- 子任务 4：补最小回归测试，验证非法参数不会进入内存态，也不会被持久化。
- 近期：先覆盖最容易把板端拖崩的参数组合。
- 后续：若未来支持更多板型或资源规格，再把护栏做成更可配置化的策略。

## 9. Edge cases
- `patch_size` 不是 512 时如何拒绝。
- `stride <= 0`、显示宽高为负、FPS 极端值如何拒绝。
- `rd.memory_limit_mb` 超出板端可接受范围时如何提示。
- 输入合法但组合不合理时，是否需要组合级校验。

## 10. Validation
```text
g++ -std=c++17 -fsyntax-only main/src/web_console_controller.cpp main/src/web_console_protocol.cpp main/src/infer_config.cpp main/src/rd_config.cpp -Imain/include
g++ -std=c++17 main/tests/web_console_settings_validation_test.cpp -o build_tests/web_console_settings_validation_test.exe
build_tests/web_console_settings_validation_test.exe
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
- [ ] 明显非法或超预算参数会在 Web 层被拒绝。
- [ ] 非法参数不会写入内存态，也不会被持久化。
- [ ] 当前板端受限环境的关键护栏被文档化并回归覆盖。

## 14. Follow-up
- HDMI 显示接口的类型收口已并入 `TASK_REBUILE_10` / `TASK_REBUILE_06` 的后续边界整理，不再单独立文件。
- 如果未来支持更多板型或资源规格，可在本任务建立的规则基础上再做可配置化。
