#include "workflow/web/web_console_controller.hpp"

#include "workflow/infer/infer_workflow.hpp"
#include "workflow/rd/rd_workflow.hpp"
#include "workflow/shared/config_utils.hpp"
#include "workflow/web/web_console_protocol.hpp"

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace workflow::web
{
    namespace fs = std::filesystem;

    namespace
    {
        // 用于区分“字段格式错误”和“板端资源预算超限”两类设置校验失败，
        // 这样 HTTP 层可以把不同错误编码返回给前端。
        struct SettingsValidationError : std::runtime_error
        {
            SettingsValidationError(std::string code, std::string message)
                : std::runtime_error(std::move(message)), code(std::move(code))
            {
            }

            std::string code;
        };

        // 统一抛出普通字段校验错误，避免各处分散拼接错误码。
        [[noreturn]] void throwInvalidSettings(const std::string &message)
        {
            throw SettingsValidationError("invalid_settings", message);
        }

        // 统一抛出板端预算超限错误，前端可据此提示“配置合法但设备跑不动”。
        [[noreturn]] void throwBoardBudgetExceeded(const std::string &message)
        {
            throw SettingsValidationError("board_budget_exceeded", message);
        }

        constexpr int kMinBoardStride = 64;
        constexpr int kMaxBoardDisplayWidth = 1920;
        constexpr int kMaxBoardDisplayHeight = 1080;
        constexpr int kMaxBoardDisplayFps = 60;
        constexpr int kMaxBoardDisplayPixels = kMaxBoardDisplayWidth * kMaxBoardDisplayHeight;
        constexpr int kMaxBoardDisplayPixelsAtHighFps = 1280 * 720;
        constexpr int kHighFpsThreshold = 30;
        constexpr int kMaxBoardMemoryLimitMb = 512;

        // 将字符串严格解析成整数；若存在空串、尾随字符或格式问题，一律视为非法输入。
        int parseStrictInt(const std::string &value, const std::string &field)
        {
            const std::string trimmed = shared::Trim(value);
            size_t consumed = 0;
            try
            {
                const int parsed = std::stoi(trimmed, &consumed);
                if (consumed != trimmed.size())
                {
                    throwInvalidSettings(field + " must be an integer.");
                }
                return parsed;
            }
            catch (const SettingsValidationError &)
            {
                throw;
            }
            catch (const std::exception &)
            {
                throwInvalidSettings(field + " must be an integer.");
            }
        }

        // Web Console 目前只支持 HDMI 实时显示和 PNG 落盘两种输出模式。
        bool isValidInferOutputMode(const std::string &value)
        {
            return value == "hdmi" || value == "png";
        }

        // debug_raster 是调试路径，语义上等价于“离线规则切图”，不走实时 HDMI 输出。
        bool isDebugRasterMode(shared::SelectedPatchMode patch_mode)
        {
            return patch_mode == shared::SelectedPatchMode::DebugRaster;
        }

        // 检查 workflow / patch / output 三元组选项是否自洽，返回空串表示合法。
        std::string validateInferSelection(shared::SelectedPatchMode patch_mode, const std::string &output_mode)
        {
            if (isDebugRasterMode(patch_mode) && output_mode != "png")
            {
                return "debug_raster requires output_mode=png.";
            }
            return {};
        }

        // RD 当前允许三种执行模式：自动选择、强制 float32 内存管线、强制 double scratch 管线。
        bool isValidRdExecutionMode(const std::string &value)
        {
            return value == "auto" ||
                   value == "memory_float32" ||
                   value == "scratch_double";
        }

        // 这里不是做“语法解析”，而是做“板端能否承受”的预算校验。
        // 约束集中放在这里，避免前端和后端各自维护一套规则。
        void validateSettingsBudget(const infer::AppConfig &infer_cfg, const rd::AppConfig &rd_cfg)
        {
            if (infer_cfg.patch_size != infer::kExpectedH || infer_cfg.patch_size != infer::kExpectedW)
            {
                throwInvalidSettings("infer.pipeline.patch.patch_size must be 512.");
            }
            if (infer_cfg.stride <= 0)
            {
                throwInvalidSettings("infer.pipeline.patch.stride must be positive.");
            }
            if (infer_cfg.debug_stride_x_px <= 0 || infer_cfg.debug_stride_y_px <= 0)
            {
                throwInvalidSettings("infer.pipeline.debug.stride_x_px and stride_y_px must be positive.");
            }
            if (infer_cfg.stride > infer::kExpectedW)
            {
                throwInvalidSettings("infer.pipeline.patch.stride must not exceed 512.");
            }
            if (infer_cfg.stride < kMinBoardStride)
            {
                throwBoardBudgetExceeded("infer.pipeline.patch.stride below 64 would exceed the board budget.");
            }
            if (infer_cfg.display_width <= 0 || infer_cfg.display_height <= 0)
            {
                throwInvalidSettings("infer.display.width and infer.display.height must be positive.");
            }
            if (infer_cfg.display_fps < 0)
            {
                throwInvalidSettings("infer.display.fps must be zero or positive.");
            }
            if (infer_cfg.display_width > kMaxBoardDisplayWidth || infer_cfg.display_height > kMaxBoardDisplayHeight)
            {
                throwBoardBudgetExceeded("infer.display width/height exceed the supported board budget.");
            }
            if (infer_cfg.display_fps > kMaxBoardDisplayFps)
            {
                throwBoardBudgetExceeded("infer.display.fps exceeds the supported board budget.");
            }

            const long long display_pixels = static_cast<long long>(infer_cfg.display_width) * infer_cfg.display_height;
            if (display_pixels > kMaxBoardDisplayPixels)
            {
                throwBoardBudgetExceeded("infer.display resolution exceeds the supported board budget.");
            }
            if (infer_cfg.display_fps > kHighFpsThreshold && display_pixels > kMaxBoardDisplayPixelsAtHighFps)
            {
                throwBoardBudgetExceeded("high-FPS display settings exceed the supported board budget.");
            }
            if (rd_cfg.column_tile <= 0 || rd_cfg.row_tile <= 0)
            {
                throwInvalidSettings("rd.column_tile and rd.row_tile must be positive.");
            }
            if (rd_cfg.memory_limit_mb <= 0)
            {
                throwInvalidSettings("rd.memory_limit_mb must be positive.");
            }
            if (rd_cfg.memory_limit_mb > kMaxBoardMemoryLimitMb)
            {
                throwBoardBudgetExceeded("rd.memory_limit_mb exceeds the supported board budget.");
            }
        }

        // 飞行参数虽然很多在当前 cursor 模式下未全部生效，但仍保持基本正值约束。
        void validateFlightSettings(const FlightSettings &flight_settings)
        {
            if (flight_settings.manual_step_px <= 0 ||
                flight_settings.boost_step_px <= 0 ||
                flight_settings.trigger_distance_px <= 0 ||
                flight_settings.cache_grid_px <= 0)
            {
                throwInvalidSettings("flight settings must be positive integers.");
            }
        }

        // 仅推理输入支持预览，因此这里识别常见图片格式供 /api/source/preview 使用。
        bool isImageFile(const fs::path &path)
        {
            const auto ext = shared::ToLower(path.extension().string());
            return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp";
        }

        // 用于确认用户在前端选中的 source id 仍然存在，避免目录刷新后落到悬空引用。
        bool hasSourceId(const std::vector<SourceInfo> &sources, const std::string &id)
        {
            return std::any_of(sources.begin(), sources.end(), [&](const SourceInfo &info) {
                return info.id == id;
            });
        }

        // 扫描工作流输入源并转成统一的前端展示结构。
        // Infer 侧可能递归扫描图片目录，RD 侧则通常只平铺扫描 echo bin。
        std::vector<SourceInfo> scanSources(const fs::path &root,
                                            const std::string &ext_filter,
                                            bool recursive,
                                            const std::string &type,
                                            bool previewable)
        {
            std::vector<SourceInfo> sources;
            if (root.empty())
            {
                return sources;
            }

            const std::string lowered_ext = shared::ToLower(ext_filter);
            auto tryAdd = [&](const fs::path &path) {
                if (!fs::is_regular_file(path))
                {
                    return;
                }
                if (!lowered_ext.empty() && shared::ToLower(path.extension().string()) != lowered_ext)
                {
                    return;
                }
                SourceInfo info;
                info.id = path.string();
                info.name = path.filename().string();
                info.type = type;
                info.detail = path.string();
                info.status = "ready";
                info.previewable = previewable;
                sources.push_back(std::move(info));
            };

            std::error_code ec;
            if (fs::is_regular_file(root, ec))
            {
                tryAdd(root);
            }
            else if (fs::is_directory(root, ec))
            {
                if (recursive)
                {
                    for (const auto &entry : fs::recursive_directory_iterator(root))
                    {
                        tryAdd(entry.path());
                    }
                }
                else
                {
                    for (const auto &entry : fs::directory_iterator(root))
                    {
                        tryAdd(entry.path());
                    }
                }
            }

            std::sort(sources.begin(), sources.end(), [](const SourceInfo &lhs, const SourceInfo &rhs) {
                return lhs.detail < rhs.detail;
            });
            return sources;
        }
    }

    // 控制器保存三类职责：
    // 1. 运行时配置与前端选择状态；
    // 2. 工作线程和停止/暂停控制句柄；
    // 3. SSE 事件分发所需的状态快照。
    WebConsoleController::WebConsoleController(infer::AppConfig infer_cfg, rd::AppConfig rd_cfg)
        : infer_cfg_(std::move(infer_cfg)), rd_cfg_(std::move(rd_cfg))
    {
        infer::ConfigureManualFlight(toInferManualSettings(flight_settings_));
        runtime_snapshot_.state = shared::ControlState::Idle;
        runtime_snapshot_.selection.workflow = shared::SelectedWorkflow::InferOnly;
        runtime_snapshot_.selection.patch_mode = shared::SelectedPatchMode::AutoSnake;
        runtime_snapshot_.selection.output_mode = infer_cfg_.output_mode;

        const auto infer_sources = listInferSourcesLocked();
        if (!infer_sources.empty())
        {
            runtime_snapshot_.selection.selected_source = infer_sources.front().id;
        }

        last_applied_.infer_cfg = infer_cfg_;
        last_applied_.rd_cfg = rd_cfg_;
        last_applied_.flight_settings = flight_settings_;
        last_applied_.selection = runtime_snapshot_.selection;
    }

    // 析构时只做“协作式停机 + join”，不主动杀线程，保证底层 workflow 能在安全点收尾。
    WebConsoleController::~WebConsoleController()
    {
        std::thread worker_to_join;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (run_control_)
            {
                run_control_->requestStop();
            }
            worker_active_ = false;
            if (worker_.joinable())
            {
                worker_to_join = std::move(worker_);
            }
        }
        if (worker_to_join.joinable())
        {
            worker_to_join.join();
        }
    }

    // Server 层注册一个回调，控制器后续把状态/日志/error 事件推给 SSE hub。
    void WebConsoleController::setEventCallback(EventCallback callback)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        event_callback_ = std::move(callback);
    }

    // Web Console 关闭时调用：把停止信号传给后台 workflow，并立即向前端广播 stopping 状态。
    void WebConsoleController::RequestWorkerStop()
    {
        EventCallback callback;
        std::vector<PendingEvent> events;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_requested_by_controller_ = true;
            if (run_control_)
            {
                run_control_->requestStop();
            }
            if (isRunningState(runtime_snapshot_.state))
            {
                runtime_snapshot_.state = shared::ControlState::Stopping;
                callback = event_callback_;
                queueStateLocked(events);
                queueLogLocked(events, "Stop requested by web shutdown.");
            }
        }
        dispatchEvents(callback, events);
    }

    // 显式等待工作线程退出，供 web 退出路径在持久化配置前做收尾同步。
    void WebConsoleController::JoinWorker()
    {
        joinWorkerIfNeeded();
    }

    // 返回当前控制器维护的统一运行态快照，HTTP `/api/state` 直接依赖这里。
    shared::WorkflowRuntimeSnapshot WebConsoleController::snapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return runtime_snapshot_;
    }

    // 返回当前内存中的 infer 配置，不一定已经被写回 YAML。
    infer::AppConfig WebConsoleController::inferConfig() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return infer_cfg_;
    }

    // 返回当前内存中的 RD 配置，不一定已经被写回 YAML。
    rd::AppConfig WebConsoleController::rdConfig() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return rd_cfg_;
    }

    // 返回 Web 侧缓存的飞行参数，用于设置页回显与持久化。
    FlightSettings WebConsoleController::flightSettings() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return flight_settings_;
    }

    // 将 infer 子模块暴露的 manual telemetry 适配成 Web 协议层结构。
    ManualFlightTelemetry WebConsoleController::manualTelemetry() const
    {
        const infer::ManualFlightTelemetry infer_telemetry = infer::GetManualFlightTelemetry();
        ManualFlightTelemetry telemetry;
        telemetry.configured = infer_telemetry.configured;
        telemetry.active = infer_telemetry.active;
        telemetry.paused = infer_telemetry.paused;
        telemetry.edge_blocked = infer_telemetry.edge_blocked;
        telemetry.position_x = infer_telemetry.position_x;
        telemetry.position_y = infer_telemetry.position_y;
        telemetry.last_inferred_center_x = infer_telemetry.last_inferred_center_x;
        telemetry.last_inferred_center_y = infer_telemetry.last_inferred_center_y;
        telemetry.path_points = infer_telemetry.path_points;
        telemetry.patch_count = infer_telemetry.patch_count;
        telemetry.current_direction = infer_telemetry.current_direction;
        telemetry.pending_direction = infer_telemetry.pending_direction;
        return telemetry;
    }

    // 依据当前 workflow 枚举输入源列表，供 `/api/sources` 统一调用。
    std::vector<SourceInfo> WebConsoleController::listSources(shared::SelectedWorkflow workflow) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return workflow == shared::SelectedWorkflow::InferOnly ? listInferSourcesLocked() : listRdSourcesLocked();
    }

    // 只有 infer 图片源允许预览，这里同时完成“是否存在”和“是否真的是图片”的双重校验。
    std::optional<std::filesystem::path> WebConsoleController::resolveInferPreviewPath(const std::string &id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!hasSourceId(listInferSourcesLocked(), id))
        {
            return std::nullopt;
        }
        const fs::path path(id);
        if (!fs::exists(path) || !isImageFile(path))
        {
            return std::nullopt;
        }
        return path;
    }

    // 处理前端“模式/输出/source 选择”变更。
    // 该接口只改选择本身；如果当前处于 paused，会先停掉旧 worker，再切到新选择。
    std::string WebConsoleController::applySelection(const std::unordered_map<std::string, std::string> &fields)
    {
        std::thread worker_to_join;
        EventCallback callback;
        std::vector<PendingEvent> events;
        shared::WorkflowSelection next_selection;
        infer::AppConfig next_infer_cfg;
        bool stop_paused_workflow = false;
        bool workflow_changed = false;
        bool selected_source_provided = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (runtime_snapshot_.state == shared::ControlState::Starting ||
                runtime_snapshot_.state == shared::ControlState::Running ||
                runtime_snapshot_.state == shared::ControlState::Stopping)
            {
                return MakeErrorResponse("invalid_state", "Cannot change selection while a workflow is running.");
            }

            next_selection = runtime_snapshot_.selection;
            next_infer_cfg = infer_cfg_;
            if (const auto it = fields.find("workflow_mode"); it != fields.end())
            {
                shared::SelectedWorkflow workflow = next_selection.workflow;
                if (!ParseSelectedWorkflow(it->second, workflow))
                {
                    return MakeErrorResponse("invalid_selection", "workflow_mode must be rd or infer.");
                }
                workflow_changed = workflow != next_selection.workflow;
                next_selection.workflow = workflow;
            }
            if (const auto it = fields.find("patch_mode"); it != fields.end())
            {
                shared::SelectedPatchMode patch_mode = next_selection.patch_mode;
                if (!ParseSelectedPatchMode(it->second, patch_mode))
                {
                    return MakeErrorResponse("invalid_selection", "patch_mode must be auto_snake, manual_flight, or debug_raster.");
                }
                next_selection.patch_mode = patch_mode;
                next_infer_cfg.patch_mode = ToString(patch_mode);
            }
            if (const auto it = fields.find("output_mode"); it != fields.end())
            {
                const auto output_mode = shared::ToLower(shared::Trim(it->second));
                if (output_mode != "hdmi" && output_mode != "png")
                {
                    return MakeErrorResponse("invalid_selection", "output_mode must be hdmi or png.");
                }
                next_selection.output_mode = output_mode;
                next_infer_cfg.output_mode = output_mode;
            }
            const std::string selection_error = validateInferSelection(next_selection.patch_mode, next_selection.output_mode);
            if (!selection_error.empty())
            {
                return MakeErrorResponse("invalid_selection", selection_error);
            }
            if (const auto it = fields.find("selected_source"); it != fields.end())
            {
                next_selection.selected_source = it->second;
                selected_source_provided = true;
            }

            if (workflow_changed && !selected_source_provided)
            {
                next_selection.selected_source.clear();
            }

            if (!next_selection.selected_source.empty())
            {
                const auto &sources = next_selection.workflow == shared::SelectedWorkflow::InferOnly
                                          ? listInferSourcesLocked()
                                          : listRdSourcesLocked();
                if (!hasSourceId(sources, next_selection.selected_source))
                {
                    if (workflow_changed)
                    {
                        next_selection.selected_source.clear();
                    }
                    else
                    {
                        return MakeErrorResponse("invalid_selection", "selected_source is not available for the current workflow.");
                    }
                }
            }

            if (runtime_snapshot_.state == shared::ControlState::Paused)
            {
                stop_paused_workflow = true;
                stop_requested_by_controller_ = true;
                if (run_control_)
                {
                    run_control_->requestStop();
                }
                runtime_snapshot_.state = shared::ControlState::Stopping;
                callback = event_callback_;
                queueStateLocked(events);
                queueLogLocked(events, "Stopping paused workflow before applying selection.");
                if (worker_.joinable())
                {
                    worker_to_join = std::move(worker_);
                }
            }
        }
        dispatchEvents(callback, events);

        if (worker_to_join.joinable())
        {
            worker_to_join.join();
        }
        joinWorkerIfNeeded();

        events.clear();
        callback = EventCallback{};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            infer_cfg_ = next_infer_cfg;
            runtime_snapshot_.selection = next_selection;
            if (stop_paused_workflow)
            {
                runtime_snapshot_.state = shared::ControlState::Idle;
                runtime_snapshot_.current_stage = "selection updated";
                runtime_snapshot_.current_item.clear();
                runtime_snapshot_.last_error.clear();
                runtime_snapshot_.current_index = 0;
                runtime_snapshot_.total_count = 0;
                runtime_snapshot_.infer_ms = 0.0;
                runtime_snapshot_.total_ms = 0.0;
                runtime_snapshot_.fps = 0.0;
            }
            callback = event_callback_;
            queueStateLocked(events);
        }
        dispatchEvents(callback, events);
        return MakeOkResponse("Selection updated.");
    }

    // 处理前端设置面板的批量字段提交。
    // 这里负责：解析字符串 -> 更新临时配置副本 -> 做跨字段校验 -> 原子替换控制器内存状态。
    std::string WebConsoleController::applySettings(const std::unordered_map<std::string, std::string> &fields)
    {
        EventCallback callback;
        std::vector<PendingEvent> events;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (isRunningState(runtime_snapshot_.state))
            {
                return MakeErrorResponse("invalid_state", "Cannot change settings while a workflow is running.");
            }

            try
            {
                infer::AppConfig next_infer_cfg = infer_cfg_;
                rd::AppConfig next_rd_cfg = rd_cfg_;
                FlightSettings next_flight_settings = flight_settings_;
                shared::WorkflowSelection next_selection = runtime_snapshot_.selection;

                for (const auto &[key, value] : fields)
                {
                    if (key == "infer.sys.device")
                    {
                        next_infer_cfg.device_url = value;
                    }
                    else if (key == "infer.sys.run_backend")
                    {
                        next_infer_cfg.run_backend = value;
                    }
                    else if (key == "infer.sys.mmuMode")
                    {
                        next_infer_cfg.mmu_mode = shared::ParseBool(value, key);
                    }
                    else if (key == "infer.sys.speedMode")
                    {
                        next_infer_cfg.speed_mode = shared::ParseBool(value, key);
                    }
                    else if (key == "infer.sys.compressFtmp")
                    {
                        next_infer_cfg.compress_ftmp = shared::ParseBool(value, key);
                    }
                    else if (key == "infer.sys.ocm_option")
                    {
                        next_infer_cfg.ocm_option = parseStrictInt(value, key);
                    }
                    else if (key == "infer.sys.profile")
                    {
                        next_infer_cfg.enable_profile = shared::ParseBool(value, key);
                    }
                    else if (key == "infer.input.sar_img_dir")
                    {
                        next_infer_cfg.sar_img_dir = value;
                    }
                    else if (key == "infer.input.sar_img_ext")
                    {
                        next_infer_cfg.sar_img_ext = value;
                    }
                    else if (key == "infer.input.recursive")
                    {
                        next_infer_cfg.recursive = shared::ParseBool(value, key);
                    }
                    else if (key == "infer.pipeline.patch.mode")
                    {
                        shared::SelectedPatchMode patch_mode = next_selection.patch_mode;
                        if (!ParseSelectedPatchMode(value, patch_mode))
                        {
                            throw std::runtime_error("infer.pipeline.patch.mode must be auto_snake, manual_flight, or debug_raster.");
                        }
                        next_selection.patch_mode = patch_mode;
                        next_infer_cfg.patch_mode = ToString(patch_mode);
                    }
                    else if (key == "infer.pipeline.patch.patch_size")
                    {
                        next_infer_cfg.patch_size = parseStrictInt(value, key);
                    }
                    else if (key == "infer.pipeline.patch.stride")
                    {
                        next_infer_cfg.stride = parseStrictInt(value, key);
                    }
                    else if (key == "infer.pipeline.debug.stride_x_px")
                    {
                        next_infer_cfg.debug_stride_x_px = parseStrictInt(value, key);
                    }
                    else if (key == "infer.pipeline.debug.stride_y_px")
                    {
                        next_infer_cfg.debug_stride_y_px = parseStrictInt(value, key);
                    }
                    else if (key == "infer.pipeline.output_wait_ms")
                    {
                        next_infer_cfg.output_wait_ms = parseStrictInt(value, key);
                    }
                    else if (key == "infer.display.width")
                    {
                        next_infer_cfg.display_width = parseStrictInt(value, key);
                    }
                    else if (key == "infer.display.height")
                    {
                        next_infer_cfg.display_height = parseStrictInt(value, key);
                    }
                    else if (key == "infer.display.fps")
                    {
                        next_infer_cfg.display_fps = parseStrictInt(value, key);
                    }
                    else if (key == "infer.output.mode")
                    {
                        next_infer_cfg.output_mode = shared::ToLower(shared::Trim(value));
                        if (!isValidInferOutputMode(next_infer_cfg.output_mode))
                        {
                            throw std::runtime_error("infer.output.mode must be hdmi or png.");
                        }
                        next_selection.output_mode = next_infer_cfg.output_mode;
                    }
                    else if (key == "infer.output.dir")
                    {
                        next_infer_cfg.output_dir = value;
                    }
                    else if (key == "infer.output.overwrite")
                    {
                        next_infer_cfg.overwrite = shared::ParseBool(value, key);
                    }
                    else if (key == "rd.execution_mode")
                    {
                        next_rd_cfg.execution_mode = shared::ToLower(shared::Trim(value));
                        if (!isValidRdExecutionMode(next_rd_cfg.execution_mode))
                        {
                            throw std::runtime_error("rd.execution_mode must be auto, memory_float32, or scratch_double.");
                        }
                    }
                    else if (key == "rd.echo_dir")
                    {
                        next_rd_cfg.echo_dir = value;
                    }
                    else if (key == "rd.echo_ext")
                    {
                        next_rd_cfg.echo_ext = value;
                    }
                    else if (key == "rd.output_dir")
                    {
                        next_rd_cfg.output_dir = value;
                    }
                    else if (key == "rd.scratch_dir")
                    {
                        next_rd_cfg.scratch_dir = value;
                    }
                    else if (key == "rd.column_tile")
                    {
                        next_rd_cfg.column_tile = parseStrictInt(value, key);
                    }
                    else if (key == "rd.row_tile")
                    {
                        next_rd_cfg.row_tile = parseStrictInt(value, key);
                    }
                    else if (key == "rd.memory_limit_mb")
                    {
                        next_rd_cfg.memory_limit_mb = parseStrictInt(value, key);
                    }
                    else if (key == "rd.prefer_memory_pipeline")
                    {
                        next_rd_cfg.prefer_memory_pipeline = shared::ParseBool(value, key);
                    }
                    else if (key == "rd.keep_scratch")
                    {
                        next_rd_cfg.keep_scratch = shared::ParseBool(value, key);
                    }
                    else if (key == "rd.overwrite")
                    {
                        next_rd_cfg.overwrite = shared::ParseBool(value, key);
                    }
                    else if (key == "flight.manual_step_px")
                    {
                        next_flight_settings.manual_step_px = parseStrictInt(value, key);
                    }
                    else if (key == "flight.boost_step_px")
                    {
                        next_flight_settings.boost_step_px = parseStrictInt(value, key);
                    }
                    else if (key == "flight.trigger_distance_px")
                    {
                        next_flight_settings.trigger_distance_px = parseStrictInt(value, key);
                    }
                    else if (key == "flight.cache_grid_px")
                    {
                        next_flight_settings.cache_grid_px = parseStrictInt(value, key);
                    }
                    else if (key == "flight.path_overlay")
                    {
                        next_flight_settings.path_overlay = shared::ParseBool(value, key);
                    }
                    else if (key == "flight.control_bindings")
                    {
                        next_flight_settings.control_bindings = value;
                    }
                }

                const std::string selection_error = validateInferSelection(next_selection.patch_mode, next_selection.output_mode);
                if (!selection_error.empty())
                {
                    throw std::runtime_error(selection_error);
                }

                validateSettingsBudget(next_infer_cfg, next_rd_cfg);
                validateFlightSettings(next_flight_settings);

                infer_cfg_ = std::move(next_infer_cfg);
                rd_cfg_ = std::move(next_rd_cfg);
                flight_settings_ = std::move(next_flight_settings);
                runtime_snapshot_.selection = std::move(next_selection);
            }
            catch (const SettingsValidationError &e)
            {
                return MakeErrorResponse(e.code, e.what());
            }
            catch (const std::exception &e)
            {
                return MakeErrorResponse("invalid_settings", e.what());
            }

            infer::ConfigureManualFlight(toInferManualSettings(flight_settings_));
            callback = event_callback_;
            queueStateLocked(events);
        }
        dispatchEvents(callback, events);
        return MakeOkResponse("Settings updated in memory.");
    }

    // Start 既承担“首次启动”也承担“从 paused 恢复”两种语义。
    // 首次启动时会冻结一份 last_applied_，供 reset 回退到最近一次成功启动前的配置。
    std::string WebConsoleController::commandStart()
    {
        joinWorkerIfNeeded();

        EventCallback callback;
        std::vector<PendingEvent> events;
        bool resume_requested = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (runtime_snapshot_.state == shared::ControlState::Paused)
            {
                if (run_control_)
                {
                    run_control_->requestResume();
                }
                if (runtime_snapshot_.selection.patch_mode == shared::SelectedPatchMode::ManualFlight)
                {
                    infer::SetManualFlightPaused(false);
                }
                runtime_snapshot_.state = shared::ControlState::Running;
                runtime_snapshot_.last_error.clear();
                callback = event_callback_;
                queueStateLocked(events);
                queueLogLocked(events, "Resume requested.");
                resume_requested = true;
            }
            else if (runtime_snapshot_.state == shared::ControlState::Running ||
                     runtime_snapshot_.state == shared::ControlState::Starting ||
                     runtime_snapshot_.state == shared::ControlState::Stopping)
            {
                return MakeErrorResponse("invalid_state", "Workflow is already active.");
            }
            else
            {
                stop_requested_by_controller_ = false;
                runtime_snapshot_.state = shared::ControlState::Starting;
                runtime_snapshot_.current_stage = "prepare worker";
                runtime_snapshot_.current_item.clear();
                runtime_snapshot_.last_error.clear();

                last_applied_.infer_cfg = infer_cfg_;
                last_applied_.rd_cfg = rd_cfg_;
                last_applied_.flight_settings = flight_settings_;
                last_applied_.selection = runtime_snapshot_.selection;

                auto control = std::make_unique<shared::WorkflowRunControl>();
                control->setSnapshotCallback([this](const shared::WorkflowRuntimeSnapshot &snapshot) {
                    onWorkflowSnapshot(snapshot);
                });
                run_control_ = std::move(control);

                shared::WorkflowSelection selection = runtime_snapshot_.selection;
                infer::AppConfig infer_cfg = infer_cfg_;
                rd::AppConfig rd_cfg = rd_cfg_;
                if (!selection.selected_source.empty())
                {
                    if (selection.workflow == shared::SelectedWorkflow::InferOnly)
                    {
                        infer_cfg.sar_img_dir = selection.selected_source;
                    }
                    else
                    {
                        rd_cfg.echo_dir = selection.selected_source;
                    }
                }
                infer_cfg.patch_mode = ToString(selection.patch_mode);
                infer_cfg.output_mode = selection.output_mode;
                infer::ConfigureManualFlight(toInferManualSettings(flight_settings_));
                infer::ResetManualFlight();

                worker_active_ = true;
                worker_ = std::thread(&WebConsoleController::workerMain,
                                      this,
                                      selection,
                                      infer_cfg,
                                      rd_cfg,
                                      run_control_.get());
                callback = event_callback_;
                queueStateLocked(events);
                queueLogLocked(events, "Workflow thread started.");
            }
        }
        dispatchEvents(callback, events);
        return MakeOkResponse(resume_requested ? "Workflow resumed." : "Workflow start requested.");
    }

    // Pause 只发协作式暂停请求，真正停在什么位置由 workflow 内部安全点决定。
    std::string WebConsoleController::commandPause()
    {
        EventCallback callback;
        std::vector<PendingEvent> events;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (runtime_snapshot_.state != shared::ControlState::Running || !run_control_)
            {
                return MakeErrorResponse("invalid_state", "Pause is only valid while running.");
            }
            run_control_->requestPause();
            if (runtime_snapshot_.selection.patch_mode == shared::SelectedPatchMode::ManualFlight)
            {
                infer::SetManualFlightPaused(true);
            }
            runtime_snapshot_.state = shared::ControlState::Paused;
            callback = event_callback_;
            queueStateLocked(events);
            queueLogLocked(events, "Pause requested at next safe point.");
        }
        dispatchEvents(callback, events);
        return MakeOkResponse("Pause requested.");
    }

    // Stop 不等待线程立刻退出，只把状态推进到 stopping 并请求后台在安全点停止。
    std::string WebConsoleController::commandStop()
    {
        return stopWorkflow(false, "Stop requested.");
    }

    // Reset 的语义是“回到最近一次成功 start 之前的控制器内存态”，不是回默认配置。
    std::string WebConsoleController::commandReset()
    {
        if (isRunningState(snapshot().state))
        {
            const std::string stop_response = stopWorkflow(true, "Workflow stopped.");
            if (stop_response.find("\"ok\":false") != std::string::npos)
            {
                return stop_response;
            }
        }

        joinWorkerIfNeeded();

        EventCallback callback;
        std::vector<PendingEvent> events;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            restoreLastAppliedLocked();
            infer::ConfigureManualFlight(toInferManualSettings(flight_settings_));
            infer::ResetManualFlight();
            runtime_snapshot_.state = shared::ControlState::Idle;
            runtime_snapshot_.current_stage = "reset";
            runtime_snapshot_.current_item.clear();
            runtime_snapshot_.last_error.clear();
            runtime_snapshot_.current_index = 0;
            runtime_snapshot_.total_count = 0;
            runtime_snapshot_.infer_ms = 0.0;
            runtime_snapshot_.total_ms = 0.0;
            runtime_snapshot_.fps = 0.0;
            callback = event_callback_;
            queueStateLocked(events);
            queueLogLocked(events, "Controller state reset to last applied snapshot.");
        }
        dispatchEvents(callback, events);
        return MakeOkResponse("Controller reset completed.");
    }

    // 关闭 Web Console 前必须先确保 workflow 已停掉，否则后续配置落盘与线程收尾次序会混乱。
    std::string WebConsoleController::commandShutdownWeb()
    {
        if (isRunningState(snapshot().state))
        {
            const std::string stop_response = stopWorkflow(true, "Workflow stopped for web console shutdown.");
            if (stop_response.find("\"ok\":false") != std::string::npos)
            {
                return stop_response;
            }
        }

        joinWorkerIfNeeded();

        EventCallback callback;
        std::vector<PendingEvent> events;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callback = event_callback_;
            queueLogLocked(events, "Web console shutdown requested by browser.");
        }
        dispatchEvents(callback, events);
        return MakeOkResponse("Web Console shutdown requested.");
    }

    // 将前端 W/A/S/D 事件转给 manual_flight。
    // 当前 cursor 模式只消费 keydown，keyup 会被明确忽略。
    std::string WebConsoleController::commandManualKey(const std::unordered_map<std::string, std::string> &fields)
    {
        EventCallback callback;
        std::vector<PendingEvent> events;
        std::string key;
        std::string action;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (runtime_snapshot_.selection.workflow != shared::SelectedWorkflow::InferOnly ||
                runtime_snapshot_.selection.patch_mode != shared::SelectedPatchMode::ManualFlight)
            {
                return MakeErrorResponse("invalid_state", "manual_flight input is only valid for infer/manual_flight.");
            }
            if (runtime_snapshot_.state != shared::ControlState::Running &&
                runtime_snapshot_.state != shared::ControlState::Paused)
            {
                return MakeErrorResponse("invalid_state", "manual_flight input is only valid while the workflow is active.");
            }

            const auto key_it = fields.find("key");
            const auto action_it = fields.find("action");
            if (key_it == fields.end() || action_it == fields.end())
            {
                return MakeErrorResponse("invalid_request", "manual_flight requires key and action.");
            }

            key = shared::ToLower(shared::Trim(key_it->second));
            action = shared::ToLower(shared::Trim(action_it->second));
            if (action != "down" && action != "up")
            {
                return MakeErrorResponse("invalid_request", "manual_flight action must be down or up.");
            }

            callback = event_callback_;
        }

        if (action == "up")
        {
            return MakeOkResponse("manual direction release ignored in cursor mode.");
        }

        std::string message;
        if (!infer::SubmitManualFlightKey(key, true, &message))
        {
            return MakeErrorResponse("invalid_manual_key", message);
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            callback = event_callback_;
            queueStateLocked(events);
        }
        dispatchEvents(callback, events);
        return MakeOkResponse(message);
    }

    // 往待发送队列里压入一条事件；真正发给 SSE 客户端由 dispatchEvents 统一完成。
    void WebConsoleController::queueEventLocked(std::vector<PendingEvent> &events,
                                                const std::string &event_name,
                                                const std::string &payload) const
    {
        events.push_back(PendingEvent{event_name, payload});
    }

    // 生成最新状态事件，状态体里还会带上 manual telemetry 的投影字段。
    void WebConsoleController::queueStateLocked(std::vector<PendingEvent> &events) const
    {
        queueEventLocked(events, "state", MakeStateResponse(runtime_snapshot_, manualTelemetry()));
    }

    // 统一产生日志事件，前端会直接写进 event stream 面板。
    void WebConsoleController::queueLogLocked(std::vector<PendingEvent> &events, const std::string &message) const
    {
        queueEventLocked(events, "log", MakeLogEvent(message));
    }

    // 统一产生 error 事件，供前端做醒目提示。
    void WebConsoleController::queueErrorLocked(std::vector<PendingEvent> &events, const std::string &message) const
    {
        queueEventLocked(events, "error", MakeErrorEvent(message));
    }

    // 在锁外执行回调，避免 SSE 回调路径反向进入控制器造成锁重入风险。
    void WebConsoleController::dispatchEvents(const EventCallback &callback, const std::vector<PendingEvent> &events)
    {
        if (!callback)
        {
            return;
        }
        for (const auto &event : events)
        {
            callback(event.name, event.payload);
        }
    }

    // workflow 线程持续通过 run_control_ 回传快照。
    // 控制器在这里把“底层状态”与“前端选择态”合并，并补上阶段切换日志。
    void WebConsoleController::onWorkflowSnapshot(const shared::WorkflowRuntimeSnapshot &snapshot)
    {
        EventCallback callback;
        std::vector<PendingEvent> events;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            shared::WorkflowRuntimeSnapshot updated = snapshot;
            updated.selection = runtime_snapshot_.selection;

            if (runtime_snapshot_.state == shared::ControlState::Paused && run_control_ && run_control_->isPauseRequested())
            {
                updated.state = shared::ControlState::Paused;
            }
            else if (runtime_snapshot_.state == shared::ControlState::Stopping && run_control_ && run_control_->shouldStop())
            {
                updated.state = shared::ControlState::Stopping;
            }

            if (!updated.current_stage.empty() && updated.current_stage != last_stage_for_log_)
            {
                last_stage_for_log_ = updated.current_stage;
                queueLogLocked(events, "Stage: " + updated.current_stage);
            }

            runtime_snapshot_ = std::move(updated);
            callback = event_callback_;
            queueStateLocked(events);
        }
        dispatchEvents(callback, events);
    }

    // 真正执行 Infer/RD 的后台线程入口。
    // 这里不做业务逻辑，只做状态切换、异常兜底和结束态归并。
    void WebConsoleController::workerMain(shared::WorkflowSelection selection,
                                          infer::AppConfig infer_cfg,
                                          rd::AppConfig rd_cfg,
                                          shared::WorkflowRunControl *control)
    {
        int result = 0;
        std::string error_message;

        EventCallback callback;
        std::vector<PendingEvent> events;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            runtime_snapshot_.state = shared::ControlState::Running;
            runtime_snapshot_.selection = selection;
            runtime_snapshot_.current_stage = "run workflow";
            runtime_snapshot_.current_item = selection.selected_source;
            callback = event_callback_;
            queueStateLocked(events);
        }
        dispatchEvents(callback, events);

        try
        {
            if (selection.workflow == shared::SelectedWorkflow::InferOnly)
            {
                result = workflow::infer::Run(infer_cfg, control);
            }
            else
            {
                result = workflow::rd::Run(rd_cfg, control);
            }
        }
        catch (const std::exception &e)
        {
            result = 2;
            error_message = e.what();
        }

        events.clear();
        callback = EventCallback{};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (selection.workflow == shared::SelectedWorkflow::InferOnly &&
                selection.patch_mode == shared::SelectedPatchMode::ManualFlight)
            {
                infer::ResetManualFlight();
            }
            worker_active_ = false;
            if (!error_message.empty())
            {
                runtime_snapshot_.state = shared::ControlState::Error;
                runtime_snapshot_.last_error = error_message;
                queueErrorLocked(events, error_message);
                queueLogLocked(events, "Workflow failed: " + error_message);
            }
            else if (stop_requested_by_controller_ || (control && control->shouldStop()))
            {
                runtime_snapshot_.state = shared::ControlState::Idle;
                runtime_snapshot_.current_stage = "stopped";
                runtime_snapshot_.current_item.clear();
                runtime_snapshot_.last_error.clear();
                queueLogLocked(events, "Workflow stopped.");
            }
            else if (result == 0)
            {
                runtime_snapshot_.state = shared::ControlState::Finished;
                runtime_snapshot_.current_stage = "finished";
                queueLogLocked(events, "Workflow finished successfully.");
            }
            else
            {
                runtime_snapshot_.state = shared::ControlState::Error;
                runtime_snapshot_.last_error = "workflow exited with code " + std::to_string(result);
                queueErrorLocked(events, runtime_snapshot_.last_error);
                queueLogLocked(events, "Workflow exited with code " + std::to_string(result) + ".");
            }
            callback = event_callback_;
            queueStateLocked(events);
        }
        dispatchEvents(callback, events);
    }

    // 统一的停止实现，`wait_for_worker=true` 时用于 reset / shutdown 这种必须同步收尾的路径。
    std::string WebConsoleController::stopWorkflow(bool wait_for_worker, const std::string &success_message)
    {
        std::thread worker_to_join;
        EventCallback callback;
        std::vector<PendingEvent> events;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!isRunningState(runtime_snapshot_.state))
            {
                return MakeErrorResponse("invalid_state", "No running workflow to stop.");
            }
            stop_requested_by_controller_ = true;
            runtime_snapshot_.state = shared::ControlState::Stopping;
            if (run_control_)
            {
                run_control_->requestStop();
            }
            if (wait_for_worker && worker_.joinable())
            {
                worker_to_join = std::move(worker_);
            }
            callback = event_callback_;
            queueStateLocked(events);
            queueLogLocked(events, wait_for_worker ? "Stop requested. Waiting for workflow thread to exit." : "Stop requested at next safe point.");
        }

        dispatchEvents(callback, events);

        if (worker_to_join.joinable())
        {
            worker_to_join.join();
        }
        if (wait_for_worker)
        {
            joinWorkerIfNeeded();
        }
        return MakeOkResponse(success_message);
    }

    // 如果线程对象仍然 joinable 且逻辑上已结束，就在这里补做 join，避免句柄长期悬挂。
    void WebConsoleController::joinWorkerIfNeeded()
    {
        std::thread worker_to_join;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!worker_active_ && worker_.joinable())
            {
                worker_to_join = std::move(worker_);
            }
        }
        if (worker_to_join.joinable())
        {
            worker_to_join.join();
        }
    }

    // Web 侧把 Starting/Running/Paused/Stopping 都视作“占用中”，禁止并发改设置。
    bool WebConsoleController::isRunningState(shared::ControlState state) const
    {
        return state == shared::ControlState::Starting ||
               state == shared::ControlState::Running ||
               state == shared::ControlState::Paused ||
               state == shared::ControlState::Stopping;
    }

    // Web 配置结构与 infer 子模块内部结构字段名一致但不完全同型，这里做显式拷贝转换。
    infer::ManualFlightSettings WebConsoleController::toInferManualSettings(const FlightSettings &settings)
    {
        infer::ManualFlightSettings infer_settings;
        infer_settings.manual_step_px = settings.manual_step_px;
        infer_settings.boost_step_px = settings.boost_step_px;
        infer_settings.trigger_distance_px = settings.trigger_distance_px;
        infer_settings.cache_grid_px = settings.cache_grid_px;
        infer_settings.path_overlay = settings.path_overlay;
        infer_settings.control_bindings = settings.control_bindings;
        return infer_settings;
    }

    // 回退到最近一次成功启动前的配置与选择快照，供 Reset 复位使用。
    void WebConsoleController::restoreLastAppliedLocked()
    {
        infer_cfg_ = last_applied_.infer_cfg;
        rd_cfg_ = last_applied_.rd_cfg;
        flight_settings_ = last_applied_.flight_settings;
        runtime_snapshot_.selection = last_applied_.selection;
    }

    // Infer 源列表来自 SAR 图片目录，支持递归扫描。
    std::vector<SourceInfo> WebConsoleController::listInferSourcesLocked() const
    {
        return scanSources(infer_cfg_.sar_img_dir, infer_cfg_.sar_img_ext, infer_cfg_.recursive, "SAR PNG", true);
    }

    // RD 源列表来自 echo bin 目录，当前只做一层扫描。
    std::vector<SourceInfo> WebConsoleController::listRdSourcesLocked() const
    {
        return scanSources(rd_cfg_.echo_dir, rd_cfg_.echo_ext, false, "Echo BIN", false);
    }
}
