# Task title

Phase 3 Fix4：在 Web Console 配置中增加板子 IP，并在终端进入 Web 模式时输出板子 IP 与端口

---

## 1. Background

这个任务为什么存在？

- 当前命令行进入 `3. Web Console` 后，终端只会输出：

```text
Web Console listening on http://0.0.0.0:8080
Press Ctrl+C to stop the embedded web service.
```

- 其中 `0.0.0.0` 是服务端监听地址，不是用户真正拿来访问板子的地址。
- 在实际使用场景里：
  - 板子作为服务端
  - 电脑通过网线直连访问板子
  - 用户需要知道“浏览器该连哪个板子 IP 和端口”
- 当前 [web_console.yaml](G:/Docker_windows_disk/PSIN_SAR_workflow/main/configs/web_console.yaml) 里只有：
  - `server.bind`
  - `server.port`
  - `server.sse_heartbeat_ms`
  没有单独可配置的“板子对外访问 IP”字段。
- 当前真实落点已经明确：
  - 配置结构在 [web_console_config.hpp](G:/Docker_windows_disk/PSIN_SAR_workflow/main/include/workflow/web/web_console_config.hpp)
  - 配置读取与写回在 [web_console_config.cpp](G:/Docker_windows_disk/PSIN_SAR_workflow/main/src/web_console_config.cpp)
  - Web 模式启动打印在 [web_console.cpp](G:/Docker_windows_disk/PSIN_SAR_workflow/main/src/web_console.cpp)

---

## 2. Goal

明确写出要实现的行为。

**必须是可验证的。**

- 在 `web_console.yaml` 中新增一个板子 IP 配置字段。
- 程序进入 `3. Web Console` 模式时，终端输出除了监听地址外，还要明确打印“板子 IP:端口”。
- 该输出必须直接可用于指导用户在电脑浏览器中访问板子。
- 新增字段必须纳入现有 `LoadConfig(...) / SaveConfig(...)` 闭环。

---

## 3. Out of scope

明确不做什么。

- 不做自动探测网卡 IP
- 不通过系统网络接口动态枚举板子地址
- 不修改 Web Console 的前端页面
- 不修改 HTTP / SSE 协议
- 不改 `WebConsoleServer` 的 bind 行为
- 不改 RD / Infer 工作流逻辑
- 不实现多网卡、多 IP 自动选择

---

## 4. Allowed files to modify

只列允许改的文件。

```text
main/include/workflow/web/web_console_config.hpp
main/src/web_console_config.cpp
main/src/web_console.cpp
main/configs/web_console.yaml
task/TASK_PHASE3_FIX4.md
```

---

## 5. Files/modules to avoid

写清楚不许动的范围。

```text
main/src/web_console_server.cpp
main/src/web_console_controller.cpp
main/src/web_console_assets.cpp
main/src/infer_workflow.cpp
main/src/rd_imaging_stream.cpp
main/include/workflow/shared/run_control.hpp
deps/**
ARCHITECTURE_TEMPLATE.md
CODEBASE_MAP_TEMPLATE.md
task/TASK_PHASE3.md
task/TASK_PHASE3_FIX1.md
task/TASK_PHASE3_FIX2.md
task/TASK_PHASE3_FIX3.md
```

---

## 6. Chosen implementation direction

这项决策已经确定，不再二选一：

- 在 `web_console.yaml` 中新增固定字段：

```text
server.board_ip = 192.168.x.x
```

- `server.bind` 继续表示服务监听地址。
- `server.board_ip` 专门表示用户从电脑浏览器访问板子时应使用的地址。
- 启动 Web Console 时，终端建议保留现有监听输出，同时新增一行明确输出板子访问地址。
- 推荐输出形式：

```text
Web Console listening on http://0.0.0.0:8080
Board access URL: http://192.168.x.x:8080
Press Ctrl+C to stop the embedded web service.
```

- `server.board_ip` 纳入 `LoadConfig(...)` 和 `SaveConfig(...)`，避免配置写回后丢失。

---

## 7. Functional requirements

列成可验收条目：

- [ ] `web_console.yaml` 新增 `server.board_ip`
- [ ] `WebConsoleConfig` 新增对应字段
- [ ] `LoadConfig(...)` 能读取 `server.board_ip`
- [ ] `SaveConfig(...)` 能写回 `server.board_ip`
- [ ] 选择 `3. Web Console` 后，终端仍保留当前监听地址输出
- [ ] 选择 `3. Web Console` 后，终端新增输出板子访问地址与端口
- [ ] 新输出中的端口必须与 `server.port` 一致
- [ ] 若 `board_ip` 配置为 `192.168.1.10` 且端口为 `8080`，终端输出中必须出现 `192.168.1.10:8080`

---

## 8. Non-functional requirements

- [ ] 保持最小范围修改
- [ ] 不引入新依赖
- [ ] 不修改现有 Web server 监听语义
- [ ] 输出文案必须清晰，避免把 `bind` 地址和对外访问地址混淆
- [ ] diff 要小且 review 友好

---

## 9. Interface expectations

给出希望的接口草案：

```cpp
struct WebConsoleConfig
{
    std::string bind_address = "0.0.0.0";
    std::string board_ip = "192.168.1.10";
    int port = 8080;
    int sse_heartbeat_ms = 1000;
    std::string ui_title = "PSIN SAR Web Console";
    std::filesystem::path infer_config_path = "configs/infer_workflow.yaml";
    std::filesystem::path rd_config_path = "configs/rd_imaging.yaml";
    FlightSettings flight_settings;
};
```

说明：

- `board_ip` 字段名可以微调，但建议固定为 `board_ip`，与需求语义最直接。
- 若实现时选择输出 `Board IP:Port` 而不是 `Board access URL`，也可以，但必须保证用户一眼能知道浏览器该访问哪里。

---

## 10. File-level design

### `main/include/workflow/web/web_console_config.hpp`

- 在 `WebConsoleConfig` 中新增板子 IP 字段。

### `main/src/web_console_config.cpp`

- `LoadConfig(...)` 读取 `server.board_ip`
- `SaveConfig(...)` 写回 `server.board_ip`

### `main/src/web_console.cpp`

- 在现有：
  - `Web Console listening on http://...`
  之后
- 新增一行输出板子访问地址，例如：
  - `Board access URL: http://<board_ip>:<port>`

### `main/configs/web_console.yaml`

- 新增 `server.board_ip` 示例值

---

## 11. Edge cases

要求 Codex 必须考虑这些：

- `server.board_ip` 缺失时如何处理
- `server.board_ip` 为空字符串时如何处理
- `server.board_ip` 与 `server.bind` 相同
- `server.bind = 0.0.0.0` 但 `server.board_ip` 是具体地址
- 配置写回后 `board_ip` 不能丢失

---

## 12. Validation

明确要求运行什么验证：

```text
1. g++ -fsyntax-only 覆盖：
   - main/src/web_console_config.cpp
   - main/src/web_console.cpp

2. 静态检查：
   - 确认 WebConsoleConfig 新增 board_ip 字段
   - 确认 LoadConfig/SaveConfig 都处理了 board_ip
   - 确认终端输出包含 board_ip 与 port

3. 运行验证：
   - 在 web_console.yaml 中设置 board_ip = 192.168.1.10
   - 启动程序并选择 3
   - 终端输出中能看到：
     - 监听地址
     - 板子访问地址 192.168.1.10:8080
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

- `web_console.yaml` 已支持配置板子 IP
- 程序进入 `Web Console` 模式时，终端能打印板子 IP 与端口
- `LoadConfig(...) / SaveConfig(...)` 已完整覆盖新字段
- 输出信息不会再让用户把 `0.0.0.0` 误认为实际访问地址
- 修改范围符合约束，diff 可 review


## 16. md update

需要你随着代码更新而更新的文件：

- ARCHITECTURE_TEMPLATE.md
- CODEBASE_MAP_TEMPLATE.md
