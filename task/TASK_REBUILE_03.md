# TASK_REBUILE_03: 分离 example 配置与本地运行时配置，并修正安全写回目标

## 0. Meta
- 阶段：重建期，近期落地。
- 优先级：P1。
- 板端约束关联：仅调整配置文件落点和写回目标，不改板端算法与推理链路。
- 对应 review 建议：`PROJ_GPT_REVIEW.md` 的配置落点建议；经主审查后吸收“写回安全目标”这一半内容。
- 主责任域：配置文件布局、本地副本 bootstrap、写回目标、安全写回策略。
- 依赖前置任务：无；这是当前最小、最直接的配置侧收益项。

## 1. Background
- 当前 `main/configs/*.yaml` 同时承担示例和运行时写回目标，正常运行后会把仓库工作树写脏。
- 当前 `main/src/config_utils.cpp` 的 `WriteTextFileAtomically()` 采用 `remove -> rename`，在失败窗口里存在原文件丢失风险。
- 这条任务的重点不是升级配置格式，而是先把“写到哪里”和“如何安全写”这两个现实风险收住。

## 2. Goal
- 仓库只保留 example 配置。
- 运行时配置写回到本地副本，不再污染仓库跟踪文件。
- 写回目标改成更安全的同目录临时文件替换策略，避免 `remove -> rename` 失败窗口。

## 3. Out of scope
- 不改 RD / Infer / Web 的业务逻辑。
- 不在这个任务里替换成完整 YAML 解析器。
- 不改前端页面表现。

## 4. Allowed files to modify
```text
task/TASK_REBUILE_03.md
main/configs/infer_workflow.example.yaml
main/configs/rd_imaging.example.yaml
main/configs/web_console.example.yaml
main/include/workflow/shared/config_utils.hpp
main/src/config_utils.cpp
main/src/infer_config.cpp
main/src/rd_config.cpp
main/src/web_console_config.cpp
main/src/web_console.cpp
main/README.md
main/Host Computer Software.md
.gitignore
```

## 5. Files/modules to avoid
```text
main/src/infer_workflow.cpp
main/src/rd_imaging_stream.cpp
main/src/web_console_server.cpp
main/src/web_console_controller.cpp
main/src/web_console_assets.cpp
deps/**
```

## 6. Functional requirements
- [ ] 仓库示例配置与本地运行时配置分离。
- [ ] 启动时能从 example 配置 bootstrap 本地副本。
- [ ] 写回只影响本地配置，不改仓库内示例文件。
- [ ] 写回目标改为更安全的同目录替换，不再先删原文件。
- [ ] `LoadConfig / SaveConfig` 的现有字段保持完整。

## 7. Non-functional requirements
- [ ] 配置布局清晰可理解。
- [ ] 不引入新依赖。
- [ ] 配置迁移行为可重复、可回滚。
- [ ] 不把运行时环境信息混进仓库默认值。
- [ ] 变更必须兼顾当前板端离线运行和 Web Console 退出持久化流程。

## 8. Implementation decomposition
- 主任务：把 example 配置、本地运行时配置、写回目标三者边界拆清楚。
- 子任务 1：定义 example 文件命名、本地副本落点和 `.gitignore` 策略。
- 子任务 2：把加载逻辑改成优先读本地副本、缺失时从 example bootstrap。
- 子任务 3：把写回路径固定到本地副本，并修正 `WriteTextFileAtomically()` 的目标替换策略。
- 子任务 4：补最小 round-trip 验证，确认 infer/rd/web 三份配置都能保持字段完整。
- 近期：先消除“仓库配置被写脏”和“失败时丢原文件”这两个现实问题。
- 长期：后续再考虑统一迁移到用户目录或系统配置目录。

## 9. Edge cases
- 本地配置缺失时应能自动生成。
- 本地配置内容损坏时应有明确报错。
- 示例配置缺字段时应保持默认值策略。
- 写回失败时必须尽量保留原文件。

## 10. Validation
```text
g++ -std=c++17 -fsyntax-only main/src/config_utils.cpp main/src/infer_config.cpp main/src/rd_config.cpp main/src/web_console_config.cpp main/src/web_console.cpp -Imain/include
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
- [ ] 仓库示例配置不再承担运行时写回。
- [ ] 本地配置可自动生成并写回。
- [ ] 写回目标更安全，不再先删原文件。
- [ ] 运行后工作树不再因正常保存变脏。

## 14. Follow-up
- `TASK_REBUILE_08` 只再处理“配置格式/解析边界是否继续收敛”的后续问题，不再重复覆盖本任务的写回安全性。
- 若后续需要多用户配置目录，可再单独开任务。
