#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>

namespace workflow::shared
{
    enum class ControlState
    {
        Idle,
        Starting,
        Running,
        Paused,
        Stopping,
        Finished,
        Error,
    };

    enum class SelectedWorkflow
    {
        RdOnly,
        InferOnly,
    };

    enum class SelectedPatchMode
    {
        AutoSnake,
        ManualFlight,
    };

    struct WorkflowSelection
    {
        SelectedWorkflow workflow = SelectedWorkflow::InferOnly;
        SelectedPatchMode patch_mode = SelectedPatchMode::AutoSnake;
        std::string output_mode = "hdmi";
        std::string selected_source;
    };

    struct WorkflowRuntimeSnapshot
    {
        ControlState state = ControlState::Idle;
        WorkflowSelection selection;
        std::string current_stage;
        std::string current_item;
        std::string last_error;
        int current_index = 0;
        int total_count = 0;
        double infer_ms = 0.0;
        double total_ms = 0.0;
        double fps = 0.0;
    };

    class WorkflowRunControl
    {
    public:
        using SnapshotCallback = std::function<void(const WorkflowRuntimeSnapshot &)>;

        void requestPause()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pause_requested_ = true;
        }

        void requestResume()
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                pause_requested_ = false;
            }
            pause_cv_.notify_all();
        }

        void requestStop()
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stop_requested_ = true;
                pause_requested_ = false;
            }
            pause_cv_.notify_all();
        }

        void requestReset()
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                reset_requested_ = true;
                stop_requested_ = true;
                pause_requested_ = false;
            }
            pause_cv_.notify_all();
        }

        void waitIfPaused()
        {
            std::unique_lock<std::mutex> lock(mutex_);
            pause_cv_.wait(lock, [&] {
                return !pause_requested_ || stop_requested_;
            });
        }

        bool shouldStop() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return stop_requested_;
        }

        bool shouldReset() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return reset_requested_;
        }

        bool isPauseRequested() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return pause_requested_;
        }

        void clearReset()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            reset_requested_ = false;
        }

        void publish(const WorkflowRuntimeSnapshot &snapshot)
        {
            SnapshotCallback callback;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                latest_snapshot_ = snapshot;
                callback = snapshot_callback_;
            }
            if (callback)
            {
                callback(snapshot);
            }
        }

        void setSnapshotCallback(SnapshotCallback callback)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_callback_ = std::move(callback);
        }

        WorkflowRuntimeSnapshot latestSnapshot() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return latest_snapshot_;
        }

    private:
        mutable std::mutex mutex_;
        std::condition_variable pause_cv_;
        bool pause_requested_ = false;
        bool stop_requested_ = false;
        bool reset_requested_ = false;
        WorkflowRuntimeSnapshot latest_snapshot_;
        SnapshotCallback snapshot_callback_;
    };
}
