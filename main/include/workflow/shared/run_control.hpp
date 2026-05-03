#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>

namespace workflow::shared
{
    // Web 控制面和后台工作流共享的运行状态枚举。
    enum class ControlState
    {
        // 尚未启动任何后台流程。
        Idle,
        // 正在准备资源或启动线程。
        Starting,
        // 工作流处于正常运行态。
        Running,
        // 工作流收到暂停请求并停在安全点。
        Paused,
        // 工作流收到停止请求，等待退出。
        Stopping,
        // 工作流正常执行完成。
        Finished,
        // 工作流内部出现错误。
        Error,
    };

    // 当前选择的是 RD 工作流还是 Infer 工作流。
    enum class SelectedWorkflow
    {
        RdOnly,
        InferOnly,
    };

    // Infer 工作流可用的 patch 生成策略。
    enum class SelectedPatchMode
    {
        AutoSnake,
        ManualFlight,
        DebugRaster,
    };

    // Web 控制面当前选中的工作模式、patch 模式和输入源。
    struct WorkflowSelection
    {
        // 选中的顶层工作流。
        SelectedWorkflow workflow = SelectedWorkflow::InferOnly;
        // 当 workflow=InferOnly 时使用的 patch 规划方式。
        SelectedPatchMode patch_mode = SelectedPatchMode::AutoSnake;
        // 推理输出目标，如 hdmi 或 png。
        std::string output_mode = "hdmi";
        // 当前被选中的输入文件或目录。
        std::string selected_source;
    };

    // 暴露给 Web 层的统一运行时快照。
    struct WorkflowRuntimeSnapshot
    {
        // 当前后台工作流状态。
        ControlState state = ControlState::Idle;
        // 当前选择项快照。
        WorkflowSelection selection;
        // 当前执行阶段，如 load config、patch processed。
        std::string current_stage;
        // 当前处理的文件或条目。
        std::string current_item;
        // 最近一次错误描述；无错误时为空。
        std::string last_error;
        // 当前进度序号。
        int current_index = 0;
        // 总条目数。
        int total_count = 0;
        // 单次推理耗时，单位毫秒。
        double infer_ms = 0.0;
        // 当前条目的总耗时，单位毫秒。
        double total_ms = 0.0;
        // 当前估算帧率。
        double fps = 0.0;
    };

    // Web 控制面对后台工作流发出 pause / stop / reset 的协作控制器。
    class WorkflowRunControl
    {
    public:
        // 工作流状态快照发布回调，通常由 WebConsoleController 订阅。
        using SnapshotCallback = std::function<void(const WorkflowRuntimeSnapshot &)>;

        // 请求后台工作流在下一个安全点进入暂停态。
        void requestPause()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pause_requested_ = true;
        }

        // 取消暂停请求，并唤醒等待中的工作流线程。
        void requestResume()
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                pause_requested_ = false;
            }
            pause_cv_.notify_all();
        }

        // 请求后台工作流停止运行；工作流会在安全点退出。
        void requestStop()
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stop_requested_ = true;
                pause_requested_ = false;
            }
            pause_cv_.notify_all();
        }

        // 请求“复位”语义：先停掉当前工作流，再由上层决定是否恢复到最近一次已应用配置。
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

        // 若当前处于暂停请求状态，则阻塞到 resume 或 stop。
        void waitIfPaused()
        {
            std::unique_lock<std::mutex> lock(mutex_);
            pause_cv_.wait(lock, [&] {
                return !pause_requested_ || stop_requested_;
            });
        }

        // 查询是否已收到 stop 请求。
        bool shouldStop() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return stop_requested_;
        }

        // 查询是否已收到 reset 请求。
        bool shouldReset() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return reset_requested_;
        }

        // 查询是否已收到 pause 请求。
        bool isPauseRequested() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return pause_requested_;
        }

        // 在上层完成 reset 收尾后清掉 reset 标志。
        void clearReset()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            reset_requested_ = false;
        }

        // 发布一份新的运行时快照，并同步调用订阅回调。
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

        // 注册快照回调；通常在 worker 启动前完成。
        void setSnapshotCallback(SnapshotCallback callback)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_callback_ = std::move(callback);
        }

        // 返回最近一次发布的快照副本，供 HTTP 轮询接口读取。
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
