#include "workflow/web/web_console_controller.hpp"

#include "workflow/infer/infer_workflow.hpp"
#include "workflow/rd/rd_workflow.hpp"
#include "workflow/shared/config_utils.hpp"
#include "workflow/web/web_console_protocol.hpp"

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <stdexcept>

namespace workflow::web
{
    namespace fs = std::filesystem;

    namespace
    {
        bool isValidInferOutputMode(const std::string &value)
        {
            return value == "hdmi" || value == "png";
        }

        bool isValidRdExecutionMode(const std::string &value)
        {
            return value == "auto" ||
                   value == "memory_float32" ||
                   value == "scratch_double";
        }

        bool isImageFile(const fs::path &path)
        {
            const auto ext = shared::ToLower(path.extension().string());
            return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp";
        }

        bool hasSourceId(const std::vector<SourceInfo> &sources, const std::string &id)
        {
            return std::any_of(sources.begin(), sources.end(), [&](const SourceInfo &info) {
                return info.id == id;
            });
        }

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

    WebConsoleController::WebConsoleController(infer::AppConfig infer_cfg, rd::AppConfig rd_cfg)
        : infer_cfg_(std::move(infer_cfg)), rd_cfg_(std::move(rd_cfg))
    {
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

    void WebConsoleController::setEventCallback(EventCallback callback)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        event_callback_ = std::move(callback);
    }

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

    void WebConsoleController::JoinWorker()
    {
        joinWorkerIfNeeded();
    }

    shared::WorkflowRuntimeSnapshot WebConsoleController::snapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return runtime_snapshot_;
    }

    infer::AppConfig WebConsoleController::inferConfig() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return infer_cfg_;
    }

    rd::AppConfig WebConsoleController::rdConfig() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return rd_cfg_;
    }

    FlightSettings WebConsoleController::flightSettings() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return flight_settings_;
    }

    std::vector<SourceInfo> WebConsoleController::listSources(shared::SelectedWorkflow workflow) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return workflow == shared::SelectedWorkflow::InferOnly ? listInferSourcesLocked() : listRdSourcesLocked();
    }

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

    std::string WebConsoleController::applySelection(const std::unordered_map<std::string, std::string> &fields)
    {
        std::thread worker_to_join;
        EventCallback callback;
        std::vector<PendingEvent> events;
        shared::WorkflowSelection next_selection;
        infer::AppConfig next_infer_cfg;
        bool stop_paused_workflow = false;
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
                next_selection.workflow = workflow;
            }
            if (const auto it = fields.find("patch_mode"); it != fields.end())
            {
                shared::SelectedPatchMode patch_mode = next_selection.patch_mode;
                if (!ParseSelectedPatchMode(it->second, patch_mode))
                {
                    return MakeErrorResponse("invalid_selection", "patch_mode must be auto_snake or manual_flight.");
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
            if (const auto it = fields.find("selected_source"); it != fields.end())
            {
                next_selection.selected_source = it->second;
            }

            if (!next_selection.selected_source.empty())
            {
                const auto &sources = next_selection.workflow == shared::SelectedWorkflow::InferOnly
                                          ? listInferSourcesLocked()
                                          : listRdSourcesLocked();
                if (!hasSourceId(sources, next_selection.selected_source))
                {
                    return MakeErrorResponse("invalid_selection", "selected_source is not available for the current workflow.");
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
                for (const auto &[key, value] : fields)
                {
                    if (key == "infer.sys.device")
                    {
                        infer_cfg_.device_url = value;
                    }
                    else if (key == "infer.sys.run_backend")
                    {
                        infer_cfg_.run_backend = value;
                    }
                    else if (key == "infer.sys.mmuMode")
                    {
                        infer_cfg_.mmu_mode = shared::ParseBool(value, key);
                    }
                    else if (key == "infer.sys.speedMode")
                    {
                        infer_cfg_.speed_mode = shared::ParseBool(value, key);
                    }
                    else if (key == "infer.sys.compressFtmp")
                    {
                        infer_cfg_.compress_ftmp = shared::ParseBool(value, key);
                    }
                    else if (key == "infer.sys.ocm_option")
                    {
                        infer_cfg_.ocm_option = std::stoi(value);
                    }
                    else if (key == "infer.sys.profile")
                    {
                        infer_cfg_.enable_profile = shared::ParseBool(value, key);
                    }
                    else if (key == "infer.input.sar_img_dir")
                    {
                        infer_cfg_.sar_img_dir = value;
                    }
                    else if (key == "infer.input.sar_img_ext")
                    {
                        infer_cfg_.sar_img_ext = value;
                    }
                    else if (key == "infer.input.recursive")
                    {
                        infer_cfg_.recursive = shared::ParseBool(value, key);
                    }
                    else if (key == "infer.pipeline.patch.mode")
                    {
                        shared::SelectedPatchMode patch_mode = runtime_snapshot_.selection.patch_mode;
                        if (!ParseSelectedPatchMode(value, patch_mode))
                        {
                            throw std::runtime_error("infer.pipeline.patch.mode must be auto_snake or manual_flight.");
                        }
                        infer_cfg_.patch_mode = ToString(patch_mode);
                        runtime_snapshot_.selection.patch_mode = patch_mode;
                    }
                    else if (key == "infer.pipeline.patch.patch_size")
                    {
                        infer_cfg_.patch_size = std::stoi(value);
                    }
                    else if (key == "infer.pipeline.patch.stride")
                    {
                        infer_cfg_.stride = std::stoi(value);
                    }
                    else if (key == "infer.pipeline.output_wait_ms")
                    {
                        infer_cfg_.output_wait_ms = std::stoi(value);
                    }
                    else if (key == "infer.display.width")
                    {
                        infer_cfg_.display_width = std::stoi(value);
                    }
                    else if (key == "infer.display.height")
                    {
                        infer_cfg_.display_height = std::stoi(value);
                    }
                    else if (key == "infer.display.fps")
                    {
                        infer_cfg_.display_fps = std::stoi(value);
                    }
                    else if (key == "infer.output.mode")
                    {
                        infer_cfg_.output_mode = shared::ToLower(shared::Trim(value));
                        if (!isValidInferOutputMode(infer_cfg_.output_mode))
                        {
                            throw std::runtime_error("infer.output.mode must be hdmi or png.");
                        }
                        runtime_snapshot_.selection.output_mode = infer_cfg_.output_mode;
                    }
                    else if (key == "infer.output.dir")
                    {
                        infer_cfg_.output_dir = value;
                    }
                    else if (key == "infer.output.overwrite")
                    {
                        infer_cfg_.overwrite = shared::ParseBool(value, key);
                    }
                    else if (key == "rd.execution_mode")
                    {
                        rd_cfg_.execution_mode = shared::ToLower(shared::Trim(value));
                        if (!isValidRdExecutionMode(rd_cfg_.execution_mode))
                        {
                            throw std::runtime_error("rd.execution_mode must be auto, memory_float32, or scratch_double.");
                        }
                    }
                    else if (key == "rd.echo_dir")
                    {
                        rd_cfg_.echo_dir = value;
                    }
                    else if (key == "rd.echo_ext")
                    {
                        rd_cfg_.echo_ext = value;
                    }
                    else if (key == "rd.output_dir")
                    {
                        rd_cfg_.output_dir = value;
                    }
                    else if (key == "rd.scratch_dir")
                    {
                        rd_cfg_.scratch_dir = value;
                    }
                    else if (key == "rd.column_tile")
                    {
                        rd_cfg_.column_tile = std::stoi(value);
                    }
                    else if (key == "rd.row_tile")
                    {
                        rd_cfg_.row_tile = std::stoi(value);
                    }
                    else if (key == "rd.memory_limit_mb")
                    {
                        rd_cfg_.memory_limit_mb = std::stoi(value);
                    }
                    else if (key == "rd.prefer_memory_pipeline")
                    {
                        rd_cfg_.prefer_memory_pipeline = shared::ParseBool(value, key);
                    }
                    else if (key == "rd.keep_scratch")
                    {
                        rd_cfg_.keep_scratch = shared::ParseBool(value, key);
                    }
                    else if (key == "rd.overwrite")
                    {
                        rd_cfg_.overwrite = shared::ParseBool(value, key);
                    }
                    else if (key == "flight.manual_step_px")
                    {
                        flight_settings_.manual_step_px = std::stoi(value);
                    }
                    else if (key == "flight.boost_step_px")
                    {
                        flight_settings_.boost_step_px = std::stoi(value);
                    }
                    else if (key == "flight.trigger_distance_px")
                    {
                        flight_settings_.trigger_distance_px = std::stoi(value);
                    }
                    else if (key == "flight.cache_grid_px")
                    {
                        flight_settings_.cache_grid_px = std::stoi(value);
                    }
                    else if (key == "flight.path_overlay")
                    {
                        flight_settings_.path_overlay = shared::ParseBool(value, key);
                    }
                    else if (key == "flight.control_bindings")
                    {
                        flight_settings_.control_bindings = value;
                    }
                }
            }
            catch (const std::exception &e)
            {
                return MakeErrorResponse("invalid_settings", e.what());
            }

            callback = event_callback_;
            queueStateLocked(events);
        }
        dispatchEvents(callback, events);
        return MakeOkResponse("Settings updated in memory.");
    }

    std::string WebConsoleController::commandStart()
    {
        joinWorkerIfNeeded();

        EventCallback callback;
        std::vector<PendingEvent> events;
        bool resume_requested = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (runtime_snapshot_.selection.patch_mode == shared::SelectedPatchMode::ManualFlight)
            {
                return MakeErrorResponse("not_implemented", "manual_flight is reserved for a future phase.");
            }

            if (runtime_snapshot_.state == shared::ControlState::Paused)
            {
                if (run_control_)
                {
                    run_control_->requestResume();
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

    std::string WebConsoleController::commandPause()
    {
        EventCallback callback;
        std::vector<PendingEvent> events;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (runtime_snapshot_.selection.patch_mode == shared::SelectedPatchMode::ManualFlight)
            {
                return MakeErrorResponse("not_implemented", "manual_flight pause is reserved for a future phase.");
            }
            if (runtime_snapshot_.state != shared::ControlState::Running || !run_control_)
            {
                return MakeErrorResponse("invalid_state", "Pause is only valid while running.");
            }
            run_control_->requestPause();
            runtime_snapshot_.state = shared::ControlState::Paused;
            callback = event_callback_;
            queueStateLocked(events);
            queueLogLocked(events, "Pause requested at next safe point.");
        }
        dispatchEvents(callback, events);
        return MakeOkResponse("Pause requested.");
    }

    std::string WebConsoleController::commandStop()
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
            if (worker_.joinable())
            {
                worker_to_join = std::move(worker_);
            }
            callback = event_callback_;
            queueStateLocked(events);
            queueLogLocked(events, "Stop requested at next safe point.");
        }

        dispatchEvents(callback, events);

        if (worker_to_join.joinable())
        {
            worker_to_join.join();
        }
        joinWorkerIfNeeded();
        return MakeOkResponse("Workflow stopped.");
    }

    std::string WebConsoleController::commandReset()
    {
        if (isRunningState(snapshot().state))
        {
            const std::string stop_response = commandStop();
            if (stop_response.find("\"ok\":false") != std::string::npos)
            {
                return stop_response;
            }
        }

        EventCallback callback;
        std::vector<PendingEvent> events;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            restoreLastAppliedLocked();
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

    std::string WebConsoleController::commandManualKey(const std::unordered_map<std::string, std::string> &)
    {
        return MakeErrorResponse("not_implemented", "manual_flight controls are reserved for a future phase.");
    }

    void WebConsoleController::queueEventLocked(std::vector<PendingEvent> &events,
                                                const std::string &event_name,
                                                const std::string &payload) const
    {
        events.push_back(PendingEvent{event_name, payload});
    }

    void WebConsoleController::queueStateLocked(std::vector<PendingEvent> &events) const
    {
        queueEventLocked(events, "state", MakeStateResponse(runtime_snapshot_));
    }

    void WebConsoleController::queueLogLocked(std::vector<PendingEvent> &events, const std::string &message) const
    {
        queueEventLocked(events, "log", MakeLogEvent(message));
    }

    void WebConsoleController::queueErrorLocked(std::vector<PendingEvent> &events, const std::string &message) const
    {
        queueEventLocked(events, "error", MakeErrorEvent(message));
    }

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

    bool WebConsoleController::isRunningState(shared::ControlState state) const
    {
        return state == shared::ControlState::Starting ||
               state == shared::ControlState::Running ||
               state == shared::ControlState::Paused ||
               state == shared::ControlState::Stopping;
    }

    void WebConsoleController::restoreLastAppliedLocked()
    {
        infer_cfg_ = last_applied_.infer_cfg;
        rd_cfg_ = last_applied_.rd_cfg;
        flight_settings_ = last_applied_.flight_settings;
        runtime_snapshot_.selection = last_applied_.selection;
    }

    std::vector<SourceInfo> WebConsoleController::listInferSourcesLocked() const
    {
        return scanSources(infer_cfg_.sar_img_dir, infer_cfg_.sar_img_ext, infer_cfg_.recursive, "SAR PNG", true);
    }

    std::vector<SourceInfo> WebConsoleController::listRdSourcesLocked() const
    {
        return scanSources(rd_cfg_.echo_dir, rd_cfg_.echo_ext, false, "Echo BIN", false);
    }
}
