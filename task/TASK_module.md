# Task title

- 为了便于后续的扩展，降低代码复杂度，将main\src\full_workflow.cpp和main\src\rd_imaging_stream.cpp两个文件中的各种模块拆分出来，写成源文件.h，.hpp形式，然后主文件用一个main打包
- 主文件应该有一个简单的终端控制台，用于控制下一步运行RD成像还是推理工作流
- 注意后续模块的扩展接口预留： main\Host Computer Software.md

---

## 1. Background

这个任务为什么存在？

- 主要代码main\src\full_workflow.cpp和main\src\rd_imaging_stream.cpp过长，拆分为main函数文件和源文件便于阅读
- 目前只有我们打通了整体工作流，但是由于内存原因，RD成像和推理工作流必须分开。这个任务是把这两个任务合并打包
- 注意：必须保证两个任务是隔离的
---

## 2. Goal

明确写出要实现的行为。

**必须是可验证的。**
**模块化后的逻辑是不变的**
**整体代码是留有扩展的**

---

## 3. Out of scope

明确不做什么。


---

## 4. Allowed files to modify

只列允许改的文件。

- main下所有
- ARCHITECTURE_TEMPLATE.md
- CODEBASE_MAP_TEMPLATE.md

---

## 6. Functional requirements

列成可验收条目：
具体由你规划

---

## 7. Non-functional requirements

- 保持整体逻辑不变
- 不引入新依赖
- 不做多余重构
- 保证性能优化

---

## 8. Interface expectations

自己考虑

---

## 9. Edge cases

要求 Codex 必须考虑这些：
自己考虑
---

## 10. Validation

明确要求运行什么验证：

自己考虑


---

## 11. Required response format before editing

要求 Codex 在动手前先输出：

1. 对任务的理解
2. 计划修改的文件
3. 不会修改的文件
4. 实现方案
5. 风险点
6. 验证方案

---

## 12. Required response format after editing

要求 Codex 改完后输出：

1. 修改了哪些文件
2. 每个文件做了什么改动
3. 为什么这样设计
4. 跑了哪些命令
5. 哪些验证通过 / 未通过
6. 剩余风险

---

## 13. Done when

写成客观验收标准：

- patch 顺序与预期一致
- 边缘不足不返回
- 测试通过
- 修改范围符合约束
- diff 可 review
