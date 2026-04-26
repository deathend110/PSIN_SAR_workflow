# TASK_REBUILE_04: 校正文档与真实运行行为的漂移

## 0. Meta
- 阶段：重建期，近期落地。
- 优先级：P1。
- 板端约束关联：仅校正文档，不改变板端运行逻辑。
- 对应 review 建议：`PROJ_GPT_REVIEW.md` 的 P2-8，避免文档继续误导后续维护。
- 主责任域：README、阶段任务文档、运行说明。
- 依赖前置任务：建议在 `TASK_REBUILE_01` 到 `TASK_REBUILE_03` 的事实边界明确后再做最终对齐。

## 1. Background
- 现有文档里有一些描述已经和当前运行行为不一致。
- 近期任务是把文档改成“以当前代码事实为准”。
- 长期如果行为再变化，应同步更新文档，不再让旧描述漂移。

## 2. Goal
- 让文档中的菜单流程、Web Console 输出、配置写回、HDMI 终止行为与当前代码一致。
- 清理过时的架构性描述，避免继续引用已经不成立的旧前提。
- 保持文档风格与现有 `task/TASK_PHASE*.md` 一致。

## 3. Out of scope
- 不改业务代码。
- 不做架构重写。
- 不补新功能。

## 4. Allowed files to modify
```text
task/TASK_REBUILE_04.md
main/README.md
main/Host Computer Software.md
task/TASK_PHASE1.md
task/TASK_PHASE1_FIX1.md
task/TASK_PHASE1_FIX2.md
task/TASK_PHASE1_FIX3.md
task/TASK_PHASE1_FIX4.md
task/TASK_PHASE2.md
task/TASK_PHASE2_FIX1.md
task/TASK_PHASE3.md
task/TASK_PHASE3_FIX1.md
task/TASK_PHASE3_FIX2.md
task/TASK_PHASE3_FIX3.md
task/TASK_PHASE3_FIX4.md
task/TASK_PHASE4.md
task/TASK_PHASE4_FIX1.md
task/TASK_PHASE4_FIX2.md
task/TASK_module.md
```

## 5. Files/modules to avoid
```text
main/src/**
main/include/**
deps/**
```

## 6. Functional requirements
- [ ] 文档对主菜单行为的描述与 `main/src/main.cpp` 一致。
- [ ] 文档对 Web Console 输出与配置写回的描述与运行事实一致。
- [ ] 文档对 HDMI 终止态行为的描述不再与当前代码冲突。
- [ ] 删除或改写已经失效的旧架构前提。

## 7. Non-functional requirements
- [ ] 仅改文档，不碰源码。
- [ ] 文风保持和现有任务文档一致。
- [ ] 不重新定义项目架构。
- [ ] 保留后续可持续更新的写法。

## 8. Implementation decomposition
- 主任务：逐份对齐 README 和阶段任务文档。
- 子任务 1：核对当前代码事实，再修正文档表述。
- 子任务 2：统一术语与路径写法。
- 子任务 3：删掉已经过期的“应该如此”式描述。
- 近期：先修最明显的漂移点。
- 长期：后续如果代码继续变化，文档同步跟进。

## 9. Edge cases
- 文档与代码不同步时，以代码事实为准。
- 不要把“未来计划”写成“当前已实现”。
- 不要把板端约束误写成 host 约束。
- 不要把已知的临时实现写成长期架构承诺。

## 10. Validation
```text
rg -n "Web Console|STOPPED|board_ip|main menu|SaveConfig|LoadConfig" main/README.md main/Host Computer Software.md task/TASK_PHASE*.md task/TASK_module.md
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
- [ ] 文档与当前运行行为一致。
- [ ] 不再保留明显失真的旧描述。
- [ ] 文档风格与现有任务集统一。

## 14. Follow-up
- 后续如果 `TASK_REBUILE_05` 或 `TASK_REBUILE_08` 改了构建 / 配置行为，要同步回写相关说明。
