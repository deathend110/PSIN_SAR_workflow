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
    // 活动 manual runtime 的前置声明，供 UI/编排层按需读取。
    class ManualFlightRuntimeState;

    // 描述一个 patch 在原图中的几何位置和编号信息。
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

    // 推理流程在 patch 级别维护的运行态快照。
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

    // 小地图渲染所需的上下文，包括原图预览和路径叠加数据。
    struct MiniMapContext
    {
        cv::Mat sar_preview_bgr;
        int source_width = 0;
        int source_height = 0;
        int patch_size = 0;
        bool path_overlay = false;
        std::vector<cv::Point> path_points;
    };

    // 工业 UI 合成所需的标签和小地图上下文。
    struct UiRenderContext
    {
        std::string status_label = "RUNNING";
        std::string mode_label = "INFERENCE ONLY";
        std::string output_label;
        std::string restore_label = "GRAY OUTPUT";
        std::string seg_label = "RGB MASK / 6 CLASS";
        MiniMapContext mini_map;
    };

    // HDMI 路径中通过邮箱线程间传递的一帧完整快照。
    struct InferenceSnapshot
    {
        RuntimeState state;
        UiRenderContext ui_context;
        cv::Mat restore_bgr;
        cv::Mat mask_bgr;
    };

    // patch 规划器产出的一个输入单元：几何信息 + 归一化图像块。
    struct PatchPacket
    {
        PatchInfo info;
        cv::Mat patch_norm;
    };

    // 把类别 id 映射到固定的 BGR 可视化颜色。
    cv::Vec3b classColorBgr(int cls);

    // 返回当前活动的 manual runtime；若没有活动实例则返回空指针。
    std::shared_ptr<ManualFlightRuntimeState> activeManualFlightRuntimeState();

    // 基于整幅 SAR 图构造小地图上下文。
    MiniMapContext buildMiniMapContext(const cv::Mat &sar_norm, int patch_size, int stride, int rows, int cols);

    // 把 manual runtime 的运行态注入到 RuntimeState 和 UI 上下文中。
    void applyManualTelemetry(RuntimeState &state, UiRenderContext &ui_context);

    // 把 `CV_32FC1 512x512` patch 打包成模型输入 tensor。
    class PatchTensorBuilder
    {
    public:
        // 根据网络输入定义校验 shape / dtype，并缓存输入描述。
        explicit PatchTensorBuilder(const icraft::xir::NetworkView &network_view);

        // 构建一份可直接送入 Session::forward 的输入 tensor。
        icraft::xrt::Tensor build(const cv::Mat &patch_norm) const;

    private:
        icraft::xir::Value input_value_;
    };

    // 对一次 patch 推理所需的 session / device 操作做轻量封装。
    class PatchInferenceRunner
    {
    public:
        // 保存 session、device 引用以及输出等待超时。
        PatchInferenceRunner(icraft::xrt::Session &session, icraft::xrt::Device &device, int output_wait_ms);

        // 执行一次 forward，并把输出拷回 host 内存。
        std::vector<icraft::xrt::Tensor> forward(const icraft::xrt::Tensor &input_tensor);

    private:
        icraft::xrt::Session &session_;
        icraft::xrt::Device &device_;
        int output_wait_ms_ = 20000;
    };

    // 把模型 restore 输出转成 8 位灰度图。
    cv::Mat restoreToGrayU8(const icraft::xrt::Tensor &tensor);
    // 把分割 logits 转成彩色 mask。
    cv::Mat logitsToMaskBgr(const icraft::xrt::Tensor &tensor);

    // 输出目标抽象；当前具体实现为 PNG 或 HDMI。
    class IFrameSink
    {
    public:
        // 多态基类析构函数。
        virtual ~IFrameSink() = default;
        // 写出一帧最终 UI 图像。
        virtual void write(const RuntimeState &state, const cv::Mat &frame_bgr) = 0;
    };

    // 负责把推理结果落盘成 patch PNG。
    class PngFrameSink : public IFrameSink
    {
    public:
        // 初始化输出目录和覆盖策略。
        PngFrameSink(std::filesystem::path output_dir, bool overwrite);

        // 把一帧完整 UI 图写到 `<sar_stem>/patch_xxxxxx.png`。
        void write(const RuntimeState &state, const cv::Mat &frame_bgr) override;
        // 在 debug_raster 模式下额外写出恢复图和类别图。
        void writeDebugPatchOutputs(const RuntimeState &state,
                                    const cv::Mat &restore_gray,
                                    const cv::Mat &mask_class);

    private:
        std::filesystem::path output_dir_;
        bool overwrite_ = true;
    };

    // 负责把最终帧转换成 RGB565 并送到 HDMI 设备。
    class HdmiFrameSink : public IFrameSink
    {
    public:
        // 绑定设备并创建底层显示适配器。
        HdmiFrameSink(icraft::xrt::ZG330Device &device, int width, int height, int fps);
        // 默认析构即可，但单独声明便于在实现文件中控制依赖。
        ~HdmiFrameSink();

        // 把一帧 BGR 图转换为 RGB565 后显示。
        void write(const RuntimeState &state, const cv::Mat &frame_bgr) override;

    private:
        std::unique_ptr<infer_workflow::RGB565HDMIDisplay<icraft::xrt::ZG330Device>> display_;
        int fps_ = 0;
    };

    // HDMI 路径中的“最新帧邮箱”。
    // 推理线程持续覆盖最新快照，渲染线程只消费最新值，不回放历史帧。
    class LatestSnapshotMailbox
    {
    public:
        // 渲染线程从等待中被唤醒的原因。
        enum class WakeReason
        {
            Timeout,
            NewSnapshot,
            InputClosed,
            StopRequested,
        };

        // 发布一份新的最新快照。
        void publish(InferenceSnapshot &&snapshot);
        // 读取当前最新快照及其序号；若还没有快照则返回 false。
        bool loadLatest(InferenceSnapshot &snapshot, std::uint64_t &sequence);
        // 等待“有更新 / 输入关闭 / 收到 stop”中的任一事件。
        WakeReason waitForChangeOrStop(std::uint64_t known_sequence, std::chrono::microseconds timeout);
        // 标记输入结束，通知渲染线程输出停止帧后退出。
        void markInputClosed();
        // 请求渲染线程尽快退出。
        void requestStop();

    private:
        std::mutex mutex_;
        std::condition_variable cv_;
        std::optional<InferenceSnapshot> latest_snapshot_;
        std::uint64_t published_sequence_ = 0;
        bool input_closed_ = false;
        bool stop_requested_ = false;
    };

    // HDMI 渲染线程 worker。
    // 它周期性刷新显示，并从邮箱中消费最新快照来更新画面。
    class HdmiRenderWorker
    {
    public:
        // 保存 sink、邮箱和显示参数；真正线程在 start() 后启动。
        HdmiRenderWorker(IFrameSink &sink,
                         LatestSnapshotMailbox &mailbox,
                         const UiRenderContext &placeholder_ui_context,
                         int display_width,
                         int display_height,
                         int display_fps,
                         std::mutex &device_access_mutex);

        // 启动渲染线程。
        void start();
        // 请求渲染线程停止。
        void requestStop();
        // 等待渲染线程退出，并在必要时重抛线程内异常。
        void join();
        // 若渲染线程内部失败，则把异常重新抛到调用线程。
        void rethrowIfFailed();

    private:
        // 线程主循环：刷新当前帧、等待邮箱变化、处理停止帧。
        void run();
        // 在设备访问互斥保护下写出一帧。
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

    // 在 PNG 输出模式下处理一个 patch：推理、后处理、UI 合成并落盘。
    RuntimeState processPatchToPng(const PatchPacket &packet,
                                   const RuntimeState &base_state,
                                   const UiRenderContext &ui_context,
                                   PatchTensorBuilder &tensor_builder,
                                   PatchInferenceRunner &runner,
                                   IFrameSink &sink,
                                   const AppConfig &cfg,
                                   int &frame_counter);

    // 在 HDMI 输出模式下处理一个 patch：推理后把最新快照投递给渲染线程。
    RuntimeState processPatchToHdmi(const PatchPacket &packet,
                                    const RuntimeState &base_state,
                                    const UiRenderContext &ui_context,
                                    PatchTensorBuilder &tensor_builder,
                                    PatchInferenceRunner &runner,
                                    std::mutex &device_access_mutex,
                                    LatestSnapshotMailbox &mailbox,
                                    HdmiRenderWorker &render_worker,
                                    int &frame_counter);

    // 蛇形扫描 patch 规划器：偶数行从左到右，奇数行从右到左。
    class SnakePatchSource
    {
    public:
        // 根据图像尺寸、patch 大小和 stride 初始化扫描网格。
        SnakePatchSource(cv::Mat image_norm, int patch_size, int stride);

        // 生成下一个 patch；若耗尽则返回 false。
        bool next(PatchPacket &packet);

        // 返回总 patch 数。
        int totalPatches() const;
        // 返回扫描网格的行数。
        int rows() const;
        // 返回扫描网格的列数。
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

    // 调试用规则栅格 patch 规划器：按固定 X/Y stride 行优先扫描。
    class DebugRasterPatchSource
    {
    public:
        // 根据独立的 X/Y 步长初始化调试网格。
        DebugRasterPatchSource(cv::Mat image_norm, int patch_size, int stride_x_px, int stride_y_px);

        // 生成下一个调试 patch；若耗尽则返回 false。
        bool next(PatchPacket &packet);

        // 返回总 patch 数。
        int totalPatches() const;
        // 返回调试网格行数。
        int rows() const;
        // 返回调试网格列数。
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

    // 把恢复图、mask、小地图和状态标签合成为最终工业 UI 画面。
    cv::Mat composeIndustrialUiFrame(const UiRenderContext &ui_context,
                                     const RuntimeState &state,
                                     const cv::Mat &restore_bgr,
                                     const cv::Mat &mask_bgr,
                                     int width,
                                     int height);
}
