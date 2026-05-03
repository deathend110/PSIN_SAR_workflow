#pragma once

#include "workflow/infer/infer_workflow.hpp"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

namespace workflow::infer
{
    // `manual_flight` 的核心状态机。
    // 它负责保存当前游标中心、方向切换、暂停状态，以及“下一次 patch 请求”的调度关系。
    class ManualFlightRuntimeState
    {
    public:
        // 构造一个空闲态 runtime；默认方向为 right，尚未激活。
        ManualFlightRuntimeState();

        // 同步外部配置快照，但不改变 active / position 等运行状态。
        void setConfiguration(const ManualFlightSettings &settings, bool configured);
        // 应用新的 manual 配置，并唤醒等待线程。
        void configure(const ManualFlightSettings &settings);
        // 把 runtime 还原为空闲初始状态。
        void reset();
        // 基于图像尺寸和 patch 约束激活 runtime，并计算第一个合法中心点。
        void activate(int image_width, int image_height, int patch_size, int stride);
        // 请求 runtime 停止，唤醒所有等待中的调用方。
        void requestStop();
        // 切换暂停状态；恢复时如果需要会继续排队下一个 patch。
        void setPaused(bool paused);
        // 提交一条方向键输入；当前仅接受 W/A/S/D 的“按下”语义。
        bool submitDirectionKey(const std::string &key, bool pressed, std::string *message = nullptr);
        // 阻塞等待下一个合法中心点；若收到 stop 则返回 false。
        bool waitNextCenter(const std::function<bool()> &should_stop, cv::Point &center);
        // 告知 runtime 当前 patch 已推理完成，并据此推进到下一个 patch。
        void markInferenceCommitted(const cv::Point &center);
        // 导出可供 UI/HTTP 使用的遥测快照。
        ManualFlightTelemetry telemetry() const;
        // 导出路径点副本，用于小地图轨迹叠加。
        std::vector<cv::Point> pathPoints() const;

    private:
        // 在持锁上下文中把所有运行态字段恢复到初始值。
        void resetLocked();
        // 在持锁上下文中按当前方向排队下一个 patch；撞边时返回 false。
        bool queueNextManualPatchLocked();

        mutable std::mutex mutex_;
        std::condition_variable cv_;
        ManualFlightSettings settings_;
        bool configured_ = false;
        bool active_ = false;
        bool paused_ = false;
        bool stop_requested_ = false;
        int image_width_ = 0;
        int image_height_ = 0;
        int patch_size_ = kExpectedH;
        int stride_ = 256;
        cv::Point current_center_{kExpectedH / 2, kExpectedH / 2};
        cv::Point requested_center_{kExpectedH / 2, kExpectedH / 2};
        cv::Point last_inferred_center_{kExpectedH / 2, kExpectedH / 2};
        std::uint64_t request_sequence_ = 0;
        std::uint64_t consumed_sequence_ = 0;
        bool edge_blocked_ = false;
        int patch_count_ = 0;
        std::string current_direction_ = "right";
        std::string pending_direction_ = "right";
        std::vector<cv::Point> path_points_;
    };
}
