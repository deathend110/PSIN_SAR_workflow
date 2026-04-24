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

**第二阶段再做 Web 控制台**
Web 控制台负责：

- start / pause / stop / reset
- 切 HDMI 显示模式
- 开关 patch 框、性能统计、保存中间结果
- 查看状态和日志
- 推送实时 telemetry
