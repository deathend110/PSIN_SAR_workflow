# TASK_REBUILE_SCHEDULE: REBUILE 任务排期总表

## 0. Meta
- 范围：`TASK_REBUILE_01` ~ `TASK_REBUILE_11`
- 目标：给现有 11 条重建任务建立执行顺序、阶段门、并行关系和暂停点
- 排期原则：
  - 先修“能验证、能护栏、能减少失误”的近期项
  - 再做“收边界、拆职责、隔离硬件”的长期项
  - 任何会影响 host/native 测试通道的任务，优先级高于纯结构美化
  - 任何直接保护 700MB 内存 + 1GHz CPU + ZG330 板端稳定性的任务，优先级高于长期抽象

---

## 1. 总体分期

### Wave 0：排期准备
- [ ] 以本文件作为后续执行总入口
- [ ] 以后续实现时，严格按每个 `TASK_REBUILE_xxx.md` 的 `Allowed files to modify` 执行
- [ ] 每完成一个 Wave，先做验证，再进入下一个 Wave

### Wave 1：构建与回归入口
- [ ] `TASK_REBUILE_05`
- [ ] `TASK_REBUILE_01`

### Wave 2：控制面防护与配置安全
- [ ] `TASK_REBUILE_02`
- [ ] `TASK_REBUILE_03`
- [ ] `TASK_REBUILE_11`

### Wave 3：文档基线对齐
- [ ] `TASK_REBUILE_04`

### Wave 4：运行时状态与 Web 内部边界
- [ ] `TASK_REBUILE_09`
- [ ] `TASK_REBUILE_07`

### Wave 5：Infer 编排层收窄
- [ ] `TASK_REBUILE_06`

### Wave 6：硬件边界隔离
- [ ] `TASK_REBUILE_10`

### Wave 7：后续可选收敛
- [ ] `TASK_REBUILE_08`

---

## 2. 详细顺序

### Step 1
- 执行：`TASK_REBUILE_05`
- 原因：
  - 为 host/native 测试建立可运行的构建通道
  - 是 `TASK_REBUILE_01` 的强前置
- 阶段门：
  - 必须能区分 host/native 与 ZG330 构建入口
  - host/native 侧至少能开始构建测试目标

### Step 2
- 执行：`TASK_REBUILE_01`
- 原因：
  - 把已有 smoke/regression tests 接入正式入口
  - 后续 Web / config / infer 调整都需要稳定回归底座
- 阶段门：
  - `main_menu_regression_test`
  - `hdmi_stopped_regression_test`
  - 至少这两条测试进入 `ctest`

### Step 3
- 执行：`TASK_REBUILE_02`
- 原因：
  - 当前 Web 控制面最直接的鲁棒性缺口
  - 风险集中在 `web_console_server.cpp`，可局部修复
- 阶段门：
  - header/body 上限、读取超时、错误码路径可验证

### Step 4
- 执行：`TASK_REBUILE_03`
- 原因：
  - 当前配置写回会污染仓库
  - 还存在 `remove -> rename` 的失败窗口
- 阶段门：
  - example/local 配置边界明确
  - 正常退出后不再写脏仓库配置

### Step 5
- 执行：`TASK_REBUILE_11`
- 原因：
  - 直接保护板端资源预算
  - 防止 Web 参数把板端拖到运行时才失败
- 阶段门：
  - 明显非法或超预算参数会在 Web 层被拒绝
  - 非法参数不会被持久化

### Step 6
- 执行：`TASK_REBUILE_04`
- 原因：
  - 前 5 步完成后，文档基线才稳定
  - 过早修文档会很快再次漂移
- 阶段门：
  - README / 阶段文档与当前事实一致

### Step 7
- 执行：`TASK_REBUILE_09`
- 原因：
  - 这是 `infer_workflow.cpp` 当前最清晰的窄切口
  - 是 `TASK_REBUILE_06` 的前置
- 阶段门：
  - ManualFlight 状态显式对象化
  - 行为不变

### Step 8
- 执行：`TASK_REBUILE_07`
- 原因：
  - 在已有 Web 控制面边界稳定后，继续抽 parser / SSE / route
  - 不与 `TASK_REBUILE_02` 混做，避免近期补丁被大重构吞掉
- 阶段门：
  - parser / route / SSE 有独立边界与最小测试口

### Step 9
- 执行：`TASK_REBUILE_06`
- 原因：
  - 必须在 `TASK_REBUILE_09` 之后做
  - 否则会一边拆文件一边搬隐式状态，风险过高
- 阶段门：
  - `infer_workflow.cpp` 更接近编排层
  - 输出和设备访问顺序不变

### Step 10
- 执行：`TASK_REBUILE_10`
- 原因：
  - 这是长期价值最高的结构项之一
  - 但必须建立在构建、测试和 infer 编排边界已经清楚的前提上
- 阶段门：
  - fake backend 可用于 host 侧测试
  - 业务层不再直接贴着硬件实现写

### Step 11
- 执行：`TASK_REBUILE_08`
- 原因：
  - 当前不是近期阻塞问题
  - 只有当配置复杂度继续增长时才值得推进
- 阶段门：
  - 必须先证明现有简化 YAML 已经成为维护瓶颈

---

## 3. 可并行关系

### 可以并行准备，但不建议同时改代码
- `TASK_REBUILE_02` 和 `TASK_REBUILE_03`
  - 可以并行做设计/测试准备
  - 不建议同时改，因为都可能影响 Web 配置路径和验证方式

### 可以并行做文档整理
- `TASK_REBUILE_04`
  - 可以在 `02/03/11` 进入收尾时开始核对
  - 但正式落文档应等这些任务事实稳定

### 不应并行执行
- `TASK_REBUILE_05` 与 `TASK_REBUILE_01`
  - 前者是后者前置
- `TASK_REBUILE_09` 与 `TASK_REBUILE_06`
  - 前者是后者前置
- `TASK_REBUILE_06` 与 `TASK_REBUILE_10`
  - 同时碰 infer 边界，冲突风险高

---

## 4. 阶段门

### Gate A：进入 Wave 2 前
- [ ] `TASK_REBUILE_05` 完成近期子阶段
- [ ] `TASK_REBUILE_01` 完成
- [ ] host/native 回归测试入口稳定

### Gate B：进入 Wave 4 前
- [ ] `TASK_REBUILE_02` 完成
- [ ] `TASK_REBUILE_03` 完成
- [ ] `TASK_REBUILE_11` 完成
- [ ] Web 控制面和配置持久化边界稳定

### Gate C：进入 Wave 5 前
- [ ] `TASK_REBUILE_09` 完成
- [ ] ManualFlight 状态已显式对象化

### Gate D：进入 Wave 6 前
- [ ] `TASK_REBUILE_06` 完成
- [ ] infer 编排层边界清楚

---

## 5. 推荐近期排期

如果按最小可落地顺序执行，推荐就是：

1. `TASK_REBUILE_05`
2. `TASK_REBUILE_01`
3. `TASK_REBUILE_02`
4. `TASK_REBUILE_03`
5. `TASK_REBUILE_11`
6. `TASK_REBUILE_04`
7. `TASK_REBUILE_09`
8. `TASK_REBUILE_07`
9. `TASK_REBUILE_06`
10. `TASK_REBUILE_10`
11. `TASK_REBUILE_08`

---

## 6. 执行约束

- 不允许跨 Wave 随意插入大重构
- 每个 Wave 结束前必须做验证
- 如果某任务需要突破其 `Allowed files to modify`，必须先回到排期层重审
- `TASK_REBUILE_08` 默认不进入近期开发窗口，除非配置复杂度继续上涨

---

## 7. Done when

- 11 条重建任务有明确执行顺序
- 前置依赖和不可并行关系清楚
- 近期任务与长期任务分层清楚
- 后续实现者可以直接按本文件进入执行，而不需要重新做一次排期判断
