#pragma once

#include "workflow/web/web_console_config.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace workflow::web
{
    class WebConsoleController;

    namespace assets
    {
        const std::string &IndexHtml();
        const std::string &AppJs();
        const std::string &AppCss();
    }

    class WebConsoleServer
    {
    public:
        WebConsoleServer(const WebConsoleConfig &config, WebConsoleController &controller);
        ~WebConsoleServer();

        void Run();
        void Stop();

    private:
        struct SseClient;
        struct QueuedEvent
        {
            std::string name;
            std::string payload;
        };

        void acceptLoop(int listen_fd);
        void handleClient(int client_fd);
        bool handleSseClient(int client_fd);
        void queueEvent(const std::string &event_name, const std::string &payload);
        void flushQueuedEvents();
        void sendHeartbeatFrames();
        bool sendSseFrame(const std::shared_ptr<SseClient> &client, const std::string &payload);
        void pruneDeadClients();

        WebConsoleConfig config_;
        WebConsoleController &controller_;
        std::atomic<bool> stop_requested_{false};
        int listen_fd_ = -1;
        std::mutex clients_mutex_;
        std::vector<std::shared_ptr<SseClient>> sse_clients_;
        std::mutex events_mutex_;
        std::vector<QueuedEvent> pending_events_;
    };
}
