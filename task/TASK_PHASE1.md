# Task title

**第一阶段优先做 HDMI 展示增强**

先不要急着做 Web 控制台。当前主链路已经打通，第一阶段优先把推理结果组织成可直接上板展示的工业风 UI，并让 HDMI 与 PNG 共用同一张最终输出帧。

本阶段重点展示以下信息：

- 项目标题
- 当前模式
- 当前 echo / SAR / patch / frame 编号
- FPS
- NPU 推理耗时
- 总延时
- 分割类别图例
- 左上角小地图区域显示当前输入 SAR 图片的完整略缩图，并用红色框标出当前 patch 位置，使用虚线绘制蛇形扫描路径
- 参考 `main/src/hdmi_ui_preview_1080_p_industrial.jsx` 的布局风格，但图例要明显缩小，主区域优先展示恢复图和分割图

第一阶段实现决策已经锁定：

- 采用单线程方案：`单 patch 推理完成后，再把参数和结果图嵌入 UI，最后统一输出到 HDMI 或保存为 PNG`
- 本阶段不采用“UI 线程 + 模型推理线程”双线程实现
- 不改变当前 `waitForReady -> host copy -> device.reset(1)` 的设备敏感顺序
- 注意:预留双线程的可能,后续验证正常后需要改双线程进行

分割图例颜色继续以现有 `classColorBgr(int cls)` 作为当前阶段语义基准，后续若接入真实类别字典，再单独调整映射：

```cpp
cv::Vec3b classColorBgr(int cls)
{
    static const cv::Vec3b colors[SEG_CLASSES] = {
        cv::Vec3b(255, 0, 0),
        cv::Vec3b(0, 255, 0),
        cv::Vec3b(0, 0, 255),
        cv::Vec3b(255, 255, 0),
        cv::Vec3b(0, 255, 255),
        cv::Vec3b(255, 0, 255)};
    return colors[std::max(0, std::min(cls, SEG_CLASSES - 1))];
}
```

---

## 1. Background

这个任务为什么存在？

- 当前问题：我们已经完成了第 0 阶段，打通全链路流程，也完成了当前工作区的基础模块化。现在进入第一阶段，优先整合 HDMI 展示。
- 相关上下文：参考 UI 设计为 `main/src/hdmi_ui_preview_1080_p_industrial.jsx`。本阶段以该设计为视觉参考，不要求逐像素复刻。
- 触发场景：`output.mode=hdmi` 和 `output.mode=png` 都需要输出同一张 UI 帧。
- 已知限制：
  - 当前 `main/src/infer_workflow.cpp` 主链路是单线程串行模型
  - 当前设备/session 生命周期顺序具有隐式约束，不能为了 UI 展示引入高风险线程改造
  - 当前模型输入输出接口固定，patch 规则固定，不能为了 UI 改动推理链路语义

---

## 2. Goal

明确写出要实现的行为。

**必须是可验证的。**

- 在当前 `Inference only` 主链路中，把“恢复图 + 分割图”升级为完整工业风 UI 帧
- HDMI 输出展示该 UI 帧，而不是纯 side-by-side 结果图
- PNG 输出保存的也是同一张 UI 帧
- UI 必须包含标题栏、小地图、遥测信息、恢复图窗口、分割图窗口、缩小后的图例
- 本阶段只批准单线程“后合成 UI”方案，不引入 UI/推理双线程

---

## 3. Out of scope

明确不做什么。

- 不修改 runtime 模块
- 不修改模型输入格式、模型输出格式和 patch 规则
- 不引入 `deps/modelzoo_utils` 的 actor / queue 框架
- 不恢复单进程 `RD + Inference` 自动串联
- 不实现真实飞控轨迹接入，当前只使用 patch 蛇形扫描路径近似表示路线
- 不实现通用分辨率自适应布局系统
- 不为第一阶段做额外性能优化重构

---

## 4. Allowed files to modify

只列允许改的文件。

- `main/` 下的文件
- `ARCHITECTURE_TEMPLATE.md`
- `CODEBASE_MAP_TEMPLATE.md`
- `task/TASK_PHASE1.md`

---

## 5. Chosen implementation direction

这项决策已经确定，不再二选一：

- 第一阶段采用单线程方案：单 patch 推理完成后，再把结果和运行时参数嵌入 UI，最后统一输出到 HDMI / PNG
- 不引入 UI 线程
- 不引入推理结果队列
- 不把 UI 生成从当前 `processPatch(...) -> sink.write(...)` 边界中拆到独立线程

选择理由：

- 当前推理主链路已经是单线程串行
- `waitForReady -> host copy -> device.reset(1)` 顺序具有设备敏感性
- 第一阶段目标是展示增强，不是执行模型重构
- 单线程后合成更容易保持 scope 可控、review 可控、验证可控

---

## 6. Functional requirements

列成可验收条目：

1. `output.mode=hdmi` 时，输出画面为完整 UI 帧。
2. `output.mode=png` 时，保存的 `io/output/<stem>/patch_*.png` 为完整 UI 帧。
3. UI 顶部必须显示：
   - 项目标题
   - 运行状态
   - 当前模式
   - 输出信息
4. UI 左侧必须显示：
   - 当前 SAR 的完整缩略图
   - 当前 patch 的位置框
   - 按蛇形 patch 顺序绘制的虚线路径
   - 遥测信息区
5. 遥测信息至少包含：
   - SAR 名称
   - patch 序号
   - grid row / col
   - patch x / y
   - FPS
   - RD 耗时
   - NPU 推理耗时
   - 总耗时
6. 主显示区必须保留两个窗口：
   - 恢复图窗口
   - 分割图窗口
7. 分割图例必须使用当前 `classColorBgr(int cls)` 的颜色语义，且面积明显小于参考 JSX 中的初版图例。
8. 小于 `512x512` 的 SAR 图继续沿用当前逻辑跳过处理，不生成异常 UI。
9. 本阶段不得改变 `session.apply()`、`forward()`、`waitForReady()`、`device.reset(1)` 的顺序。

---

## 7. Non-functional requirements

- 保持改动范围集中在推理输出阶段，不扩散到 RD 主流程
- 不引入新的线程、锁、条件变量、任务队列
- 不改变当前异常处理和返回码风格
- UI 合成应保持 review 友好：边界清晰、命名明确、输出语义单一
- 新增 UI 信息应尽量复用现有 `RuntimeState`、`PatchInfo` 和配置数据，不重复创造状态源

---

## 8. Interface expectations

参考的 HDMI UI 设计：

- `main/src/hdmi_ui_preview_1080_p_industrial.jsx`

接口边界要求：

- 允许在推理模块内部新增明确的 UI 合成边界，例如 `UiRenderContext`、`UiFrameComposer` 或等价 helper
- `processPatch(...)` 可以新增小地图上下文入参
- `sink.write(...)` 保持接收最终 `cv::Mat frame_bgr`，不改 `IFrameSink` 抽象
- `PatchInferenceRunner` 接口不改
- `HdmiFrameSink` 仍只负责最终帧的 BGR565 转换与显示
- `PngFrameSink` 仍只负责最终帧落盘

---

## 9. Edge cases

要求 Codex 必须考虑这些：

- SAR 图尺寸不足 `512x512`
- 最后一行/列不足完整 patch，必须继续丢弃，不能强行补边
- `display_width` / `display_height` 不是 1920x1080 时允许有限缩放，但不要求完全响应式
- `output.mode=png` 时必须保证目录结构不变
- `RD only` 模式不接入此 UI
- `SAR 成像耗时` 在 `Inference only` 场景下如无真实值，必须明确显示为 `N/A` 或占位文本，不能伪造数据
- patch 数量较多时，路线绘制和文本布局不能覆盖主图主体
- 图例区域不得遮挡恢复图和分割图的关键显示区域

---

## 10. Validation

运行编译语法验证：

- 对涉及的 `main/` C++ 变更执行 `g++ -fsyntax-only` 级别检查
- 检查 `output.mode=png` 输出文件路径是否仍为 `io/output/<stem>/patch_*.png`
- 检查 HDMI 与 PNG 是否共用同一张 UI 帧
- 检查 patch 位置信息、小地图红框、虚线路径是否与蛇形扫描顺序一致
- 检查文档约束是否与当前真实调用链一致

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

- `Inference only` 模式下，单 patch 推理完成后会生成一张完整 UI 帧
- `hdmi` 与 `png` 两种输出模式展示的是同一套 UI 内容
- 不引入双线程 UI/推理模型
- 不改变模型接口、patch 扫描规则、输出目录语义和设备重置顺序
- 小地图、遥测、恢复图、分割图、图例均已落到最终输出中
- 文档已明确写死第一阶段采用单线程后合成 UI 的方案
