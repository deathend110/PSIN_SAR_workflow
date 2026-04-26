# TASK_REBUILE_07: 抽离 HTTP request parser、SSE client 管理与路由分发边界

## 0. Meta
- 阶段：长期重构，分阶段推进。
- 优先级：P1。
- 板端约束关联：仅重整 Web 控制面的 parser / SSE / route 边界，不改推理板端语义。
- 对应 review 建议：`PROJ_GPT_REVIEW.md` 的 Web 模块建议；经主审查后收敛到 parser、SSE 和路由分发三个真实痛点。
- 主责任域：HTTP request parser、route dispatch、SSE client 管理、response builder。
- 依赖前置任务：建议先完成 `TASK_REBUILE_02` 和 `TASK_REBUILE_03`，再做更深的模块切分。

## 1. Background
- 当前 `Server/Controller` 已经是分文件的，真正的问题不是“再抽象一层 controller”，而是 `web_console_server.cpp` 内部把阻塞解析、路由分发、SSE client 管理混在一起。
- 这条任务应该直指 parser / route / SSE 三个可测试边界，而不是笼统写成“拆 Server / Controller”。
- 必须分阶段可回退，不在这条任务里顺手改 Web 业务协议。

## 2. Goal
- 把 request parser、route dispatch、SSE client 管理从 `web_console_server.cpp` 中抽成独立边界。
- 让 `web_console_server.cpp` 更专注 socket accept / 连接级生命周期。
- 保持现有 API 和页面行为不变。

## 3. Out of scope
- 不改前端页面结构。
- 不改业务协议。
- 不在这个任务里重做配置系统。
- 不把 controller 的内部状态机一口气重写。

## 4. Allowed files to modify
```text
task/TASK_REBUILE_07.md
main/include/workflow/web/web_console_server.hpp
main/include/workflow/web/web_console_controller.hpp
main/include/workflow/web/web_console_protocol.hpp
main/src/web_console_server.cpp
main/src/web_console_controller.cpp
main/src/web_console_protocol.cpp
main/src/web/http_request_parser.cpp
main/src/web/route_handlers.cpp
main/src/web/sse_hub.cpp
main/tests/web_console_boundary_regression_test.cpp
```

## 5. Files/modules to avoid
```text
main/src/web_console_assets.cpp
main/src/infer_workflow.cpp
main/src/rd_imaging_stream.cpp
main/src/main.cpp
deps/**
```

## 6. Functional requirements
- [ ] HTTP 请求解析可以单独测试。
- [ ] 路由分发可以单独观察。
- [ ] SSE client 管理与 server 主循环分离。
- [ ] `web_console_server.cpp` 更专注连接生命周期和 accept 循环。
- [ ] 外部接口和页面行为不变。

## 7. Non-functional requirements
- [ ] 这是分阶段拆分，不是一次性搬家。
- [ ] 不引入新的线程模型。
- [ ] 不改变现有持久化语义。
- [ ] 尽量保持当前命名空间风格。

## 8. Implementation decomposition
- 主任务：先把最稳定、最容易测试的 parser / route / SSE 边界抽离出来。
- 子任务 1：抽 request parser，并给 header/body/非法请求边界留测试口。
- 子任务 2：抽 route handlers / response builders，避免 `handleClient()` 继续膨胀。
- 子任务 3：把 SSE client 管理从 server 主循环中分离出来，减少事件广播路径耦合。
- 子任务 4：仅在 parser / route / SSE 收口后，再评估 controller 内部是否还需要二次收束。
- 中期：先拆 parser 和路由，保证可测。
- 长期：再评估 controller 内部状态机是否需要继续切分。

## 9. Edge cases
- 路由不存在时应继续返回稳定错误。
- SSE 断线或慢连接不能拖垮 controller。
- controller 持久化失败时应有明确错误路径。
- Web Console 退出时不能留下悬挂线程。

## 10. Validation
```text
g++ -std=c++17 -fsyntax-only main/src/web_console_server.cpp main/src/web_console_controller.cpp main/src/web_console_protocol.cpp -Imain/include
ctest --test-dir build/main-host --output-on-failure
```

## 11. Required response format before editing
1. 对任务的理解
2. 计划修改的文件
3. 不会修改的文件
4. 实现方案
5. 风险点
6. 验证方案

## 12. Required response format after editing
1. 修改了哪些文件
2. 每个文件做了什么改动
3. 为什么这样设计
4. 跑了哪些命令
5. 哪些验证通过 / 未通过
6. 剩余风险

## 13. Done when
- [ ] parser / route / SSE 已按职责拆开。
- [ ] 新边界有最小测试。
- [ ] 外部行为和退出语义不变。

## 14. Follow-up
- `TASK_REBUILE_02` 的请求边界可直接复用到新的 parser。
- `TASK_REBUILE_03` 的配置分离可继续支撑 controller 的持久化边界。
