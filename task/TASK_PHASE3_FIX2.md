# Task title

Phase 3 Fix2：Web Console 设置持久化与设置面板折叠化

---

## 1. Background

这个任务为什么存在？

- 当前 Web Console 已经有完整的 `Settings Workspace`，前端通过 `/api/settings` 读取配置，并通过 `POST /api/settings` 调用 [web_console_controller.cpp](G:/Docker_windows_disk/PSIN_SAR_workflow/main/src/web_console_controller.cpp) 的 `applySettings(...)` 把修改写入内存。
- 但当前设置修改只停留在内存态：
  - 推理配置来自 [infer_workflow.yaml](G:/Docker_windows_disk/PSIN_SAR_workflow/main/configs/infer_workflow.yaml)
  - RD 配置来自 [rd_imaging.yaml](G:/Docker_windows_disk/PSIN_SAR_workflow/main/configs/rd_imaging.yaml)
  - Web 配置来自 [web_console.yaml](G:/Docker_windows_disk/PSIN_SAR_workflow/main/configs/web_console.yaml)
  - 现有实现只有 `LoadConfig(...)`，没有对应的 YAML 写回能力。
- 当前页面布局中，`Settings Workspace` 常驻占据大块区域，导致默认打开页面时，下面的 `Event Stream` 和 `Reserved Endpoints` 往往需要滚动才能看到。
- 现在希望把“设置”从“常驻大块面板”改成“按需展开的设置入口”，同时在退出 Web Console 时，把已经应用到内存中的设置直接写回 YAML，形成下次启动可复用的持久配置。

---

## 2. Goal

明确写出要实现的行为。

**必须是可验证的。**

- Web Console 在正常退出时，将当前已应用到内存的设置写回对应 YAML 文件。
- 设置写回范围明确为：
  - 推理设置写回 `infer_workflow.yaml`
  - RD 设置写回 `rd_imaging.yaml`
  - Web 侧保留的 flight settings 写回 `web_console.yaml`
- 页面默认进入时，`Settings Workspace` 不展开、不占主内容高度。
- 页面提供一个清晰可见的齿轮入口，用于展开/收起设置面板。
- 在设置面板默认收起的情况下，页面无需滚轮即可更容易看到 `Event Stream` 和 `Reserved Endpoints`。

---

## 3. Out of scope

明确不做什么。

- 不把设置改成“每次点击 Apply 就立即写磁盘”
- 不实现 YAML 注释、空行、原始缩进风格的完整保留式 round-trip
- 不引入第三方 YAML 库
- 不改 Web Console 的 HTTP / JSON / SSE 协议
- 不改 `workflow::infer::Run(...)` 或 `workflow::rd::Run(...)` 的算法行为
- 不重做 Web Console 的整体视觉风格，只做设置面板入口与折叠交互
- 不新增独立的“设置页面”或路由

---

## 4. Allowed files to modify

只列允许改的文件。

```text
main/include/workflow/shared/config_utils.hpp
main/src/config_utils.cpp
main/include/workflow/infer/infer_config.hpp
main/src/infer_config.cpp
main/include/workflow/rd/rd_config.hpp
main/src/rd_config.cpp
main/include/workflow/web/web_console_config.hpp
main/src/web_console_config.cpp
main/src/web_console.cpp
main/src/web_console_assets.cpp
main/configs/web_console.yaml
task/TASK_PHASE3_FIX2.md
```

---

## 5. Files/modules to avoid

写清楚不许动的范围。

```text
main/src/infer_workflow.cpp
main/src/rd_imaging_stream.cpp
main/src/web_console_server.cpp
main/src/web_console_controller.cpp
main/src/web_console_protocol.cpp
main/include/workflow/shared/run_control.hpp
deps/**
ARCHITECTURE_TEMPLATE.md
CODEBASE_MAP_TEMPLATE.md
task/TASK_PHASE3.md
task/TASK_PHASE3_FIX1.md
```

---

## 6. Chosen implementation direction

这项决策已经确定，不再二选一：

- 配置持久化采用“退出时统一写回”，不采用“Apply 时立即落盘”。
- 写回采用“配置专用序列化”而不是“通用 YAML 原文修改”：
  - `infer::SaveConfig(...)` 负责稳定输出 `infer_workflow.yaml`
  - `rd::SaveConfig(...)` 负责稳定输出 `rd_imaging.yaml`
  - `web::SaveConfig(...)` 负责稳定输出 `web_console.yaml`
- `web_console.yaml` 需要扩展一个 `flight:` 段，作为当前 Web Console 保留设置的唯一持久化位置。
- 写文件时采用“先写临时文件，再原子替换”的策略，避免退出时写出半截配置。
- Web UI 上不单独新增新页面，而是在现有首页头部或设置区旁提供一个齿轮按钮：
  - 默认收起设置区
  - 点击展开
  - 再次点击收起
- 设置区收起时，只隐藏设置编辑区域，不影响当前页面其他功能。

---

## 7. Functional requirements

列成可验收条目：

- [ ] `POST /api/settings` 之后，设置仍然只先写入内存，不改变现有即时交互语义
- [ ] Web Console 正常退出时，把当前 controller 中的 infer 配置写回 `web_console.yaml` 指定的 `infer_config_path`
- [ ] Web Console 正常退出时，把当前 controller 中的 RD 配置写回 `web_console.yaml` 指定的 `rd_config_path`
- [ ] Web Console 正常退出时，把当前 flight settings 写回 `web_console.yaml`
- [ ] `web_console.yaml` 新增 `flight.manual_step_px`
- [ ] `web_console.yaml` 新增 `flight.boost_step_px`
- [ ] `web_console.yaml` 新增 `flight.trigger_distance_px`
- [ ] `web_console.yaml` 新增 `flight.cache_grid_px`
- [ ] `web_console.yaml` 新增 `flight.path_overlay`
- [ ] `web_console.yaml` 新增 `flight.control_bindings`
- [ ] 下次启动 Web Console 时，flight settings 能从 `web_console.yaml` 正确恢复
- [ ] 设置面板默认收起
- [ ] 页面上存在一个齿轮按钮用于展开/收起设置面板
- [ ] 设置面板展开后，仍能编辑原有 Inference / RD / Flight 三组字段
- [ ] 设置面板收起后，`Event Stream` 和 `Reserved Endpoints` 在常见桌面分辨率下更早出现在首屏

---

## 8. Non-functional requirements

- [ ] 保持最小范围修改
- [ ] 不引入新依赖
- [ ] 不修改现有 `/api/settings` 协议字段
- [ ] YAML 写回必须是稳定、可 review 的固定字段顺序
- [ ] YAML 写回必须避免部分写入导致配置文件损坏
- [ ] 退出写回失败时必须打印明确错误，不能静默吞掉
- [ ] 前端折叠设置面板不能影响现有 source 切换、start/pause/stop/reset、event stream
- [ ] 不为了 UI 折叠去新增复杂状态管理框架

---

## 9. Interface expectations

给出希望的接口草案：

```cpp
namespace workflow::infer {
    AppConfig LoadConfig(const std::filesystem::path& config_path);
    void SaveConfig(const std::filesystem::path& config_path, const AppConfig& cfg);
}

namespace workflow::rd {
    AppConfig LoadConfig(const std::filesystem::path& config_path);
    void SaveConfig(const std::filesystem::path& config_path, const AppConfig& cfg);
}

namespace workflow::web {
    struct WebConsoleConfig {
        std::string bind_address;
        int port;
        int sse_heartbeat_ms;
        std::string ui_title;
        std::filesystem::path infer_config_path;
        std::filesystem::path rd_config_path;
        FlightSettings flight_settings;
    };

    WebConsoleConfig LoadConfig(const std::filesystem::path& config_path);
    void SaveConfig(const std::filesystem::path& config_path, const WebConsoleConfig& cfg);
}

namespace workflow::shared {
    void WriteTextFileAtomically(const std::filesystem::path& path, const std::string& content);
}
```

说明：

- `WriteTextFileAtomically(...)` 可以是独立 helper，也可以换成等价的小范围实现。
- `web::SaveConfig(...)` 是否直接接收 `FlightSettings` 单独参数，可以按实现方便程度调整。
- 不要求保留现有 YAML 注释；允许用仓库风格重新生成内容，但字段值必须正确、结构必须稳定。

---

## 10. File-level design

### `main/src/web_console.cpp`

- 在 Web Console 退出路径上增加“配置持久化”步骤。
- 推荐顺序：
  - `server->Stop()`
  - `controller->RequestWorkerStop()`
  - `join web thread`
  - `controller->JoinWorker()`
  - 从 controller 读取最终 `inferConfig()` / `rdConfig()` / `flightSettings()`
  - 调用各自 `SaveConfig(...)`
- 若任一写回失败：
  - 输出明确错误信息
  - 返回非 0 退出码

### `main/include/workflow/infer/infer_config.hpp` + `main/src/infer_config.cpp`

- 补充 `SaveConfig(...)`
- 固定输出与当前 `LoadConfig(...)` 一一对应的字段：
  - `sys.*`
  - `input.*`
  - `pipeline.patch.*`
  - `pipeline.icore.*`
  - `pipeline.output_wait_ms`
  - `display.*`
  - `output.*`
  - `debug.dump_backend_log`

### `main/include/workflow/rd/rd_config.hpp` + `main/src/rd_config.cpp`

- 补充 `SaveConfig(...)`
- 固定输出 `rd.*` 字段，顺序与当前配置文件一致

### `main/include/workflow/web/web_console_config.hpp` + `main/src/web_console_config.cpp`

- 把 `flight settings` 纳入 `WebConsoleConfig`
- `LoadConfig(...)` 新增对 `flight.*` 的读取
- `SaveConfig(...)` 输出：
  - `server.*`
  - `ui.*`
  - `config.*`
  - `flight.*`

### `main/include/workflow/shared/config_utils.hpp` + `main/src/config_utils.cpp`

- 如有必要，增加最小公共写文件 helper
- 仅服务于这次配置落盘需求，不要扩展成复杂 YAML 库

### `main/src/web_console_assets.cpp`

- 把 `Settings Workspace` 从常驻区块改成默认折叠
- 新增齿轮按钮
- 控制设置区展开/收起的前端状态
- 保留原有设置输入框和 `Apply In-Memory Settings` 按钮
- 收起时释放页面纵向空间，让日志区和保留接口区更早可见

---

## 11. Edge cases

要求 Codex 必须考虑这些：

- `web_console.yaml` 中尚不存在 `flight:` 段
- `infer_config_path` 或 `rd_config_path` 指向相对路径
- 写回目标目录存在，但旧文件只读或写权限不足
- Web Console 因异常路径退出，能否拿到最后一份内存配置
- 设置面板在展开状态下刷新页面，默认应仍然收起还是恢复展开状态
- flight settings 为空字符串字段，如 `control_bindings`
- bool 值写回时统一使用 `true/false`
- path 字段中包含 `:`、`?`、空格或 Windows 路径分隔符

---

## 12. Validation

明确要求运行什么验证：

```text
1. g++ -fsyntax-only 覆盖：
   - main/src/config_utils.cpp
   - main/src/infer_config.cpp
   - main/src/rd_config.cpp
   - main/src/web_console_config.cpp
   - main/src/web_console.cpp
   - main/src/web_console_assets.cpp

2. 静态检查：
   - 确认 SaveConfig 输出字段与 LoadConfig 读取字段一致
   - 确认 web_console.cpp 只在退出路径写回，不在 Apply 时写盘
   - 确认 flight settings 来源和落盘位置一致

3. 板端/运行验证：
   - 启动 Web Console
   - 修改一组 infer 设置并点 Apply In-Memory Settings
   - 修改一组 RD 设置并点 Apply In-Memory Settings
   - 修改一组 flight settings 并点 Apply In-Memory Settings
   - 正常退出 Web Console
   - 检查 infer_workflow.yaml / rd_imaging.yaml / web_console.yaml 已更新
   - 重新启动 Web Console
   - 确认页面中的 settings 值与上次退出前一致

4. UI 验证：
   - 首次打开页面时设置区默认不可见
   - 点击齿轮后设置区展开
   - 再次点击齿轮后设置区收起
   - 收起状态下 Event Stream 和 Reserved Endpoints 比原来更早进入可视区域
```

---

## 13. Required response format before editing

要求 Codex 在动手前先输出：

1. 对任务的理解
2. 计划修改的文件
3. 不会修改的文件
4. 实现方案
5. 风险点
6. 验证方案

---

## 14. Required response format after editing

要求 Codex 改完后输出：

1. 修改了哪些文件
2. 每个文件做了什么改动
3. 为什么这样设计
4. 跑了哪些命令
5. 哪些验证通过 / 未通过
6. 剩余风险

---

## 15. Done when

写成客观验收标准：

- Web Console 退出后，已应用到内存的 infer 设置会写回 `infer_workflow.yaml`
- Web Console 退出后，已应用到内存的 RD 设置会写回 `rd_imaging.yaml`
- Web Console 退出后，flight settings 会写回 `web_console.yaml`
- 重新进入 Web Console 后，设置值能够正确恢复
- 设置区默认隐藏
- 齿轮按钮可以稳定展开/收起设置区
- 页面首屏可见内容相较原实现更靠下，减少滚动需求
- diff 范围只集中在配置序列化和 Web UI 折叠交互


## 16. md update

需要你随着代码更新而更新的文件：

- ARCHITECTURE_TEMPLATE.md
- CODEBASE_MAP_TEMPLATE.md
