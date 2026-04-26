# TASK_REBUILE_08: 评估并收敛配置格式与解析边界（后续可选任务）

## 0. Meta
- 阶段：长期收敛，后续可选。
- 优先级：P2。
- 板端约束关联：仅收敛配置格式边界，不改板端推理流程。
- 对应 review 建议：`PROJ_GPT_REVIEW.md` 的配置系统建议；经主审查后，从“近期修复”降级为“后续可选收敛任务”。
- 主责任域：配置格式边界、解析器支持范围、长期可维护性判断。
- 依赖前置任务：建议先完成 `TASK_REBUILE_03`，把 example/local 配置和写回安全性先收住。

## 1. Background
- 当前 `main/src/config_utils.cpp` 的解析器是手写的简化 YAML，只适合现有标量配置集。
- 但在当前仓库现实里，更紧急的问题已经由 `TASK_REBUILE_03` 负责处理：example/local 配置分离和写回安全性。
- 因此本任务不再承担近期修复角色，而是保留为“如果配置继续膨胀，再决定是否收敛配置格式与解析边界”的后续可选任务。

## 2. Goal
- 明确当前配置系统支持的最小语法边界。
- 评估是否继续维持“简化 YAML”这一受限格式，还是迁移到更窄、更可预测的配置格式。
- 只有在配置复杂度继续上升时，才推进解析器/格式收敛改造。

## 3. Out of scope
- 不覆盖 `TASK_REBUILE_03` 已经承担的写回目标和本地配置落点问题。
- 不默认引入 `yaml-cpp`。
- 不把配置格式扩大成完整 YAML 生态。
- 不重写所有调用方的业务逻辑。

## 4. Allowed files to modify
```text
task/TASK_REBUILE_08.md
main/include/workflow/shared/config_utils.hpp
main/src/config_utils.cpp
main/src/infer_config.cpp
main/src/rd_config.cpp
main/src/web_console_config.cpp
main/README.md
```

## 5. Files/modules to avoid
```text
main/src/infer_workflow.cpp
main/src/rd_imaging_stream.cpp
main/src/web_console_server.cpp
main/src/web_console_controller.cpp
deps/**
```

## 6. Functional requirements
- [ ] 当前配置加载支持的语法边界被明确写出。
- [ ] 是否继续保留简化 YAML 有明确结论。
- [ ] 如果要迁移格式，迁移前提、收益和回滚策略被写清楚。
- [ ] 当前调用方的兼容边界被明确列出。

## 7. Non-functional requirements
- [ ] 不引入新依赖，除非评估结论证明收益明显且交叉编译成本可控。
- [ ] 不把简单配置系统一次性换成大库。
- [ ] 配置边界说明要和文档同步。
- [ ] 不能让这条后续任务反向阻塞近期配置修复。

## 8. Implementation decomposition
- 主任务：把“当前支持什么配置语法”和“未来是否需要格式迁移”讲清楚。
- 子任务 1：梳理 infer / rd / web 三类配置实际使用到的语法子集。
- 子任务 2：明确哪些语法边界是当前解析器故意不支持的。
- 子任务 3：如果配置继续膨胀，评估继续保留简化 YAML 与迁移更窄格式的取舍。
- 子任务 4：只在确有必要时，再规划迁移路径和兼容验证。
- 近期：不执行。
- 长期：仅在配置复杂度继续增长时再推进。

## 9. Edge cases
- 新字段继续增加时，现有解析器是否还能稳定表达。
- 包含 `#`、引号、路径分隔符等字符的值是否会误解析。
- 缺字段时默认值策略是否仍然清楚。
- 非法字符串或布尔值是否明确报错。

## 10. Validation
```text
1. 静态梳理 infer / rd / web 三类配置实际使用到的语法子集
2. 复核 main/src/config_utils.cpp 的注释剥离、key:value 解析和 scope 拼接边界
3. 如果提出迁移方案，必须同时给出 host 构建、交叉编译、回滚成本评估
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
- [ ] 是否继续保留简化 YAML 有明确结论。
- [ ] 配置语法与解析边界被文档化。
- [ ] 若进入迁移阶段，迁移前置条件和回滚路径清楚。

## 14. Follow-up
- `TASK_REBUILE_03` 继续负责 example/local 配置分离和写回安全性。
- 只有当配置复杂度继续上升时，才把本任务从“后续可选”转成真实执行任务。
