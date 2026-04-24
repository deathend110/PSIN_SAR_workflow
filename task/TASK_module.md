# Task Title

- 单入口模块化重构：将 `main/src/infer_workflow.cpp` 与 `main/src/rd_imaging_stream.cpp` 的入口逻辑收口为一个 `main.cpp`，并按中等粒度拆出可维护的模块边界

---

## 1. Background

这个任务存在的原因：

- 当前两个主流程分别放在两个大 `.cpp` 中，入口分散，阅读和 review 成本高
- RD 成像与推理流程已经打通，但由于内存与设备资源约束，二者不能在 v1 中同次进程自动串联
- 后续还会继续扩展控制端和模式切换，因此需要先把主入口和模块边界收干净

当前已知事实：

- RD 成像主路径来自 `main/src/rd_imaging_stream.cpp`
- 推理主路径来自 `main/src/infer_workflow.cpp`
- 当前工程运行目标是 `Linux + aarch64 + ZG330`
- `ManualFlightPatchSource` 只是预留扩展点，不属于本次接线范围

---

## 2. Goal

需要实现并可验证的目标：

1. 主程序只保留一个入口 `main/src/main.cpp`
2. 用户可见主程序名统一为 `psin_workflow`
3. 启动后显示一个简单菜单，只支持：
   - `RD only`
   - `Inference only`
   - `Exit`
4. `RD only` 调度 RD 模块，不改变原有成像逻辑、输出路径与 scratch 生命周期
5. `Inference only` 调度推理模块，不改变原有 patch 规则、模型接口、HDMI/PNG 输出语义
6. RD 和推理模块分别暴露单一顶层运行接口，供 `main.cpp` 调用
7. 将重复的轻量基础能力抽到共享模块，但不做过度公共化
8. 更新 `ARCHITECTURE_TEMPLATE.md`、`CODEBASE_MAP_TEMPLATE.md`、`main/README.md`，使文档反映单入口结构
9. 将推理阶段残留的旧命名 `full_workflow` 统一为更符合实际职责的 `infer_workflow`，包括源码文件名、配置文件名、HDMI 适配头文件名及对应引用，但不改变对外运行行为

---

## 3. Out Of Scope

本次明确不做：

- 单进程自动串联 `RD + Inference`
- HDMI UI 增强或接入 `TASK_PHASE1.md` 的额外界面需求
- 控制端协议、Web 界面、网络通信
- 修改模型输入输出接口
- 变更 patch 规则
- 引入新依赖
- 大规模重写算法实现
- 重写底层 HDMI 适配层

---

## 4. Allowed Files To Modify

允许修改：

- `main/CMakeLists.txt`
- `main/src/main.cpp`
- `main/src/config_utils.cpp`
- `main/src/rd_config.cpp`
- `main/src/infer_config.cpp`
- `main/src/infer_workflow.cpp`
- `main/include/infer_workflow_hdmi_display.hpp`
- `main/configs/infer_workflow.yaml`
- `main/configs/rd_imaging.yaml`
- `main/include/workflow/**`
- `main/README.md`
- `ARCHITECTURE_TEMPLATE.md`
- `CODEBASE_MAP_TEMPLATE.md`
- `task/TASK_module.md`

原则上不应修改：

- `deps/**`
- `icraft/**`
- `BOOT_*`
- `main/imodel/**`
- `main/src/rd_imaging_stream.cpp`
- `main/src/hdmi_ui_preview_1080_p_industrial.jsx`
- `main/Host Computer Software.md`
- `task/TASK_PHASE1.md`
- `tools/build_all_examples.sh`

---

## 5. Functional Requirements

### 5.1 主入口

- 新增统一主入口 `main/src/main.cpp`
- 菜单必须固定提供：
  - `1. RD only`
  - `2. Inference only`
  - `0. Exit`
- 非法输入必须可重试或安全退出

### 5.2 RD 模块

- 继续读取 `configs/rd_imaging.yaml`
- 继续从 `io/echo/*.bin` 读取输入
- 继续输出到 `io/sar_img/*.png`
- 继续保留原有 `rd.execution_mode` 选择逻辑：
  - `auto`
  - `memory_float32`
  - `memory_double`
  - `scratch_double`
- 继续保留 scratch 清理语义

### 5.3 推理模块

- 继续读取 `configs/infer_workflow.yaml`
- 继续从 `io/sar_img/*.png` 读取输入
- 继续按以下规则扫描 patch：
  - `patch_size=512`
  - `stride=256`
  - `auto_snake`
  - 边缘不足完整 patch 直接丢弃
- 继续保持模型接口：
  - input: `FP32 [1,512,512,1]`
  - output[0]: `FP32 [1,512,512,1]`
  - output[1]: `FP32 [1,512,512,6]`
- 继续支持：
  - `output.mode=hdmi`
  - `output.mode=png`
- `output.mode=png` 时输出路径保持：
  - `io/output/<stem>/patch_*.png`
- 允许将推理阶段内部文件与配置命名从 `full_workflow` 统一为 `infer_workflow`
  - `src/full_workflow.cpp -> src/infer_workflow.cpp`
  - `configs/full_workflow.yaml -> configs/infer_workflow.yaml`
  - `include/full_workflow_hdmi_display.hpp -> include/infer_workflow_hdmi_display.hpp`
  - `main.cpp`、CMake、文档与日志文案必须同步更新
  - 不改变 `workflow::infer::Run(...)`、模型接口、patch 规则、HDMI/PNG 语义

### 5.4 模块边界

- 共享模块只抽取轻量、真实重复的基础能力
- RD 与推理分别暴露单一顶层接口：
  - `workflow::rd::Run(...)`
  - `workflow::infer::Run(...)`
- 允许模块内部保留私有辅助函数，不要求每个 helper 都单独拆文件

---

## 6. Non-functional Requirements

- 保持算法行为不变
- 保持 I/O 路径与输出命名不变
- 不引入新依赖
- 不做与任务无关的重构
- 保持 patch 可 review
- 保持资源生命周期清晰
- 保持现有异常处理风格一致

---

## 7. Interface Expectations

建议的接口边界：

```cpp
namespace workflow::rd {
int Run(const std::filesystem::path& config_path);
}

namespace workflow::infer {
int Run(const std::filesystem::path& config_path);
}
```

共享模块至少应支持：

- 简单 YAML 文本读取
- 字符串 trim / lower / bool parse
- 运行模式枚举

要求：

- `main.cpp` 不直接塞入 RD / 推理细节
- RD 和推理模块之间不要共享设备状态
- 不以“抽象漂亮”为理由引入过多层次

---

## 8. Edge Cases

Codex 在实现时必须考虑：

- SAR 图片小于 `512x512` 时继续跳过
- 菜单非法输入不能导致崩溃
- `debug.dump_backend_log=true` 时行为保持不变
- `output.mode=png` 时覆盖策略保持配置语义
- `waitForReady()` 与 `device.reset(1)` 的时序不被破坏
- Windows 本机不能完整构建时，需要如实说明验证边界

---

## 9. Validation

应执行的验证：

### 构建 / 语法

- 尝试执行 `cmake -S main -B main/build/ZG -DCMAKE_BUILD_TYPE=Release`
- 若平台约束导致无法 configure，需记录原因
- 对可在当前环境单独验证的新增/改动源文件做语法检查

### 功能回归

- 核对菜单只包含三种模式
- 核对主程序用户可见名称统一为 `psin_workflow`
- 核对 `RD only` 仍使用 `configs/rd_imaging.yaml`
- 核对 `Inference only` 仍使用 `configs/infer_workflow.yaml`
- 核对 CMake 当前只构建一个主程序

### 文档一致性

- `main/README.md` 与当前目录结构一致
- `ARCHITECTURE_TEMPLATE.md` 反映单入口架构
- `CODEBASE_MAP_TEMPLATE.md` 反映新的主调用链和模块边界
- `task/TASK_module.md` 中的允许修改范围、目标和验证项与实际 patch 一致

### 命名一致性

- 核对 `src/infer_workflow.cpp`、`include/infer_workflow_hdmi_display.hpp`、`configs/infer_workflow.yaml` 的引用全部接通
- 检查当前工程语义相关位置不再残留：
  - `src/full_workflow.cpp`
  - `include/full_workflow_hdmi_display.hpp`
  - `configs/full_workflow.yaml`
- 允许历史或无关目录保留旧名，不作为本次任务目标

### 推荐静态检查

- `rg -n "full_workflow|full_workflow_hdmi_display|configs/full_workflow.yaml|src/full_workflow.cpp" ...`
- `rg -n "infer_workflow|configs/infer_workflow.yaml|infer_workflow_hdmi_display" ...`
- `git diff --check -- <本次目标文件>`
- `bash -n main/build_main.sh`
- `g++ -std=c++17 -fsyntax-only`：
  - `main/src/main.cpp`
  - `main/src/infer_config.cpp`
  - `main/src/rd_config.cpp`
  - `main/src/rd_imaging_stream.cpp`
- 如 `infer_workflow.cpp` 因当前机器缺少板端依赖无法完整编译，需在结论中明确记录验证边界，而不是算作已通过

---

## 10. Required Response Format Before Editing

动手前需要明确输出：

1. 对任务的理解
2. 计划修改的文件
3. 不会修改的文件
4. 实现思路
5. 风险点
6. 验证方案

---

## 11. Required Response Format After Editing

改完后需要明确输出：

1. 修改了哪些文件
2. 每个文件做了什么改动
3. 为什么这样设计
4. 跑了哪些命令
5. 哪些验证通过 / 未通过
6. 剩余风险

---

## 12. Done When

任务满足以下条件才算完成：

- 单入口 `main.cpp` 已接管启动流程
- 菜单只支持 `RD only / Inference only / Exit`
- RD 与推理分别通过模块顶层接口运行
- 当前行为与既有算法逻辑保持一致
- 文档已同步到新的结构
- 变更范围可 review
- 验证结果已如实记录
