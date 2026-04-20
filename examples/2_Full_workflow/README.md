# examples/2_Full_workflow 使用说明

本工程现在只保留一个干净的完整链路程序：`full_workflow`。

```text
echo 回波 bin -> RD SAR 成像 -> 512x512 patch 蛇形扫描 -> icraft 模型推理 -> 后处理 -> HDMI/PNG 输出
```

当前只面向 Linux/ZG 板端编译运行，不维护 Windows 编译路径。输出方式由 `configs/full_workflow.yaml` 控制：可以直接送 HDMI，也可以在板端保存 PNG 调试图。

## 1. 工程结构

```text
examples/2_Full_workflow/
  CMakeLists.txt
  configs/
    full_workflow.yaml      # 完整链路配置
  src/
    full_workflow.cpp       # 唯一主程序入口
  io/
    echo/                   # 输入 echo 回波 bin
    output/                 # output.mode=png 时保存调试图
```

编译后只生成：

```text
full_workflow
```

## 2. 编译

在板端或 Linux 交叉编译环境中，推荐使用脚本编译。脚本会把 CMake 配置和编译输出保存到 `log/`：

```bash
cd examples/2_Full_workflow
chmod +x build_full_workflow.sh
./build_full_workflow.sh
```

日志文件：

```text
log/cmake_configure.log
log/cmake_build.log
```

如果你想手动编译，也可以自己加 `tee`：

```bash
cd examples/2_Full_workflow
mkdir -p log
cmake -S . -B build/ZG -DCMAKE_BUILD_TYPE=Release 2>&1 | tee log/cmake_configure.log
cmake --build build/ZG -j$(nproc) 2>&1 | tee log/cmake_build.log
```
编译产物：

```text
build/ZG/full_workflow
```

如果运行时报 `Permission denied`：

```bash
chmod +x build/ZG/full_workflow
```

## 3. 准备输入

把 echo 回波 bin 放入：

```text
io/echo
```

程序会按文件名排序逐个处理，例如：

```text
io/echo/1_hh_amp_echo_1bit.bin
io/echo/1_hv_amp_echo_1bit.bin
io/echo/1_vh_amp_echo_1bit.bin
io/echo/1_vv_amp_echo_1bit.bin
```

模型路径在 `configs/full_workflow.yaml` 中配置：

```yaml
pipeline:
  icore:
    json: ./imodel/ZG/bf16/RAAUNet_DeepCA48_MobileNetV3Unet_V1_generated.json
    raw: ./imodel/ZG/bf16/RAAUNet_DeepCA48_MobileNetV3Unet_V1_generated.raw
```

运行前确认模型输入输出满足：

```text
input[0]  = [1,512,512,1]   # FP32，输入值为 RD 幅值 min-max 后的 0-1 patch
output[0] = [1,512,512,1]   # 恢复灰度图
output[1] = [1,512,512,6]   # 分割 logits
```

## 4. 运行

建议从 `examples/2_Full_workflow` 目录运行，因为配置里使用了 `./io`、`./imodel` 这类相对路径。

```bash
cd examples/2_Full_workflow
./build/ZG/full_workflow configs/full_workflow.yaml
```

程序内部流程：

```text
1. 遍历 io/echo 下的 echo bin
2. 使用 RD 算法生成复数 SAR 图，并对幅值做全图 min-max 得到 float32 0-1 SAR 图
3. 以 512x512、stride=256 自动蛇形裁 float32 0-1 patch
4. 构造 NHWC [1,512,512,1] FP32 输入 tensor
5. 调用 icraft session.forward 推理
6. output[0] 转恢复灰度图
7. output[1] 对最后一维 argmax，转 RGB 分割 mask
8. 左侧恢复图、右侧分割图并排合成
9. 根据 output.mode 送 HDMI 或保存 PNG
```

## 5. HDMI 输出

配置：

```yaml
output:
  mode: hdmi
```

HDMI 画面为左右并排：

```text
左侧：模型恢复灰度图
右侧：分割 mask RGB 图
```

显示参数：

```yaml
display:
  width: 1280
  height: 720
  fps: 0
```

建议先使用 `1280x720`，通常比 1080p 更稳。`fps: 0` 表示不主动限帧；如果需要限制显示速度，可以设置例如：

```yaml
display:
  fps: 10
```

## 6. PNG 调试输出

如果不接 HDMI，或者想保存结果排查，把输出模式改成：

```yaml
output:
  mode: png
  dir: ./io/output
  overwrite: true
```

输出路径示例：

```text
io/output/1_hh_amp_echo_1bit/patch_000000.png
io/output/1_hh_amp_echo_1bit/patch_000001.png
```

每张 PNG 仍然是左恢复图、右分割 mask。

如果不想覆盖已有结果：

```yaml
output:
  overwrite: false
```

## 7. Debug 参数

### 7.1 生成 generate_memopt.log

```yaml
debug:
  dump_backend_log: true
```

程序会在 `session.apply()` 后调用 `ZG330Backend::log()`，生成后端部署和内存优化日志。运行后检查：

```bash
find . -name "generate_memopt.log"
find . -path "*/.icraft/logs/*" -type f
```

如果不需要生成日志：

```yaml
debug:
  dump_backend_log: false
```

### 7.2 开启内部耗时统计

```yaml
sys:
  profile: true
```

这会开启 `session.enableTimeProfile(true)`。当前程序会打印每个 patch 的推理耗时和总耗时，后续控制端/性能页可以继续接更细的 profiling 结果。

### 7.3 输出等待超时

```yaml
pipeline:
  output_wait_ms: 20000
```

这是等待模型输出 ready 的最长超时时间，单位毫秒。它不是固定等待 20 秒，只是超时保护上限。

## 8. Patch 扫描参数

当前实现自动蛇形扫描：

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

代码里已预留 `ManualFlightPatchSource`，后续可以接上位机/控制端，实现“手动无人机巡航”：用无人机中心点控制当前裁剪的 512x512 patch。

## 9. 常见问题

### Permission denied

```bash
chmod +x build/ZG/full_workflow
```

### 找不到 echo 文件

确认运行目录：

```bash
pwd
# 应该是 examples/2_Full_workflow
```

确认输入文件：

```bash
ls io/echo
```

### 找不到模型 json/raw

检查配置路径：

```bash
ls ./imodel/ZG/bf16/
```

如果实际文件名不同，修改：

```yaml
pipeline:
  icore:
    json: ...
    raw: ...
```

### HDMI 没有画面

确认：

```yaml
output:
  mode: hdmi
```

并确认 HDMI 线和显示器分辨率支持。建议优先使用：

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

然后查看 `io/output` 下是否正常生成 PNG。

