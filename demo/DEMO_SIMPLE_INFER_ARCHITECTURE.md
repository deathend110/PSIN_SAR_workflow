# Demo Simple Infer Architecture

## 1. 目标

本文档定义 `demo/` 目录下一个“纯净、简洁、独立”的离线推理小工程。

它的职责只有：

- 读取一个极简配置文件
- 遍历输入目录下的图片
- 对每张合法 `512x512` 图像执行一次模型推理
- 输出恢复图 `restore.png`
- 输出类别 mask 图 `mask_class.png`

`demo` 与 `main` 的关系是：

- 只参考 `main` 中 `debug_raster` 路径的输出语义
- 不复用 `main/src/**` 和 `main/include/**` 的源码
- 不进入 `workflow`、`Web Console`、`HDMI`、`manual_flight` 等体系

本 demo 是一个逻辑上与 `main` 无关、源码上与 `main` 解耦、工程上独立构建的小程序。

---

## 2. 行为参考边界

`demo` 只参考如下行为语义，不参考 `main` 的整体架构：

- `debug_raster` 输出恢复图与 class mask 的思路，参考 `main/src/infer/output_sink.cpp:215`
- `debug_raster` 作为“每个输入单元各自产生恢复结果和 mask 结果”的行为，参考 `main/src/infer_workflow.cpp:693`

明确不参考、不复制的部分：

- `workflow::infer::Run(...)` 的整体 workflow 控制流
- `WorkflowRunControl`
- `WebConsoleController`
- `WebConsoleServer`
- `HdmiFrameSink`
- `LatestSnapshotMailbox`
- `HdmiRenderWorker`
- `manual_flight`
- UI 合成与工业风界面绘制

---

## 3. 非目标

本 demo 明确不负责：

- patch 切分与多 patch 扫描
- Web 控制台
- HDMI 显示
- 多线程渲染
- 运行态 pause/resume/stop/reset
- 运行时状态快照发布
- 彩色可视化 mask 输出
- RD 成像
- 多模型调度
- 训练、量化、导出

如果未来需要这些能力，应在 `demo` 外另起任务，不在本次最小 demo 中扩张。

---

## 4. 输入输出契约

### 4.1 输入

配置文件最小字段：

- `input_dir`
- `output_dir`
- `json_path`
- `raw_path`

输入图片要求：

- 位于 `input_dir`
- 支持常见静态图片格式，例如 `.png`、`.jpg`、`.jpeg`、`.bmp`
- 尺寸必须严格为 `512x512`

尺寸非法处理策略：

- 跳过该图片
- 记录日志
- 不中断整个批处理

### 4.2 输出

对于每张合法输入图 `<stem>`，输出目录约定为：

```text
output_dir/
  <stem>/
    restore.png
    mask_class.png
```

输出格式约定：

- `restore.png`
  - 单通道灰度图
  - `uint8`
- `mask_class.png`
  - 单通道类别图
  - `uint8`
  - 像素值直接表达类别 `1~6`

不输出：

- 彩色 mask
- UI 合成图
- patch 序列图
- 额外中间结果

---

## 5. 建议目录结构

建议 `demo/` 保持如下最小目录结构：

```text
demo/
  CMakeLists.txt
  README.md
  DEMO_SIMPLE_INFER_ARCHITECTURE.md
  configs/
    demo_infer.yaml
  include/
    demo_config.hpp
    image_collector.hpp
    model_runner.hpp
    result_writer.hpp
  src/
    demo_main.cpp
    demo_config.cpp
    image_collector.cpp
    model_runner.cpp
    result_writer.cpp
```

说明：

- `demo` 有自己的 `CMakeLists.txt`
- `demo` 不从 `main/CMakeLists.txt` 借模块边界
- `demo` 可以在构建层面复用仓库已有 `deps/**` 和第三方库路径

---

## 6. 模块划分

### 6.1 `DemoConfig`

文件：

- `demo/include/demo_config.hpp`
- `demo/src/demo_config.cpp`

职责：

- 读取 demo 自己的 yaml
- 保存最小配置快照
- 校验关键字段非空

建议结构：

```cpp
struct DemoConfig {
    std::filesystem::path input_dir;
    std::filesystem::path output_dir;
    std::filesystem::path json_path;
    std::filesystem::path raw_path;
};
```

边界约束：

- 不引入 `device_url`
- 不引入 `run_backend`
- 不引入 `display.*`
- 不引入 `patch_*`
- 不引入 Web / RD / workflow 相关字段

### 6.2 `ImageCollector`

文件：

- `demo/include/image_collector.hpp`
- `demo/src/image_collector.cpp`

职责：

- 枚举 `input_dir` 下图片
- 根据扩展名筛选
- 稳定排序
- 读取灰度图
- 校验是否为 `512x512`

边界约束：

- 不做 patch planner
- 不做复杂递归控制
- 不做图像缩放或自动修正

非法图像策略：

- 读失败：记录并跳过
- 尺寸不等于 `512x512`：记录并跳过

### 6.3 `ModelRunner`

文件：

- `demo/include/model_runner.hpp`
- `demo/src/model_runner.cpp`

职责：

- 加载模型 `json/raw`
- 初始化 device 与 session
- 校验模型输入输出 shape
- 对单张 `512x512` 灰度图执行推理
- 后处理生成：
  - `restore_gray`
  - `mask_class`

建议接口：

```cpp
struct InferenceResult {
    cv::Mat restore_gray;
    cv::Mat mask_class;
};

class ModelRunner {
public:
    explicit ModelRunner(const DemoConfig& cfg);
    InferenceResult Run(const cv::Mat& gray_512);
};
```

边界约束：

- 只处理单张完整输入图
- 不维护 batch workflow 状态机
- 不暴露 pause/stop/reset
- 不处理 HDMI / UI

### 6.4 `ResultWriter`

文件：

- `demo/include/result_writer.hpp`
- `demo/src/result_writer.cpp`

职责：

- 创建每张图的输出子目录
- 写 `restore.png`
- 写 `mask_class.png`

边界约束：

- 不写彩色 mask
- 不写额外日志文件
- 不写 patch 命名序列

---

## 7. 主调用链

建议 `demo` 的真实调用链为：

```text
main()
 -> LoadDemoConfig(config_path)
 -> EnumerateInputImages(input_dir)
 -> ModelRunner runner(cfg)
 -> for each image
    -> LoadGrayImage(path)
    -> ValidateImageIs512x512(gray)
       -> invalid: log + continue
    -> runner.Run(gray)
       -> build input tensor
       -> session.forward
       -> postprocess restore_gray
       -> postprocess mask_class
    -> WriteOutputs(output_dir/<stem>/restore.png, mask_class.png)
 -> return
```

核心特点：

- 单线程
- 单图输入
- 单图推理
- 单图双结果落盘

---

## 8. 数据流

### 8.1 单图推理数据流

```text
input image file
 -> cv::imread(gray)
 -> validate 512x512
 -> normalize / build input tensor
 -> session.forward
 -> restore output tensor
 -> seg logits tensor
 -> restore_gray(uint8)
 -> mask_class(uint8, 1~6)
 -> disk
```

### 8.2 目录批处理数据流

```text
input_dir
 -> enumerate files
 -> filter image files
 -> sort
 -> per file infer
 -> output_dir/<stem>/restore.png
 -> output_dir/<stem>/mask_class.png
```

---

## 9. 线程与资源生命周期

### 9.1 线程模型

本 demo 只使用主线程。

明确不创建：

- Web thread
- workflow worker thread
- HDMI render thread
- manual runtime thread

理由：

- demo 目标只是离线目录推理
- 多线程只会增加复杂度和调试成本

### 9.2 模型资源生命周期

建议生命周期：

```text
program start
 -> load config
 -> open device
 -> load network
 -> init session
 -> apply session
 -> process all valid images
 -> close device
 -> program exit
```

原则：

- 模型与 session 初始化一次
- 目录内所有图片复用同一个 runner
- 不对每张图重复加载模型

### 9.3 文件资源生命周期

单图处理时：

- 读取源图片
- 生成内存中的 `restore_gray` 和 `mask_class`
- 写入磁盘
- 释放临时 `cv::Mat`

不引入：

- scratch 目录
- 持久化中间二进制文件
- mailbox / snapshot 缓冲

---

## 10. 配置文件草案

建议 demo 的 yaml 结构如下：

```yaml
demo:
  input_dir: ./io/demo_input
  output_dir: ./io/demo_output
  json_path: ./models/model.json
  raw_path: ./models/model.raw
```

约束：

- `input_dir` 必须存在
- `output_dir` 不存在时可自动创建
- `json_path` 和 `raw_path` 必须存在

---

## 11. 错误处理策略

### 11.1 启动级错误

以下错误应直接失败并退出：

- 配置文件不存在
- 配置字段缺失
- `json_path/raw_path` 不存在
- 模型加载失败
- session 初始化失败
- 输入输出 shape 校验失败

### 11.2 单图级错误

以下错误应跳过当前图片并继续：

- 图片无法读取
- 图片不是灰度可读格式
- 图片尺寸不是 `512x512`
- 单张图推理或写文件失败

日志应包含：

- 图片路径
- 失败原因

---

## 12. 构建边界

### 12.1 与 `main` 的构建关系

`demo` 构建上可以复用仓库的第三方依赖环境，但逻辑上独立于 `main`。

允许复用：

- `deps/**`
- 已有第三方 include / link 目录
- 已有 toolchain 设定方式

不允许依赖：

- `main/src/**`
- `main/include/**`
- `main` 的 workflow 模块链接产物

### 12.2 代码组织原则

- 如果某段逻辑来自 `main` 的思路，应在 `demo` 中重新最小实现
- 不为了“复用”而把 `main` 的复杂状态机带入 demo
- 保持每个文件职责单一

---

## 13. 验证计划

后续实现完成后，至少验证：

1. 给定 2 张合法 `512x512` 图片
   - 每张图都生成：
     - `restore.png`
     - `mask_class.png`

2. 混入 1 张非 `512x512` 图片
   - 程序不中断
   - 该图被跳过

3. 校验 `mask_class.png`
   - 为单通道 `uint8`
   - 像素值落在 `1~6`

4. 校验 demo 不依赖 `main/src/**`
   - 构建链和 include 不应引用 `main` 内部模块

---

## 14. 为什么这样设计

该设计选择的是最低复杂度路径：

- 目标单一：目录推理
- 模块最少：4 个
- 线程最少：1 个
- 配置最少：4 个字段
- 输出最少：2 张图

这样可以保证：

- 评审简单
- 调试直接
- 与 `main` 边界清晰
- 后续如果 demo 要扩展，也能在独立工程内演进

---

## 15. 后续实现建议

建议实现顺序：

1. `DemoConfig`
2. `ImageCollector`
3. `ModelRunner`
4. `ResultWriter`
5. `demo_main`
6. 最小构建接入
7. 目录级验证

不建议一开始就做：

- 通用框架封装
- 多线程
- 多模式输出
- 彩色 mask
- 与 `main` 共用抽象层

本 demo 的价值在于：先把最小闭环跑通，再讨论抽象与复用。
