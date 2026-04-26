# TASK_REBUILE_02: 为 Web Console HTTP 控制面增加请求边界与读取超时

## 0. Meta
- 阶段：重建期，近期落地。
- 优先级：P0。
- 板端约束关联：仅限 Web Console 控制面，不卡住板端推理流程。
- 对应 review 建议：`PROJ_GPT_REVIEW.md` 的 P0-3，补 HTTP 请求边界与超时。
- 主责任域：HTTP 解析、防阻塞读取、控制面健壮性。
- 依赖前置任务：优先依赖 `TASK_REBUILE_05` 的 host 测试边界；实现本身不依赖板端。

## 1. Background
- 现在 `main/src/web_console_server.cpp` 的 `parseRequest()` 会一直阻塞读到完整头部和 body。
- 当前实现没有明确的 header / body 上限，也没有单独的读取超时。
- 近期任务是先把最小防护边界补上，长期再把 parser 拆成独立模块并补更细的测试。

## 2. Goal
- 给 HTTP 请求头和 body 增加明确上限。
- 给读取过程增加超时，避免慢连接拖死控制面。
- 对超限、超时、非法请求返回可区分的 HTTP 错误码。

## 3. Out of scope
- 不改 Web Console 路由语义。
- 不改 SSE 协议。
- 不把 controller 和 server 的职责拆开到这个任务里。
- 不引入第三方 HTTP 库。

## 4. Allowed files to modify
```text
task/TASK_REBUILE_02.md
main/include/workflow/web/web_console_server.hpp
main/src/web_console_server.cpp
main/src/web_console_protocol.cpp
main/tests/web_console_request_regression_test.cpp
```

## 5. Files/modules to avoid
```text
main/src/web_console_controller.cpp
main/src/web_console.cpp
main/src/web_console_assets.cpp
main/src/infer_workflow.cpp
main/src/rd_imaging_stream.cpp
main/src/main.cpp
deps/**
```

## 6. Functional requirements
- [ ] 请求头存在最大字节数限制。
- [ ] 请求 body 存在最大字节数限制。
- [ ] 读取 socket 时存在超时。
- [ ] 超限请求返回明确错误码，优先区分 `400 / 408 / 413`。
- [ ] 合法请求的现有路由行为保持不变。

## 7. Non-functional requirements
- [ ] 不改变 SSE 长连接的正常行为。
- [ ] 不增加新依赖。
- [ ] 对正常请求的性能影响尽量小。
- [ ] 解析边界应集中在 server 层，不扩散到 controller。

## 8. Implementation decomposition
- 主任务：在 `parseRequest()` 前后补边界与超时。
- 子任务 1：定义 header / body / read timeout 的最小常量。
- 子任务 2：把超时和超限错误映射成稳定的 HTTP 响应。
- 子任务 3：补一个请求解析回归测试，覆盖慢读和超大请求。
- 近期：先保证 server 不会被单个慢客户端拖死。
- 长期：再把 request parser 抽成独立模块。

## 9. Edge cases
- 只有头部没有 body 的 `GET` 请求仍应正常工作。
- `Content-Length` 非法时应直接拒绝。
- header 过长但 body 正常时应尽早失败。
- 连接中断时不应把内部异常冒泡成不稳定状态。

## 10. Validation
```text
g++ -std=c++17 -fsyntax-only main/src/web_console_server.cpp -Imain/include
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
- [ ] 慢连接不会无限阻塞控制面。
- [ ] 超大请求会被拒绝。
- [ ] 合法路由保持原行为。
- [ ] 相关回归测试通过。

## 14. Follow-up
- 后续可以把 parser / route handler 拆成独立文件。
- 若 `TASK_REBUILE_07` 开始拆 Web 模块，这里的边界可直接复用。
