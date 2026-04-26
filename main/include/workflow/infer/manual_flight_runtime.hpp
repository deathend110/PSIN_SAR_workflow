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
    class ManualFlightRuntimeState
    {
    public:
        ManualFlightRuntimeState();

        void setConfiguration(const ManualFlightSettings &settings, bool configured);
        void configure(const ManualFlightSettings &settings);
        void reset();
        void activate(int image_width, int image_height, int patch_size, int stride);
        void requestStop();
        void setPaused(bool paused);
        bool submitDirectionKey(const std::string &key, bool pressed, std::string *message = nullptr);
        bool waitNextCenter(const std::function<bool()> &should_stop, cv::Point &center);
        void markInferenceCommitted(const cv::Point &center);
        ManualFlightTelemetry telemetry() const;
        std::vector<cv::Point> pathPoints() const;

    private:
        void resetLocked();
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
