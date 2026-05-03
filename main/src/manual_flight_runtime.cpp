#include "workflow/infer/manual_flight_runtime.hpp"

#include "workflow/shared/config_utils.hpp"

#include <algorithm>
#include <chrono>

namespace workflow::infer
{
    namespace
    {
        // 把 manual 中心点裁剪到“仍能完整取出一个 patch”的合法范围内。
        cv::Point ClampManualCenter(const cv::Point &point, int image_width, int image_height, int patch_size)
        {
            const int half = patch_size / 2;
            const int x = std::max(half, std::min(point.x, image_width - half));
            const int y = std::max(half, std::min(point.y, image_height - half));
            return cv::Point(x, y);
        }

        // 判断一个字符串是否已经是内部使用的标准方向名。
        bool IsManualDirection(const std::string &value)
        {
            return value == "up" || value == "left" || value == "down" || value == "right";
        }

        // 把前端传来的 W/A/S/D 输入归一化为 up/left/down/right。
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

        // 把方向名映射成单位步长位移向量。
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

        // 限制路径历史长度，避免长时间运行后 UI 轨迹无限膨胀。
        void TrimManualPath(std::vector<cv::Point> &path_points)
        {
            constexpr size_t kMaxPathPoints = 256;
            if (path_points.size() > kMaxPathPoints)
            {
                path_points.erase(path_points.begin(), path_points.end() - static_cast<std::ptrdiff_t>(kMaxPathPoints));
            }
        }
    }

    // 构造空闲态 runtime，并用统一初始值填充内部状态。
    ManualFlightRuntimeState::ManualFlightRuntimeState()
    {
        resetLocked();
    }

    // 仅同步配置快照，不改变 active / position 等运行态。
    void ManualFlightRuntimeState::setConfiguration(const ManualFlightSettings &settings, bool configured)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        settings_ = settings;
        configured_ = configured;
    }

    // 应用新的配置，并通知等待中的调用方重新读取状态。
    void ManualFlightRuntimeState::configure(const ManualFlightSettings &settings)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            settings_ = settings;
            configured_ = true;
        }
        cv_.notify_all();
    }

    // 把 runtime 重置回空闲初始状态。
    void ManualFlightRuntimeState::reset()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            resetLocked();
        }
        cv_.notify_all();
    }

    // 用给定图像尺寸和 patch 约束激活 runtime。
    // 起点会放在左上角第一个合法 patch 中心，而不是固定写死到 (0, 0)。
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

    // 请求停止 runtime，并唤醒所有 waitNextCenter 调用。
    void ManualFlightRuntimeState::requestStop()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_requested_ = true;
        }
        cv_.notify_all();
    }

    // 切换暂停态；从暂停恢复时，若当前没有待处理请求，会补发下一次 patch 请求。
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

    // 处理来自 Web/UI 的方向键输入。
    // 当前语义是“方向切换”，不是“按住持续移动”；keyup 只会被忽略。
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

    // 等待下一次可推理的中心点。
    // 返回 false 表示外部 stop 或上层 should_stop 条件成立。
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

    // 告知 runtime 当前 patch 已经真正完成推理。
    // 这一步既更新统计，也会决定是否排队下一个 patch。
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

    // 导出一份对外可见的 manual telemetry。
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

    // 返回路径点历史的副本，供小地图叠加轨迹。
    std::vector<cv::Point> ManualFlightRuntimeState::pathPoints() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return path_points_;
    }

    // 在持锁上下文中把内部状态重置为统一初始值。
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

    // 在持锁上下文中排队“下一次要推理的 patch”。
    // 这里采用 latest-wins 语义：始终按当前 pending_direction 计算下一步，而不回放历史方向。
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
