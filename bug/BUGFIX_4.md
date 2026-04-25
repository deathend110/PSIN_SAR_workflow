# BUGFIX_4.md

## 1. Bug title

Clicking `Shutdown Web Console` exits the whole process instead of returning from Web mode back to the main menu.

---

## 2. Symptom

- 现象：
  - Web 控制台右上角点击 `Shutdown Web Console`
  - 浏览器连接断开后，板子上运行的 `psin_workflow` 进程也一起退出
- 发生频率：
  - 稳定复现
- 发生条件：
  - 主程序通过菜单选择 `3. Web Console`
  - 浏览器端点击右上角黄色圆形 shutdown 按钮
- 相关行为链：
  - 前端按钮在 [web_console_assets.cpp](g:/Docker_windows_disk/PSIN_SAR_workflow/main/src/web_console_assets.cpp#L512) 调用 `POST /api/command/shutdown_web`
  - server 路由在 [web_console_server.cpp](g:/Docker_windows_disk/PSIN_SAR_workflow/main/src/web_console_server.cpp#L475)
  - controller 命令在 [web_console_controller.cpp](g:/Docker_windows_disk/PSIN_SAR_workflow/main/src/web_console_controller.cpp#L721)
  - 外层模式运行函数在 [web_console.cpp](g:/Docker_windows_disk/PSIN_SAR_workflow/main/src/web_console.cpp#L63)
  - 主程序入口在 [main.cpp](g:/Docker_windows_disk/PSIN_SAR_workflow/main/src/main.cpp#L56)

---

## 3. Minimal reproduction

- 输入条件：
  - 板子上启动 `./build/ZG/psin_workflow`
  - 终端菜单选择 `3. Web Console`
  - 浏览器成功连接 Web 控制台
- 触发步骤：
  1. 点击右上角 `Shutdown Web Console`
  2. 在确认框中点击确认
  3. 观察浏览器连接和板子终端
- 预期行为：
  - 关闭嵌入式 Web 服务
  - 退出 `Web Console` 模式
  - 主程序不退出，而是回到启动菜单：
    - `1. RD only`
    - `2. Inference only`
    - `3. Web Console`
    - `0. Exit`
- 实际行为：
  - Web 服务关闭后，`workflow::web::Run(...)` 结束
  - `main()` 直接返回，整个程序退出到 shell

---

## 4. Suspected root causes

1. `shutdown_web` 当前实现本来就会结束 `workflow::web::Run(...)`；而 [main.cpp](g:/Docker_windows_disk/PSIN_SAR_workflow/main/src/main.cpp#L64) 对模式 3 是直接 `return workflow::web::Run(...)`，所以 Web 模式一旦返回，整个程序就退出。  
   证据：
   - [web_console_server.cpp](g:/Docker_windows_disk/PSIN_SAR_workflow/main/src/web_console_server.cpp#L475)
   - [web_console.cpp](g:/Docker_windows_disk/PSIN_SAR_workflow/main/src/web_console.cpp#L103)
   - [main.cpp](g:/Docker_windows_disk/PSIN_SAR_workflow/main/src/main.cpp#L64)

2. 主程序当前不是菜单循环，而是一次性 `switch -> return Run(...)` 控制流。  
   这意味着要修成“退出 Web 模式后回到菜单”，必须补一个最小的上层调度调整，不能只改按钮文案或 server 标志位。

3. 先前 `Shutdown Web Console` 的实现方向更像“结束 Web 模式”，但现在产品语义已经明确固定为“关闭 Web Console 后回到主菜单”。  
   所以这次修复目标应收敛为“返回菜单”，不再考虑“只关 Web 服务但空驻留”。

---

## 5. Constraints

- 不允许把问题误判成简单前端 bug
- 不允许顺手重构整个主程序架构
- 不允许用 `std::exit(...)`、死循环空驻留等方式糊过去
- 语义已经明确固定为：
  - `Shutdown Web Console` 后退出 Web 模式并回到主菜单
- 如果需要改行为，优先保持最小、可 review 的主入口/模式返回修复

---

## 6. Required workflow for Codex

必须按这个顺序做：
1. 复述 bug
2. 找最小复现
3. 列出根因候选并排序
4. 写一个最小 failing test / 最小失败验证
5. 再做最小修复
6. 跑回归验证
7. 解释影响面

---

## 7. Required output before editing

1. 如何理解这个 bug
2. 最小复现路径
3. 根因候选列表（按概率排序）
4. 计划修改的文件
5. 测试方案

---

## 8. Required output after editing

1. failing test / 最小失败验证是什么
2. 最终根因是什么
3. 改了哪些文件
4. 修复逻辑是什么
5. 为什么这是最小修复
6. 回归验证结果
7. 仍未覆盖的风险

---

## 9. Done when

- 点击 `Shutdown Web Console` 后，不会再直接把整个 `psin_workflow` 进程一起结束
- Web 服务关闭后，程序返回主菜单，而不是回到 shell
- 用户可以在同一进程内继续重新选择：
  - `1. RD only`
  - `2. Inference only`
  - `3. Web Console`
  - `0. Exit`
- 增加或更新了最小回归验证
- 修改范围集中在 Web Console 退出控制链和必要的主入口调度层
