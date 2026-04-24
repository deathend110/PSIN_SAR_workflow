# TASK_PHASE1_FIX2: Remove PATCH_XY from left sidebar & link STRIDE display to config

---

## 1. Background

- `composeIndustrialUiFrame()` 左栏 metrics 列表里有一行 `PATCH_XY`（行 1315），显示当前 patch 的像素坐标，信息与 `GRID_RC` 重复且对操作员无直接价值，增加视觉噪声。
- 下方 status strip 和 footer 中 stride 值硬编码为 `256`（行 1352、1470），实际运行时 `cfg.stride` 可能为 128 或其他值，导致显示与实际不符。

---

## 2. Goal

1. 从左栏 telemetry metrics 中移除 `PATCH_XY` 行。
2. 将 status strip（行 1352）和 footer（行 1470）中的硬编码 `256` 替换为从 `cfg.stride` 或等效运行时值读取。

两处改动均为纯 UI 文本变更，不涉及推理逻辑、patch 扫描顺序或输出语义。

---

## 3. Out of scope

- 不修改 `SnakePatchSource`、`PatchTensorBuilder`、`PatchInferenceRunner` 的逻辑
- 不修改模型输入/输出格式
- 不修改 HDMI 显示驱动或 `RGB565HDMIDisplay`
- 不修改 RD 模块
- 不修改配置解析（`infer_config.cpp`）
- 不重构 UI 合成函数签名（除非传递 stride 必须）

---

## 4. Allowed files to modify

```text
main/src/infer_workflow.cpp
```

---

## 5. Files/modules to avoid

```text
main/src/main.cpp
main/src/rd_imaging_stream.cpp
main/src/config_utils.cpp
main/src/rd_config.cpp
main/src/infer_config.cpp
main/include/**
deps/**
main/CMakeLists.txt
```

---

## 6. Functional requirements

- [ ] 左栏 metrics 不再包含 `PATCH_XY` 行
- [ ] status strip "PATCH RULE" 单元格显示的 stride 值等于实际 `cfg.stride`
- [ ] footer 字符串显示的 stride 值等于实际 `cfg.stride`
- [ ] 其余 metrics（SYSTEM, MODE, ECHO, SAR, PATCH, FRAME, SAR_NAME, GRID_RC, FPS, NPU_MS, TOTAL_MS）保持不变

---

## 7. Non-functional requirements

- [ ] 改动行数 <= 10 行 diff
- [ ] 不引入新依赖或新类型
- [ ] 不改变 `composeIndustrialUiFrame` 的公共签名（如果 stride 可通过 `RuntimeState` 或 `UiRenderContext` 传入则允许新增字段）
- [ ] 能通过 `g++ -fsyntax-only`（Windows 平台无法做完整 CMake 构建）

---

## 8. Interface expectations

stride 传递方案（二选一，推荐 A）：

**A. 通过 `RuntimeState` 新增 `int stride` 字段**

```cpp
struct RuntimeState {
    // ... existing fields ...
    int stride = 256;  // <-- 新增
};
```

在 `processPatch` 或外层循环赋值 `state.stride = cfg.stride;`，`composeIndustrialUiFrame` 内部直接读取 `state.stride`。签名不变。

**B. 在 `composeIndustrialUiFrame` 参数列表末尾加 `int stride`**

侵入性稍大，但也可接受。

---

## 9. Edge cases

- `cfg.stride` 为 128 时，status strip 和 footer 应显示 `128` 而非 `256`
- `cfg.stride` 为 256 时，行为与当前硬编码一致（回归安全）

---

## 10. Validation

```text
g++ -std=c++17 -fsyntax-only -I main/include -I deps/thirdparty/include -I deps/modelzoo_utils/include main/src/infer_workflow.cpp
```

目视确认：diff 只涉及 metrics 列表删行 + 两处字符串硬编码替换。

---

## 11. Required response format before editing

1. 对任务的理解
2. 计划修改的文件
3. 不会修改的文件
4. 实现方案（选 A 或 B）
5. 风险点
6. 验证方案

---

## 12. Required response format after editing

1. 修改了哪些文件
2. 每个文件做了什么改动
3. 为什么这样设计
4. 跑了哪些命令
5. 哪些验证通过 / 未通过
6. 剩余风险

---

## 13. Done when

- [ ] 左栏 metrics 无 `PATCH_XY`
- [ ] status strip stride 数值 == `cfg.stride`
- [ ] footer stride 数值 == `cfg.stride`
- [ ] 其余 UI 元素无变化
- [ ] diff <= 10 行
- [ ] 语法检查通过

---

## 14. md update

需要随代码更新而更新的文件：

- CODEBASE_MAP_TEMPLATE.md（Recent Change Log 追加条目）
