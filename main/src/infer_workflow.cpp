#include "workflow/infer/infer_config.hpp"
#include "workflow/infer/infer_workflow.hpp"
#include "workflow/infer/infer_workflow_internal.hpp"
#include "workflow/infer/manual_flight_runtime.hpp"
#include "workflow/shared/config_utils.hpp"
#include <icraft-backends/hostbackend/backend.h>
#include <icraft-backends/hostbackend/utils.h>
#include <icraft-backends/zg330backend/zg330backend.h>
#include <icraft-xir/serialize/json.h>
#include <icraft-xrt/core/session.h>
#include <icraft-xrt/dev/host_device.h>
#include <icraft-xrt/dev/zg330_device.h>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace icraft::xrt;
using namespace icraft::xir;
using FPAIDevice = icraft::xrt::ZG330Device;

namespace workflow::infer
{
    namespace fs = std::filesystem;

    namespace
    {
        constexpr int EXPECTED_N = kExpectedN;
        constexpr int EXPECTED_H = kExpectedH;
        constexpr int EXPECTED_W = kExpectedW;
        constexpr int EXPECTED_C = kExpectedC;
        constexpr int SEG_CLASSES = kSegClasses;

        const char *g_runtime_stage = "startup";

        void setStage(const char *stage)
        {
            g_runtime_stage = stage;
            spdlog::info("[stage] {}", stage);
        }

        void handleSegfault(int)
        {
            std::fprintf(stderr, "\n[fatal] Segmentation fault near stage: %s\n", g_runtime_stage);
            std::_Exit(139);
        }

        struct ManualFlightCoordinatorState
        {
            std::mutex mutex;
            ManualFlightSettings settings;
            bool configured = false;
            std::shared_ptr<ManualFlightRuntimeState> runtime;
        };

        ManualFlightCoordinatorState &manualFlightCoordinatorState()
        {
            static ManualFlightCoordinatorState state;
            return state;
        }

        void syncManualFlightRuntimeConfiguration(const std::shared_ptr<ManualFlightRuntimeState> &runtime)
        {
            if (runtime == nullptr)
            {
                return;
            }

            ManualFlightSettings settings;
            bool configured = false;
            {
                auto &coordinator = manualFlightCoordinatorState();
                std::lock_guard<std::mutex> lock(coordinator.mutex);
                settings = coordinator.settings;
                configured = coordinator.configured;
            }

            runtime->setConfiguration(settings, configured);
        }

        void registerManualFlightRuntimeState(const std::shared_ptr<ManualFlightRuntimeState> &runtime)
        {
            auto &coordinator = manualFlightCoordinatorState();
            std::lock_guard<std::mutex> lock(coordinator.mutex);
            coordinator.runtime = runtime;
        }

        void unregisterManualFlightRuntimeState(const std::shared_ptr<ManualFlightRuntimeState> &runtime)
        {
            auto &coordinator = manualFlightCoordinatorState();
            std::lock_guard<std::mutex> lock(coordinator.mutex);
            if (coordinator.runtime == runtime)
            {
                coordinator.runtime.reset();
            }
        }

        shared::SelectedPatchMode parsePatchMode(const std::string &mode)
        {
            const std::string lowered = shared::ToLower(mode);
            if (lowered == "manual_flight")
            {
                return shared::SelectedPatchMode::ManualFlight;
            }
            if (lowered == "debug_raster")
            {
                return shared::SelectedPatchMode::DebugRaster;
            }
            return shared::SelectedPatchMode::AutoSnake;
        }

        shared::WorkflowSelection makeSelection(const AppConfig &cfg)
        {
            shared::WorkflowSelection selection;
            selection.workflow = shared::SelectedWorkflow::InferOnly;
            selection.patch_mode = parsePatchMode(cfg.patch_mode);
            selection.output_mode = cfg.output_mode;
            selection.selected_source = cfg.sar_img_dir.string();
            return selection;
        }

        void publishSnapshot(shared::WorkflowRunControl *control,
                             const AppConfig &cfg,
                             const RuntimeState &state,
                             const std::string &stage,
                             shared::ControlState control_state,
                             const std::string &current_item = {},
                             const std::string &last_error = {})
        {
            if (control == nullptr)
            {
                return;
            }

            shared::WorkflowRuntimeSnapshot snapshot;
            snapshot.state = control_state;
            snapshot.selection = makeSelection(cfg);
            snapshot.current_stage = stage;
            snapshot.current_item = current_item.empty() ? state.sar_stem : current_item;
            snapshot.last_error = last_error;
            snapshot.current_index = state.patch.index >= 0 ? (state.patch.index + 1) : state.sar_index;
            snapshot.total_count = state.patch_count > 0 ? state.patch_count : state.sar_count;
            snapshot.infer_ms = state.infer_ms;
            snapshot.total_ms = state.total_ms;
            snapshot.fps = state.fps;
            control->publish(snapshot);
        }
    }

    std::shared_ptr<ManualFlightRuntimeState> activeManualFlightRuntimeState()
    {
        auto &coordinator = manualFlightCoordinatorState();
        std::lock_guard<std::mutex> lock(coordinator.mutex);
        return coordinator.runtime;
    }

    void ConfigureManualFlight(const ManualFlightSettings &settings)
    {
        std::shared_ptr<ManualFlightRuntimeState> runtime;
        {
            auto &coordinator = manualFlightCoordinatorState();
            std::lock_guard<std::mutex> lock(coordinator.mutex);
            coordinator.settings = settings;
            coordinator.configured = true;
            runtime = coordinator.runtime;
        }
        if (runtime != nullptr)
        {
            runtime->configure(settings);
        }
    }

    void ResetManualFlight()
    {
        const auto runtime = activeManualFlightRuntimeState();
        if (runtime == nullptr)
        {
            return;
        }
        runtime->reset();
    }

    void SetManualFlightPaused(bool paused)
    {
        const auto runtime = activeManualFlightRuntimeState();
        if (runtime == nullptr)
        {
            return;
        }
        runtime->setPaused(paused);
    }

    bool SubmitManualFlightKey(const std::string &key, bool pressed, std::string *message)
    {
        const auto runtime = activeManualFlightRuntimeState();
        if (runtime == nullptr)
        {
            if (message != nullptr)
            {
                *message = "manual_flight is not active.";
            }
            return false;
        }
        return runtime->submitDirectionKey(key, pressed, message);
    }

    ManualFlightTelemetry GetManualFlightTelemetry()
    {
        const auto runtime = activeManualFlightRuntimeState();
        if (runtime == nullptr)
        {
            auto &coordinator = manualFlightCoordinatorState();
            std::lock_guard<std::mutex> lock(coordinator.mutex);
            ManualFlightRuntimeState idle_runtime;
            idle_runtime.setConfiguration(coordinator.settings, coordinator.configured);
            return idle_runtime.telemetry();
        }
        return runtime->telemetry();
    }

    namespace
    {
        template <typename ShapeType>
        std::string shapeToString(const ShapeType &shape)
        {
            std::ostringstream oss;
            oss << "[";
            for (size_t i = 0; i < shape.size(); ++i)
            {
                if (i > 0)
                {
                    oss << ",";
                }
                oss << shape[i];
            }
            oss << "]";
            return oss.str();
        }

    std::vector<fs::path> collectSarImages(const AppConfig &cfg)
    {
        if (!fs::exists(cfg.sar_img_dir))
        {
            throw std::runtime_error("Input SAR image path does not exist: " + cfg.sar_img_dir.string());
        }

        if (fs::is_regular_file(cfg.sar_img_dir))
        {
            const auto wanted_ext = shared::ToLower(cfg.sar_img_ext);
            if (shared::ToLower(cfg.sar_img_dir.extension().string()) != wanted_ext)
            {
                throw std::runtime_error("Selected SAR image does not match configured extension: " + cfg.sar_img_dir.string());
            }
            return {cfg.sar_img_dir};
        }

        if (!fs::is_directory(cfg.sar_img_dir))
        {
            throw std::runtime_error("Input SAR image directory does not exist: " + cfg.sar_img_dir.string());
        }

        std::vector<fs::path> files;
        const auto wanted_ext = shared::ToLower(cfg.sar_img_ext);
        if (cfg.recursive)
        {
            for (const auto &entry : fs::recursive_directory_iterator(cfg.sar_img_dir))
            {
                if (entry.is_regular_file() && shared::ToLower(entry.path().extension().string()) == wanted_ext)
                {
                    files.push_back(entry.path());
                }
            }
        }
        else
        {
            for (const auto &entry : fs::directory_iterator(cfg.sar_img_dir))
            {
                if (entry.is_regular_file() && shared::ToLower(entry.path().extension().string()) == wanted_ext)
                {
                    files.push_back(entry.path());
                }
            }
        }
        std::sort(files.begin(), files.end());
        return files;
    }

    cv::Mat loadSarImageNorm(const fs::path &path)
    {
        cv::Mat gray = cv::imread(path.string(), cv::IMREAD_GRAYSCALE);
        if (gray.empty())
        {
            throw std::runtime_error("Failed to read SAR image: " + path.string());
        }
        cv::Mat norm;
        gray.convertTo(norm, CV_32FC1, 1.0 / 255.0);
        return norm;
    }

    class ManualFlightRuntime
    {
    public:
        ManualFlightRuntime(cv::Mat image_norm, int patch_size, int stride)
            : image_norm_(std::move(image_norm)),
              patch_size_(patch_size),
              stride_(std::max(1, stride)),
              state_(std::make_shared<ManualFlightRuntimeState>())
        {
            if (image_norm_.cols < patch_size_ || image_norm_.rows < patch_size_)
            {
                throw std::runtime_error("ManualFlightRuntime image is smaller than patch size.");
            }
            syncManualFlightRuntimeConfiguration(state_);
            state_->activate(image_norm_.cols, image_norm_.rows, patch_size_, stride_);
            registerManualFlightRuntimeState(state_);
            syncManualFlightRuntimeConfiguration(state_);
        }

        ~ManualFlightRuntime()
        {
            requestStop();
            state_->reset();
            unregisterManualFlightRuntimeState(state_);
        }

        bool waitNextPatch(PatchPacket &packet, shared::WorkflowRunControl *control, int patch_index)
        {
            cv::Point center;
            if (!state_->waitNextCenter([&] {
                    return stop_requested_ || (control != nullptr && control->shouldStop());
                },
                center))
            {
                return false;
            }
            packet = makePacket(center, patch_index);
            return true;
        }

        void markInferenceCommitted(const PatchPacket &packet)
        {
            const cv::Point center(packet.info.x + packet.info.width / 2, packet.info.y + packet.info.height / 2);
            state_->markInferenceCommitted(center);
        }

        void requestStop()
        {
            stop_requested_ = true;
            if (state_ != nullptr)
            {
                state_->requestStop();
            }
        }

    private:
        PatchPacket makePacket(const cv::Point &center, int patch_index) const
        {
            const int half = patch_size_ / 2;
            PatchPacket packet;
            packet.info.index = patch_index;
            packet.info.grid_row = -1;
            packet.info.grid_col = -1;
            packet.info.x = center.x - half;
            packet.info.y = center.y - half;
            packet.info.width = patch_size_;
            packet.info.height = patch_size_;
            packet.info.right_to_left = false;
            packet.patch_norm = image_norm_(cv::Rect(packet.info.x, packet.info.y, patch_size_, patch_size_)).clone();
            return packet;
        }

        cv::Mat image_norm_;
        int patch_size_ = 512;
        int stride_ = 256;
        bool stop_requested_ = false;
        std::shared_ptr<ManualFlightRuntimeState> state_;
    };

    Network loadNetwork(const std::string &json_path, const std::string &raw_path)
    {
        Network network = Network::CreateFromJsonFile(json_path);
        network.lazyLoadParamsFromFile(raw_path);
        return network;
    }

    Session initSession(const AppConfig &cfg, const NetworkView &network_view, Device &device)
    {
        if (cfg.run_backend == "host")
        {
            return Session::Create<HostBackend>(network_view, {device});
        }
        if (cfg.run_backend != "zg330")
        {
            throw std::runtime_error("Only run_backend=zg330 or host is supported in infer_workflow.");
        }

        auto session = Session::Create<icraft::xrt::zg330::ZG330Backend, HostBackend>(
            network_view, {device, HostDevice::Default()});
        auto zg_backend = session->backends[0].cast<icraft::xrt::zg330::ZG330Backend>();

        if (!cfg.compress_ftmp)
        {
            zg_backend.disableEtmOptimize();
        }
        if (!cfg.speed_mode)
        {
            zg_backend.disableMergeHardOp();
        }

        if (cfg.ocm_option == 0)
        {
            zg_backend.ocmOptimize(icraft::xrt::zg330::OcmOpt::None);
        }
        else if (cfg.ocm_option == 1)
        {
            zg_backend.ocmOptimize(icraft::xrt::zg330::OcmOpt::OPTION1);
        }
        else if (cfg.ocm_option == 2)
        {
            zg_backend.ocmOptimize(icraft::xrt::zg330::OcmOpt::OPTION2);
        }
        else if (cfg.ocm_option == 3)
        {
            zg_backend.ocmOptimize(icraft::xrt::zg330::OcmOpt::OPTION3);
        }
        else if (cfg.ocm_option != 4)
        {
            throw std::runtime_error("Unsupported sys.ocm_option. Expected 0, 1, 2, 3, or 4.");
        }
        return session;
    }

    void validateNetworkIO(const NetworkView &network_view)
    {
        if (network_view.inputs().size() != 1)
        {
            throw std::runtime_error("infer_workflow supports exactly one model input.");
        }
        if (network_view.outputs().size() != 2)
        {
            throw std::runtime_error("infer_workflow expects exactly two model outputs: restore and seg logits.");
        }

        const auto restore_shape = network_view.outputs()[0].tensorType()->shape;
        const auto seg_shape = network_view.outputs()[1].tensorType()->shape;
        if (restore_shape.size() != 4 || restore_shape[0] != 1 || restore_shape[1] != 512 ||
            restore_shape[2] != 512 || restore_shape[3] != 1)
        {
            throw std::runtime_error("output[0] must be [1,512,512,1], actual=" + shapeToString(restore_shape));
        }
        if (seg_shape.size() != 4 || seg_shape[0] != 1 || seg_shape[1] != 512 ||
            seg_shape[2] != 512 || seg_shape[3] != SEG_CLASSES)
        {
            throw std::runtime_error("output[1] must be [1,512,512,6], actual=" + shapeToString(seg_shape));
        }
        if (!network_view.outputs()[0].tensorType()->element_dtype.isFP32() ||
            !network_view.outputs()[1].tensorType()->element_dtype.isFP32())
        {
            throw std::runtime_error("Stage 0 postprocess expects FP32 model outputs.");
        }
    }

    std::string buildOutputLabel(const AppConfig &cfg)
    {
        if (cfg.output_mode == "hdmi")
        {
            return "HDMI / " + std::to_string(cfg.display_width) + "x" + std::to_string(cfg.display_height) +
                   "@" + std::to_string(cfg.display_fps);
        }
        return "PNG / " + cfg.output_dir.string();
    }
    void emitBackendLogIfRequested(const AppConfig &cfg, Session &session)
    {
        if (!cfg.dump_backend_log || cfg.run_backend != "zg330")
        {
            return;
        }
        if (session->backends.empty())
        {
            throw std::runtime_error("Cannot dump backend log because session has no backend.");
        }
        setStage("dump ZG330 backend log");
        auto zg_backend = session->backends[0].cast<icraft::xrt::zg330::ZG330Backend>();
        zg_backend.log();
        spdlog::info("ZG330 backend log requested. Check .icraft/logs for generate_memopt.log.");
    }
    }

int Run(const std::filesystem::path &config_path)
{
    return Run(LoadConfig(config_path), nullptr);
}

int Run(const AppConfig &cfg, shared::WorkflowRunControl *control)
{
    Device device;
    bool device_open = false;
    std::optional<LatestSnapshotMailbox> hdmi_mailbox;
    std::unique_ptr<HdmiRenderWorker> hdmi_render_worker;
    try
    {
        std::signal(SIGSEGV, handleSegfault);
        spdlog::set_level(spdlog::level::info);

        setStage("load config");
        RuntimeState published_state;
        publishSnapshot(control, cfg, published_state, "load config", shared::ControlState::Starting, cfg.sar_img_dir.string());
        spdlog::info("Config loaded: output.mode={}, sar_img_dir={}, model_json={}",
                     cfg.output_mode,
                     cfg.sar_img_dir.string(),
                     cfg.json_path);
        const shared::SelectedPatchMode patch_mode = parsePatchMode(cfg.patch_mode);
        if (patch_mode == shared::SelectedPatchMode::DebugRaster && cfg.output_mode != "png")
        {
            throw std::runtime_error("debug_raster requires output_mode=png.");
        }

        setStage("collect SAR images");
        publishSnapshot(control, cfg, published_state, "collect SAR images", shared::ControlState::Starting, cfg.sar_img_dir.string());
        const auto sar_files = collectSarImages(cfg);
        if (sar_files.empty())
        {
            throw std::runtime_error("No SAR image files found in: " + cfg.sar_img_dir.string());
        }
        spdlog::info("Found {} SAR image file(s).", sar_files.size());

        setStage("open device");
        device = Device::Open(cfg.device_url.c_str());
        device_open = true;

        setStage("load network json/raw");
        publishSnapshot(control, cfg, published_state, "load network", shared::ControlState::Starting, cfg.json_path);
        auto network = loadNetwork(cfg.json_path, cfg.raw_path);

        setStage("create network view");
        auto network_view = network.view(0);

        setStage("validate network IO");
        validateNetworkIO(network_view);
        PatchTensorBuilder tensor_builder(network_view);

        setStage("init session");
        auto session = initSession(cfg, network_view, device);
        session.enableTimeProfile(cfg.enable_profile);

        setStage("session apply");
        session.apply();
        spdlog::info("Session apply finished.");

        emitBackendLogIfRequested(cfg, session);

        setStage("create output sink");
        std::unique_ptr<IFrameSink> sink;
        std::optional<FPAIDevice> fpai_device;
        std::mutex device_access_mutex;
        if (cfg.output_mode == "hdmi")
        {
            fpai_device.emplace(device.cast<FPAIDevice>());
            sink = std::make_unique<HdmiFrameSink>(*fpai_device, cfg.display_width, cfg.display_height, cfg.display_fps);
            UiRenderContext placeholder_ui_context;
            placeholder_ui_context.output_label = buildOutputLabel(cfg);
            placeholder_ui_context.mini_map.source_width = EXPECTED_W;
            placeholder_ui_context.mini_map.source_height = EXPECTED_H;
            placeholder_ui_context.mini_map.patch_size = EXPECTED_W;
            placeholder_ui_context.mini_map.sar_preview_bgr = cv::Mat(EXPECTED_H, EXPECTED_W, CV_8UC3, cv::Scalar(0, 0, 0));
            hdmi_mailbox.emplace();
            hdmi_render_worker = std::make_unique<HdmiRenderWorker>(*sink,
                                                                    *hdmi_mailbox,
                                                                    placeholder_ui_context,
                                                                    cfg.display_width,
                                                                    cfg.display_height,
                                                                    cfg.display_fps,
                                                                    device_access_mutex);
            hdmi_render_worker->start();
        }
        else
        {
            sink = std::make_unique<PngFrameSink>(cfg.output_dir, cfg.overwrite);
        }

        setStage("create inference runner");
        PatchInferenceRunner runner(session,
                                    device,
                                    cfg.output_wait_ms);
        int frame_counter = 0;
        auto processPatch = [&](const PatchPacket &packet,
                                const RuntimeState &base_state,
                                const UiRenderContext &patch_ui_context) -> RuntimeState {
            if (cfg.output_mode == "hdmi")
            {
                return processPatchToHdmi(packet,
                                          base_state,
                                          patch_ui_context,
                                          tensor_builder,
                                          runner,
                                          device_access_mutex,
                                          *hdmi_mailbox,
                                          *hdmi_render_worker,
                                          frame_counter);
            }
            return processPatchToPng(packet,
                                     base_state,
                                     patch_ui_context,
                                     tensor_builder,
                                     runner,
                                     *sink,
                                     cfg,
                                     frame_counter);
        };
        bool stop_requested = false;
        if (patch_mode == shared::SelectedPatchMode::ManualFlight && sar_files.size() != 1)
        {
            throw std::runtime_error("manual_flight requires exactly one selected SAR image.");
        }

        for (size_t sar_idx = 0; sar_idx < sar_files.size(); ++sar_idx)
        {
            const auto &sar_path = sar_files[sar_idx];
            RuntimeState base_state;
            base_state.sar_stem = sar_path.stem().string();
            base_state.sar_index = static_cast<int>(sar_idx + 1);
            base_state.sar_count = static_cast<int>(sar_files.size());

            spdlog::info("Processing SAR image [{}/{}]: {}", base_state.sar_index, base_state.sar_count, sar_path.string());
            const cv::Mat sar_norm = loadSarImageNorm(sar_path);
            spdlog::info("Loaded SAR image {}, size={}x{}, dtype=CV_32FC1, range=0-1", base_state.sar_stem, sar_norm.cols, sar_norm.rows);

            if (sar_norm.cols < cfg.patch_size || sar_norm.rows < cfg.patch_size)
            {
                spdlog::warn("Skip {} because SAR image is smaller than 512x512.", base_state.sar_stem);
                continue;
            }

            UiRenderContext ui_context;
            ui_context.output_label = buildOutputLabel(cfg);
            ui_context.mini_map = buildMiniMapContext(sar_norm, cfg.patch_size, cfg.stride, 0, 0);
            publishSnapshot(control, cfg, base_state, "sar loaded", shared::ControlState::Running, sar_path.string());

            if (patch_mode == shared::SelectedPatchMode::ManualFlight)
            {
                ManualFlightRuntime manual_runtime(sar_norm, cfg.patch_size, cfg.stride);
                base_state.patch_count = 0;
                base_state.stride = cfg.stride;

                PatchPacket packet;
                int patch_index = 0;
                while (manual_runtime.waitNextPatch(packet, control, patch_index))
                {
                    if (control != nullptr)
                    {
                        control->waitIfPaused();
                        if (control->shouldStop())
                        {
                            publishSnapshot(control, cfg, base_state, "stop requested", shared::ControlState::Stopping, sar_path.string());
                            stop_requested = true;
                            break;
                        }
                    }

                    RuntimeState patch_base_state = base_state;
                    patch_base_state.patch_count = patch_index + 1;
                    UiRenderContext patch_ui_context = ui_context;
                    applyManualTelemetry(patch_base_state, patch_ui_context);

                    RuntimeState patch_state = processPatch(packet, patch_base_state, patch_ui_context);
                    manual_runtime.markInferenceCommitted(packet);
                    applyManualTelemetry(patch_state, patch_ui_context);
                    publishSnapshot(control, cfg, patch_state, "manual patch processed", shared::ControlState::Running, sar_path.string());
                    base_state = patch_state;
                    ++patch_index;
                }
            }
            else if (patch_mode == shared::SelectedPatchMode::DebugRaster)
            {
                DebugRasterPatchSource patch_source(sar_norm, cfg.patch_size, cfg.debug_stride_x_px, cfg.debug_stride_y_px);
                base_state.patch_count = patch_source.totalPatches();
                base_state.stride = cfg.debug_stride_x_px;
                spdlog::info("Debug raster grid for {}: rows={}, cols={}, total={}, stride_x_px={}, stride_y_px={}",
                             base_state.sar_stem,
                             patch_source.rows(),
                             patch_source.cols(),
                             patch_source.totalPatches(),
                             cfg.debug_stride_x_px,
                             cfg.debug_stride_y_px);
                ui_context.mini_map = buildMiniMapContext(sar_norm,
                                                          cfg.patch_size,
                                                          cfg.debug_stride_x_px,
                                                          patch_source.rows(),
                                                          patch_source.cols());

                PatchPacket packet;
                while (patch_source.next(packet))
                {
                    if (control != nullptr)
                    {
                        control->waitIfPaused();
                        if (control->shouldStop())
                        {
                            publishSnapshot(control, cfg, base_state, "stop requested", shared::ControlState::Stopping, sar_path.string());
                            stop_requested = true;
                            break;
                        }
                    }

                    RuntimeState patch_state = processPatch(packet, base_state, ui_context);
                    publishSnapshot(control, cfg, patch_state, "debug patch processed", shared::ControlState::Running, sar_path.string());
                }

                if (stop_requested)
                {
                    break;
                }
            }
            else
            {
                SnakePatchSource patch_source(sar_norm, cfg.patch_size, cfg.stride);
                base_state.patch_count = patch_source.totalPatches();
                base_state.stride = cfg.stride;
                spdlog::info("Patch grid for {}: rows={}, cols={}, total={}",
                             base_state.sar_stem,
                             patch_source.rows(),
                             patch_source.cols(),
                             patch_source.totalPatches());
                ui_context.mini_map = buildMiniMapContext(sar_norm, cfg.patch_size, cfg.stride, patch_source.rows(), patch_source.cols());

                PatchPacket packet;
                while (patch_source.next(packet))
                {
                    if (control != nullptr)
                    {
                        control->waitIfPaused();
                        if (control->shouldStop())
                        {
                            publishSnapshot(control, cfg, base_state, "stop requested", shared::ControlState::Stopping, sar_path.string());
                            stop_requested = true;
                            break;
                        }
                    }

                    RuntimeState patch_state = processPatch(packet, base_state, ui_context);
                    publishSnapshot(control, cfg, patch_state, "patch processed", shared::ControlState::Running, sar_path.string());
                }

                if (stop_requested)
                {
                    break;
                }
            }

            if (stop_requested)
            {
                break;
            }
        }

        if (hdmi_mailbox.has_value())
        {
            hdmi_mailbox->markInputClosed();
        }
        if (hdmi_render_worker != nullptr)
        {
            hdmi_render_worker->join();
        }

        if (device_open)
        {
            Device::Close(device);
            device_open = false;
        }
        publishSnapshot(control,
                        cfg,
                        RuntimeState{},
                        stop_requested ? "stopped" : "finished",
                        stop_requested ? shared::ControlState::Idle : shared::ControlState::Finished,
                        cfg.sar_img_dir.string());
        if (patch_mode == shared::SelectedPatchMode::ManualFlight)
        {
            ResetManualFlight();
        }
        return 0;
    }
    catch (const std::exception &e)
    {
        publishSnapshot(control,
                        cfg,
                        RuntimeState{},
                        "error",
                        shared::ControlState::Error,
                        cfg.sar_img_dir.string(),
                        e.what());
        spdlog::error("infer_workflow failed: {}", e.what());
        try
        {
            if (hdmi_mailbox.has_value())
            {
                hdmi_mailbox->requestStop();
            }
            if (hdmi_render_worker != nullptr)
            {
                hdmi_render_worker->join();
            }
            if (device_open)
            {
                Device::Close(device);
            }
            if (parsePatchMode(cfg.patch_mode) == shared::SelectedPatchMode::ManualFlight)
            {
                ResetManualFlight();
            }
        }
        catch (...)
        {
        }
        return 2;
    }
}
}
