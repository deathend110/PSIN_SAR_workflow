# TASK_REBUILE_01: 在 host/native 测试构建通道上接入现有 smoke/regression tests

## 0. Meta
- 阶段：重建期，近期落地。
- 优先级：P0。
- 板端约束关联：仅 host/native 构建与测试接入，不改变 ZG330 运行语义。
- 对应 review 建议：`PROJ_GPT_REVIEW.md` 的测试接入建议；经主审查调整后，以 `TASK_REBUILE_05` 的近期构建边界为前置。
- 主责任域：host 测试入口、CTest 接入、smoke/regression 测试分层。
- 依赖前置任务：强依赖 `TASK_REBUILE_05` 的近期子阶段先提供 host/native 测试构建通道。

## 1. Background
- 当前仓库已经有 `main/tests/main_menu_regression_test.cpp` 和 `main/tests/hdmi_stopped_regression_test.cpp`，但它们都没有进入正式的 `ctest` 流程。
- 当前 `main/CMakeLists.txt` 仍是板端交叉编译导向，host/native 测试构建边界必须先由 `TASK_REBUILE_05` 提供。
- 这条任务不是“从零建测试体系”，而是在已有 smoke/regression tests 基础上，把它们接到稳定的 host/native 测试入口中。

## 2. Goal
- 把现有 host 侧 smoke/regression tests 纳入 `ctest` 可执行入口。
- 保持现有主菜单与 HDMI 终止态测试语义不变，只补构建与测试分层。
- 把 `main.cpp` 的进一步可测化控制在最小范围内，不把它扩成新的 CLI 重构任务。

## 3. Out of scope
- 不改 RD / Infer / Web Console 的业务行为。
- 不做完整 CLI 框架重写。
- 不引入新依赖或新测试框架。

## 4. Allowed files to modify
```text
task/TASK_REBUILE_01.md
main/CMakeLists.txt
main/src/main.cpp
main/tests/main_menu_regression_test.cpp
main/tests/hdmi_stopped_regression_test.cpp
main/README.md
```

## 5. Files/modules to avoid
```text
main/src/infer_workflow.cpp
main/src/rd_imaging_stream.cpp
main/src/web_console.cpp
main/src/web_console_server.cpp
main/src/web_console_controller.cpp
main/src/config_utils.cpp
deps/**
```

## 6. Functional requirements
- [ ] host/native 测试构建能够生成并运行 `main_menu_regression_test`。
- [ ] host/native 测试构建能够生成并运行 `hdmi_stopped_regression_test`。
- [ ] `ctest` 能发现并执行至少上述两条 smoke/regression tests。
- [ ] 若需要进一步抽出 `main.cpp` 可测 helper，必须保持当前菜单语义不变。

## 7. Non-functional requirements
- [ ] 改动尽量局限在测试入口和最小可测边界。
- [ ] 不增加额外依赖。
- [ ] 测试结果稳定、可重复。
- [ ] 若保留 include `.cpp` 的方式，只作为阶段性兼容手段，不作为长期测试结构。

## 8. Implementation decomposition
- 主任务：在 `TASK_REBUILE_05` 提供的 host/native 构建边界上，把现有 smoke/regression tests 接入 `ctest`。
- 子任务 1：把 `main_menu_regression_test.cpp` 与 `hdmi_stopped_regression_test.cpp` 变成正式测试目标。
- 子任务 2：给测试目标补必要的 include/link 边界，避免再次依赖板端后端。
- 子任务 3：仅在必要时，对 `main.cpp` 做最小可测化收口，不引入新的 CLI 设计。
- 子任务 4：明确 smoke test 与 regression test 的命名与放置习惯，避免后续测试继续散落。
- 近期：先把已有两条测试接入主流程。
- 长期：若后续主入口继续膨胀，再单独开 CLI 可测性重构任务。

## 9. Edge cases
- 输入流 EOF 时应直接退出，不应卡死。
- 非法输入应继续提示菜单，不应改变现有退出码语义。
- `Web Console` 返回值为 0 时，应继续回到主菜单。
- `RD / Infer` 直接返回非 0 时，应保持当前退出行为。

## 10. Validation
```text
cmake --preset host-tests
cmake --build --preset host-tests --target main_menu_regression_test hdmi_stopped_regression_test
ctest --preset host-tests --output-on-failure
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
- [ ] 现有 host smoke/regression tests 已接入 `ctest`。
- [ ] 两条测试都能在 host/native 测试通道中重复执行。
- [ ] 如有 `main.cpp` 最小抽取，菜单语义仍保持不变。

## 14. Follow-up
- `main.cpp` 的进一步可测化只作为本任务子项，不再单独立 rebuild 文件。
- 若后续要扩更多 host 侧行为测试，应先沿用本任务建立的 smoke/regression 入口，不要重新旁路。
