# Task title

**第一阶段优先做 HDMI 展示增强**
先不要急着做 Web 控制台。你现在主链路已经通了，最先能出效果的是在 HDMI 输出上加：

- 项目标题
- 当前模式
- 当前 echo / SAR / patch / frame 编号
- FPS
- SAR 成像耗时
- NPU 推理耗时
- 总延时
- 分割类别图例

---

## 1. Background

这个任务为什么存在？

- 当前问题：我们已经完成了第0阶段，打通全链路的流程。现在要进行第一阶段，整合HDMI显示
- 相关上下文：
- 触发场景：
- 已知限制：

---

## 2. Goal

明确写出要实现的行为。

**必须是可验证的。**

示例：

- 实现第一阶段，整合HDMI显示
- 只修改HDMI打包部分，同时同步png模块。既png保存的是要推送给HDMI的图片

---

## 3. Out of scope

明确不做什么。

示例：

- 不修改 runtime 模块
- 不修改模型输入格式
- 不重构整个 preprocess 目录

---

## 4. Allowed files to modify

只列允许改的文件。

- main下的文件
- ARCHITECTURE_TEMPLATE.md
- CODEBASE_MAP_TEMPLATE.md

---

## 5. Files/modules to avoid

写清楚不许动的范围。

```text
src/runtime/**
src/display/**
cmake/**
third_party/**
```


---

## 8. Interface expectations

参考的HDMI UI设计：
main\src\hdmi_ui_preview_1080_p_industrial.jsx

如果接口可以调整，也写清楚哪些部分可变。

---

## 9. Edge cases

要求 Codex 必须考虑这些：


---

## 10. Validation

运行编译语法验证：

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


## 14. md update

需要你随着代码更新而更新的MD文件：

- ARCHITECTURE_TEMPLATE.md
- CODEBASE_MAP_TEMPLATE.md