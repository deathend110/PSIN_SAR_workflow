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

**第二阶段再做显示模式切换**
推荐先实现这些模式：

- `SHOW_SAR`
- `SHOW_RESTORE`
- `SHOW_SEG`
- `SHOW_OVERLAY`
- `SHOW_COMPARE`

HDMI 默认可以用你文档里说的“左侧图像 + 右侧状态栏”，分辨率优先按 `1280x720` 设计，这个判断也很实际。

**第三阶段再做 Web 控制台**
Web 控制台负责：

- start / pause / stop / reset
- 切 HDMI 显示模式
- 开关 patch 框、性能统计、保存中间结果
- 查看状态和日志
- 推送实时 telemetry

这个顺序比“一上来做完整控制台”稳很多。我的建议是：先把 `prompt.md` 作为设计方向保留，下一步可以把它拆成一个更具体的实现计划：`OverlayRenderer`、`RuntimeState`、`DisplayMode`、`Telemetry`、`ControlServer` 这几个模块。