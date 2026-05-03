#pragma once

#include "workflow/web/web_console_config.hpp"

#include <atomic>
#include <memory>
#include <string>

namespace workflow::web
{
    // SSE 广播中心的前置声明；真实实现位于 .cpp 中。
    class SseHub;

    // 控制器前置声明，避免头文件互相包含过深。
    class WebConsoleController;

    namespace assets
    {
        // 返回内嵌首页 HTML 资产。
        const std::string &IndexHtml();
        // 返回内嵌前端 JS 资产。
        const std::string &AppJs();
        // 返回内嵌前端 CSS 资产。
        const std::string &AppCss();
    }

    // 轻量级单线程 HTTP + SSE 服务器，承载 Web Console 全部网络 I/O。
    class WebConsoleServer
    {
    public:
        // 保存配置和控制器引用，并注册事件回调。
        WebConsoleServer(const WebConsoleConfig &config, WebConsoleController &controller);
        // 析构时负责停止监听和清理 SSE 客户端。
        ~WebConsoleServer();

        // 进入 accept loop，直到收到 stop 请求。
        void Run();
        // 请求停止 server，并关闭监听 socket 与现有 SSE 客户端。
        void Stop();

    private:
        // 监听循环：处理 accept、心跳和事件刷新。
        void acceptLoop(int listen_fd);
        // 处理单个 HTTP 客户端连接。
        void handleClient(int client_fd);
        // 把连接升级为 SSE 客户端，并发送初始状态。
        bool handleSseClient(int client_fd);
        // 把一条控制器事件排入 SSE 队列。
        void queueEvent(const std::string &event_name, const std::string &payload);
        // 把积压事件批量推送给所有 SSE 客户端。
        void flushQueuedEvents();
        // 给 SSE 客户端发送心跳帧，避免长连接被中间层回收。
        void sendHeartbeatFrames();

        WebConsoleConfig config_;
        WebConsoleController &controller_;
        std::atomic<bool> stop_requested_{false};
        int listen_fd_ = -1;
        std::unique_ptr<SseHub> sse_hub_;
    };
}
