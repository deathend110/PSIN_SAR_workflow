# TASK_REBUILE_05: 重建 host/native 与 ZG330 交叉编译的构建边界

## 0. Meta
- 阶段：重建期，分两阶段推进。
- 优先级：P0。
- 板端约束关联：明确 host/native 与 ZG330/aarch64 边界，不改业务代码语义。
- 对应 review 建议：`PROJ_GPT_REVIEW.md` 的构建系统建议；经主审查后拆成“近期 host/native 测试构建通道”与“长期完整 toolchain/preset/warning profile 收敛”两阶段。
- 主责任域：CMake、host/native 构建开关、交叉编译 toolchain、build 脚本、warning profile。
- 依赖前置任务：无；它为 `TASK_REBUILE_01` 提供前置构建边界。

## 1. Background
- 当前 `main/CMakeLists.txt` 直接硬编码 `Linux + aarch64 + 编译器路径 + ZG330 依赖`，使 host/native 测试构建无法成为一等公民。
- 这条任务不能一步做成完整 build system 重写，必须先提供一个最小的 host/native 测试构建通道，再继续收敛完整 toolchain/preset 边界。
- 当前 review 文档里“测试未接入主构建”的建议，实际上以本任务的近期子阶段为前置。

## 2. Goal
- 近期子阶段：提取 host/native 与 ZG330 构建开关，形成可跑测试的 host/native 构建通道。
- 长期子阶段：收敛完整 toolchain file、CMake presets 和 warning profile 边界。
- 全程不改变现有业务代码语义。

## 3. Out of scope
- 不重构业务代码。
- 不引入新的第三方构建系统。
- 不做全仓库格式化。

## 4. Allowed files to modify
```text
task/TASK_REBUILE_05.md
main/CMakeLists.txt
main/build_main.sh
CMakePresets.json
cmake/toolchains/zg330-aarch64.cmake
main/README.md
main/tests/main_menu_regression_test.cpp
main/tests/hdmi_stopped_regression_test.cpp
```

## 5. Files/modules to avoid
```text
main/src/**
main/include/**
deps/**
```

## 6. Functional requirements
- [ ] 主 CMake 不再默认把所有构建锁死到 aarch64 交叉工具链。
- [ ] host/native 测试构建可单独开启。
- [ ] 交叉编译通过 toolchain file 或 preset 选择。
- [ ] warning profile 明确存在，`-Werror` 作为可控子项而不是默认全局开启。
- [ ] 现有板端构建输出不被无关改动破坏。

## 7. Non-functional requirements
- [ ] 构建边界清晰、可复用。
- [ ] 不影响现有板端交付。
- [ ] 默认配置尽量保守，避免一次性打开全局 `-Werror`。
- [ ] 改动尽量集中在构建层。

## 8. Implementation decomposition
- 主任务：按“05A 近期 / 05B 长期”两阶段重建构建边界。
- 子任务 1（05A）：让主 CMake 能区分 host/native 与 ZG330 交叉构建，不再默认把所有构建都锁死到 aarch64。
- 子任务 2（05A）：给 host/native 测试构建补最小开关和依赖裁剪，支撑 `TASK_REBUILE_01`。
- 子任务 3（05B）：把交叉编译器路径和平台细节迁到 toolchain file / preset。
- 子任务 4（05B）：补 warning profile，并把 `-Werror` 明确做成可控子项，而不是一上来全局打开。
- 近期：先让 host/native 测试通道可跑。
- 长期：再统一 toolchain/preset/warning 规则。

## 9. Edge cases
- Host 上不能误触板端专用编译器路径。
- `-Werror` 不应一开始就打断全部开发流程。
- 测试开启与关闭不应改变产品目标的语义。
- 现有脚本调用方式尽量保留兼容。

## 10. Validation
```text
cmake --preset host-debug
cmake --preset host-tests
cmake --preset zg330-release
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
- [ ] host/native 与 board 构建入口可分离选择。
- [ ] 主 CMake 不再默认硬编码交叉编译器路径。
- [ ] `TASK_REBUILE_01` 所需的 host/native 测试通道已准备好。
- [ ] warning profile 可控，`-Werror` 有独立落点。

## 14. Follow-up
- `TASK_REBUILE_01` 直接复用本任务的近期 host/native 测试通道。
- 后续如果要支持更多板端工具链，可继续扩 preset，不要回写主 CMake。
