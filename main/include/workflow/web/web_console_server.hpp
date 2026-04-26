#pragma once

#include "workflow/web/web_console_config.hpp"

#include <atomic>
#include <memory>
#include <string>

namespace workflow::web
{
    class SseHub;

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
        void acceptLoop(int listen_fd);
        void handleClient(int client_fd);
        bool handleSseClient(int client_fd);
        void queueEvent(const std::string &event_name, const std::string &payload);
        void flushQueuedEvents();
        void sendHeartbeatFrames();

        WebConsoleConfig config_;
        WebConsoleController &controller_;
        std::atomic<bool> stop_requested_{false};
        int listen_fd_ = -1;
        std::unique_ptr<SseHub> sse_hub_;
    };
}
