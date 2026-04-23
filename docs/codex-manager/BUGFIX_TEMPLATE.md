# BUGFIX_TEMPLATE.md

> 修 bug 时不要让 Codex 直接“上来就改”。  
> 必须先复现、再定位、再修复、再回归。

---

## 1. Bug title

一句话描述问题。

例如：

- Crash when decoding runtime output mask at image boundary
- Wrong snake-order patch indexing on odd rows
- HDMI overlay flickers under frame drop condition

---

## 2. Symptom

写清楚现象：

- 崩溃 / 输出错误 / 顺序错误 / 性能退化 / 偶发问题
- 发生频率
- 发生条件
- 相关日志 / 报错

---

## 3. Minimal reproduction

尽量缩到最小。

- 输入条件：
- 触发步骤：
- 预期行为：
- 实际行为：

如果有命令，直接写命令。

---

## 4. Suspected root causes

要求 Codex 先列出根因候选，并排序。

示例格式：

1. patch 行列索引在奇数行反转时处理错误
2. 边界条件下最后一列仍被访问
3. reset 后内部状态未恢复完全

---

## 5. Constraints

- 不允许顺手重构其他模块
- 不允许扩大公共接口
- 不允许改变线程模型
- 不允许为了“修复”而关闭校验

---

## 6. Required workflow for Codex

必须按这个顺序做：

1. 复述 bug
2. 找最小复现
3. 列出根因候选并排序
4. 写 failing test
5. 做最小修复
6. 跑回归验证
7. 解释修复影响面

---

## 7. Required output before editing

1. 你如何理解这个 bug
2. 最小复现路径
3. 你怀疑的根因列表（按概率排序）
4. 计划修改的文件
5. 测试方案

---

## 8. Required output after editing

1. failing test 是什么
2. 最终根因是什么
3. 改了哪些文件
4. 修复逻辑是什么
5. 为什么这是最小修复
6. 回归验证结果
7. 仍然未覆盖的风险

---

## 9. Done when

- 能稳定复现的问题不再出现
- 新增或更新了回归测试
- 没有扩大修改范围
- 没有引入新的明显风险
