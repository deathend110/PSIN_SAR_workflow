# Task title

Phase 4 Fix2：将 `manual_flight` 从自由飞行模型重构为低自由度扫描游标模式

---

## 1. Background

这个任务存在的原因：

- 当前仓库里的 `manual_flight` 已经接通了 Web Console、controller 和 infer 主链，但现有语义仍然是“图像平面上的自由飞行 patch 中心点推进”。
- 当前实现包含：
  - `keydown/keyup` 长按状态
  - 速度、加速度、阻尼
  - `trigger_distance_px` 触发新 patch
  - latest-wins 请求模型
  - infer 内部 simulation thread
- 这套模型和实际 patch 推理 / HDMI 出帧节拍并不一致，导致：
  - 操作手感差
  - 方向输入容易产生“持续滑行”错觉
  - 用户体感上“按了方向但要等一会儿才出一帧”
- 当前阶段目标已经明确调整：
  - 不再继续做“自由飞行”
  - 改成“低自由度方向控制的扫描游标模式”

触发场景：

- `patch_mode = manual_flight`
- Web Console 运行推理工作流
- 输出模式可以是 `hdmi` 或 `png`

已知限制：

- 不重做 RD 主链
- 不重做底层 NPU / session 后端
- 不重做 Web Server 大线程模型

---

## 2. Goal

必须实现并可验证的目标行为：

- `manual_flight` 的语义改为“扫描游标模式”，不再是自由飞行模式
- 起点固定为左上角第一个合法 patch 中心，不再从图像中心开始
- 默认初始方向为向右
- `W/A/S/D` 只表示方向切换，不再表示按下/松开持续运动状态
- 同方向重复输入直接忽略
- 多次方向输入不排队，只保留最新方向
- 每完成一个 patch，才推进到下一个 patch 中心
- manual 模式的移动步长优先与 `stride` 对齐
- 到边缘后停住，不反弹、不滑动、不重复提交同一 patch
- 当前方向被边缘阻塞时，必须等待新的有效方向输入后再继续
- `Pause / Resume / Stop / Reset` 在新 manual 语义下仍保持清晰且可用
- HDMI / Web 状态区展示应与新语义一致，重点展示方向、位置、边缘阻塞状态和处理计数

---

## 3. Out of scope

- 不实现真实无人机飞控或三维飞行
- 不保留加速度、阻尼、惯性、`Shift boost` 作为新模式必要特性
- 不保留当前 simulation thread 驱动的连续运动模型
- 不重做 RD 主链
- 不修改底层 `deps/**`
- 不修改构建系统
- 不做新的 Web UI 风格重设计
- 不引入新三方依赖

---

## 4. Allowed files to modify

```text
main/src/infer_workflow.cpp
main/include/workflow/infer/infer_workflow.hpp
main/src/web_console_controller.cpp
main/include/workflow/web/web_console_controller.hpp
main/src/web_console_server.cpp
main/src/web_console_assets.cpp
main/src/web_console_protocol.cpp
main/include/workflow/web/web_console_protocol.hpp
main/src/web_console_config.cpp
main/include/workflow/web/web_console_config.hpp
main/configs/web_console.yaml
main/configs/infer_workflow.yaml
main/Host Computer Software.md
ARCHITECTURE_TEMPLATE.md
CODEBASE_MAP_TEMPLATE.md
task/TASK_PHASE4_FIX2.md
```

---

## 5. Files/modules to avoid

```text
main/src/rd_imaging_stream.cpp
main/src/rd_config.cpp
main/include/workflow/rd/**
main/include/workflow/shared/run_control.hpp
main/src/main.cpp
deps/**
cmake/**
third_party/**
task/TASK_PHASE1*.md
task/TASK_PHASE2*.md
task/TASK_PHASE3*.md
task/TASK_PHASE4.md
task/TASK_PHASE4_FIX1.md
```

---

## 6. Functional requirements

- [ ] `manual_flight` 起点固定为左上角第一个合法 patch 中心
- [ ] 默认初始方向为向右
- [ ] `W/A/S/D` 只触发方向切换，不再依赖 `keydown/keyup` 长按状态驱动持续运动
- [ ] 同方向重复输入被忽略
- [ ] 多次方向输入只保留最新方向，不做 FIFO 排队
- [ ] 新 patch 的生成时机与“上一个 patch 完成”绑定
- [ ] 新 patch 的移动步长与 `stride` 对齐，或有明确且稳定的一对一语义
- [ ] 到边缘后停住，不继续重复提交边缘 patch
- [ ] 被边缘阻塞后，收到一个有效新方向输入才能继续
- [ ] `Pause` 时停在当前 patch 中心，不再推进下一步
- [ ] `Resume` 时从当前 patch 中心按当前方向继续
- [ ] `Stop` 时停止后续 manual 推进和新 patch 触发
- [ ] `Reset` 时回到左上角起点，并恢复默认方向
- [ ] HDMI / Web 状态区展示当前方向、当前位置、边缘阻塞状态和计数
- [ ] 当前 manual 模式不再依赖 `trigger_distance_px` 作为 patch 触发条件
- [ ] 当前 manual 模式不再依赖 `Shift` 作为必要控制项

---

## 7. Non-functional requirements

- [ ] 保持最小改动范围
- [ ] 不引入新依赖
- [ ] 不重做 Web server 大线程模型
- [ ] 不重做 HDMI render thread 职责
- [ ] 不重做 infer 主链的 NPU / session 生命周期
- [ ] 控制语义必须 review 友好、可预测
- [ ] 避免旧的“输入连续、显示离散”错位模型继续残留

---

## 8. Interface expectations

建议保留现有接口路径，但收敛其内部语义：

```text
POST /api/manual/key
```

如果短期不改接口结构，允许继续传：

```json
{
  "key": "w",
  "action": "down"
}
```

但在新模式下语义要求是：

- 只把它解释为“方向切换命令”
- `up` 不再承担旧的释放语义
- `shift` 不再作为新模式核心控制项

更理想的内部目标语义：

```text
direction = up | down | left | right
```

controller 侧要求：

- `commandManualKey(...)` 只负责方向输入校验与转发
- 不在 controller 里直接算 patch

infer 侧要求：

- manual runtime 改成“离散游标推进器”
- 位置推进时机与 patch 完成绑定
- 当前方向和待应用方向分离

---

## 9. Chosen implementation direction

这个任务的设计方向已经固定，不再回到“自由飞行”：

1. `manual_flight` 不是自由飞行器，而是扫描游标
2. 不再用 simulation thread 作为 manual 主时钟
3. 不再用速度、加速度、阻尼、`trigger_distance_px` 驱动 manual 前进
4. 推进节拍固定为：

```text
完成当前 patch
 -> 读取最新方向
 -> 计算下一中心点
 -> 若可移动则提交下一个 patch
 -> 若撞边则停住等待新方向
```

5. 方向输入采用 latest-wins，但位置推进必须一步一步走，不跳过逻辑步长
6. HDMI render thread 继续只负责显示
7. Web 前端只负责发送方向变更，不再维护旧的长按运动语义

---

## 10. File-level design

### `main/src/infer_workflow.cpp`

- 删除或废弃当前 manual 自由飞行骨架里的连续运动逻辑
- 将 manual runtime 改为“离散扫描游标推进器”
- 起点改为左上角第一个合法 patch 中心
- 当前方向默认向右
- patch 完成后才推进下一步
- 碰到边缘后进入阻塞等待，而不是滑动或重复提交

### `main/include/workflow/infer/infer_workflow.hpp`

- 若需要，对 manual runtime 的公开状态接口做最小收敛
- 去掉或弱化自由飞行特有语义

### `main/src/web_console_controller.cpp`

- `commandManualKey(...)` 改成“方向切换命令”的桥接层
- 不再依赖旧的 press/release 运动语义
- `Pause / Resume / Stop / Reset` 在新 manual 模式下语义明确

### `main/src/web_console_assets.cpp`

- 键盘输入不再以长按状态驱动运动
- 前端只负责发送方向切换
- 同方向重复输入可在前端做轻量去重，但以后端为准
- 状态栏文案更新为新语义

### `main/src/web_console_protocol.cpp`

- 若状态响应中已有 manual telemetry，需要收敛成新语义：
  - 当前方向
  - 当前位置
  - 是否边缘阻塞
  - 处理计数

### `main/configs/web_console.yaml`

- 保留 `path_overlay`
- 明确哪些旧参数在新模式下被忽略或弱化

### `main/configs/infer_workflow.yaml`

- 若 `manual_flight` 默认步长与 `stride` 对齐，需要保证文档和默认配置一致

---

## 11. Parameter semantics

新 manual 模式下参数语义要求：

- `path_overlay`
  - 保留，控制是否显示轨迹或路径叠加

- `control_bindings`
  - 保留，默认仍为 `W/A/S/D`

- `manual_step_px`
  - 如继续保留，必须重新定义为“离散推进步长”
  - 若项目决定直接复用 `stride`，则该参数在 manual 模式下可以降级或忽略

- `boost_step_px`
  - 在新模式下不再作为关键参数

- `trigger_distance_px`
  - 在新模式下不再作为 patch 触发条件

- `cache_grid_px`
  - 如果只服务旧自由飞行路径采样，则可以退出主语义

---

## 12. State model expectations

manual 模式至少需要这些状态：

- `Idle`
- `Running`
- `Paused`
- `Stopping`

manual 运行态至少需要这些字段：

- `current_center`
- `current_direction`
- `pending_direction`
- `edge_blocked`
- `patch_count`

不再把这些作为新语义主字段：

- `velocity_px`
- `acceleration`
- `damping`
- `trigger_distance`

---

## 13. Edge cases

必须考虑：

- 图像尺寸刚好等于 `patch_size`
- 图像尺寸小于 `patch_size`
- 起点就是唯一合法 patch
- 用户连续输入同方向
- 用户在当前 patch 推理中切换多次方向
- 用户在边缘持续输入被阻塞方向
- 用户在边缘切换到可离开边缘的新方向
- `Pause` 时输入新方向
- `Stop` / `Reset` 过程中输入方向
- `hdmi` 与 `png` 输出路径下的新 manual 语义是否一致

---

## 14. Validation

```text
1. 语法级检查
   - main/src/web_console_controller.cpp
   - main/src/web_console_server.cpp
   - main/src/web_console_assets.cpp
   - main/src/web_console_protocol.cpp
   - main/src/web_console_config.cpp
   - 若环境允许，检查 main/src/infer_workflow.cpp

2. 静态核对
   - 确认 manual 不再依赖 simulation thread 连续推进
   - 确认 manual 不再依赖 trigger_distance_px 驱动 patch 触发
   - 确认 Start 后起点在左上角
   - 确认默认方向为向右
   - 确认边缘不会重复提交同一 patch

3. 板端功能验证
   - 进入 manual_flight
   - Start 后先处理左上角 patch
   - 不输入时按默认方向推进
   - 输入 W/A/S/D 只改变方向
   - 同方向重复输入无副作用
   - 到边缘后停住
   - 输入新方向后继续
   - Pause / Resume / Stop / Reset 语义正确

4. 体感验证
   - 不再出现“按一下方向却持续滑行”
   - 不再依赖 keyup 才能停住
   - patch 节拍与 HDMI 显示节拍一致或可解释
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

- `manual_flight` 不再表现为自由飞行模型
- 起点改为左上角
- 默认方向向右
- 推进节拍与 patch 完成绑定
- `W/A/S/D` 只改方向
- 同方向重复输入无副作用
- 到边缘停住，不重复跑边缘 patch
- `Pause / Resume / Stop / Reset` 在新语义下行为正确
- HDMI / Web 状态展示与新语义一致
- 修改范围集中在 manual 相关文件内
- diff 可 review

## 18. md update

需要随着代码更新而同步更新的文档：

- `main/Host Computer Software.md`
- `ARCHITECTURE_TEMPLATE.md`
- `CODEBASE_MAP_TEMPLATE.md`

