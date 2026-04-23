# ARCHITECTURE_TEMPLATE.md

> 用于描述当前仓库中“实际运行的系统边界与关键设计”。
> 目标不是保留模板，而是让维护者和 Codex 能在几分钟内定位主链路、资源约束和不能误改的地方。

---

## 1. System goal

一句话目标：
- 这是一个面向 Linux/ZG330 板端的 SAR 推理工作流样例工程，负责把回波 `echo.bin` 成像成 SAR 灰度图，再对 SAR 图做 512x512 patch 推理，最终输出“恢复图 + 分割图”到 HDMI 或 PNG。

运行环境：
- 主运行目标是 `aarch64 Linux + ZG330`。
- `main/CMakeLists.txt` 明确禁止 Windows 编译当前 `main/` 工程。
- 工作区中保留了 Windows 侧模板、资料、依赖包和启动镜像，但当前真正可执行的主工程在 `main/`。

关键约束：
- 板端内存有限，所以回波成像程序会在 `memory_float32`、`memory_double`、`scratch_double` 三种路径之间按内存预算切换。
- 模型输入固定为 `NHWC [1,512,512,1] FP32`，输出固定为两个张量：
  - `output[0] = [1,512,512,1] FP32` 恢复灰度图
  - `output[1] = [1,512,512,6] FP32` 分割 logits
- HDMI 输出依赖 ZG330 设备和 UDMA buffer，不能按普通桌面显示模块理解。

---

## 2. Non-goals

当前工程明确不负责：

- 模型训练、量化、导出
- 上位机 GUI / Web 控制端
- 网络通信与远程控制协议
- 多相机/多流并发调度
- 通用化的跨平台显示框架
- 对第三方依赖 `deps/` 的二次封装维护

补充说明：
- `full_workflow.cpp` 里保留了一套本地 echo->SAR 成像辅助函数和 `ManualFlightPatchSource`，但当前主入口没有接这条路径，它们属于预留扩展点，不是当前主流程。

---

## 3. Module breakdown

| Module | Responsibility | Inputs | Outputs | Key dependencies |
|---|---|---|---|---|
| `rd_imaging_stream` | 将回波 `.bin` 处理为 SAR 灰度 PNG；根据内存预算选择执行模式 | `io/echo/*.bin` | `io/sar_img/*.png` | OpenCV, filesystem |
| `full_workflow` | 读取 SAR PNG，切 patch，调用 icraft runtime 推理并输出结果 | `io/sar_img/*.png`, `imodel/*.json/raw` | HDMI 帧或 `io/output/**/patch_*.png` | icraft runtime, OpenCV, spdlog |
| `SnakePatchSource` | 负责自动蛇形扫描 patch 顺序 | SAR `CV_32FC1` 图 | `PatchPacket` | OpenCV |
| `PatchTensorBuilder` | 校验模型输入约束并构造 FP32 Host tensor | `512x512 CV_32FC1` patch | icraft `Tensor` | HostDevice memory |
| `PatchInferenceRunner` | 执行 `session.forward`，等待输出 ready，搬回 Host，并重置设备状态 | 输入 tensor | Host tensors | Session, Device |
| `IFrameSink` + 实现类 | 输出最终可视化帧；当前实现为 PNG 或 HDMI | `frame_bgr` | 文件或 HDMI | OpenCV / ZG330 UDMA |
| `RGB565HDMIDisplay` | 适配板端 HDMI 写显存寄存器流程 | RGB565 frame data | 板端显示 | FPAI/ZG330 device |

---

## 4. Main data flow

### 4.1 主工作流

```text
echo.bin
 -> rd_imaging_stream
 -> SAR grayscale PNG
 -> full_workflow
 -> auto_snake patch stream
 -> FP32 NHWC tensor
 -> NPU/session.forward
 -> restore gray + seg logits
 -> gray image + color mask
 -> side-by-side frame
 -> HDMI or PNG
```

### 4.2 关键数据形态

1. 回波输入：
   - 文件：`*.bin`
   - 布局：8 字节头（rows, cols, int32 little-endian）+ `rows * cols * 2 * float32`
   - 语义：复数矩阵，实部/虚部交织

2. RD 成像中间数据：
   - 类型：`CV_64FC2` 或 `CV_32FC2`
   - 语义：复数频域/时域矩阵
   - 路径：纯内存或 scratch 文件

3. SAR 图：
   - 文件：`io/sar_img/<stem>.png`
   - 类型：读入后转换为 `CV_32FC1`
   - 范围：`0~1`

4. Patch：
   - 类型：`CV_32FC1`
   - 尺寸：固定 `512x512`
   - 扫描：蛇形，`stride=256`

5. Runtime 输入：
   - 类型：FP32 tensor
   - 形状：`[1,512,512,1]`
   - 存放：`HostDevice::MemRegion().malloc(...)`

6. Runtime 输出：
   - `output[0]`：恢复灰度 `FP32 [1,512,512,1]`
   - `output[1]`：分割 logits `FP32 [1,512,512,6]`

7. 后处理结果：
   - 恢复图：`CV_8UC1` -> 转 `CV_8UC3`
   - 分割图：argmax 后映射到固定 6 色 `CV_8UC3`

8. 最终显示帧：
   - 类型：`CV_8UC3`
   - 布局：左右并排
   - 输出前：HDMI 路径会转换成 `BGR565`

---

## 5. Thread model

当前代码没有显式的多线程流水线，两个可执行程序都是“单主线程串行处理”。

- `rd_imaging_stream`
  - 主线程负责：读配置、遍历 echo 文件、执行 RD 成像、写 PNG、清理 scratch。
  - 无显式工作线程、无队列、无锁。

- `full_workflow`
  - 主线程负责：读配置、开设备、建 session、遍历 SAR 图、生成 patch、推理、后处理、输出。
  - 无显式捕获线程、推理线程、显示线程拆分。
  - `HdmiFrameSink` 内部仅在限帧时调用 `std::this_thread::sleep_for(...)`，不是独立显示线程。

共享资源与同步：
- 无项目级 `mutex / condition_variable / ring buffer / task queue`。
- `session.forward()` 的底层后端可能异步执行，但调用方会立刻 `waitForReady(...)`，因此上层语义仍然是串行阻塞。
- HDMI 写显存、设备 reset、session apply 都发生在主线程。

结论：
- 当前最大的并发风险不在 C++ 显式线程，而在“硬件后端内部状态 + 主线程资源复用”的顺序约束。

---

## 6. Ownership and lifetime rules

| Object | Created by | Owned by | Destroyed by | Notes |
|---|---|---|---|---|
| `AppConfig` | `loadConfig()` | `main()` 栈对象 | 作用域结束 | 只读配置快照 |
| `Device` | `Device::Open()` | `full_workflow::main()` | `Device::Close()` | 失败路径也会关闭 |
| `Network` / `NetworkView` | `loadNetwork()` / `network.view(0)` | `full_workflow::main()` | 作用域结束 | 生命周期需覆盖 session 初始化 |
| `Session` | `initSession()` | `full_workflow::main()` | 作用域结束 | 必须先 `apply()` 再跑推理 |
| 输入 tensor 内存 | `PatchTensorBuilder::build()` | `Tensor` 值对象 | `Tensor` 析构 | Host 内存，每 patch 新建 |
| 输出 tensors | `PatchInferenceRunner::forward()` | 返回的 `std::vector<Tensor>` | 作用域结束 | 每 patch 一次 host copy |
| HDMI UDMA buffer | `RGB565HDMIDisplay` 构造函数 | `HdmiFrameSink::display_` | sink 析构 | 与设备绑定，显示期间复用 |
| SAR 图 `cv::Mat` | `loadSarImageNorm()` | 当前 SAR 循环体 | 作用域结束 | 被 `SnakePatchSource` 持有一份 |
| patch `cv::Mat` | `SnakePatchSource::next()` | `PatchPacket` | patch 处理结束 | 目前每次 `clone()` |
| scratch 文件 | `processOneEcho()` | 文件系统路径 | 正常或异常清理 | `keep_scratch=false` 时自动删除 |

规则：
- 当前设计基本遵循 RAII，但 `Device` 需要显式 `Close`。
- 没有共享智能指针模型；资源主要由栈对象、`unique_ptr<IFrameSink>` 和 `cv::Mat` 引用计数管理。
- HDMI 显示 buffer 不允许在设备关闭后继续使用。
- `PatchInferenceRunner` 在每次 forward 后调用 `device_.reset(1)`，这是当前设备状态回收策略的一部分，不能随意删改。

---

## 7. Error handling strategy

整体风格：
- 以异常为主，入口 `main()` 统一捕获并返回非零退出码。
- 运行中重要阶段通过 `spdlog` 或 `logLine()` 打日志。

约束：
- 配置错误、模型 IO 形状不匹配、输入文件不存在、设备打开失败、PNG 写失败：立即失败。
- `rd_imaging_stream` 对“单个 echo 文件处理失败”做降级：
  - 记录错误
  - 继续处理后续文件
  - 最终返回聚合失败码
- `full_workflow` 对 patch 级别没有局部恢复；任何异常会终止整个程序。
- `full_workflow` 额外注册了 `SIGSEGV` 处理器，只打印最近阶段并退出，便于板端定位崩溃点。

---

## 8. Performance constraints

现阶段可从代码中确认的约束：

- `rd_imaging_stream`
  - 目标不是实时帧率，而是在板端内存约束下稳定完成大回波成像。
  - 通过 `memory_limit_mb`、`column_tile`、`row_tile` 控制峰值内存。
  - 热路径：
    - range compression
    - azimuth FFT
    - RCMC
    - azimuth compression + IFFT
    - magnitude normalize

- `full_workflow`
  - 每个 patch 的热路径是：patch clone -> tensor copy -> `session.forward` -> host copy -> argmax/colorize -> compose -> sink write。
  - `output_wait_ms` 是 ready 超时保护，不是目标时延。
  - `display.fps=0` 表示不主动限帧；限帧只在 HDMI sink 中生效。

应避免的操作：
- 在热路径里放大 `cv::Mat` 复制次数
- 改动模型输入输出形状导致额外格式转换
- 在推理循环里引入不必要的磁盘 IO
- 破坏 `rd_imaging_stream` 的按 tile 处理方式，导致内存峰值失控

---

## 9. Invariants / do-not-break rules

最关键的不可破坏约束：

- `rd_imaging_stream` 输入 `.bin` 文件格式必须保持：
  - 前 8 字节为 `rows, cols`
  - 后续为复数 `float32` 交织数据
- `full_workflow` 当前主入口只接受 SAR PNG，不走在线 echo->SAR 成像。
- patch 大小必须是 `512x512`，当前代码会拒绝其他尺寸。
- patch 顺序必须保持蛇形扫描；奇数行右到左，偶数行左到右。
- 边缘不足完整 patch 的区域必须丢弃，不能自动补零。
- runtime 输入必须是 `NHWC [1,512,512,1] FP32`。
- runtime 输出必须是两个 FP32 张量，且分割类别数固定为 6。
- HDMI 路径必须在设备已打开且 UDMA buffer 已分配后使用。
- `session.apply()` 必须在首次推理前完成。
- `waitForReady()` 和 `device.reset(1)` 的先后关系不能随意改变。

---

## 10. Extension points

当前可扩展位置：

- `ManualFlightPatchSource`
  - 已预留手动巡航 patch 中心移动接口
  - 当前未接控制端，也未纳入主入口

- `full_workflow.cpp` 中的本地成像辅助函数
  - 可作为未来“单进程 echo->SAR->infer”重接线的基础
  - 但现在主流程明确采用“两阶段程序 + SAR PNG 中间产物”

- 输出模式
  - 已有 `hdmi` / `png`
  - 后续可扩展 overlay、录屏、状态叠加，但应复用 `IFrameSink` 抽象

- 后端模式
  - `run_backend` 目前允许 `zg330` / `host`
  - 真正板端路径只验证过 `zg330`

目前不建议直接做的扩展：
- 未经验证就把两个可执行程序粗暴合并成一个超大主程序
- 在没有队列设计前引入多线程 patch 并发
- 更改设备 reset / session 生命周期
- 把模板目录、第三方依赖目录和运行主工程混写到同一架构层次里
