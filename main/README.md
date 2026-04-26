# `PSIN_SAR_workflow` 工程说明

> 当前仓库状态说明：
> - 仓库只跟踪 `main/configs/*.example.yaml`，运行时实际读取和写回的是同目录下的本地 `*.yaml` 副本；副本缺失时会自动从 example 文件 bootstrap。
> - 主菜单包含 `RD only`、`Inference only`、`Web Console`、`Exit`。其中只有 `Web Console` 正常退出后会返回主菜单。
> - HDMI 推理界面在终止态会补写最终一帧，右上角状态 badge 从绿色 `RUNNING` 切换为红色 `STOPPED`。

这是复旦微FOML30TAI板子的CPP后端处理代码。`main/` 是当前仓库里真正参与构建和运行的主工程目录，目标平台为 `Linux + aarch64 + ZG330`。

当前程序构建出的主可执行文件为：

```text
build/ZG/psin_workflow
```

它承载 3 个真实运行模式：

- `RD only`
- `Inference only`
- `Web Console`

典型数据链路为：

```text
echo.bin -> RD 成像 -> SAR 灰度 PNG -> 512x512 patch 推理 -> 恢复图 / 分割图 -> HDMI 或 PNG
```

## 1. 当前目录结构

```text
main/
  CMakeLists.txt
  README.md
  build_main.sh
  Host Computer Software.md
  configs/
    infer_workflow.example.yaml
    rd_imaging.example.yaml
    web_console.example.yaml
  include/
    infer_workflow_hdmi_display.hpp
    workflow/
      shared/
        app_mode.hpp
        config_utils.hpp
        run_control.hpp
      rd/
        rd_config.hpp
        rd_workflow.hpp
      infer/
        infer_config.hpp
        infer_workflow.hpp
      web/
        web_console.hpp
        web_console_config.hpp
        web_console_controller.hpp
        web_console_protocol.hpp
        web_console_server.hpp
  src/
    main.cpp
    config_utils.cpp
    rd_config.cpp
    rd_imaging_stream.cpp
    infer_config.cpp
    infer_workflow.cpp
    web_console.cpp
    web_console_config.cpp
    web_console_protocol.cpp
    web_console_controller.cpp
    web_console_server.cpp
    web_console_assets.cpp
    hdmi_ui_preview_1080_p_industrial.jsx
```

## 2. 程序入口与菜单行为

总入口在 [src/main.cpp](./src/main.cpp)。

程序启动后会显示菜单：

```text
1. RD only
2. Inference only
3. Web Console
0. Exit
```

当前代码中的真实行为如下：

- 选择 `1. RD only`
  - 调用 `workflow::rd::Run("configs/rd_imaging.yaml")`
  - RD 工作流结束后，进程直接按返回码退出
- 选择 `2. Inference only`
  - 调用 `workflow::infer::Run("configs/infer_workflow.yaml")`
  - 推理工作流结束后，进程直接按返回码退出
- 选择 `3. Web Console`
  - 调用 `workflow::web::Run("configs/web_console.yaml")`
  - 如果 Web 模式正常结束，例如浏览器点击 `Shutdown Web Console`，则返回主菜单
  - 如果 Web 模式异常退出，则进程按错误码退出
- 选择 `0. Exit`
  - 不运行任何 workflow，直接退出

也就是说，当前“返回菜单”的行为只对 `Web Console` 正常退出路径成立；`RD only` 和 `Inference only` 仍然保持单次运行后直接退出。

## 3. 模块划分

### `src/main.cpp`

主程序入口，只负责：

- 显示菜单
- 接收模式选择
- 调度到 `rd / infer / web`

不负责：

- HTTP 处理
- RD 算法细节
- 推理后处理细节

### `include/workflow/shared` + `src/config_utils.cpp`

共享基础设施：

- `AppMode` 运行模式枚举
- 简化 YAML 读写
- 字符串清理
- 布尔 / 整数解析
- 原子写回文本配置
- `WorkflowRunControl` 协作式运行控制与运行时快照

### `include/workflow/rd` + `src/rd_config.cpp` + `src/rd_imaging_stream.cpp`

RD 成像模块：

- 读取 `rd_imaging.yaml`
- 收集 echo bin
- 校验输入 shape 与文件大小
- 选择执行模式
  - `auto`
  - `memory_float32`
  - `scratch_double`
- 执行 Range Compression / Azimuth FFT / RCMC / Azimuth Compression
- 写出 SAR PNG
- 管理 scratch 文件生命周期

### `include/workflow/infer` + `src/infer_config.cpp` + `src/infer_workflow.cpp`

推理模块：

- 读取 `infer_workflow.yaml`
- 收集 SAR PNG
- 打开 ZG330 / Host backend 设备与 session
- 校验模型输入输出 shape
- 生成 patch
  - `auto_snake`
  - `manual_flight`
- 执行模型 forward
- 输出恢复图与 6 类分割 mask
- 合成工业风 UI 帧
- 输出到 HDMI 或 PNG

### `include/workflow/web` + `src/web_console*.cpp`

Web Console 模块：

- 读取 / 写回 `web_console.yaml`
- 暴露 HTTP + JSON + SSE 接口
- 管理内存态选择项与设置项
- 启动 / 暂停 / 停止 / 复位后台 workflow
- 转发 manual flight 输入
- 暴露运行状态、日志事件与图片预览
- 安全关闭 Web Console

Web 侧职责边界：

- `web_console_server.cpp`
  - 唯一网络 I/O 承载点
- `web_console_controller.cpp`
  - 状态机、worker 生命周期、命令收口
- `web_console_protocol.cpp`
  - JSON DTO、flat JSON / query 解析
- `web_console_assets.cpp`
  - 内嵌前端 HTML / CSS / JS

### `include/infer_workflow_hdmi_display.hpp`

HDMI 适配层，负责 RGB565 帧显示。

## 4. 当前运行模式

### RD only

入口：

- `workflow::rd::Run(const std::filesystem::path&)`
- `workflow::rd::Run(const AppConfig&, WorkflowRunControl*)`

主链：

```text
rd_imaging.yaml
 -> LoadConfig
 -> collectEchoBins
 -> processOneEcho
    -> readEchoShapeAndValidate
    -> choose execution mode
       -> memory_float32
       -> memory_double
       -> scratch_double
    -> write PNG
    -> cleanup scratch
```

输入：

- `rd.echo_dir` 下的 `.bin`

输出：

- `rd.output_dir` 下的 SAR 灰度 PNG

### Inference only

入口：

- `workflow::infer::Run(const std::filesystem::path&)`
- `workflow::infer::Run(const AppConfig&, WorkflowRunControl*)`

主链：

```text
infer_workflow.yaml
 -> LoadConfig
 -> collectSarImages
 -> Device::Open
 -> loadNetwork / validateNetworkIO / initSession / session.apply
 -> create sink (HDMI or PNG)
 -> patch source
    -> auto_snake: SnakePatchSource
    -> manual_flight: ManualFlightRuntime
 -> for each patch
    -> PatchTensorBuilder::build
    -> PatchInferenceRunner::forward
    -> restoreToGrayU8
    -> logitsToMaskBgr
    -> applyManualTelemetry
    -> composeIndustrialUiFrame
    -> sink.write
 -> Device::Close
```

输入：

- `infer.input.sar_img_dir` 下的 SAR PNG

输出：

- `output.mode=hdmi`
- 或 `output.dir/<sar_stem>/patch_*.png`

### Web Console

入口：

- `workflow::web::Run(const std::filesystem::path&)`

协议：

- `HTTP + JSON + SSE`

主链：

```text
browser
 -> GET /, /app.js, /app.css
 -> GET /api/state, /api/settings, /api/sources, /api/source/preview
 -> POST /api/selection, /api/settings
 -> POST /api/command/start|pause|stop|reset|shutdown_web
 -> POST /api/manual/key
 -> GET /events
 -> WebConsoleServer
 -> WebConsoleController
 -> worker thread
    -> workflow::rd::Run(...)
    -> workflow::infer::Run(...)
```

当前 Web UI 真实行为：

- 设置面板默认隐藏，由齿轮按钮展开
- 右上角有黄色圆形 `Shutdown Web Console` 按钮
- 支持 `patch_mode=manual_flight`
- `W/A/S/D` 和页面按钮会真正发送到 `/api/manual/key`
- manual 状态主要通过 SSE `state` 事件更新

## 5. 配置文件

Note: the repository tracks `*.example.yaml` only. On first run, each workflow bootstraps a local `*.yaml` copy next to the example file if the runtime file is missing.

### `configs/rd_imaging.example.yaml`

关键字段：

- `rd.execution_mode`
- `rd.echo_dir`
- `rd.echo_ext`
- `rd.output_dir`
- `rd.scratch_dir`
- `rd.column_tile`
- `rd.row_tile`
- `rd.memory_limit_mb`
- `rd.prefer_memory_pipeline`
- `rd.keep_scratch`
- `rd.overwrite`

当前执行模式只允许：

- `auto`
- `memory_float32`
- `scratch_double`

### `configs/infer_workflow.example.yaml`

关键字段：

- `sys.device`
- `sys.run_backend`
- `sys.mmuMode`
- `sys.speedMode`
- `sys.compressFtmp`
- `sys.ocm_option`
- `sys.profile`
- `input.sar_img_dir`
- `input.sar_img_ext`
- `input.recursive`
- `pipeline.patch.mode`
- `pipeline.patch.patch_size`
- `pipeline.patch.stride`
- `pipeline.icore.json`
- `pipeline.icore.raw`
- `pipeline.output_wait_ms`
- `display.width`
- `display.height`
- `display.fps`
- `output.mode`
- `output.dir`
- `output.overwrite`
- `debug.dump_backend_log`

当前模型接口约束：

```text
input[0]  = [1,512,512,1] FP32
output[0] = [1,512,512,1] FP32
output[1] = [1,512,512,6] FP32
```

当前代码约束：

- `patch_size` 必须为 `512`
- `stride` 必须为正整数
- `output.mode` 只允许 `hdmi` 或 `png`

### `configs/web_console.example.yaml`

关键字段：

- `server.bind`
- `server.board_ip`
- `server.port`
- `server.sse_heartbeat_ms`
- `ui.title`
- `config.infer_path`
- `config.rd_path`
- `flight.manual_step_px`
- `flight.boost_step_px`
- `flight.trigger_distance_px`
- `flight.cache_grid_px`
- `flight.path_overlay`
- `flight.control_bindings`

说明：

- `server.bind` 是监听地址
- 浏览器实际访问应优先使用 `server.board_ip:server.port`
- Web 退出时会把 infer / rd / web 三份配置写回 YAML

## 6. 线程与资源生命周期

### RD only

- 单工作线程
- 每个 echo 的 scratch 目录独立创建与清理

### Inference only

- 推理主线程
- `output.mode=hdmi` 时会额外起一个 HDMI render thread
- `patch.mode=manual_flight` 时不再有独立 simulation thread

### Web Console

- main thread
- dedicated web thread
- background workflow worker thread
- 如果后台 workflow 选择 HDMI，还会再有 HDMI render thread

当前 Web Console 退出顺序：

```text
server->Stop()
controller->RequestWorkerStop()
join web_thread
controller->JoinWorker()
PersistControllerConfigs(...)
```

## 7. Manual Flight 当前状态

`manual_flight` 现在已经接入真实执行链路，不再是占位模式。

当前控制链：

```text
browser key/button
 -> POST /api/manual/key
 -> WebConsoleServer
 -> WebConsoleController::commandManualKey(...)
 -> workflow::infer::SubmitManualFlightKey(...)
 -> ManualFlightRuntime
```

当前运行语义：

- 不是自由飞行线程模型
- 而是“patch 级扫描游标”模型
- `W/A/S/D` 表示方向切换
- 使用 latest-wins 方向变更语义
- 位置推进与 patch 完成节拍绑定

## 8. 构建

当前仓库的构建入口分成两条：

- `host/native`：只用于最小回归测试构建，不产出 `psin_workflow`
- `ZG330`：保留现有板端主程序构建入口

### ZG330 主程序构建

推荐在 `main/` 目录下构建：

```bash
cd main
chmod +x build_main.sh
./build_main.sh
```

手动构建：

```bash
cd main
mkdir -p log
cmake -S . -B build/ZG \
  -DCMAKE_BUILD_TYPE=Release \
  -DPSIN_BUILD_PROFILE=zg330 \
  -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchains/zg330-aarch64.cmake 2>&1 | tee log/cmake_configure.log
cmake --build build/ZG -j$(nproc) 2>&1 | tee log/cmake_build.log
```

说明：

- `CMakeLists.txt` 不再默认把所有构建固定到 `Linux/aarch64`
- ZG330 交叉编译器路径移动到 `../cmake/toolchains/zg330-aarch64.cmake`
- Windows 上直接配置会被显式拒绝
- 依赖来自 `../deps` 以及系统里的 icraft 包
- 仓库根目录也提供了 `zg330-release` preset，等价命令是 `cmake --preset zg330-release`

### host/native 回归测试构建

`host/native` 路径只接最小回归测试目标：

- `main_menu_regression_test`
- `hdmi_stopped_regression_test`

示例：

```bash
cmake --preset host-debug
cmake --preset host-tests
cmake --build --preset host-tests
ctest --preset host-tests --output-on-failure
```

说明：

- `host-debug` 和 `host-tests` 都走 `PSIN_BUILD_PROFILE=host-tests`
- `host-debug` 用于手动检查 host/native Debug 配置是否可用
- `host-tests` 用于标准化的回归测试配置、构建和 `ctest` 入口

## 9. 运行

必须在 `main/` 目录下启动，因为菜单里使用相对配置路径：

```bash
cd main
./build/ZG/psin_workflow
```

### 启动 RD only

```text
1
```

使用：

  - local runtime copy of `configs/rd_imaging.yaml`, bootstrapped from `configs/rd_imaging.example.yaml` when missing

### 启动 Inference only

```text
2
```

使用：

  - local runtime copy of `configs/infer_workflow.yaml`, bootstrapped from `configs/infer_workflow.example.yaml` when missing

### 启动 Web Console

```text
3
```

使用：

  - local runtime copy of `configs/web_console.yaml`, bootstrapped from `configs/web_console.example.yaml` when missing

程序启动后会在终端打印：

- `bind_address:port`
- `board_ip:port`

浏览器应优先访问 `board_ip:port`。

### 退出 Web Console

可通过以下方式结束 Web 模式：

- 浏览器点击 `Shutdown Web Console`
- 板子终端 `Ctrl+C`

在当前代码里，Web 模式正常关闭后会回到主菜单，而不是直接回 shell。

## 10. 当前关键约束

- 同一时刻只运行一个模式
- Web Console 下同一时刻只允许一个后台 workflow
- `WebConsoleServer` 是唯一网络 I/O 承载点
- `patch_size` 必须保持 `512`
- `RD execution_mode` 只允许 `auto / memory_float32 / scratch_double`
- `manual_flight` 使用 latest-wins 方向更新，不维护历史方向 FIFO
- `stop` 是协作式停止，不是硬抢占中断
- Web Console 退出前会写回当前 infer / rd / web 配置
- Web 设置接口会先在内存里完成整批校验，再一次性提交；失败时不会留下部分生效的脏状态
- 语义非法参数返回 `invalid_settings`
- 明显超板端预算的参数返回 `board_budget_exceeded`
- 当前 Web 设置护栏阈值：
  - `infer.pipeline.patch.patch_size` 必须是 `512`
  - `infer.pipeline.patch.stride` 必须是正整数；小于 `64` 会被视为超板端预算，大于 `512` 会被视为非法值
  - `infer.display.width` / `infer.display.height` 必须为正整数，且不超过 `1920x1080`
  - `infer.display.fps` 允许 `0`，大于 `60` 会被视为超预算
  - 当显示分辨率超过 `1280x720` 时，`infer.display.fps > 30` 会被视为超预算
  - `rd.memory_limit_mb` 必须为正整数，且大于 `512` 会被视为超预算

## 11. 不参与当前主构建 / 主链的文件

- `Host Computer Software.md`
  - 说明性文档，不参与构建
- `src/hdmi_ui_preview_1080_p_industrial.jsx`
  - UI 预览稿，不参与当前 CMake 构建

## 12. 快速检查

检查 echo 输入：

```bash
ls io/echo
```

检查 SAR PNG：

```bash
ls io/sar_img
```

检查模型文件：

```bash
ls ./imodel/ZG/bf16/
```

如果离线调试不接 HDMI，可在 `infer_workflow.yaml` 中设置：

```yaml
output:
  mode: png
```

然后检查：

```text
io/output/
```
