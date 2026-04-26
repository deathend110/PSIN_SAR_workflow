> 说明：
> - 这份文档保留了阶段性设计和实现路线，适合看“为什么这么拆”。
> - 当前仓库的实时运行事实以 [main/README.md](./README.md) 为准，尤其是主菜单行为、`manual_flight` 现状、`HDMI STOPPED` 终止态，以及 `*.example.yaml -> 本地 *.yaml` 的配置语义。
> - 当前仓库只跟踪 `main/configs/*.example.yaml`，运行时会在同目录生成本地 `*.yaml` 副本并把 Web 修改后的配置写回这些本地副本。
> - `debug_raster` 调试模式已实现；设计与实现边界见 [task/TASK_DEBUG.md](../task/TASK_DEBUG.md)。它集成在现有 Web Console 的 `infer` 模式内，而不是新增主菜单 workflow。

**第一阶段优先做 HDMI 展示增强**
先不要急着做 Web 控制台。你现在主链路已经通了，最先能出效果的是在 HDMI 输出上加：

- 项目标题
- 当前模式
- 当前 echo / SAR / patch / frame 编号
- FPS
- SAR 成像耗时
- NPU 推理耗时
- 总延时
- 分割类别图例
- 底部流程状态条：`Echo -> SAR -> Slice -> Infer -> Post -> HDMI`

这个改动最直接，也最适合答辩/演示。

已完成

**第二阶段：HDMI和推理拆分为双线程，提高推理速度**
- 要怎么实现，你分析一下，然后落地到task\TASK_PHASE2.md

完成


**第三阶段再做 Web 控制台**
在板端运行一个简单的TCP，链接上位机浏览器生成一个web可视化控制台
Web 控制台负责：

- 可选择是1.RD成像还是2.模型推理模式
- 可选择1.自动蛇形patch模型，2.低自由度方向控制的 `manual_flight` 扫描游标模式，支持 `W/A/S/D` 改方向
- 自动蛇形patch模式：start / pause / stop / reset
- 可选择1.HDMI 显示模式，2.png保存模式
- 可自由选择底图：列出可选底图并点击加载
- 有设置页面，可设置一些飞行参数和RD参数，具体逻辑看config文件
- 目前没有实现的模块留空出接口就行

实现轻量级web控制台，与上述功能接口

当前补充说明：

- Web Console 的实际访问地址配置在 [web_console.example.yaml](g:/Docker_windows_disk/PSIN_SAR_workflow/main/configs/web_console.example.yaml)，首次运行会自动生成同名本地 `web_console.yaml` 副本
- `server.bind` 表示板子服务端监听地址
- `server.board_ip` 表示上位机浏览器实际应访问的板子 IP
- 当命令行选择：
  - `3. Web Console`
  终端会输出两类地址：
  - 监听地址，例如：`http://0.0.0.0:8080`
  - 板子访问地址，例如：`Board IP: 192.168.1.10:8080`
- 上位机浏览器应使用 `server.board_ip:server.port` 访问板子，而不是直接使用 `0.0.0.0`
- 典型配置示例：

```yaml
server:
  bind: 0.0.0.0
  board_ip: 192.168.1.10
  port: 8080
```

- 若电脑与板子通过网线直连，需要保证两端 IP 同网段，且浏览器访问的是这里配置的 `board_ip`



**第四阶段：重设计 `manual_flight` 为低自由度扫描游标模式**

目标不是继续做“自由飞行”的无人机模拟，而是把 `manual_flight` 收敛成一个和 patch 推理节拍严格对齐的扫描游标模式，优先保证：

- 操作语义简单
- HDMI 输出稳定
- 推理节拍可预测
- 不再出现“按一下方向却持续滑行”或“要等一会才来一帧”的体感

新的目标语义如下：

- 起点固定在左上角第一个合法 patch 中心，不再从图像中心开始
- 默认初始方向为向右
- 保持恒定移动，不再引入加速度、阻尼、惯性或 `Shift boost`
- `W/A/S/D` 只表示“切换方向”，不再表示按下/松开状态
- 同方向重复输入直接忽略，不做重复处理
- 多次方向输入不排队，只保留最新方向
- 每完成一个 patch，才推进到下一个 patch 中心
- 移动步长优先与 `stride` 对齐，不再依赖 `trigger_distance_px` 做位置触发
- 到边缘后直接停住，不反弹、不滑动、不重复提交同一 patch
- 若当前方向被边缘阻塞，则等待新的有效方向输入后再继续

线程与执行模型要求：

- 不再使用当前的自由飞行 simulation thread 作为 manual 运动主时钟
- `manual_flight` 的位置推进与 patch 完成绑定，由 infer worker 主循环推进
- HDMI render thread 继续只负责显示，不并入 manual 控制逻辑
- Web server / controller 继续只负责方向输入转发和状态展示

Web Console 接口语义要求：

- 保留 `manual_flight` 模式入口，但语义改为“方向控制的扫描游标”
- Web 输入只负责发送方向变更命令
- 若继续沿用 `/api/manual/key`，则内部语义也应收敛为“方向切换命令”，不再依赖 `keydown/keyup` 长按状态
- `Shift` 不再作为 manual 模式必要控制项

配置收敛要求：

- `path_overlay` 和 `control_bindings` 可以继续保留
- `boost_step_px` 不再作为新模式的关键参数
- `trigger_distance_px` 不再作为新模式的 patch 触发条件
- `cache_grid_px` 若仅服务旧的自由飞行路径采样，可退出主语义
- manual 模式的推进步长优先复用 `stride`

运行态与状态展示要求：

- `Pause`：暂停在当前 patch 中心，不再推进下一步
- `Resume`：从当前 patch 中心按当前方向继续
- `Stop`：停止后续 manual 推进和新 patch 触发
- `Reset`：回到左上角起点，方向恢复为默认向右，并清空 manual 运行态
- HDMI / Web 状态区应更关注当前方向、当前位置、边缘阻塞状态和已处理 patch 计数

当前实现与下一步改造关系说明：

- 当前仓库里的 `manual_flight` 已经接通 Web Console、controller 和 infer 主链，不再是保留接口
- 当前实现已经切换为低自由度扫描游标模型：
  - 左上角起点
  - 默认向右
  - `W/A/S/D` 只改方向
  - patch 完成后再推进一步
  - 到边缘停住，等待新的有效方向输入
- 旧的自由飞行骨架（`keydown/keyup` 长按驱动、速度/阻尼、`trigger_distance_px` 触发、manual simulation thread）已经退出主语义


**Debug Raster 调试模式**

为后续比对 GPU 浮点模型和板端量化模型的 patch 级恢复/分割效果，当前仓库已实现一个集成在 Web Console 中的 `debug_raster` 模式，设计见 [task/TASK_DEBUG.md](../task/TASK_DEBUG.md)。

当前已经确认的设计边界：

- `debug_raster` 不新增主菜单 workflow，而是作为 `infer` 的新增 patch mode
- 底图选择逻辑与普通 `infer` 一致
- patch 扫描逻辑独立于 `auto_snake`
  - 左到右
  - 上到下
  - 逐行扫描
- 行列步长继续使用像素值参数
- 输出固定为 PNG，不走 HDMI
- 每个 patch 分别落盘两张 `uint8` 图：
  - `output_dir/debug_<sar_stem>/restore/patch_000000.png`
  - `output_dir/debug_<sar_stem>/mask_class/patch_000000.png`

当前实现补充：
- `debug_raster` 已经接入现有 Web Console / `infer`
- `debug_raster` 保持 PNG-only，不允许 HDMI
- patch 扫描为左到右、上到下、逐行输出

这个模式的目标是“最小改动、最大复用现有 infer 与 PNG 输出链”，而不是新增一个大而全的调试 workflow。
