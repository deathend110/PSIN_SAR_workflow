# examples/2_Full_workflow 使用说明

本工程用于接通第 0 阶段完整链路：

```text
echo 回波 bin -> RD SAR 成像 -> 512x512 patch 蛇形扫描 -> icraft 模型推理 -> 后处理 -> HDMI/PNG 输出
```

当前只面向 Linux/ZG 板端编译运行，不维护 Windows 编译路径。输出方式由 `configs/full_workflow.yaml` 控制：可以直接送 HDMI，也可以在板端保存 PNG 调试图。

## 1. 工程入口

主要文件：

```text
examples/2_Full_workflow/
  CMakeLists.txt
  configs/
    full_workflow.yaml      # 完整链路配置
    sar_imaging.yaml        # 单独 RD 成像工具配置
  src/
    full_workflow.cpp       # 完整链路入口
    sar_imaging.cpp         # echo bin -> SAR PNG 单独工具
  io/
    echo/                   # 输入 echo 回波 bin
    output/                 # png 模式输出目录
    sar_img/                # sar_imaging 单工具输出目录
```

编译后会生成两个可执行程序：

```text
sar_imaging     # 只做 echo bin -> SAR image
full_workflow   # 做完整 echo -> SAR -> patch -> AI -> HDMI/PNG
```

## 2. 编译

在板端或交叉编译 Linux 环境中执行：

```bash
cd examples/2_Full_workflow
cmake -S . -B build/ZG -DCMAKE_BUILD_TYPE=Release
cmake --build build/ZG -j$(nproc)
```

编译完成后，程序位于：

```text
examples/2_Full_workflow/build/ZG/full_workflow
examples/2_Full_workflow/build/ZG/sar_imaging
```

如果运行时报 `Permission denied`，赋予执行权限：

```bash
chmod +x build/ZG/full_workflow
chmod +x build/ZG/sar_imaging
```

## 3. 准备输入文件

把 echo 回波 bin 放入：

```text
examples/2_Full_workflow/io/echo
```

程序会按文件名排序依次处理，例如：

```text
io/echo/1_hh_amp_echo_1bit.bin
io/echo/1_hv_amp_echo_1bit.bin
io/echo/1_vh_amp_echo_1bit.bin
io/echo/1_vv_amp_echo_1bit.bin
```

模型文件路径由 `configs/full_workflow.yaml` 配置：

```yaml
pipeline:
  icore:
    json: ./imodel/ZG/bf16/RAAUNet_DeepCA48_MobileNetV3Unet_V1_generated.json
    raw: ./imodel/ZG/bf16/RAAUNet_DeepCA48_MobileNetV3Unet_V1_generated.raw
```

运行前确认 json/raw 文件存在，并且模型输入输出满足：

```text
input[0]  = [1,512,512,1]
output[0] = [1,512,512,1]   # 恢复灰度图
output[1] = [1,512,512,6]   # 分割 logits
```

## 4. 运行完整链路

建议从 `examples/2_Full_workflow` 目录运行，因为配置里使用了 `./io`、`./imodel` 这类相对路径。

```bash
cd examples/2_Full_workflow
./build/ZG/full_workflow configs/full_workflow.yaml
```

完整流程如下：

```text
1. 遍历 io/echo 下的 echo bin
2. 使用 RD 算法生成 SAR 灰度大图
3. 以 512x512、stride=256 自动蛇形裁 patch
4. 构造 NHWC [1,512,512,1] 输入 tensor
5. 调用 icraft session.forward 推理
6. output[0] 转恢复灰度图
7. output[1] 对最后一维 argmax，转 RGB 分割 mask
8. 左侧恢复图、右侧分割图并排合成
9. 根据 output.mode 送 HDMI 或保存 PNG
```

## 5. HDMI 输出模式

配置：

```yaml
output:
  mode: hdmi
```

运行：

```bash
./build/ZG/full_workflow configs/full_workflow.yaml
```

板子接 HDMI 显示器后，画面会显示左右并排结果：

```text
左侧：模型恢复灰度图
右侧：分割 mask RGB 图
```

显示分辨率由这里控制：

```yaml
display:
  width: 1280
  height: 720
  fps: 0
```

建议优先使用 `1280x720`，比 1080p 更稳。`fps: 0` 表示不主动限帧；如果希望限制显示速度，可以改成例如：

```yaml
display:
  fps: 10
```

## 6. PNG 调试模式

如果不想接 HDMI，或者想保存中间显示结果排查，可以改为：

```yaml
output:
  mode: png
  dir: ./io/output
  overwrite: true
```

运行后输出路径类似：

```text
io/output/1_hh_amp_echo_1bit/patch_000000.png
io/output/1_hh_amp_echo_1bit/patch_000001.png
io/output/1_hh_amp_echo_1bit/patch_000002.png
```

每张 PNG 仍然是左右并排：左恢复图，右分割 mask。

如果不想覆盖已有结果：

```yaml
output:
  overwrite: false
```

## 7. Debug 参数

### 7.1 生成 generate_memopt.log

配置：

```yaml
debug:
  dump_backend_log: true
```

程序会在 `session.apply()` 后调用 `ZG330Backend::log()`，生成后端部署和内存优化日志。运行后可检查：

```bash
find . -name "generate_memopt.log"
find . -path "*/.icraft/logs/*" -type f
```

如果不需要生成日志，可以关闭：

```yaml
debug:
  dump_backend_log: false
```

### 7.2 内部耗时统计

配置：

```yaml
sys:
  profile: true
```

这会开启 `session.enableTimeProfile(true)`。当前第 0 阶段主要打印每个 patch 的推理耗时和总耗时，后续控制端/性能页可以继续读取更细的 profiling 结果。

### 7.3 输出等待超时

配置：

```yaml
pipeline:
  output_wait_ms: 20000
```

这是等待模型输出 ready 的最长超时时间，单位毫秒。它不是固定等待 20 秒，只是保护上限。如果模型正常很快完成，不会等满。

## 8. Patch 扫描参数

当前只实现自动蛇形扫描：

```yaml
pipeline:
  patch:
    mode: auto_snake
    patch_size: 512
    stride: 256
```

规则：

```text
patch_size = 512
stride = 256
边缘不足 512x512 的区域丢弃
偶数行从左到右
奇数行从右到左
```

代码里已经预留 `ManualFlightPatchSource`，后续可以接上位机/控制端，实现“手动无人机巡航”：用无人机中心点控制当前裁剪的 512x512 patch。

## 9. 单独运行 RD 成像工具

如果只想测试 echo bin 到 SAR PNG，可以使用 `sar_imaging`：

```bash
cd examples/2_Full_workflow
./build/ZG/sar_imaging configs/sar_imaging.yaml
```

也可以单文件运行：

```bash
./build/ZG/sar_imaging io/echo/1_hh_amp_echo_1bit.bin io/sar_img/1_hh_amp_cpp.png
```

## 10. 常见问题

### Permission denied

```bash
chmod +x build/ZG/full_workflow
```

### 找不到 echo 文件

确认运行目录是：

```bash
pwd
# 应该是 examples/2_Full_workflow
```

确认文件存在：

```bash
ls io/echo
```

### 找不到模型 json/raw

检查配置路径：

```bash
ls ./imodel/ZG/bf16/
```

如果你的实际模型文件名不是 `*_generated.json/raw`，修改 `configs/full_workflow.yaml` 中的：

```yaml
pipeline:
  icore:
    json: ...
    raw: ...
```

### HDMI 没有画面

检查：

```yaml
output:
  mode: hdmi
```

同时确认 HDMI 线已经接好，显示器支持当前分辨率。建议先用：

```yaml
display:
  width: 1280
  height: 720
```

### 想先不接 HDMI 调试

改成：

```yaml
output:
  mode: png
```

然后看 `io/output` 下是否正常生成 PNG。
