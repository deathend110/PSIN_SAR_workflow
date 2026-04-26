# TASK_DEBUG: Web Console 集成 debug_raster 模式，用于逐 patch 导出 restore / mask_class PNG

## 0. Meta
- 阶段：新增专项调试任务，独立于 `TASK_REBUILE_*` 排期。
- 优先级：P1。
- 板端约束关联：中；目标平台仍是板端，但主要用途是生成可离线比对的 patch 级 `uint8` PNG 调试结果。
- 目标用户：在板端运行量化模型，并与本地 GPU/训练测试集 patch 结果对比的开发者。
- 主责任域：`infer` patch 模式、Web Console 选择项 / 设置项、PNG 调试输出。
- 设计原则：最小改动、最大复用现有 `infer` 和 PNG 输出链，不新增独立 workflow。

## 1. Background
- 当前 `Inference only` / `Web Console` 的推理链只支持：
  - `patch.mode = auto_snake`
  - `patch.mode = manual_flight`
- 当前 PNG 模式只输出一张工业风合成图，路径形如：
  - `output.dir/<sar_stem>/patch_000000.png`
- 这不适合做“GPU 浮点模型 vs 板端量化模型”的 patch 级效果对比，因为：
  - patch 扫描顺序和训练/测试集逻辑不一致
  - 不能分别拿到模型恢复图和 `mask RGB`
  - 合成 UI 图会干扰后续数值和视觉比对
- 用户需要一个板端可控的 debug 模式：
  - 集成到现有 Web Console
  - 复用现有底图选择逻辑
  - 输出固定为 PNG
  - 每个 patch 分别落 `restore` 和 `mask_class`
  - patch 扫描逻辑独立于 `auto_snake`，改成“左到右、上到下、逐行扫描”
  - 行列步长参数继续使用像素值，保持与现有 Web 控制台 patch 参数风格一致

## 2. Goal
- 在现有 `infer` workflow 内新增一个 Web 可选的 `debug_raster` patch mode。
- `debug_raster` 下：
  - patch 扫描顺序固定为左到右、上到下、逐行扫描
  - 输出固定为 PNG，不走 HDMI
  - 每个 patch 自动落两张 `uint8` 图：
    - `restore`
    - `mask_class`
- 输出目录固定为：
  - `output_dir/debug_<sar_stem>/restore/patch_000000.png`
  - `output_dir/debug_<sar_stem>/mask_class/patch_000000.png`
- 复用现有底图选择、模型加载、patch 推理、`restoreToGrayU8()`、PNG 落盘链路；mask 直接输出原始 `1~6` 类别图，不再做 RGB 可视化。

## 3. Out of scope
- 不新增主菜单顶层 workflow，例如不新增独立 `Debug only` 菜单模式。
- 不改 `manual_flight` 语义。
- 不改 `auto_snake` 语义。
- 不做整图拼接，不输出 stitched full-image。
- 不改变现有 HDMI 模式的显示逻辑。
- 不把这条任务扩成新的 backend 抽象或新的测试平台。
- 不改模型输入尺寸约束；`patch_size` 仍保持现有工程支持值。

## 4. Allowed files to modify
```text
task/TASK_DEBUG.md
main/include/workflow/shared/app_mode.hpp
main/include/workflow/infer/infer_config.hpp
main/include/workflow/infer/infer_workflow_internal.hpp
main/include/workflow/web/web_console_controller.hpp
main/src/infer_config.cpp
main/src/infer/patch_planner.cpp
main/src/infer/output_sink.cpp
main/src/infer_workflow.cpp
main/src/web_console_controller.cpp
main/src/web_console_protocol.cpp
main/src/web_console_assets.cpp
main/tests/infer_workflow_regression_test.cpp
main/tests/web_console_settings_validation_test.cpp
main/Host Computer Software.md
main/README.md
main/configs/infer_workflow.example.yaml
```

## 5. Files/modules to avoid
```text
main/src/main.cpp
main/src/web_console_server.cpp
main/src/rd_imaging_stream.cpp
main/src/rd_config.cpp
deps/**
cmake/**
```

## 6. Functional requirements
- [ ] Web Console 中可选择新的 `patch_mode = debug_raster`。
- [ ] `debug_raster` 仍复用普通 `infer` 的底图选择逻辑。
- [ ] `debug_raster` 的 patch 扫描顺序固定为：
  - 左到右
  - 上到下
  - 一行扫完再到下一行
- [ ] `debug_raster` 不复用 `auto_snake` 的蛇形左右翻转逻辑。
- [ ] `debug_raster` 使用独立的 X/Y 步长像素参数，而不是沿用单一 `stride`。
- [ ] `debug_raster` 下输出固定为 PNG，不允许 HDMI。
- [ ] 每个 patch 自动落盘两张 `uint8` 图：
  - `restore`
  - `mask_class`
- [ ] 输出目录固定为：
  - `output_dir/debug_<sar_stem>/restore/patch_000000.png`
  - `output_dir/debug_<sar_stem>/mask_class/patch_000000.png`
- [ ] `restore` 图应直接复用 `restoreToGrayU8()` 的结果，不再合成 UI。
- [ ] `mask_class` 图应输出单通道 `uint8` 原始类别图，像素值直接表达 `1~6`，不再做 RGB 可视化，也不再合成 UI。
- [ ] 不要求输出整图拼接结果。

## 7. Non-functional requirements
- [ ] 保持最小修改范围，优先新增小边界而不是重写推理主链。
- [ ] 复用已有 `collectSarImages()`、`loadSarImageNorm()`、`PatchTensorBuilder`、`PatchInferenceRunner`、PNG 路径组织代码。
- [ ] 不引入新依赖。
- [ ] 不改变当前 `auto_snake` / `manual_flight` / HDMI / 普通 PNG 模式语义。
- [ ] 文档必须明确区分“已实现的当前模式”和“新增的 debug 模式”。

## 8. Interface expectations
建议以“新增 patch mode + 新增 debug 参数”的方式接线，而不是新增独立 workflow。

### 8.1 Patch mode
建议新增：

```text
debug_raster
```

位置：

- Web selection / settings
- `infer` 配置
- `infer` patch source 选择分支

### 8.2 Debug 参数
建议在 `infer` 配置中新增：

```text
infer.pipeline.debug.stride_x_px
infer.pipeline.debug.stride_y_px
```

或等价 YAML：

```yaml
pipeline:
  debug:
    stride_x_px: 128
    stride_y_px: 128
```

要求：
- 使用像素值
- 与现有 Web 控制台的 patch 参数风格一致
- 首版不做百分比覆盖率参数

### 8.3 输出路径
建议固定落盘结构：

```text
output.dir/debug_<sar_stem>/restore/patch_000000.png
output.dir/debug_<sar_stem>/mask_class/patch_000000.png
```

`<sar_stem>` 直接复用现有 SAR 输入文件 stem。

## 9. Edge cases
- 输入图尺寸刚好等于 `patch_size`
- 输入图尺寸小于 `patch_size`
- `debug_stride_x_px <= 0` 或 `debug_stride_y_px <= 0`
- `debug_stride_x_px > image_width` 或 `debug_stride_y_px > image_height`
- 最后一行/列边缘不足完整 patch 时，是否丢弃
- `debug_raster` 被选中时，若用户仍把 `output.mode` 设成 `hdmi`，系统应如何强制纠正或拒绝
- 多张 SAR 图目录输入时，目录组织是否稳定、不会互相覆盖
- `restore` / `mask_class` 任意一张写盘失败时，错误是否能明确暴露

## 10. Validation
```text
cmake --build build/ZG -j
ctest --test-dir build/main-host --output-on-failure
```

建议补充的最小验证：

```text
1. Web Console 能选择 debug_raster
2. 设置页面能编辑 debug stride_x_px / stride_y_px
3. debug_raster 下实际输出为：
   output_dir/debug_<sar_stem>/restore/patch_000000.png
   output_dir/debug_<sar_stem>/mask_class/patch_000000.png
4. patch 顺序为左到右、上到下、逐行扫描
5. 输出图 dtype 为 uint8
6. auto_snake / manual_flight 行为未回退
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
- [ ] Web Console 中可以选择 `debug_raster`
- [ ] `debug_raster` 使用独立的逐行扫描 patch 逻辑
- [ ] 行/列步长使用像素值参数
- [ ] 每个 patch 会分别输出 `restore` 与 `mask_class` 两张 `uint8` PNG
- [ ] 输出目录严格符合：
  - `output_dir/debug_<sar_stem>/restore/patch_000000.png`
  - `output_dir/debug_<sar_stem>/mask_class/patch_000000.png`
- [ ] 普通 `infer`、`auto_snake`、`manual_flight`、HDMI 模式不回退
- [ ] 文档与配置模板同步更新

## 14. Follow-up
- 若后续需要与训练/测试集 patch 逻辑更强对齐，可继续扩展：
  - 边缘补齐策略
  - patch 命名附带行列坐标
  - 与 GPU 输出对齐的批量对比脚本
- 若将来还要导出整图拼接结果，应单独立新任务，不要把本任务扩成 full-image stitching。
