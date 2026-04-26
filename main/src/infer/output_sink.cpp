#include "infer_workflow_hdmi_display.hpp"
#include "workflow/infer/infer_workflow_internal.hpp"

#include <icraft-backends/hostbackend/utils.h>
#include <icraft-backends/zg330backend/zg330backend.h>
#include <icraft-xrt/dev/host_device.h>
#include <icraft-xrt/dev/zg330_device.h>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <sstream>
#include <string>
#include <thread>
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
        template <typename ShapeT>
        std::string shapeToString(const ShapeT &shape)
        {
            std::ostringstream oss;
            oss << "[";
            for (size_t i = 0; i < shape.size(); ++i)
            {
                if (i != 0)
                {
                    oss << ", ";
                }
                oss << shape[i];
            }
            oss << "]";
            return oss.str();
        }

        Tensor dataToFp32Tensor(const float *input_data, const Value &input_value)
        {
            TensorType out_dtype;
            if (input_value.tensorType()->shape[0] == -1)
            {
                out_dtype = input_value.getUsesOp()[0]->outputs[0].tensorType().clone();
            }
            else
            {
                out_dtype = input_value.tensorType().clone();
            }
            const auto size = out_dtype.numElements();
            auto param_chunk = HostDevice::MemRegion().malloc(size * sizeof(float));
            auto *dst = reinterpret_cast<float *>(param_chunk->begin.cptr());
            std::memcpy(dst, input_data, size * sizeof(float));
            return Tensor(out_dtype, param_chunk);
        }
    }

    PatchTensorBuilder::PatchTensorBuilder(const NetworkView &network_view)
        : input_value_(network_view.inputs()[0])
    {
        const auto shape = input_value_.tensorType()->shape;
        if (shape.size() != 4 || shape[0] != kExpectedN || shape[1] != kExpectedH ||
            shape[2] != kExpectedW || shape[3] != kExpectedC)
        {
            throw std::runtime_error("Model input must be NHWC [1,512,512,1], actual=" + shapeToString(shape));
        }

        const auto storage_type = input_value_.tensorType()->element_dtype.getStorageType();
        if (!storage_type.isFP32())
        {
            throw std::runtime_error("Model input storage dtype must be FP32 for normalized 0-1 patch input.");
        }
    }

    Tensor PatchTensorBuilder::build(const cv::Mat &patch_norm) const
    {
        if (patch_norm.empty() || patch_norm.type() != CV_32FC1 || patch_norm.rows != kExpectedH || patch_norm.cols != kExpectedW)
        {
            throw std::runtime_error("Patch tensor input must be CV_32FC1 512x512 normalized to 0-1.");
        }
        cv::Mat continuous = patch_norm.isContinuous() ? patch_norm : patch_norm.clone();
        return dataToFp32Tensor(reinterpret_cast<const float *>(continuous.data), input_value_);
    }

    PatchInferenceRunner::PatchInferenceRunner(Session &session, Device &device, int output_wait_ms)
        : session_(session), device_(device), output_wait_ms_(output_wait_ms)
    {
    }

    std::vector<Tensor> PatchInferenceRunner::forward(const Tensor &input_tensor)
    {
        auto outputs = session_.forward({input_tensor});
        std::vector<Tensor> host_outputs;
        host_outputs.reserve(outputs.size());
        for (auto &output : outputs)
        {
            output.waitForReady(std::chrono::milliseconds(output_wait_ms_));
            host_outputs.push_back(output.to(HostDevice::MemRegion()));
        }
        device_.reset(1);
        return host_outputs;
    }

    cv::Mat restoreToGrayU8(const Tensor &tensor)
    {
        const auto *data = reinterpret_cast<const float *>(tensor.data().cptr());
        cv::Mat gray(kExpectedH, kExpectedW, CV_8UC1);
        for (int y = 0; y < kExpectedH; ++y)
        {
            auto *row = gray.ptr<std::uint8_t>(y);
            for (int x = 0; x < kExpectedW; ++x)
            {
                const float v = std::max(0.0f, std::min(1.0f, data[y * kExpectedW + x]));
                row[x] = static_cast<std::uint8_t>(std::round(v * 255.0f));
            }
        }
        return gray;
    }

    cv::Mat logitsToMaskBgr(const Tensor &tensor)
    {
        const auto *data = reinterpret_cast<const float *>(tensor.data().cptr());
        cv::Mat mask_bgr(kExpectedH, kExpectedW, CV_8UC3);
        for (int y = 0; y < kExpectedH; ++y)
        {
            auto *row = mask_bgr.ptr<cv::Vec3b>(y);
            for (int x = 0; x < kExpectedW; ++x)
            {
                const int base = (y * kExpectedW + x) * kSegClasses;
                int best_cls = 0;
                float best_score = data[base];
                for (int cls = 1; cls < kSegClasses; ++cls)
                {
                    const float score = data[base + cls];
                    if (score > best_score)
                    {
                        best_score = score;
                        best_cls = cls;
                    }
                }
                row[x] = classColorBgr(best_cls);
            }
        }
        return mask_bgr;
    }

    PngFrameSink::PngFrameSink(std::filesystem::path output_dir, bool overwrite)
        : output_dir_(std::move(output_dir)), overwrite_(overwrite)
    {
        fs::create_directories(output_dir_);
    }

    void PngFrameSink::write(const RuntimeState &state, const cv::Mat &frame_bgr)
    {
        const auto sar_dir = output_dir_ / state.sar_stem;
        fs::create_directories(sar_dir);
        char name[64] = {0};
        std::snprintf(name, sizeof(name), "patch_%06d.png", state.patch.index);
        const auto path = sar_dir / name;
        if (fs::exists(path) && !overwrite_)
        {
            return;
        }
        if (!cv::imwrite(path.string(), frame_bgr))
        {
            throw std::runtime_error("Failed to write PNG output: " + path.string());
        }
    }

    HdmiFrameSink::HdmiFrameSink(FPAIDevice &device, int width, int height, int fps)
        : display_(std::make_unique<infer_workflow::RGB565HDMIDisplay<FPAIDevice>>(0, device, width, height)),
          fps_(fps)
    {
    }

    HdmiFrameSink::~HdmiFrameSink() = default;

    void HdmiFrameSink::write(const RuntimeState &, const cv::Mat &frame_bgr)
    {
        cv::Mat rgb565;
        cv::cvtColor(frame_bgr, rgb565, cv::COLOR_BGR2BGR565);
        display_->show(reinterpret_cast<int8_t *>(rgb565.data));
    }

    void LatestSnapshotMailbox::publish(InferenceSnapshot &&snapshot)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_snapshot_ = std::move(snapshot);
        ++published_sequence_;
        cv_.notify_all();
    }

    bool LatestSnapshotMailbox::loadLatest(InferenceSnapshot &snapshot, std::uint64_t &sequence)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!latest_snapshot_.has_value())
        {
            return false;
        }
        snapshot = *latest_snapshot_;
        sequence = published_sequence_;
        return true;
    }

    LatestSnapshotMailbox::WakeReason LatestSnapshotMailbox::waitForChangeOrStop(std::uint64_t known_sequence,
                                                                                 std::chrono::microseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, timeout, [&] {
                return stop_requested_ || input_closed_ || published_sequence_ != known_sequence;
            }))
        {
            return WakeReason::Timeout;
        }
        if (stop_requested_)
        {
            return WakeReason::StopRequested;
        }
        if (published_sequence_ != known_sequence)
        {
            return WakeReason::NewSnapshot;
        }
        return WakeReason::InputClosed;
    }

    void LatestSnapshotMailbox::markInputClosed()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        input_closed_ = true;
        cv_.notify_all();
    }

    void LatestSnapshotMailbox::requestStop()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_requested_ = true;
        cv_.notify_all();
    }

    HdmiRenderWorker::HdmiRenderWorker(IFrameSink &sink,
                                       LatestSnapshotMailbox &mailbox,
                                       const UiRenderContext &placeholder_ui_context,
                                       int display_width,
                                       int display_height,
                                       int display_fps,
                                       std::mutex &device_access_mutex)
        : sink_(sink),
          mailbox_(mailbox),
          placeholder_ui_context_(placeholder_ui_context),
          display_width_(display_width),
          display_height_(display_height),
          display_interval_(display_fps > 0 ? std::chrono::microseconds(1000000 / display_fps)
                                            : std::chrono::microseconds(33333)),
          device_access_mutex_(device_access_mutex)
    {
    }

    void HdmiRenderWorker::start()
    {
        worker_ = std::thread(&HdmiRenderWorker::run, this);
    }

    void HdmiRenderWorker::requestStop()
    {
        mailbox_.requestStop();
    }

    void HdmiRenderWorker::join()
    {
        if (worker_.joinable())
        {
            worker_.join();
        }
        rethrowIfFailed();
    }

    void HdmiRenderWorker::rethrowIfFailed()
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        if (worker_error_ != nullptr)
        {
            std::rethrow_exception(worker_error_);
        }
    }

    void HdmiRenderWorker::run()
    {
        try
        {
            RuntimeState placeholder_state;
            placeholder_state.sar_stem = "WAITING";
            placeholder_state.patch.width = kExpectedW;
            placeholder_state.patch.height = kExpectedH;
            placeholder_state.stride = kExpectedW / 2;

            UiRenderContext waiting_ui = placeholder_ui_context_;
            waiting_ui.status_label = "WAITING";

            cv::Mat empty_restore(kExpectedH, kExpectedW, CV_8UC3, cv::Scalar(0, 0, 0));
            cv::Mat empty_mask(kExpectedH, kExpectedW, CV_8UC3, cv::Scalar(0, 0, 0));

            cv::Mat current_frame = composeIndustrialUiFrame(waiting_ui,
                                                             placeholder_state,
                                                             empty_restore,
                                                             empty_mask,
                                                             display_width_,
                                                             display_height_);
            RuntimeState current_state = placeholder_state;
            InferenceSnapshot current_snapshot;
            std::uint64_t current_sequence = 0;
            auto next_present_time = std::chrono::steady_clock::now();
            auto emitStoppedFrame = [&]() {
                UiRenderContext stopped_ui = current_snapshot.ui_context;
                RuntimeState stopped_state = current_state;
                cv::Mat stopped_restore = current_snapshot.restore_bgr;
                cv::Mat stopped_mask = current_snapshot.mask_bgr;

                if (stopped_ui.output_label.empty())
                {
                    stopped_ui = placeholder_ui_context_;
                }
                if (stopped_restore.empty())
                {
                    stopped_restore = empty_restore;
                }
                if (stopped_mask.empty())
                {
                    stopped_mask = empty_mask;
                }

                stopped_ui.status_label = "STOPPED";
                cv::Mat stopped_frame = composeIndustrialUiFrame(stopped_ui,
                                                                 stopped_state,
                                                                 stopped_restore,
                                                                 stopped_mask,
                                                                 display_width_,
                                                                 display_height_);
                writeFrame(stopped_state, stopped_frame);
            };

            while (true)
            {
                const auto now = std::chrono::steady_clock::now();
                if (now >= next_present_time)
                {
                    writeFrame(current_state, current_frame);
                    next_present_time += display_interval_;

                    const auto after_present = std::chrono::steady_clock::now();
                    while (next_present_time <= after_present)
                    {
                        next_present_time += display_interval_;
                    }
                }

                const auto wait_now = std::chrono::steady_clock::now();
                const auto wait_timeout = next_present_time > wait_now
                                              ? std::chrono::duration_cast<std::chrono::microseconds>(next_present_time - wait_now)
                                              : std::chrono::microseconds(0);
                const auto wake_reason = mailbox_.waitForChangeOrStop(current_sequence, wait_timeout);
                if (wake_reason == LatestSnapshotMailbox::WakeReason::StopRequested)
                {
                    emitStoppedFrame();
                    return;
                }

                InferenceSnapshot latest_snapshot;
                std::uint64_t latest_sequence = current_sequence;
                if (mailbox_.loadLatest(latest_snapshot, latest_sequence) && latest_sequence != current_sequence)
                {
                    current_snapshot = std::move(latest_snapshot);
                    current_sequence = latest_sequence;
                    current_frame = composeIndustrialUiFrame(current_snapshot.ui_context,
                                                             current_snapshot.state,
                                                             current_snapshot.restore_bgr,
                                                             current_snapshot.mask_bgr,
                                                             display_width_,
                                                             display_height_);
                    current_state = current_snapshot.state;
                    continue;
                }

                if (wake_reason == LatestSnapshotMailbox::WakeReason::InputClosed)
                {
                    emitStoppedFrame();
                    return;
                }
            }
        }
        catch (...)
        {
            {
                std::lock_guard<std::mutex> lock(error_mutex_);
                worker_error_ = std::current_exception();
            }
            mailbox_.requestStop();
        }
    }

    void HdmiRenderWorker::writeFrame(const RuntimeState &state, const cv::Mat &frame_bgr)
    {
        std::lock_guard<std::mutex> device_lock(device_access_mutex_);
        sink_.write(state, frame_bgr);
    }

    RuntimeState processPatchToPng(const PatchPacket &packet,
                                   const RuntimeState &base_state,
                                   const UiRenderContext &ui_context,
                                   PatchTensorBuilder &tensor_builder,
                                   PatchInferenceRunner &runner,
                                   IFrameSink &sink,
                                   const AppConfig &cfg,
                                   int &frame_counter)
    {
        RuntimeState state = base_state;
        state.patch = packet.info;
        const auto patch_start = std::chrono::steady_clock::now();

        Tensor input_tensor = tensor_builder.build(packet.patch_norm);
        const auto infer_start = std::chrono::steady_clock::now();
        auto host_outputs = runner.forward(input_tensor);
        const auto infer_end = std::chrono::steady_clock::now();

        cv::Mat restore_gray = restoreToGrayU8(host_outputs[0]);
        cv::Mat restore_bgr;
        cv::cvtColor(restore_gray, restore_bgr, cv::COLOR_GRAY2BGR);
        cv::Mat mask_bgr = logitsToMaskBgr(host_outputs[1]);

        state.infer_ms = std::chrono::duration<double, std::milli>(infer_end - infer_start).count();
        state.frame_index = ++frame_counter;
        state.total_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - patch_start).count();
        state.fps = state.total_ms > 0.0 ? (1000.0 / state.total_ms) : 0.0;
        cv::Mat frame_bgr = composeIndustrialUiFrame(ui_context, state, restore_bgr, mask_bgr, cfg.display_width, cfg.display_height);
        state.total_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - patch_start).count();
        state.fps = state.total_ms > 0.0 ? (1000.0 / state.total_ms) : 0.0;
        frame_bgr = composeIndustrialUiFrame(ui_context, state, restore_bgr, mask_bgr, cfg.display_width, cfg.display_height);
        sink.write(state, frame_bgr);

        spdlog::info("[{}/{}] {} patch #{}/{} frame={} row={} col={} x={} y={} infer={:.2f}ms total={:.2f}ms fps={:.1f}",
                     state.sar_index,
                     state.sar_count,
                     state.sar_stem,
                     state.patch.index + 1,
                     state.patch_count,
                     state.frame_index,
                     state.patch.grid_row,
                     state.patch.grid_col,
                     state.patch.x,
                     state.patch.y,
                     state.infer_ms,
                     state.total_ms,
                     state.fps);
        return state;
    }

    RuntimeState processPatchToHdmi(const PatchPacket &packet,
                                    const RuntimeState &base_state,
                                    const UiRenderContext &ui_context,
                                    PatchTensorBuilder &tensor_builder,
                                    PatchInferenceRunner &runner,
                                    std::mutex &device_access_mutex,
                                    LatestSnapshotMailbox &mailbox,
                                    HdmiRenderWorker &render_worker,
                                    int &frame_counter)
    {
        render_worker.rethrowIfFailed();

        RuntimeState state = base_state;
        state.patch = packet.info;
        const auto patch_start = std::chrono::steady_clock::now();

        Tensor input_tensor = tensor_builder.build(packet.patch_norm);
        const auto infer_start = std::chrono::steady_clock::now();
        std::vector<Tensor> host_outputs;
        {
            std::lock_guard<std::mutex> device_lock(device_access_mutex);
            host_outputs = runner.forward(input_tensor);
        }
        const auto infer_end = std::chrono::steady_clock::now();

        cv::Mat restore_gray = restoreToGrayU8(host_outputs[0]);
        cv::Mat restore_bgr;
        cv::cvtColor(restore_gray, restore_bgr, cv::COLOR_GRAY2BGR);
        cv::Mat mask_bgr = logitsToMaskBgr(host_outputs[1]);

        state.infer_ms = std::chrono::duration<double, std::milli>(infer_end - infer_start).count();

        const int next_frame_index = frame_counter + 1;
        state.frame_index = next_frame_index;
        state.total_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - patch_start).count();
        state.fps = state.total_ms > 0.0 ? (1000.0 / state.total_ms) : 0.0;

        InferenceSnapshot snapshot;
        snapshot.state = state;
        snapshot.ui_context = ui_context;
        snapshot.restore_bgr = std::move(restore_bgr);
        snapshot.mask_bgr = std::move(mask_bgr);
        mailbox.publish(std::move(snapshot));

        frame_counter = next_frame_index;
        render_worker.rethrowIfFailed();

        spdlog::info("[{}/{}] {} patch #{}/{} frame={} row={} col={} x={} y={} infer={:.2f}ms total={:.2f}ms fps={:.1f} [snapshot->hdmi]",
                     state.sar_index,
                     state.sar_count,
                     state.sar_stem,
                     state.patch.index + 1,
                     state.patch_count,
                     state.frame_index,
                     state.patch.grid_row,
                     state.patch.grid_col,
                     state.patch.x,
                     state.patch.y,
                     state.infer_ms,
                     state.total_ms,
                     state.fps);
        return state;
    }
}
