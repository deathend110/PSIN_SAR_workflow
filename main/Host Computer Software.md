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
- 可选择1.自动蛇形patch模型，2.手动操控的无人机模拟飞行模式，支持上下左右wasd（无人机模式实现依然留空）
- 自动蛇形patch模式：start / pause / stop / reset
- 可选择1.HDMI 显示模式，2.png保存模式
- 可自由选择底图：列出可选底图并点击加载
- 有设置页面，可设置一些飞行参数和RD参数，具体逻辑看config文件
- 目前没有实现的模块留空出接口就行

实现轻量级web控制台，与上述功能接口

当前补充说明：

- Web Console 的实际访问地址配置在 [web_console.yaml](g:/Docker_windows_disk/PSIN_SAR_workflow/main/configs/web_console.yaml)
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



**第四阶段：实现无人机模式**

- wasd控制无人机上下左右
- 你分析一下无人机模拟模块应该和推理一个线程还是应该分开，并入HDMI UI线程
- （可选）若可在不影响整题工作流输出FPS下，可以设计一个简单的加速度和速度系统，模拟实际无人机飞行的加速减速惯性。


