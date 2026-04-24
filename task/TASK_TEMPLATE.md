# TASK_TEMPLATE.md

> 每个具体任务复制一份，放到 `TASKS/xxx.md`。  
> 然后把这份文件直接给 Codex 作为主要任务上下文。

---

## Task title

一句话命名任务。

例如：

- Implement snake-order patch scanner for grayscale large images
- Fix out-of-bounds risk in postprocess mask decode
- Add regression tests for runtime NHWC input conversion

---

## 1. Background

这个任务为什么存在？

- 当前问题：
- 相关上下文：
- 触发场景：
- 已知限制：

---

## 2. Goal

明确写出要实现的行为。

**必须是可验证的。**

示例：

- 实现一个 patch 扫描器，按蛇形顺序返回 512x512 patch，stride=256，边缘不足丢弃。

---

## 3. Out of scope

明确不做什么。

示例：

- 不修改 runtime 模块
- 不修改 HDMI 显示逻辑
- 不修改模型输入格式
- 不重构整个 preprocess 目录

---

## 4. Allowed files to modify

只列允许改的文件。

```text
include/preprocess/patch_scanner.h
src/preprocess/patch_scanner.cpp
tests/test_patch_scanner.cpp
```

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

## 6. Functional requirements

列成可验收条目：

- [ ] 输入为单通道灰度图
- [ ] patch size = 512 x 512
- [ ] stride = 256
- [ ] 扫描顺序为蛇形
- [ ] 边缘不足 patch 丢弃
- [ ] 可顺序读取下一个 patch
- [ ] 不要求一次性返回全部 patch

---

## 7. Non-functional requirements

- [ ] 保持最小 patch 范围修改
- [ ] 不引入新依赖
- [ ] 不做多余重构
- [ ] 能通过现有 build
- [ ] 在合理情况下避免整图复制

---

## 8. Interface expectations

给出你希望的接口草案：

```cpp
class PatchScanner {
public:
    PatchScanner(const GrayImageView& image, int patch_w, int patch_h, int stride);
    bool has_next() const;
    Patch next();
    void reset();
};
```

如果接口可以调整，也写清楚哪些部分可变。

---

## 9. Edge cases

要求 Codex 必须考虑这些：

- 图像尺寸刚好等于 patch size
- 图像尺寸小于 patch size
- stride 非法
- 空图
- 宽高不一致
- 最后一行/列边缘不足

---

## 10. Validation

明确要求运行什么验证：

```text
cmake --build build -j
ctest --test-dir build --output-on-failure
```

如果有特定脚本，也写上。

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

需要你随着代码更新而更新的文件：

- ARCHITECTURE_TEMPLATE.md
- CODEBASE_MAP_TEMPLATE.md