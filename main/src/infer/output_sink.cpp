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
        // 把 shape 向量格式化成带逗号分隔的文本，供报错打印。
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

        // 把一块 host 浮点数据封装成与模型输入定义一致的 Tensor。
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

    // 根据模型输入定义校验 shape / dtype，并缓存输入描述。
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

    // 把 `CV_32FC1 512x512` patch 转成一份可直接 forward 的 FP32 tensor。
    Tensor PatchTensorBuilder::build(const cv::Mat &patch_norm) const
    {
        if (patch_norm.empty() || patch_norm.type() != CV_32FC1 || patch_norm.rows != kExpectedH || patch_norm.cols != kExpectedW)
        {
            throw std::runtime_error("Patch tensor input must be CV_32FC1 512x512 normalized to 0-1.");
        }
        cv::Mat continuous = patch_norm.isContinuous() ? patch_norm : patch_norm.clone();
        return dataToFp32Tensor(reinterpret_cast<const float *>(continuous.data), input_value_);
    }

    // 保存 session、device 和输出等待超时。
    PatchInferenceRunner::PatchInferenceRunner(Session &session, Device &device, int output_wait_ms)
        : session_(session), device_(device), output_wait_ms_(output_wait_ms)
    {
    }

    // 执行一次 forward，把所有输出等到 ready 后统一拷回 host。
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

    // 把 restore 输出张量量化成 8 位灰度图。
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

    // 对每个像素取 argmax，把 6 类 logits 渲染成彩色 mask。
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

    // 把每个像素的 argmax 类别写成单通道类别图，供 debug_raster 落盘分析。
    cv::Mat logitsToMaskClassU8(const Tensor &tensor)
    {
        const auto *data = reinterpret_cast<const float *>(tensor.data().cptr());
        cv::Mat mask_class(kExpectedH, kExpectedW, CV_8UC1);
        for (int y = 0; y < kExpectedH; ++y)
        {
            auto *row = mask_class.ptr<std::uint8_t>(y);
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
                row[x] = static_cast<std::uint8_t>(best_cls + 1);
            }
        }
        return mask_class;
    }

    // 初始化 PNG 输出根目录。
    PngFrameSink::PngFrameSink(std::filesystem::path output_dir, bool overwrite)
        : output_dir_(std::move(output_dir)), overwrite_(overwrite)
    {
        fs::create_directories(output_dir_);
    }

    // 把最终 UI 帧写到 `<output_dir>/<sar_stem>/patch_xxxxxx.png`。
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

    // 在 debug_raster 模式下额外保存恢复图和类别图。
    void PngFrameSink::writeDebugPatchOutputs(const RuntimeState &state,
                                              const cv::Mat &restore_gray,
                                              const cv::Mat &mask_class)
    {
        const auto debug_dir = output_dir_ / ("debug_" + state.sar_stem);
        const auto restore_dir = debug_dir / "restore";
        const auto mask_dir = debug_dir / "mask_class";
        fs::create_directories(restore_dir);
        fs::create_directories(mask_dir);

        char name[64] = {0};
        std::snprintf(name, sizeof(name), "patch_%06d.png", state.patch.index);
        const auto restore_path = restore_dir / name;
        const auto mask_path = mask_dir / name;

        if ((fs::exists(restore_path) || fs::exists(mask_path)) && !overwrite_)
        {
            return;
        }
        if (!cv::imwrite(restore_path.string(), restore_gray))
        {
            throw std::runtime_error("Failed to write PNG output: " + restore_path.string());
        }
        if (!cv::imwrite(mask_path.string(), mask_class))
        {
            throw std::runtime_error("Failed to write PNG output: " + mask_path.string());
        }
    }

    // 创建底层 RGB565 HDMI 显示适配器。
    HdmiFrameSink::HdmiFrameSink(FPAIDevice &device, int width, int height, int fps)
        : display_(std::make_unique<infer_workflow::RGB565HDMIDisplay<FPAIDevice>>(0, device, width, height)),
          fps_(fps)
    {
    }

    // 默认析构即可。
    HdmiFrameSink::~HdmiFrameSink() = default;

    // 把 BGR 帧转成 RGB565 后交给底层 HDMI 适配器显示。
    void HdmiFrameSink::write(const RuntimeState &, const cv::Mat &frame_bgr)
    {
        cv::Mat rgb565;
        cv::cvtColor(frame_bgr, rgb565, cv::COLOR_BGR2BGR565);
        display_->show(reinterpret_cast<int8_t *>(rgb565.data));
    }

    // 覆盖式发布一份最新推理快照。
    void LatestSnapshotMailbox::publish(InferenceSnapshot &&snapshot)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_snapshot_ = std::move(snapshot);
        ++published_sequence_;
        cv_.notify_all();
    }

    // 读取当前最新快照及其发布序号。
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

    // 等待“新快照 / 输入关闭 / stop 请求”中的任一事件。
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

    // 标记输入结束，让渲染线程输出停止帧后退出。
    void LatestSnapshotMailbox::markInputClosed()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        input_closed_ = true;
        cv_.notify_all();
    }

    // 请求渲染线程尽快停止。
    void LatestSnapshotMailbox::requestStop()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_requested_ = true;
        cv_.notify_all();
    }

    // 保存 sink、邮箱和显示参数；真正的线程在 start() 之后启动。
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

    // 启动 HDMI 渲染线程。
    void HdmiRenderWorker::start()
    {
        worker_ = std::thread(&HdmiRenderWorker::run, this);
    }

    // 请求渲染线程停止。
    void HdmiRenderWorker::requestStop()
    {
        mailbox_.requestStop();
    }

    // 等待线程退出，并在必要时重抛线程内部异常。
    void HdmiRenderWorker::join()
    {
        if (worker_.joinable())
        {
            worker_.join();
        }
        rethrowIfFailed();
    }

    // 如果渲染线程失败，则把异常重新抛回调用线程。
    void HdmiRenderWorker::rethrowIfFailed()
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        if (worker_error_ != nullptr)
        {
            std::rethrow_exception(worker_error_);
        }
    }

    // 渲染线程主循环。
    // 它会持续刷新当前帧，并在 stop / input_closed 时输出一帧 STOPPED 画面再退出。
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

    // 在设备互斥保护下写出一帧，避免与 NPU/device 访问并发冲突。
    void HdmiRenderWorker::writeFrame(const RuntimeState &state, const cv::Mat &frame_bgr)
    {
        std::lock_guard<std::mutex> device_lock(device_access_mutex_);
        sink_.write(state, frame_bgr);
    }

    // PNG 路径下处理一个 patch：
    // forward -> 后处理 -> UI 合成 -> 落盘，并返回更新后的 RuntimeState。
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
        cv::Mat mask_bgr = logitsToMaskBgr(host_outputs[1]);

        state.infer_ms = std::chrono::duration<double, std::milli>(infer_end - infer_start).count();
        state.frame_index = ++frame_counter;
        state.total_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - patch_start).count();
        state.fps = state.total_ms > 0.0 ? (1000.0 / state.total_ms) : 0.0;
        if (cfg.patch_mode == "debug_raster")
        {
            auto *png_sink = dynamic_cast<PngFrameSink *>(&sink);
            if (png_sink == nullptr)
            {
                throw std::runtime_error("debug_raster requires PNG frame sink.");
            }
            cv::Mat mask_class = logitsToMaskClassU8(host_outputs[1]);
            png_sink->writeDebugPatchOutputs(state, restore_gray, mask_class);
        }
        else
        {
            cv::Mat restore_bgr;
            cv::cvtColor(restore_gray, restore_bgr, cv::COLOR_GRAY2BGR);
            cv::Mat frame_bgr = composeIndustrialUiFrame(ui_context, state, restore_bgr, mask_bgr, cfg.display_width, cfg.display_height);
            state.total_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - patch_start).count();
            state.fps = state.total_ms > 0.0 ? (1000.0 / state.total_ms) : 0.0;
            frame_bgr = composeIndustrialUiFrame(ui_context, state, restore_bgr, mask_bgr, cfg.display_width, cfg.display_height);
            sink.write(state, frame_bgr);
        }

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

    // HDMI 路径下处理一个 patch：
    // forward -> 后处理 -> 发布最新快照，由渲染线程异步刷新显示。
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
