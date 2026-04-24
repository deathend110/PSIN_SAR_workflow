# Task title

Unify industrial UI framework colors to JSX reference palette

---

## 1. Background

这个任务为什么存在？

- 当前问题：`main/src/infer_workflow.cpp` 的工业风 UI 已经具备结构，但与 `main/src/hdmi_ui_preview_1080_p_industrial.jsx` 的参考配色不完全一致，主要问题是 OpenCV `cv::Scalar(B,G,R)` 与 JSX `#RRGGBB` 颜色顺序不匹配，导致大量框架色相偏反。
- 相关上下文：参考文件为 `main/src/hdmi_ui_preview_1080_p_industrial.jsx`。
- 触发场景：`output.mode=hdmi` 和 `output.mode=png` 使用同一张最终 UI 帧，需要整体风格与 JSX 参考版本一致。
- 已知限制：
  - 只做颜色修正，不动布局和线程模型
  - 不改变 `forward -> waitForReady -> host copy -> device.reset(1)` 顺序
  - 不改变 segmentation mask 的实际类别着色语义

---

## 2. Goal

明确写出要实现的行为。

**必须是可验证的。**

- 统一现有工业风 UI 中与 JSX 参考一一对应的框架色
- 修正所有目标色到正确的 OpenCV BGR 顺序
- 让整体外观与参考 JSX 在背景、面板、边框、文字、badge、小地图和主显示区的配色风格一致
- patch 框改为参考 JSX 的绿色，当前位置点保持红色

---

## 3. Out of scope

明确不做什么。

- 不改布局
- 不改文案
- 不改线程模型
- 不改 patch / output / session 生命周期
- 不改 segmentation mask 实际类别颜色
- 不重构 `composeIndustrialUiFrame(...)` 之外的推理流程

---

## 4. Allowed files to modify

```text
main/src/infer_workflow.cpp
task/TASK_PHASE1_FIX3.md
```

---

## 5. Files/modules to avoid

```text
main/src/main.cpp
main/src/rd_imaging_stream.cpp
main/include/**
deps/**
ARCHITECTURE_TEMPLATE.md
CODEBASE_MAP_TEMPLATE.md
```

---

## 6. Functional requirements

- [ ] 页面外层背景统一为 JSX `#2a2f35`，OpenCV 使用 `cv::Scalar(53, 47, 42)`
- [ ] 主外壳背景统一为 JSX `#737a84`，OpenCV 使用 `cv::Scalar(132, 122, 115)`
- [ ] 主外壳外边框统一为 JSX `#9aa3af`，OpenCV 使用 `cv::Scalar(175, 163, 154)`
- [ ] 主外壳内粗边框统一为 JSX `#5f6772`，OpenCV 使用 `cv::Scalar(114, 103, 95)`
- [ ] 顶栏 / panel header 背景统一为 JSX `#d6dbe2`，OpenCV 使用 `cv::Scalar(226, 219, 214)`
- [ ] 通用 panel 背景统一为 JSX `#eef2f6`，OpenCV 使用 `cv::Scalar(246, 242, 238)`
- [ ] 通用边框统一为 JSX `#a2acb8`，OpenCV 使用 `cv::Scalar(184, 172, 162)`
- [ ] 次级边框统一为 JSX `#97a3b0`，OpenCV 使用 `cv::Scalar(176, 163, 151)`
- [ ] 左栏背景统一为 JSX `#aeb6c0`，OpenCV 使用 `cv::Scalar(192, 182, 174)`
- [ ] 左栏分隔线统一为 JSX `#48505b`，OpenCV 使用 `cv::Scalar(91, 80, 72)`
- [ ] 主标题文字统一为 JSX `#0f172a`，OpenCV 使用 `cv::Scalar(42, 23, 15)`
- [ ] 次级文字统一为 JSX `#475569`，OpenCV 使用 `cv::Scalar(105, 85, 71)`
- [ ] 常规深色文字统一为 JSX `#334155`，OpenCV 使用 `cv::Scalar(85, 65, 51)`
- [ ] 外层默认文字统一为 JSX `#1f2937`，OpenCV 使用 `cv::Scalar(55, 41, 31)`
- [ ] badge 背景统一为 JSX `#f6f8fb`，OpenCV 使用 `cv::Scalar(251, 248, 246)`
- [ ] Running 绿点统一为 JSX `#22c55e`，OpenCV 使用 `cv::Scalar(94, 197, 34)`
- [ ] Running 文字统一为 JSX `#166534`，OpenCV 使用 `cv::Scalar(52, 101, 22)`
- [ ] 小地图主体背景统一为 JSX `#e7ecf2`，OpenCV 使用 `cv::Scalar(242, 236, 231)`
- [ ] 恢复图窗口主体底色统一为 JSX `#edf1f5`，OpenCV 使用 `cv::Scalar(245, 241, 237)`
- [ ] 分割图窗口主体底色统一为 JSX `#dde3ea`，OpenCV 使用 `cv::Scalar(234, 227, 221)`
- [ ] Telemetry / 参数表分隔线统一为 JSX `#c8d0d9`，OpenCV 使用 `cv::Scalar(217, 208, 200)`
- [ ] 小地图 patch 框改为参考绿色 JSX `#86efac`，OpenCV 使用 `cv::Scalar(172, 239, 134)`
- [ ] 小地图当前点改为参考红色 JSX `#ef4444`，OpenCV 使用 `cv::Scalar(68, 68, 239)`
- [ ] 当前点外环继续使用同一红色系近似，不强行模拟 JSX alpha ring
- [ ] segmentation mask 类别颜色保持现有 `classColorBgr(...)` 语义不变

---

## 7. Non-functional requirements

- [ ] 保持最小范围修改
- [ ] 不引入新依赖
- [ ] 不做无关重构
- [ ] 颜色命名清晰，可 review
- [ ] 不影响现有 UI 布局、计数、状态和输出逻辑

---

## 8. Interface expectations

- 允许把当前零散颜色常量整理成 `composeIndustrialUiFrame(...)` 内部的命名色板
- 不新增公共头文件
- 不抽到跨模块公共接口
- `RuntimeState`、`UiRenderContext`、`MiniMapContext` 的语义保持不变

---

## 9. Edge cases

- OpenCV BGR / JSX RGB 顺序混淆
- 同一颜色在不同 panel 中存在近似但不完全相同的参考值
- patch 框和当前点不能再共用同一个颜色变量
- 颜色修正后只允许出现视觉层差异，不应带来布局漂移或文本变化

---

## 10. Validation

```text
1. 静态检查每个目标色是否与 JSX 参考 hex 对应
2. 如环境允许，运行:
   g++ -fsyntax-only "main/src/infer_workflow.cpp" "-Imain/include" "-Ideps/modelzoo_utils/include" "-Ideps/thirdparty/include" "-Ideps/thirdparty/include/Eigen-3.4.0" "-Ideps/thirdparty/include/Eigen-3.4.0/Eigen"
3. 运行 output.mode=png，人工比对生成 UI 图与 JSX 风格
4. 重点核对外层背景、header、panel、badge、地图、restore/seg 主图区
```

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

- 对应部分颜色与 JSX 参考一致
- patch 框为绿、当前点为红
- segmentation mask 语义色未改
- diff 可 review，且仅限颜色统一相关内容

---

## 14. md update

无需同步更新 `ARCHITECTURE_TEMPLATE.md` / `CODEBASE_MAP_TEMPLATE.md`
