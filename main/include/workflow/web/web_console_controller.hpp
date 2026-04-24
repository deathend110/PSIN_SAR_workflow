#pragma once

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
    class WebConsoleController
    {
    public:
        using EventCallback = std::function<void(const std::string &, const std::string &)>;

        WebConsoleController(infer::AppConfig infer_cfg, rd::AppConfig rd_cfg);
        ~WebConsoleController();

        void setEventCallback(EventCallback callback);
        void RequestWorkerStop();
        void JoinWorker();

        shared::WorkflowRuntimeSnapshot snapshot() const;
        infer::AppConfig inferConfig() const;
        rd::AppConfig rdConfig() const;
        FlightSettings flightSettings() const;

        std::vector<SourceInfo> listSources(shared::SelectedWorkflow workflow) const;
        std::optional<std::filesystem::path> resolveInferPreviewPath(const std::string &id) const;

        std::string applySelection(const std::unordered_map<std::string, std::string> &fields);
        std::string applySettings(const std::unordered_map<std::string, std::string> &fields);
        std::string commandStart();
        std::string commandPause();
        std::string commandStop();
        std::string commandReset();
        std::string commandManualKey(const std::unordered_map<std::string, std::string> &fields);

    private:
        struct SavedState
        {
            infer::AppConfig infer_cfg;
            rd::AppConfig rd_cfg;
            FlightSettings flight_settings;
            shared::WorkflowSelection selection;
        };

        struct PendingEvent
        {
            std::string name;
            std::string payload;
        };

        void queueEventLocked(std::vector<PendingEvent> &events,
                              const std::string &event_name,
                              const std::string &payload) const;
        void queueStateLocked(std::vector<PendingEvent> &events) const;
        void queueLogLocked(std::vector<PendingEvent> &events, const std::string &message) const;
        void queueErrorLocked(std::vector<PendingEvent> &events, const std::string &message) const;
        static void dispatchEvents(const EventCallback &callback, const std::vector<PendingEvent> &events);
        void onWorkflowSnapshot(const shared::WorkflowRuntimeSnapshot &snapshot);
        void workerMain(shared::WorkflowSelection selection,
                        infer::AppConfig infer_cfg,
                        rd::AppConfig rd_cfg,
                        shared::WorkflowRunControl *control);
        void joinWorkerIfNeeded();
        bool isRunningState(shared::ControlState state) const;
        void restoreLastAppliedLocked();
        std::vector<SourceInfo> listInferSourcesLocked() const;
        std::vector<SourceInfo> listRdSourcesLocked() const;

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
