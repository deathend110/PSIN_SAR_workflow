# Task title

Phase 4：实现 manual_flight 无人机模式，并接通 Web Console 控制、推理触发与 HDMI UI 展示
---

## 1. Background

这个任务为什么存在？

- 当前仓库已经为 `manual_flight` 预留了一部分骨架，但仍未形成可运行闭环：
  - [main/src/infer_workflow.cpp](G:/Docker_windows_disk/PSIN_SAR_workflow/main/src/infer_workflow.cpp) 中已有 `ManualFlightPatchSource`
  - [main/src/web_console_server.cpp](G:/Docker_windows_disk/PSIN_SAR_workflow/main/src/web_console_server.cpp) 中已有 `POST /api/manual/key`
  - [main/src/web_console_assets.cpp](G:/Docker_windows_disk/PSIN_SAR_workflow/main/src/web_console_assets.cpp) 中已有 `manual_flight` 选项与 `W/A/S/D` 键盘监听
  - [main/src/web_console_controller.cpp](G:/Docker_windows_disk/PSIN_SAR_workflow/main/src/web_console_controller.cpp) 中已有 `commandManualKey(...)` 占位
- 但当前真实行为仍是“保留阶段，未实现”：
  - `manual_flight` 启动会被拒绝
  - `manual_flight` 下的 `Pause` 被拒绝
  - `commandManualKey(...)` 直接返回 `not_implemented`
  - [main/src/infer_workflow.cpp](G:/Docker_windows_disk/PSIN_SAR_workflow/main/src/infer_workflow.cpp) 仍会抛出 `manual_flight is reserved for a future phase.`
- 当前 Web 配置里已经有一组 `flight.*` 参数，并且已经接入读写闭环：
  - `flight.manual_step_px`
  - `flight.boost_step_px`
  - `flight.trigger_distance_px`
  - `flight.cache_grid_px`
  - `flight.path_overlay`
  - `flight.control_bindings`
- 当前系统线程结构也已经比较明确：
  - Web server 独立线程
  - workflow worker 独立线程
  - HDMI 模式下还有独立 render 线程
- 本阶段的目标不是重做整套推理架构，而是在现有结构上把 `manual_flight` 接成一个可控、可停、可显示、可扩展的运行模式。

---

## 2. Goal

明确写出要实现的行为。
**必须是可验证的。**

- 用户在 Web Console 中将 `patch_mode` 设为 `manual_flight` 后，可以通过 `W/A/S/D` 控制 patch 中心点在 SAR 大图上的上下左右移动。
- `manual_flight` 必须真正进入推理链，而不再停留在 `not_implemented`。
- 控制输入、飞行状态推进、推理执行、HDMI/UI 展示必须职责清晰，不允许把所有逻辑塞进单一线程。
- `manual_flight` 必须复用现有 `flight.*` 配置项，并为这些参数赋予明确语义。
- Web Console 必须定义并实现 `manual_flight` 所需接口，而不是只保留按钮和占位错误。
- 在 HDMI 模式下，UI 可以展示当前位置、轨迹或其它必要飞行叠加信息，并受 `flight.path_overlay` 控制。
- 在不明显影响工作流输出 FPS 的前提下，可选支持一个简单的“加速度 + 速度 + 阻尼”系统，模拟飞行惯性。

---

## 3. Out of scope

明确不做什么。

- 不实现真实无人机硬件接入
- 不实现网络手柄、串口摇杆或遥控器支持
- 不实现三维飞行、姿态角、航向角、真实飞控动力学
- 不改 RD 主链
- 不重做 Web server 线程模型
- 不改 NPU / session 底层库接口
- 不引入新三方依赖
- 不做整页 UI 重设计
- 不做自动路径规划、自动巡航、避障
- 不做多用户同时控制

---

## 4. Allowed files to modify

只列允许改的文件。
```text
main/src/infer_workflow.cpp
main/src/web_console.cpp
main/src/web_console_server.cpp
main/src/web_console_controller.cpp
main/src/web_console_assets.cpp
main/src/web_console_protocol.cpp
main/src/web_console_config.cpp
main/include/workflow/web/web_console_controller.hpp
main/include/workflow/web/web_console_server.hpp
main/include/workflow/web/web_console_protocol.hpp
main/include/workflow/web/web_console_config.hpp
main/configs/web_console.yaml
main/configs/infer_workflow.yaml
task/TASK_PHASE4.md
```

---

## 5. Files/modules to avoid

写清楚不许动的范围。
```text
main/src/rd_imaging_stream.cpp
main/src/rd_config.cpp
main/include/workflow/rd/**
main/include/workflow/shared/run_control.hpp
deps/**
cmake/**
third_party/**
ARCHITECTURE_TEMPLATE.md
CODEBASE_MAP_TEMPLATE.md
task/TASK_PHASE3.md
task/TASK_PHASE3_FIX1.md
task/TASK_PHASE3_FIX2.md
task/TASK_PHASE3_FIX3.md
task/TASK_PHASE3_FIX4.md
```

---

## 6. Chosen implementation direction

这项决策已经确定，不再二选一：

- `manual_flight` 不与推理执行共用同一个“阻塞式控制循环”。
- `manual_flight` 也不并入 HDMI UI 线程。
- 推荐结构固定为四层：
  1. Web 输入层
  2. Flight runtime 状态推进层
  3. Inference worker 推理执行层
  4. HDMI/UI 展示层
- Web 输入层只负责接收 `keydown/keyup` 或按钮事件，并维护当前按键状态，不直接做大步位移。
- Flight runtime 负责：
  - 根据当前按键状态推进位置
  - 可选推进速度/加速度
  - 维护轨迹
  - 判断是否跨过 `trigger_distance_px`
  - 生成“新的 patch 推理请求中心点”
- Inference worker 只消费“最新请求中心点”，采用 latest-wins 语义，不积压历史 patch 请求。
- HDMI/UI 线程只负责显示，不负责物理推进，不直接调推理。
- `manual_flight` 的几何控制对象定义为“图像平面上的 patch 中心点”，不是三维飞行器状态。
- `W/A/S/D` 语义固定为：
  - `W`：上
  - `S`：下
  - `A`：左
  - `D`：右
- 若实现惯性，必须是轻量固定步长模型：
  - `intent -> acceleration -> velocity -> position`
  - 松键后通过阻尼减速
  - 不能把每帧都变成一次推理
- 推理触发原则固定为：
  - 位置变化达到 `trigger_distance_px` 才触发新的 patch 推理
  - 推理中如果又收到了更新的位置，只保留最新目标，不排队处理旧目标

---

## 7. Functional requirements

列成可验收条目：

- [ ] `patch_mode = manual_flight` 时，`Start` 能真正进入运行态
- [ ] `W/A/S/D` 可以驱动 patch 中心点移动
- [ ] `keydown` 与 `keyup` 都被正确处理
- [ ] `manual_flight` 不再返回 `not_implemented`
- [ ] 推理输入 patch 来自当前飞行中心点，而非蛇形扫描
- [ ] 飞行中心点必须被限制在合法图像边界内
- [ ] 位置变化未达到 `trigger_distance_px` 时，不触发新的 patch 推理
- [ ] 位置变化达到 `trigger_distance_px` 时，会触发新的 patch 推理
- [ ] manual 模式下推理请求采用 latest-wins，不积压陈旧请求
- [ ] `Pause` / `Stop` / `Reset` 在 `manual_flight` 下语义清晰且可用
- [ ] HDMI 模式下可显示飞行位置相关 UI 信息
- [ ] `flight.path_overlay = false` 时，不显示路径叠加
- [ ] `flight.path_overlay = true` 时，可以显示路径叠加
- [ ] `flight.control_bindings` 至少能继续表达 `W/A/S/D`
- [ ] Web Console 的保留按钮 `/api/manual/key` 能与真实运行态接通

---

## 8. Non-functional requirements

- [ ] 保持最小范围修改
- [ ] 不引入新依赖
- [ ] 不改变当前 Web server / worker / HDMI render 的大线程框架
- [ ] 不把输入响应绑定到单次 patch 推理耗时上
- [ ] 不允许推理请求无限排队
- [ ] 长时间运行不能出现路径点无上限增长导致的明显内存累积
- [ ] 当用户快速连续按键时，系统仍应以“最新状态”为准，而不是回放历史动作
- [ ] 设计必须 review 友好，职责分层要清楚

---

## 9. Interface expectations

给出希望的接口草案：

### Web API

继续使用现有接口，并补全语义：

```text
POST /api/manual/key
```

请求体建议固定为：

```json
{
  "key": "w",
  "action": "down"
}
```

```json
{
  "key": "w",
  "action": "up"
}
```

如需支持加速键，可扩展为：

```json
{
  "key": "shift",
  "action": "down"
}
```

### Controller 层

```cpp
class WebConsoleController {
public:
    std::string commandManualKey(const std::unordered_map<std::string, std::string>& fields);
};
```

说明：
- `commandManualKey(...)` 不应直接做重推理，而应只更新控制状态或转发到 manual runtime。

### Infer 层

建议把当前单次 patch 获取器扩展成“可持续更新中心点”的 manual source / runtime：

```cpp
class ManualFlightRuntime {
public:
    void setKeyState(std::string_view key, bool pressed);
    void tick(double dt_seconds);
    bool shouldTriggerInference() const;
    PatchPacket makePatch() const;
    void markInferenceCommitted();
};
```

说明：
- 具体类名可以调整
- 但职责必须覆盖：
  - 输入状态
  - 位置推进
  - 推理触发判定
  - 当前 patch 生成
  - 已完成推理点更新

---

## 10. File-level design

### `main/src/infer_workflow.cpp`

- 将 `manual_flight` 从“保留模式”接入真实推理流程
- 复用或重构 `ManualFlightPatchSource`
- 增加 manual runtime 的位置状态、边界钳制、触发判定
- 推理请求模型采用 latest-wins
- 在 HDMI UI 合成路径中接入当前位置、轨迹或相关叠加信息
- `Pause/Stop` 必须在 manual 模式下也能走通

### `main/src/web_console_controller.cpp`

- 实现 `commandManualKey(...)`
- 对输入字段做严格校验：
  - `key`
  - `action`
- 只允许在 `manual_flight` 运行态下接收手动控制
- 将按键状态交给 manual runtime，而不是在 controller 里直接计算 patch
- 让 `Start/Pause/Stop/Reset` 在 `manual_flight` 下有明确定义

### `main/src/web_console_server.cpp`

- 保留并正式启用 `POST /api/manual/key`
- 补参数校验与错误响应边界
- 不在 server 层实现飞行逻辑

### `main/src/web_console_assets.cpp`

- 前端键盘事件必须完整区分 `keydown` / `keyup`
- 避免重复按住键时发送无意义风暴请求
- `manual_flight` 状态下才激活键盘控制
- 需要把飞行状态展示接到现有状态区或 UI 叠加展示中

### `main/src/web_console_protocol.cpp`

- 若状态响应需要补飞行 telemetry，则在这里扩展序列化
- 例如：
  - 当前位置
  - 当前速度
  - 是否存在待处理目标中心点

### `main/src/web_console_config.cpp`

- 保持 `flight.*` 配置读写闭环
- 若新增少量必要字段，应纳入 `LoadConfig(...) / SaveConfig(...)`

### `main/configs/web_console.yaml`

- 保持并明确 `flight.*` 的说明和值域建议

### `main/configs/infer_workflow.yaml`

- 如 manual 模式对 stride、patch_mode 默认值等有最小配置要求，可做最小补充

---

## 11. Parameter semantics

要求把当前已有参数正式定义清楚：

- `flight.manual_step_px`
  - 无惯性模式下：每个逻辑 tick 的基础位移
  - 有惯性模式下：基础速度上限的参考值

- `flight.boost_step_px`
  - boost 或快速移动时的速度上限参考值

- `flight.trigger_distance_px`
  - 当前位置与上次完成推理位置的距离阈值
  - 是控制推理频率的关键参数

- `flight.cache_grid_px`
  - 用于 manual 模式结果缓存或网格量化的基础单元
  - 如果第一版未实现缓存，也必须保留配置兼容，并在代码中留清晰语义

- `flight.path_overlay`
  - 是否显示飞行轨迹叠加

- `flight.control_bindings`
  - 当前默认值继续为 `W/A/S/D`
  - 如果暂不支持自定义映射，也要保证配置项不会破坏行为

---

## 12. State model expectations

manual_flight 至少需要明确这些状态：

- `Idle`
- `Running`
- `Paused`
- `Stopping`

并且至少需要维护这些运行态字段：

- `position_px`
- `velocity_px`
- `requested_center_px`
- `last_inferred_center_px`
- `active_keys`
- `path_points`

要求：
- `Stop` 能终止后续 manual 推进与新推理触发
- `Pause` 能冻结位置推进
- `Reset` 能清理路径、速度和当前位置状态

---

## 13. Edge cases

要求 Codex 必须考虑这些：

- 图像尺寸小于 `patch_size`
- patch 中心点靠近边界
- 用户长按按键
- 用户快速交替按 `W/A/S/D`
- 用户在推理执行中连续改变方向
- 用户在 `Paused` 时按键
- 用户在 `Stopping` 时按键
- `manual_flight` 下点击 `Reset`
- HDMI 模式与 PNG 输出模式下行为差异
- `trigger_distance_px <= 0`
- `manual_step_px > boost_step_px`
- `path_overlay = false`
- 键盘事件丢失 `keyup`

---

## 14. Validation

明确要求运行什么验证：

```text
1. g++ -fsyntax-only 覆盖：
   - main/src/infer_workflow.cpp
   - main/src/web_console_controller.cpp
   - main/src/web_console_server.cpp
   - main/src/web_console_assets.cpp
   - main/src/web_console_protocol.cpp
   - main/src/web_console_config.cpp

2. 静态检查：
   - 确认 manual_flight 不再返回保留阶段错误
   - 确认 commandManualKey(...) 已接通真实运行态
   - 确认 latest-wins 语义成立，而不是 FIFO patch 队列
   - 确认 HDMI/UI 线程不直接承担飞行推进逻辑
   - 确认 flight.* 配置与代码语义一致

3. 板端功能验证：
   - Web Console 选择 manual_flight
   - Start 后按 W/A/S/D 能驱动位置变化
   - Pause 后位置冻结
   - Stop 后不再触发新 patch
   - Reset 后路径和运行态清空
   - HDMI 模式下能看到位置/轨迹叠加
   - 长按按键时不会出现推理请求堆积导致明显滞后

4. 性能验证：
   - 对比 auto_snake 与 manual_flight 的输出 FPS
   - 确认开启 path overlay 后无明显异常掉帧
   - 若实现惯性，确认逻辑更新不会成为主要性能瓶颈
```

---

## 15. Required response format before editing

要求 Codex 在动手前先输出：

1. 对任务的理解
2. 计划修改的文件
3. 不会修改的文件
4. 实现方案
5. 风险点
6. 验证方案

---

## 16. Required response format after editing

要求 Codex 改完后输出：

1. 修改了哪些文件
2. 每个文件做了什么改动
3. 为什么这样设计
4. 跑了哪些命令
5. 哪些验证通过 / 未通过
6. 剩余风险

---

## 17. Done when

写成客观验收标准：

- `manual_flight` 能真正运行，不再是占位模式
- `W/A/S/D` 能控制 patch 中心点移动
- manual 模式的推理触发遵循 `trigger_distance_px`
- 推理请求采用 latest-wins，而不是历史排队
- `Pause/Stop/Reset` 在 manual 模式下语义正确
- HDMI/UI 能展示必要飞行信息
- `flight.*` 参数与实现行为一致
- diff 范围控制在 Web Console 与 infer manual 模式相关文件内
- 改动可 review，线程职责清晰


## 18. md update

需要你随着代码更新而更新的文件：
- ARCHITECTURE_TEMPLATE.md
- CODEBASE_MAP_TEMPLATE.md
- main/Host Computer Software.md
