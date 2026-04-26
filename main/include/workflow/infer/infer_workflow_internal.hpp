#pragma once

#include "workflow/infer/infer_config.hpp"

#include <icraft-xir/serialize/json.h>
#include <icraft-xrt/core/session.h>

#include <opencv2/core.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace icraft::xrt
{
    class ZG330Device;
}

namespace infer_workflow
{
    template <typename DeviceType>
    class RGB565HDMIDisplay;
}

namespace workflow::infer
{
    class ManualFlightRuntimeState;

    struct PatchInfo
    {
        int index = -1;
        int grid_row = -1;
        int grid_col = -1;
        int x = -1;
        int y = -1;
        int width = 0;
        int height = 0;
        bool right_to_left = false;
    };

    struct RuntimeState
    {
        std::string sar_stem;
        int sar_index = 0;
        int sar_count = 0;
        int patch_count = 0;
        int frame_index = 0;
        PatchInfo patch;
        double fps = 0.0;
        double infer_ms = 0.0;
        double total_ms = 0.0;
        int stride = 256;
        bool manual_active = false;
        int manual_pos_x = 0;
        int manual_pos_y = 0;
        int manual_last_inferred_x = 0;
        int manual_last_inferred_y = 0;
        int manual_path_points = 0;
        int manual_patch_count = 0;
        bool manual_edge_blocked = false;
        std::string manual_direction;
        std::string manual_pending_direction;
    };

    struct MiniMapContext
    {
        cv::Mat sar_preview_bgr;
        int source_width = 0;
        int source_height = 0;
        int patch_size = 0;
        bool path_overlay = false;
        std::vector<cv::Point> path_points;
    };

    struct UiRenderContext
    {
        std::string status_label = "RUNNING";
        std::string mode_label = "INFERENCE ONLY";
        std::string output_label;
        std::string restore_label = "GRAY OUTPUT";
        std::string seg_label = "RGB MASK / 6 CLASS";
        MiniMapContext mini_map;
    };

    struct InferenceSnapshot
    {
        RuntimeState state;
        UiRenderContext ui_context;
        cv::Mat restore_bgr;
        cv::Mat mask_bgr;
    };

    struct PatchPacket
    {
        PatchInfo info;
        cv::Mat patch_norm;
    };

    cv::Vec3b classColorBgr(int cls);

    std::shared_ptr<ManualFlightRuntimeState> activeManualFlightRuntimeState();

    MiniMapContext buildMiniMapContext(const cv::Mat &sar_norm, int patch_size, int stride, int rows, int cols);

    void applyManualTelemetry(RuntimeState &state, UiRenderContext &ui_context);

    class PatchTensorBuilder
    {
    public:
        explicit PatchTensorBuilder(const icraft::xir::NetworkView &network_view);

        icraft::xrt::Tensor build(const cv::Mat &patch_norm) const;

    private:
        icraft::xir::Value input_value_;
    };

    class PatchInferenceRunner
    {
    public:
        PatchInferenceRunner(icraft::xrt::Session &session, icraft::xrt::Device &device, int output_wait_ms);

        std::vector<icraft::xrt::Tensor> forward(const icraft::xrt::Tensor &input_tensor);

    private:
        icraft::xrt::Session &session_;
        icraft::xrt::Device &device_;
        int output_wait_ms_ = 20000;
    };

    cv::Mat restoreToGrayU8(const icraft::xrt::Tensor &tensor);
    cv::Mat logitsToMaskBgr(const icraft::xrt::Tensor &tensor);

    class IFrameSink
    {
    public:
        virtual ~IFrameSink() = default;
        virtual void write(const RuntimeState &state, const cv::Mat &frame_bgr) = 0;
    };

    class PngFrameSink : public IFrameSink
    {
    public:
        PngFrameSink(std::filesystem::path output_dir, bool overwrite);

        void write(const RuntimeState &state, const cv::Mat &frame_bgr) override;
        void writeDebugPatchOutputs(const RuntimeState &state,
                                    const cv::Mat &restore_gray,
                                    const cv::Mat &mask_class);

    private:
        std::filesystem::path output_dir_;
        bool overwrite_ = true;
    };

    class HdmiFrameSink : public IFrameSink
    {
    public:
        HdmiFrameSink(icraft::xrt::ZG330Device &device, int width, int height, int fps);
        ~HdmiFrameSink();

        void write(const RuntimeState &state, const cv::Mat &frame_bgr) override;

    private:
        std::unique_ptr<infer_workflow::RGB565HDMIDisplay<icraft::xrt::ZG330Device>> display_;
        int fps_ = 0;
    };

    class LatestSnapshotMailbox
    {
    public:
        enum class WakeReason
        {
            Timeout,
            NewSnapshot,
            InputClosed,
            StopRequested,
        };

        void publish(InferenceSnapshot &&snapshot);
        bool loadLatest(InferenceSnapshot &snapshot, std::uint64_t &sequence);
        WakeReason waitForChangeOrStop(std::uint64_t known_sequence, std::chrono::microseconds timeout);
        void markInputClosed();
        void requestStop();

    private:
        std::mutex mutex_;
        std::condition_variable cv_;
        std::optional<InferenceSnapshot> latest_snapshot_;
        std::uint64_t published_sequence_ = 0;
        bool input_closed_ = false;
        bool stop_requested_ = false;
    };

    class HdmiRenderWorker
    {
    public:
        HdmiRenderWorker(IFrameSink &sink,
                         LatestSnapshotMailbox &mailbox,
                         const UiRenderContext &placeholder_ui_context,
                         int display_width,
                         int display_height,
                         int display_fps,
                         std::mutex &device_access_mutex);

        void start();
        void requestStop();
        void join();
        void rethrowIfFailed();

    private:
        void run();
        void writeFrame(const RuntimeState &state, const cv::Mat &frame_bgr);

        IFrameSink &sink_;
        LatestSnapshotMailbox &mailbox_;
        UiRenderContext placeholder_ui_context_;
        int display_width_ = 1920;
        int display_height_ = 1080;
        std::chrono::microseconds display_interval_{0};
        std::mutex &device_access_mutex_;
        std::thread worker_;
        std::mutex error_mutex_;
        std::exception_ptr worker_error_;
    };

    RuntimeState processPatchToPng(const PatchPacket &packet,
                                   const RuntimeState &base_state,
                                   const UiRenderContext &ui_context,
                                   PatchTensorBuilder &tensor_builder,
                                   PatchInferenceRunner &runner,
                                   IFrameSink &sink,
                                   const AppConfig &cfg,
                                   int &frame_counter);

    RuntimeState processPatchToHdmi(const PatchPacket &packet,
                                    const RuntimeState &base_state,
                                    const UiRenderContext &ui_context,
                                    PatchTensorBuilder &tensor_builder,
                                    PatchInferenceRunner &runner,
                                    std::mutex &device_access_mutex,
                                    LatestSnapshotMailbox &mailbox,
                                    HdmiRenderWorker &render_worker,
                                    int &frame_counter);

    class SnakePatchSource
    {
    public:
        SnakePatchSource(cv::Mat image_norm, int patch_size, int stride);

        bool next(PatchPacket &packet);

        int totalPatches() const;
        int rows() const;
        int cols() const;

    private:
        cv::Mat image_norm_;
        int patch_size_ = 512;
        int stride_ = 256;
        int rows_ = 0;
        int cols_ = 0;
        int total_ = 0;
        int cursor_ = 0;
    };

    class DebugRasterPatchSource
    {
    public:
        DebugRasterPatchSource(cv::Mat image_norm, int patch_size, int stride_x_px, int stride_y_px);

        bool next(PatchPacket &packet);

        int totalPatches() const;
        int rows() const;
        int cols() const;

    private:
        cv::Mat image_norm_;
        int patch_size_ = 512;
        int stride_x_px_ = 256;
        int stride_y_px_ = 256;
        int rows_ = 0;
        int cols_ = 0;
        int total_ = 0;
        int cursor_ = 0;
    };

    cv::Mat composeIndustrialUiFrame(const UiRenderContext &ui_context,
                                     const RuntimeState &state,
                                     const cv::Mat &restore_bgr,
                                     const cv::Mat &mask_bgr,
                                     int width,
                                     int height);
}
