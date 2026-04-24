# Codex + 复杂 C++ 工程模板包

这套模板用于把复杂 C++ 工程整理成 **Codex 易读、易执行、易审查** 的项目上下文。

## 推荐放置方式

- 仓库根目录：
  - `AGENTS.md`
  - `README_CODEX_WORKFLOW.md`
  - `ARCHITECTURE_TEMPLATE.md`
  - `TASK_TEMPLATE.md`
  - `REVIEW_CHECKLIST.md`
  - `BUGFIX_TEMPLATE.md`
  - `CODEBASE_MAP_TEMPLATE.md`
- 子模块目录（可选）：
  - `src/preprocess/AGENTS.md`
  - `src/runtime/AGENTS.md`
  - `src/display/AGENTS.md`

## 文件作用

- `AGENTS.md`
  - 仓库级持久指导文件。Codex 会优先读取这类文件作为项目约束与默认工作方式。
- `README_CODEX_WORKFLOW.md`
  - 你给自己和团队用的操作说明，定义如何给 Codex 派任务。
- `ARCHITECTURE_TEMPLATE.md`
  - 记录系统边界、模块关系、线程模型、资源所有权。
- `TASK_TEMPLATE.md`
  - 单个任务的标准模板。每次让 Codex 做事，都尽量基于它来下达。
- `REVIEW_CHECKLIST.md`
  - 合并前人工 Review 清单。
- `BUGFIX_TEMPLATE.md`
  - 修 bug 专用模板，强制先复现再修复。
- `CODEBASE_MAP_TEMPLATE.md`
  - 用于沉淀“主调用链 / 数据流 / 类职责 / 风险点”。

## 使用原则

1. 不让 Codex 直接接管整个复杂工程。
2. 只给它 **范围明确、可验证、可回归** 的小任务。
3. 要求它先解释计划，再改代码。
4. 每次改动后都要求它说明：
   - 改了哪些文件
   - 为什么改
   - 风险点
   - 如何验证

## 适用场景

特别适合这些项目：

- 推理后端 / 板端部署 / HDMI 显示
- 图像前后处理流水线
- 多模块 CMake 工程
- 含线程、队列、缓冲区、资源生命周期管理的 C++ 工程
