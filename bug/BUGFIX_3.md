# BUGFIX_3.md

## 1. Bug title

Manual WASD request occasionally returns empty / non-JSON response, causing `Unexpected end of JSON input` and dropped direction commands.

---

## 2. Symptom

- 现象：
  - Web Console 的 manual 模式下，按 `W/A/S/D` 时日志出现：
    - `manual request failed: Failed to execute 'json' on 'Response': Unexpected end of JSON input`
  - 同一次按键对应的方向命令会丢失，体感上表现为“按键失灵”
- 发生频率：
  - 偶发，但在 manual 交互过程中可重复出现
- 发生条件：
  - `workflow=infer`
  - `patch_mode=manual_flight`
  - 通过网页按钮或键盘发送 `/api/manual/key`
- 相关日志 / 报错：
  - 浏览器日志：
    - `manual request failed: Failed to execute 'json' on 'Response': Unexpected end of JSON input`
- 当前静态证据：
  - [web_console_assets.cpp](g:/Docker_windows_disk/PSIN_SAR_workflow/main/src/web_console_assets.cpp#L230) 的 `postJson()` 对所有 POST 响应直接 `response.json()`
  - [web_console_assets.cpp](g:/Docker_windows_disk/PSIN_SAR_workflow/main/src/web_console_assets.cpp#L354) 的 `sendManualKey()` 在 JSON 解析异常时直接记日志并丢弃该次方向命令
  - [web_console_server.cpp](g:/Docker_windows_disk/PSIN_SAR_workflow/main/src/web_console_server.cpp#L484) 的 `/api/manual/key` 路由理论上始终应返回 JSON

---

## 3. Minimal reproduction

- 输入条件：
  - 启动 `./build/ZG/psin_workflow`
  - 选择 `3. Web Console`
  - 浏览器进入 Web Console
  - 切到 `infer + manual_flight`
- 触发步骤：
  1. 点击 `Start`
  2. 用键盘按 `W/A/S/D`，或点击页面上的 manual 控制按钮
  3. 观察右侧日志区
- 预期行为：
  - 每次 manual 请求都应收到结构完整的 JSON 响应
  - 即使请求失败，也应返回明确 JSON 错误，不应出现前端 JSON 解析异常
  - 单次 transient transport 异常不应直接表现为“按键失灵”
- 实际行为：
  - 偶发收到空 / 截断 / 非 JSON 响应
  - 前端在 `response.json()` 处抛出 `Unexpected end of JSON input`
  - 当前方向命令丢失

---

## 4. Suspected root causes

1. 前端 `postJson()` 对所有响应无条件 `response.json()`，缺少对空 body / 非 JSON / 截断 body 的边界处理。  
   证据： [web_console_assets.cpp](g:/Docker_windows_disk/PSIN_SAR_workflow/main/src/web_console_assets.cpp#L230)

2. manual 输入链没有对 transient transport failure 做最小恢复，导致一次响应解析失败就直接丢掉当前方向命令。  
   证据： [web_console_assets.cpp](g:/Docker_windows_disk/PSIN_SAR_workflow/main/src/web_console_assets.cpp#L354)

3. Web server 侧对 `sendString(...)` 返回值缺少统一处理与诊断，可能导致客户端拿到空 / 截断响应时缺乏后端可观测性。  
   证据： [web_console_server.cpp](g:/Docker_windows_disk/PSIN_SAR_workflow/main/src/web_console_server.cpp#L486)

---

## 5. Constraints

- 不允许顺手重构整个 Web 通信层
- 不允许扩大 manual 控制协议
- 不允许改动 infer manual 运行时语义
- 不允许因为修这个 bug 去改线程模型
- 优先做最小、可 review 的 Web 边界修复

---

## 6. Required workflow for Codex

必须按这个顺序做：
1. 复述 bug
2. 找最小复现
3. 列出根因候选并排序
4. 写 failing test 或最小失败复现验证
5. 做最小修复
6. 跑回归验证
7. 解释修复影响面

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

- manual 输入链不再因为空 / 非 JSON 响应直接抛 `Unexpected end of JSON input`
- manual 请求失败时能给出明确、可读的错误信息
- 单次 transient response 异常不会无提示地把方向命令吞掉
- 新增或更新了最小回归验证
- 修改范围保持在 Web 请求边界相关文件内
