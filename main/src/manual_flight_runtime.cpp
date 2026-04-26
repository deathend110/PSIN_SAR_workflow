#include "workflow/infer/manual_flight_runtime.hpp"

#include "workflow/shared/config_utils.hpp"

#include <algorithm>
#include <chrono>

namespace workflow::infer
{
    namespace
    {
        cv::Point ClampManualCenter(const cv::Point &point, int image_width, int image_height, int patch_size)
        {
            const int half = patch_size / 2;
            const int x = std::max(half, std::min(point.x, image_width - half));
            const int y = std::max(half, std::min(point.y, image_height - half));
            return cv::Point(x, y);
        }

        bool IsManualDirection(const std::string &value)
        {
            return value == "up" || value == "left" || value == "down" || value == "right";
        }

        std::string NormalizeManualDirectionKey(const std::string &key)
        {
            const std::string lowered = shared::ToLower(shared::Trim(key));
            if (lowered == "w")
            {
                return "up";
            }
            if (lowered == "a")
            {
                return "left";
            }
            if (lowered == "s")
            {
                return "down";
            }
            if (lowered == "d")
            {
                return "right";
            }
            return {};
        }

        cv::Point ManualDirectionDelta(const std::string &direction)
        {
            if (direction == "up")
            {
                return cv::Point(0, -1);
            }
            if (direction == "left")
            {
                return cv::Point(-1, 0);
            }
            if (direction == "down")
            {
                return cv::Point(0, 1);
            }
            return cv::Point(1, 0);
        }

        void TrimManualPath(std::vector<cv::Point> &path_points)
        {
            constexpr size_t kMaxPathPoints = 256;
            if (path_points.size() > kMaxPathPoints)
            {
                path_points.erase(path_points.begin(), path_points.end() - static_cast<std::ptrdiff_t>(kMaxPathPoints));
            }
        }
    }

    ManualFlightRuntimeState::ManualFlightRuntimeState()
    {
        resetLocked();
    }

    void ManualFlightRuntimeState::setConfiguration(const ManualFlightSettings &settings, bool configured)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        settings_ = settings;
        configured_ = configured;
    }

    void ManualFlightRuntimeState::configure(const ManualFlightSettings &settings)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            settings_ = settings;
            configured_ = true;
        }
        cv_.notify_all();
    }

    void ManualFlightRuntimeState::reset()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            resetLocked();
        }
        cv_.notify_all();
    }

    void ManualFlightRuntimeState::activate(int image_width, int image_height, int patch_size, int stride)
    {
        const cv::Point initial_center = ClampManualCenter(cv::Point(patch_size / 2, patch_size / 2),
                                                           image_width,
                                                           image_height,
                                                           patch_size);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_requested_ = false;
            active_ = true;
            paused_ = false;
            image_width_ = image_width;
            image_height_ = image_height;
            patch_size_ = patch_size;
            stride_ = std::max(1, stride);
            current_center_ = initial_center;
            requested_center_ = initial_center;
            last_inferred_center_ = initial_center;
            request_sequence_ = 1;
            consumed_sequence_ = 0;
            edge_blocked_ = false;
            patch_count_ = 0;
            current_direction_ = "right";
            pending_direction_ = "right";
            path_points_.clear();
            path_points_.push_back(initial_center);
        }
        cv_.notify_all();
    }

    void ManualFlightRuntimeState::requestStop()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_requested_ = true;
        }
        cv_.notify_all();
    }

    void ManualFlightRuntimeState::setPaused(bool paused)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            paused_ = paused;
            if (!paused_ && active_ && request_sequence_ <= consumed_sequence_)
            {
                queueNextManualPatchLocked();
            }
        }
        cv_.notify_all();
    }

    bool ManualFlightRuntimeState::submitDirectionKey(const std::string &key, bool pressed, std::string *message)
    {
        const std::string direction = NormalizeManualDirectionKey(key);

        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_)
        {
            if (message != nullptr)
            {
                *message = "manual_flight is not active.";
            }
            return false;
        }

        if (!pressed)
        {
            if (message != nullptr)
            {
                *message = "manual direction release ignored in cursor mode.";
            }
            return true;
        }

        if (!IsManualDirection(direction))
        {
            if (message != nullptr)
            {
                *message = "manual_flight only accepts W/A/S/D direction changes.";
            }
            return false;
        }

        if (pending_direction_ == direction)
        {
            if (message != nullptr)
            {
                *message = "manual direction unchanged.";
            }
            return true;
        }

        pending_direction_ = direction;

        if (edge_blocked_ && !paused_ && request_sequence_ <= consumed_sequence_)
        {
            queueNextManualPatchLocked();
        }

        if (message != nullptr)
        {
            *message = "manual direction set to " + direction + ".";
        }
        cv_.notify_all();
        return true;
    }

    bool ManualFlightRuntimeState::waitNextCenter(const std::function<bool()> &should_stop, cv::Point &center)
    {
        for (;;)
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(40), [&] {
                return stop_requested_ ||
                       request_sequence_ > consumed_sequence_ ||
                       (should_stop && should_stop());
            });

            if (stop_requested_ || (should_stop && should_stop()))
            {
                return false;
            }
            if (request_sequence_ <= consumed_sequence_)
            {
                continue;
            }

            center = requested_center_;
            consumed_sequence_ = request_sequence_;
            return true;
        }
    }

    void ManualFlightRuntimeState::markInferenceCommitted(const cv::Point &center)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            current_center_ = center;
            last_inferred_center_ = center;
            ++patch_count_;
            if (stop_requested_ || !active_ || paused_)
            {
                return;
            }

            queueNextManualPatchLocked();
        }
        cv_.notify_all();
    }

    ManualFlightTelemetry ManualFlightRuntimeState::telemetry() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ManualFlightTelemetry telemetry;
        telemetry.configured = configured_;
        telemetry.active = active_;
        telemetry.paused = paused_;
        telemetry.path_overlay = settings_.path_overlay;
        telemetry.edge_blocked = edge_blocked_;
        telemetry.position_x = current_center_.x;
        telemetry.position_y = current_center_.y;
        telemetry.last_inferred_center_x = last_inferred_center_.x;
        telemetry.last_inferred_center_y = last_inferred_center_.y;
        telemetry.path_points = static_cast<int>(path_points_.size());
        telemetry.patch_count = patch_count_;
        telemetry.current_direction = current_direction_;
        telemetry.pending_direction = pending_direction_;
        return telemetry;
    }

    std::vector<cv::Point> ManualFlightRuntimeState::pathPoints() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return path_points_;
    }

    void ManualFlightRuntimeState::resetLocked()
    {
        stop_requested_ = false;
        active_ = false;
        paused_ = false;
        image_width_ = 0;
        image_height_ = 0;
        patch_size_ = kExpectedH;
        stride_ = 256;
        current_center_ = cv::Point(kExpectedH / 2, kExpectedH / 2);
        requested_center_ = current_center_;
        last_inferred_center_ = current_center_;
        request_sequence_ = 0;
        consumed_sequence_ = 0;
        edge_blocked_ = false;
        patch_count_ = 0;
        current_direction_ = "right";
        pending_direction_ = "right";
        path_points_.clear();
    }

    bool ManualFlightRuntimeState::queueNextManualPatchLocked()
    {
        current_direction_ = pending_direction_;
        const cv::Point delta = ManualDirectionDelta(current_direction_);
        const cv::Point next_center = ClampManualCenter(cv::Point(current_center_.x + delta.x * std::max(1, stride_),
                                                                  current_center_.y + delta.y * std::max(1, stride_)),
                                                        image_width_,
                                                        image_height_,
                                                        patch_size_);
        if (next_center == current_center_)
        {
            edge_blocked_ = true;
            return false;
        }

        edge_blocked_ = false;
        current_center_ = next_center;
        requested_center_ = next_center;
        path_points_.push_back(next_center);
        TrimManualPath(path_points_);
        ++request_sequence_;
        return true;
    }
}
