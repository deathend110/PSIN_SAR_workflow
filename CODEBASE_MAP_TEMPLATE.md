# CODEBASE_MAP_TEMPLATE.md

> 这是当前仓库的“代码地图”，重点记录真实入口、主调用链、核心类职责、资源生命周期和高风险点。
> 维护目标：后续阅读或改动时，先看这里，再下钻源码。

---

## 1. Main entrypoints

| Entry | File | Responsibility |
|---|---|---|
| `main()` | `main/src/rd_imaging_stream.cpp` | 遍历 `io/echo/*.bin`，执行 RD 成像，输出 `io/sar_img/*.png` |
| `main()` | `main/src/full_workflow.cpp` | 读取 SAR PNG、切 patch、调用模型推理、做后处理、输出 HDMI/PNG |
| `build_full_workflow.sh` | `main/build_full_workflow.sh` | 在 Linux/ZG 环境中配置并编译 `main/` 工程 |

辅助但非入口：
- `main/include/full_workflow_hdmi_display.hpp`
  - HDMI 设备显示适配层
- `main/configs/*.yaml`
  - 两个程序的运行时配置

仓库中不作为当前主链路源码处理的目录：
- `deps/`
  - 三方头文件、静态库、工具库
- `icraft/`
  - `.deb` 安装包
- `BOOT_*`
  - 板端启动镜像/bitstream
- `docs/`
  - 背景资料与环境/工具文档
- `codex_cpp_template_pack/`
  - 模板资料，不参与 `main/` 构建

---

## 2. Main call chain

### 2.1 echo -> SAR

```text
rd_imaging_stream::main
 -> loadConfig
 -> create_directories(output_dir, scratch_dir)
 -> collectEchoBins
 -> for each echo file
    -> processOneEcho
       -> readEchoShapeAndValidate
       -> choose execution mode by memory_limit_mb
          -> memory_float32
             -> runMemoryFloat32Pipeline
          -> memory_double
             -> runRangeCompressionToMemory
             -> runMemoryPipeline
             -> writeNormalizedPngFromComplex
          -> scratch_double
             -> runRangeCompression
             -> runAzimuthFft
             -> runFusedRcmcAzimuthCompressionAndMagnitude
             -> writeNormalizedPng
       -> cleanup scratch
```

### 2.2 SAR -> inference -> display

```text
full_workflow::main
 -> loadConfig
 -> collectSarImages
 -> Device::Open
 -> loadNetwork
 -> network.view(0)
 -> validateNetworkIO
 -> PatchTensorBuilder(...)
 -> initSession
 -> session.enableTimeProfile(...)
 -> session.apply()
 -> emitBackendLogIfRequested
 -> create sink
    -> HdmiFrameSink or PngFrameSink
 -> PatchInferenceRunner(...)
 -> for each SAR image
    -> loadSarImageNorm
    -> SnakePatchSource(...)
    -> while next(packet)
       -> processPatch
          -> PatchTensorBuilder::build
          -> PatchInferenceRunner::forward
          -> restoreToGrayU8
          -> logitsToMaskBgr
          -> composeSideBySide
          -> sink.write
 -> Device::Close
```

### 2.3 当前未接入主链路但值得记住的函数

```text
full_workflow.cpp
 -> loadEchoBin
 -> runImaging
 -> magnitudeMinMaxToNormF32
 -> ManualFlightPatchSource
```

这些代码是预留/历史实现，不是当前 `main()` 实际调用链。

---

## 3. Core classes and responsibilities

| Class / Struct | File | Responsibility | Owns resources? | Thread affinity |
|---|---|---|---|---|
| `AppConfig` | `main/src/full_workflow.cpp` / `main/src/rd_imaging_stream.cpp` | 保存 YAML 解析后的运行配置 | no | main thread |
| `RadarConfig` | 两个 `.cpp` | 保存 RD 成像常量参数 | no | main thread |
| `PatchInfo` / `PatchPacket` | `main/src/full_workflow.cpp` | 描述 patch 元数据与图像数据 | `PatchPacket` 持有 patch `cv::Mat` | main thread |
| `RuntimeState` | `main/src/full_workflow.cpp` | 保存当前 echo/SAR/patch 的日志上下文和耗时 | no | main thread |
| `SnakePatchSource` | `main/src/full_workflow.cpp` | 根据 stride 生成蛇形 patch 顺序 | yes，持有整张 SAR 图 | main thread |
| `ManualFlightPatchSource` | `main/src/full_workflow.cpp` | 预留手动控制 patch 中心点 | yes，持有整张 SAR 图 | main thread |
| `PatchTensorBuilder` | `main/src/full_workflow.cpp` | 约束模型输入 shape/dtype，并构造 Host tensor | no，临时分配 tensor 内存 | main thread |
| `PatchInferenceRunner` | `main/src/full_workflow.cpp` | 封装 forward / wait / host copy / device reset | 持有 session/device 引用 | main thread |
| `IFrameSink` | `main/src/full_workflow.cpp` | 输出抽象接口 | abstraction only | main thread |
| `PngFrameSink` | `main/src/full_workflow.cpp` | 将最终帧写入 `io/output/<stem>/patch_xxx.png` | yes，持有输出目录配置 | main thread |
| `HdmiFrameSink` | `main/src/full_workflow.cpp` | 将最终帧转 RGB565 并送 HDMI | yes，持有 `RGB565HDMIDisplay` | main thread |
| `RGB565HDMIDisplay` | `main/include/full_workflow_hdmi_display.hpp` | 分配 UDMA buffer，写显示地址寄存器 | yes，持有 `MemChunk` | main thread |
| `EchoShape` | `main/src/rd_imaging_stream.cpp` | 回波矩阵尺寸元数据 | no | main thread |

---

## 4. Data format map

| Stage | Type | Shape | Notes |
|---|---|---|---|
| echo file payload | `float32 interleaved complex` | `rows x cols x 2` | 文件前 8 字节是 `int32 rows/cols` |
| RD scratch / memory matrix | `CV_64FC2` or `CV_32FC2` | `rows x cols` | 复数矩阵 |
| SAR PNG on disk | `uint8 grayscale PNG` | `H x W` | `rd_imaging_stream` 输出 |
| loaded SAR image | `CV_32FC1` | `H x W` | 归一化到 `0~1` |
| patch | `CV_32FC1` | `512 x 512` | `SnakePatchSource` 当前会 `clone()` |
| input tensor | `FP32` | `1 x 512 x 512 x 1` | NHWC，Host memory |
| restore output | `FP32` | `1 x 512 x 512 x 1` | 转成 `CV_8UC1` |
| seg logits output | `FP32` | `1 x 512 x 512 x 6` | 每像素 6 类 |
| restore BGR | `CV_8UC3` | `512 x 512` | 灰度转三通道 |
| color mask | `CV_8UC3` | `512 x 512` | argmax + 固定色表 |
| display frame | `CV_8UC3` | `display.height x display.width` | 左右并排 |
| HDMI write buffer | RGB565 bytes | `width * height * 2` | 写入 UDMA chunk |

---

## 5. Resource lifecycle map

### 5.1 `full_workflow`

```text
main
 |- owns AppConfig cfg
 |- opens Device
 |- owns Network / NetworkView
 |- owns Session
 |- owns PatchTensorBuilder
 |- owns unique_ptr<IFrameSink>
 |   |- PngFrameSink: owns output_dir config
 |   `- HdmiFrameSink
 |      `- owns RGB565HDMIDisplay
 |         `- owns UDMA MemChunk
 |- owns PatchInferenceRunner (references Session + Device)
 `- per SAR image
    |- owns cv::Mat sar_norm
    |- owns SnakePatchSource (stores image copy/reference counted by cv::Mat)
    `- per patch
       |- PatchPacket owns patch clone
       |- Tensor input owns HostDevice memory
       |- vector<Tensor> host_outputs owns copied output buffers
       `- frame cv::Mat objects die after sink.write
```

### 5.2 `rd_imaging_stream`

```text
main
 |- owns AppConfig cfg
 `- per echo file
    |- validates EchoShape
    |- creates scratch_root
    |- may create stage_a.bin / stage_b.bin / magnitude.bin
    |- may own large cv::Mat buffers in memory pipeline
    `- writes SAR PNG
       `- finally removes scratch_root unless keep_scratch=true
```

### 5.3 生命周期注意点

- `Device::Close(device)` 只在 `full_workflow` 里显式调用。
- `PatchInferenceRunner::forward()` 每次推理后都会 `device_.reset(1)`。
- HDMI buffer 生命周期严格绑定 `HdmiFrameSink`。
- `rd_imaging_stream` 的 scratch 文件在异常路径也会尝试清理。

---

## 6. Known hotspots

- `rd_imaging_stream`
  - `runRangeCompression*`
  - `runAzimuthFft`
  - `runFusedRcmcAzimuthCompressionAndMagnitude`
  - `runMemoryFloat32Pipeline`
  - `writeNormalizedPng*`

- `full_workflow`
  - `SnakePatchSource::next` 的 patch clone
  - `PatchTensorBuilder::build` 的 Host tensor copy
  - `PatchInferenceRunner::forward`
  - `logitsToMaskBgr` 的逐像素 argmax
  - `composeSideBySide`
  - `HdmiFrameSink::write` 的 `cvtColor(BGR -> BGR565)` + UDMA 写入

---

## 7. Known risks

- 生命周期风险：
  - HDMI sink 依赖已打开设备；如果未来把 sink 生命周期移到 `Device::Close` 之后会出错。
  - `PatchInferenceRunner` 依赖 `Session` 和 `Device` 引用有效。

- 数据/接口风险：
  - 模型 IO shape 和 dtype 被写死，模型替换时很容易在启动阶段直接失败。
  - patch 大小不是可配置扩展点，虽然 YAML 有字段，但实际只接受 `512`。

- 性能风险：
  - `SnakePatchSource` 每 patch `clone()`，大图 patch 总数高时会增加内存流量。
  - `full_workflow` 当前完全串行，没有重叠 IO / infer / display。
  - HDMI 路径每帧都做颜色空间转换和整帧写 buffer。

- 健壮性风险：
  - `full_workflow` 任一 patch 出错会中止整个进程，没有文件级恢复。
  - `rd_imaging_stream` 依赖 scratch 磁盘空间；空间不足时会失败。
  - `full_workflow.cpp` 中存在未接线的成像代码，后续阅读时容易误判主路径。

- 并发/状态风险：
  - 虽然没有显式多线程，但硬件 runtime 的内部状态和 `device.reset(1)` 调用顺序具有隐式时序要求。

---

## 8. Recent changes log

### 2026-04-24
- changed:
  - 将模板文档更新为当前仓库的真实代码地图和架构说明。
- reason:
  - 仓库已经从通用模板演变成具体的 SAR + NPU + HDMI 工作流，需要让文档反映真实入口和资源边界。
- risk:
  - 文档结论依赖当前 `main/` 源码；若后续把未接线代码重新接入主流程，需要同步更新。
- validation:
  - 已按源码核对入口、类名、配置项、输出路径和资源生命周期。

---

## 9. Questions / ambiguities

- `full_workflow.cpp` 中的本地成像函数最终是否会重新接入主入口，还是长期保持“两阶段程序”结构？
- `run_backend=host` 是否真的需要长期保留，还是仅作为调试接口占位？
- `device.reset(1)` 的具体语义和最小必要频率是否有更正式的硬件约束文档？
- `ManualFlightPatchSource` 后续控制端计划通过什么接口驱动：本地输入、串口、网络还是共享内存？
- 当前 `main/CMakeLists.txt` 禁止 Windows 构建；仓库根目录文档中提到的 Windows 路径是否只针对模板流程，而非此实际工程？
