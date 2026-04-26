审查基准：我基于 GitHub 当前 `master` 做静态审查，重点看了 `main/` 下的 C++ 工程、构建脚本、配置、Web 控制面、RD/推理主流程和测试。没有在 ZG330/aarch64 板端实际编译运行，因此以下不评价板端真实性能、驱动行为或模型精度。仓库说明里也明确：`main/` 是实际 C++ 主工程，目标环境是 `Linux + aarch64 + ZG330`，核心链路是 `echo.bin -> RD -> PNG -> patch inference -> HDMI/PNG`，构建入口是 `cd main && ./build_main.sh`。([GitHub][1])

总体判断：这是一个“比赛交付型”工程，不是简单脚本拼接。你已经把 RD、推理、Web Console、配置和主菜单分出了基本边界，README 对运行方式和限制写得比较清楚。按比赛后端标准，完成度较高；按长期维护的 C++ 工程标准，主要短板集中在构建可复现性、测试集成、超大源文件、配置系统健壮性、HTTP 控制面鲁棒性和硬件依赖隔离。

## 做得比较好的地方

第一，顶层入口相对干净。`main.cpp` 只负责模式选择、配置路径和调用 RD / Infer / Web 三个子系统，代码量约 80 行，职责没有明显膨胀。菜单行为也比较明确：RD 和 Infer 执行后退出，Web Console 正常返回后回到主菜单。([GitHub][2])

第二，工程结构有基本分层意识。`main/README.md` 把 `src/`、`include/`、`configs/`、`io/`、`tests/`、`build_main.sh`、`CMakeLists.txt` 的职责写得比较清楚，推理模块、RD 模块、Web Console 模块也有独立说明。([GitHub][3])

第三，Web Controller 的生命周期处理比很多比赛代码稳。`WorkflowController` 析构时会请求停止，把 worker 线程移出锁区后再 `join`；worker 主函数也捕获异常并把错误落到运行状态里。这说明你已经在避免“持锁 join 死锁”和线程异常直接终止进程这类常见问题。([GitHub][4])

第四，构建脚本有基本工程卫生。`build_main.sh` 使用 `set -euo pipefail`，区分 build/log 目录，做 Linux 检查，并按机器 CPU 数并行构建。([GitHub][5])

第五，配置文档写得比代码本身更“产品化”。例如推理配置明确了输入目录、patch size、stride、模型路径、输出模式、显示分辨率和 backend log 开关；RD 配置也把 tile、scratch、memory limit、overwrite 等参数暴露出来。([GitHub][6])

## P0：优先修的工程问题

### 1. 构建系统把交叉编译工具链硬编码进了 `CMakeLists.txt`

当前 `CMakeLists.txt` 在工程文件里直接设置 `CMAKE_SYSTEM_NAME Linux`、`CMAKE_SYSTEM_PROCESSOR aarch64`，并硬编码 `/usr/bin/aarch64-linux-gnu-gcc` 和 `/usr/bin/aarch64-linux-gnu-g++`。这对比赛机环境可用，但从 C++ 工程规范看，不应把工具链写死在项目主 CMake 里。它会导致本机 host 单测、CI、不同交叉编译器路径、不同 SDK 版本都很难复用同一套工程。([GitHub][7])

建议改成三层：

```cmake
# CMakeLists.txt: 只描述工程目标
add_library(psin_core ...)
add_executable(psin_workflow src/main.cpp)
target_link_libraries(psin_workflow PRIVATE psin_core)

option(PSIN_ENABLE_ZG330 "Enable ZG330 backend" ON)
option(PSIN_BUILD_TESTS "Build tests" ON)
```

然后新增：

```text
cmake/toolchains/zg330-aarch64.cmake
CMakePresets.json
```

工具链文件里放编译器路径、sysroot、find root path；`CMakePresets.json` 里定义 `zg330-release`、`host-debug`、`host-tests`。主 `CMakeLists.txt` 不应知道 `/usr/bin/aarch64-linux-gnu-g++` 这种部署细节。

同时建议补上编译警告：

```cmake
target_compile_options(psin_core PRIVATE
  -Wall -Wextra -Wpedantic
  -Wshadow -Wconversion
  -Werror=return-type
)
```

比赛阶段不要强行 `-Werror` 全开，但至少 CI 可以开一档更严格的 warning profile。

### 2. 测试存在，但没有纳入主构建

`main/tests/` 里目前只有 `main_menu_regression_test.cpp`，测试策略是 stub 掉 RD / Infer / Web 的 `Run`，然后把 `main.cpp` include 进测试文件做菜单行为回归测试。这个思路对当前入口函数是可行的，但 `CMakeLists.txt` 没有 `enable_testing()`、`add_test()` 或测试 target，因此测试不是工程的一等公民。([GitHub][8])

建议立即加：

```cmake
option(PSIN_BUILD_TESTS "Build tests" ON)

if(PSIN_BUILD_TESTS)
  enable_testing()
  add_executable(main_menu_regression_test
    tests/main_menu_regression_test.cpp
  )
  add_test(NAME main_menu_regression_test
           COMMAND main_menu_regression_test)
endif()
```

更进一步，不要长期用 `#define main ...` + include `.cpp` 的方式测试。可以把菜单解析逻辑抽成：

```cpp
RunMode ParseRunMode(std::string_view input);
int RunMainLoop(std::istream& in, std::ostream& out, IWorkflowRunner& runner);
```

这样测试不需要 include 生产 `.cpp`，也不需要链接真实硬件 backend。

### 3. Web Server 作为控制面，缺少请求大小限制和超时控制

`web_console_server.cpp` 里 `parseRequest` 使用阻塞 `recv` 读到 `\r\n\r\n`，再按 `Content-Length` 继续读 body。当前代码没有明显的 header 最大长度、body 最大长度、读取超时或慢连接防护；主循环 `accept` 后同步调用 `handleClient(client_fd)`，意味着一个慢客户端可以阻塞整个 HTTP 控制面。([GitHub][9])

如果这个 Web Console 只用于比赛现场局域网调试，这个设计可以接受；但工程上至少应该加这些边界：

```cpp
constexpr std::size_t kMaxHeaderBytes = 16 * 1024;
constexpr std::size_t kMaxBodyBytes   = 1 * 1024 * 1024;
constexpr int kSocketReadTimeoutSec   = 5;
```

并在解析过程中拒绝超限请求，返回 `413 Payload Too Large` 或 `400 Bad Request`。此外，异常处理现在会把 `e.what()` 放进 HTTP 500 响应体，这对调试方便，但对暴露在局域网的控制面不够稳妥；建议服务端日志记录详细错误，客户端只返回短错误码或通用错误信息。([GitHub][9])

## P1：结构性重构建议

### 4. 几个核心 `.cpp` 文件过大，已经超过合理维护边界

`infer_workflow.cpp` 约 2723 行、109 KB；`rd_imaging_stream.cpp` 约 1379 行、54.5 KB；Web Controller / Server 也都是数千行级别文件。比赛阶段集中写在一个实现文件里有利于快速收敛，但后续维护会出现三个问题：review 成本高、局部修改容易影响全局、单元测试很难切入。([GitHub][10])

建议按职责切分，而不是按“当前写代码顺序”切分。

推理模块可以拆成：

```text
infer/
  infer_config.cpp
  device_session.cpp
  image_source.cpp
  patch_planner.cpp
  tensor_preprocess.cpp
  model_runner.cpp
  postprocess.cpp
  output_sink_png.cpp
  output_sink_hdmi.cpp
  manual_flight_controller.cpp
  infer_workflow.cpp        # 只编排流程
```

RD 模块可以拆成：

```text
rd/
  rd_config.cpp
  echo_reader.cpp
  rd_algorithm.cpp
  tile_scheduler.cpp
  scratch_store.cpp
  rd_png_writer.cpp
  rd_workflow.cpp
```

Web 模块可以拆成：

```text
web/
  http_server.cpp
  http_request_parser.cpp
  static_assets.cpp
  route_handlers.cpp
  sse_hub.cpp
  workflow_controller.cpp
  settings_controller.cpp
```

拆分目标不是“文件越多越好”，而是让每个文件有一个可测试职责。比如 `http_request_parser` 可以完全脱离 socket 做单测，`patch_planner` 可以脱离 ZG330 做单测，`rd_config` 可以脱离 RD 算法做配置校验测试。

### 5. 当前配置解析器不是完整 YAML，后续容易踩坑

`config_utils.cpp` 的 `LoadSimpleYaml` 是手写的简化 YAML 解析器：逐行去注释、按第一个 `:` 切分 key/value、按缩进维护 scope。它会把任意 `#` 后面的内容当注释删除，也不支持 YAML 的列表、复杂字符串、多行文本等语义。当前配置比较简单，所以能跑；但文件扩展后很容易出现“看起来是 YAML，实际不是 YAML”的维护陷阱。([GitHub][11])

有两种路线：

第一，承认它不是 YAML，把格式改名成更受控的配置格式，比如 `*.conf`、`*.ini` 或 flat JSON，并在 README 写明只支持 `section.key: scalar`。

第二，直接引入 `yaml-cpp` 或 `toml++`，把配置解析交给成熟库。对 C++ 项目来说，`toml++` 通常比 YAML 更适合工程配置：语义更窄，解析结果更可预测。

另外，`WriteTextFileAtomically` 目前先写 `.tmp`，然后 `remove(path)`，再 `rename(tmp, path)`。这不是严格意义上的原子替换：如果 remove 成功但 rename 失败，原文件会丢失。POSIX 下通常应使用同目录唯一临时文件，然后直接 `rename(temp, target)` 覆盖目标；必要时再 `fsync` 文件和目录。([GitHub][11])

### 6. 运行时配置文件不宜直接作为仓库默认配置被 Web Console 修改

README 提到 Web Console 退出时会把当前设置写回配置文件。当前 `configs/infer_workflow.yaml`、`configs/rd_imaging.yaml`、`configs/web_console.yaml` 又是仓库内文件。这会导致一次本地运行后工作区变 dirty，也会把板卡 IP、路径、实验参数这类环境信息混进版本控制。([GitHub][3])

建议改成：

```text
configs/
  infer_workflow.example.yaml
  rd_imaging.example.yaml
  web_console.example.yaml

.local/
  infer_workflow.yaml
  rd_imaging.yaml
  web_console.yaml
```

或：

```text
/etc/psin_workflow/*.yaml
~/.config/psin_workflow/*.yaml
```

仓库只提交 `*.example.yaml`，真实运行时配置由首次启动复制生成，并加入 `.gitignore`。这对比赛复现实验也更干净。

### 7. 隐式全局状态需要收敛

`infer_workflow.cpp` 中存在 `ManualFlightSharedState`，并通过函数静态变量返回全局共享状态。这个模式在单进程、单任务、单 Web Console 场景下能工作，但从工程规范看，它会降低可测试性，也会让重复运行、并发扩展、状态重置变得隐蔽。([GitHub][10])

建议把它改成显式对象：

```cpp
class ManualFlightRuntime {
public:
  void UpdateSettings(...);
  void PushCommand(...);
  Snapshot Snapshot() const;
private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  ...
};
```

然后由 `WorkflowController` 或 `InferWorkflowContext` 持有生命周期。这样谁创建、谁停止、谁重置状态会更清楚，测试也能构造多个独立实例。

## P2：规范性与可维护性问题

### 8. 文档存在行为漂移

根目录 `ARCHITECTURE.md` 写到当前 `main` 不是外层菜单循环，Web 返回会导致进程退出；但实际 `main.cpp` 和 `main/README.md` 表示 Web Console 正常返回后会回到主菜单。这类文档漂移会直接误导后续维护者，尤其是在比赛代码交接时。([GitHub][12])

建议保留一个“权威架构文档”，并在文档顶部标注适用提交或版本。对已经变更的运行行为，直接删掉旧描述，不要靠“以代码为准”兜底。

### 9. HDMI 显示接口建议更 C++ 化

`infer_workflow_hdmi_display.hpp` 里 `HDMIDisplay` 构造时校验宽高并分配 UDMA buffer，这是好的；但 `show(int8_t* frame_data)` 使用裸指针，且没有显式传入 buffer size。类内部知道 `buffer_size_`，但调用者传错长度时接口无法表达。另一个细节是硬件寄存器地址 `0x40080054` 直接作为成员常量放在类里，应至少关联板卡版本或配置项。([GitHub][13])

C++17 下可以改成：

```cpp
void Show(const std::uint8_t* data, std::size_t size);
```

如果后续允许 C++20，则更推荐：

```cpp
void Show(std::span<const std::byte> frame);
```

并检查：

```cpp
if (frame.size() != buffer_size_) {
  throw std::invalid_argument("HDMI frame size mismatch");
}
```

此外，`infer_workflow_hdmi_display.hpp` 使用 `namespace infer_workflow`，而其他主模块更多是 `workflow::infer` 风格。命名空间应统一，否则大型工程里很快会出现 API 边界混乱。

### 10. 硬件依赖和业务逻辑还可以再隔离

当前 CMake 总是定义 `USE_ZG330_BACKEND`，并查找 `Icraft-HostBackend`、`icraft-zg330backend`。这符合你的目标板交付，但会阻碍 host 侧单测和算法逻辑验证。([GitHub][7])

建议定义接口层：

```cpp
class IInferenceBackend {
public:
  virtual ~IInferenceBackend() = default;
  virtual Tensor Run(const Tensor& input) = 0;
};

class Zg330InferenceBackend final : public IInferenceBackend { ... };
class FakeInferenceBackend final : public IInferenceBackend { ... };
```

这样 patch 切分、输入输出路径、Web 控制逻辑都可以在 x86 CI 上测试，只有 `Zg330InferenceBackend` 需要交叉编译和板端验证。

## 建议的近期改造顺序

第一阶段，先做“不会破坏业务”的工程化改造：增加 `.clang-format`、`.clang-tidy`、CMake presets、toolchain file、`PSIN_BUILD_TESTS`，并把现有菜单回归测试接入 CTest。这个阶段不碰算法和硬件 API，风险最低。

第二阶段，拆配置和 HTTP：把 `LoadSimpleYaml`、`WriteTextFileAtomically`、HTTP request parser 单独成库，并补边界测试。这里最容易发现隐藏 bug，且不依赖板端。

第三阶段，拆大文件：先从 `infer_workflow.cpp` 中抽出 patch planner、输出 sink、manual flight runtime；从 `web_console_server.cpp` 抽出 request parser 和 SSE hub；从 `rd_imaging_stream.cpp` 抽出 echo reader 和 scratch 管理。每拆一块就加对应单测，不要一次性大重构。

第四阶段，隔离硬件 backend：引入 `IInferenceBackend` / `IDisplaySink` / `IRdProcessor` 这类接口，把 ZG330、HDMI、文件输出和 Web 控制面解耦。这样仓库会从“比赛可运行工程”升级为“可维护、可测试、可移植的 C++ 后端”。

结论：你的主链路和工程组织已经达到比赛交付标准；现在最值得投入的是构建系统、测试和模块边界。不要优先做小风格清理，先解决“能不能稳定复现构建、能不能在 host 上跑测试、能不能安全地改 Web/配置/推理流程”这三个问题。

