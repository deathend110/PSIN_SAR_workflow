#pragma once

#include "workflow/infer/infer_workflow.hpp"
#include "workflow/infer/infer_config.hpp"
#include "workflow/rd/rd_config.hpp"
#include "workflow/shared/run_control.hpp"
#include "workflow/web/web_console_protocol.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace workflow::web
{
    // Web 控制面的核心状态机：保存内存态配置、驱动后台 worker，并向前端发布事件。
    class WebConsoleController
    {
    public:
        // 事件分发回调，由 WebConsoleServer 注册，用于把状态/日志推入 SSE 队列。
        using EventCallback = std::function<void(const std::string &, const std::string &)>;

        // 用初始的 infer / rd 配置构造控制器，并准备默认选择态。
        WebConsoleController(infer::AppConfig infer_cfg, rd::AppConfig rd_cfg);
        // 析构时确保后台 worker 被安全停止并回收。
        ~WebConsoleController();

        // 注册事件回调；通常只由 Web server 调用一次。
        void setEventCallback(EventCallback callback);
        // 请求后台 worker 停止；用于 Web Console 关闭流程。
        void RequestWorkerStop();
        // 等待后台 worker 线程退出。
        void JoinWorker();

        // 读取当前统一运行态快照。
        shared::WorkflowRuntimeSnapshot snapshot() const;
        // 读取当前内存中的 Infer 配置。
        infer::AppConfig inferConfig() const;
        // 读取当前内存中的 RD 配置。
        rd::AppConfig rdConfig() const;
        // 读取当前 flight 配置。
        FlightSettings flightSettings() const;
        // 读取当前 manual_flight 遥测。
        ManualFlightTelemetry manualTelemetry() const;

        // 列出当前工作流可选的输入源。
        std::vector<SourceInfo> listSources(shared::SelectedWorkflow workflow) const;
        // 若给定 source 可预览，则返回其图像路径。
        std::optional<std::filesystem::path> resolveInferPreviewPath(const std::string &id) const;

        // 应用 selection 类修改，如 workflow、patch_mode、output_mode、selected_source。
        std::string applySelection(const std::unordered_map<std::string, std::string> &fields);
        // 应用 settings 类修改，如 infer / rd / flight 配置。
        std::string applySettings(const std::unordered_map<std::string, std::string> &fields);
        // 启动或恢复后台 workflow。
        std::string commandStart();
        // 请求后台 workflow 进入暂停态。
        std::string commandPause();
        // 请求后台 workflow 停止。
        std::string commandStop();
        // 把控制器恢复到最近一次已应用配置的空闲态。
        std::string commandReset();
        // 请求关闭 Web Console。
        std::string commandShutdownWeb();
        // 处理 manual_flight 的键控输入请求。
        std::string commandManualKey(const std::unordered_map<std::string, std::string> &fields);

    private:
        // 记录“最近一次已成功应用”的配置快照，供 reset 恢复。
        struct SavedState
        {
            infer::AppConfig infer_cfg;
            rd::AppConfig rd_cfg;
            FlightSettings flight_settings;
            shared::WorkflowSelection selection;
        };

        // 在持锁阶段暂存要发出的事件，退出锁后统一派发。
        struct PendingEvent
        {
            std::string name;
            std::string payload;
        };

        // 在事件列表中追加一条任意事件。
        void queueEventLocked(std::vector<PendingEvent> &events,
                              const std::string &event_name,
                              const std::string &payload) const;
        // 把当前状态快照打包成 state 事件。
        void queueStateLocked(std::vector<PendingEvent> &events) const;
        // 把一条文本日志打包成 log 事件。
        void queueLogLocked(std::vector<PendingEvent> &events, const std::string &message) const;
        // 把一条错误打包成 error 事件。
        void queueErrorLocked(std::vector<PendingEvent> &events, const std::string &message) const;
        // 在无锁状态下把事件逐条投递给注册回调。
        static void dispatchEvents(const EventCallback &callback, const std::vector<PendingEvent> &events);
        // 接收后台 workflow 发布的快照，并更新控制器状态。
        void onWorkflowSnapshot(const shared::WorkflowRuntimeSnapshot &snapshot);
        // 后台 worker 线程入口，按 selection 调用 RD 或 Infer。
        void workerMain(shared::WorkflowSelection selection,
                        infer::AppConfig infer_cfg,
                        rd::AppConfig rd_cfg,
                        shared::WorkflowRunControl *control);
        // 通用 stop 实现；可选择是否同步等待 worker 退出。
        std::string stopWorkflow(bool wait_for_worker, const std::string &success_message);
        // 若 worker 已结束但线程对象仍可 join，则在这里回收。
        void joinWorkerIfNeeded();
        // 判断给定状态是否可视为“工作流仍活跃”。
        bool isRunningState(shared::ControlState state) const;
        // 把内存态恢复到最近一次已应用的配置快照。
        void restoreLastAppliedLocked();
        // 列出 Infer 可选输入源；调用方需已持锁。
        std::vector<SourceInfo> listInferSourcesLocked() const;
        // 列出 RD 可选输入源；调用方需已持锁。
        std::vector<SourceInfo> listRdSourcesLocked() const;
        // 把 Web 层 flight settings 转成 Infer 层 manual settings。
        static infer::ManualFlightSettings toInferManualSettings(const FlightSettings &settings);

        mutable std::mutex mutex_;
        infer::AppConfig infer_cfg_;
        rd::AppConfig rd_cfg_;
        FlightSettings flight_settings_;
        SavedState last_applied_;
        shared::WorkflowRuntimeSnapshot runtime_snapshot_;
        EventCallback event_callback_;
        std::unique_ptr<shared::WorkflowRunControl> run_control_;
        std::thread worker_;
        bool worker_active_ = false;
        bool stop_requested_by_controller_ = false;
        std::string last_stage_for_log_;
    };
}
