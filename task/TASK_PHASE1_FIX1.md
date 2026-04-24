# TASK_PHASE1_FIX1

## Summary

本修订基于当前已经实现的工业风 UI 版本，只做严格受控的减法修改。

目标：

- 删除用户明确点名的 6 处视觉元素
- 不修改线程模型
- 不修改推理流程
- 不修改输出语义
- 不修改未被点名的其它 UI 内容

核心原则：

- 只删，不顺手重做
- 只改 UI 合成层
- 不改 `forward -> waitForReady -> host copy -> device.reset(1)` 顺序

---

## Required UI changes

必须删除以下 6 项：

1. 删除最左上角的 `SAR` 字样和白色小方块
2. 删除顶部大号标题 `PSIN WORKFLOW`
3. 删除小地图上的标题 `GLOBAL MAP`
4. 删除小地图下面的 `CENTER (x, y)` 和长方形白框
5. 删除左边栏的大号标题 `TELEMETRY`
6. 删除小地图上的轨迹线

---

## Keep unchanged

以下内容默认保留，不得擅自修改：

- 顶部小号副标题 `EDGE AI CONTROL TERMINAL`
- 状态 badge、模式 badge、输出 badge
- 小地图中的 SAR 缩略图
- 小地图中的当前 patch 红框和当前位置红点
- telemetry 指标内容和排序
- 恢复图窗口
- 分割图窗口
- 图例
- 底部状态条
- HDMI / PNG 共用同一张最终 UI 帧的语义
- patch 规则、输出路径、设备生命周期

---

## Allowed files to modify

- `main/src/infer_workflow.cpp`
- `task/TASK_PHASE1_FIX1.md`
- `CODEBASE_MAP_TEMPLATE.md`
- `ARCHITECTURE_TEMPLATE.md`

说明：

- `ARCHITECTURE_TEMPLATE.md` 仅在存在直接失真描述时才允许最小同步
- 如果没有直接失真，不强制修改

---

## Constraints

- 不新增线程、锁、条件变量、任务队列
- 不修改 `MiniMapContext`、`UiRenderContext`、`IFrameSink` 的对外语义，除非删除目标元素必须做最小字段瘦身
- 不改 `output.mode=hdmi` / `output.mode=png` 的输出语义
- 不改 `io/output/<stem>/patch_*.png` 路径
- 不改 patch 规则、主图窗口、图例、状态条、遥测内容、颜色映射
- 不做与本次 6 项删减无关的布局重排

---

## Validation

- 确认 `main/src/infer_workflow.cpp` 中不再绘制：
  - `SAR`
  - `PSIN WORKFLOW`
  - `GLOBAL MAP`
  - `CENTER ...`
  - `TELEMETRY`
  - 小地图轨迹线
- 确认小地图仍保留：
  - SAR 缩略图
  - patch 红框
  - 当前点红点
- 确认 HDMI 和 PNG 仍使用同一张最终 UI 帧
- 确认 diff 没有扩散到与本修订无关的模块

---

## Done when

- 以上 6 个元素全部删除
- 未被点名的 UI 部分保持不变
- 线程模型和推理主链路不变
- 文档中已经补充本次 fix 要求
