#include "workflow/web/web_console_server.hpp"

#include "workflow/web/web_console_controller.hpp"
#include "workflow/web/web_console_protocol.hpp"
#include "workflow/shared/config_utils.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace workflow::web
{
    namespace
    {
        // 选择 socket 发送标志；在支持的平台上避免对端断开时触发 SIGPIPE。
        int socketSendFlags()
        {
            int flags = 0;
#ifdef MSG_NOSIGNAL
            flags |= MSG_NOSIGNAL;
#endif
            return flags;
        }

        // 尽力把缓冲区全部写入 socket；中途中断会重试，其它错误返回 false。
        bool sendAll(int fd, const void *data, size_t size)
        {
            const char *cursor = static_cast<const char *>(data);
            size_t remaining = size;
            while (remaining > 0)
            {
                const ssize_t written = ::send(fd, cursor, remaining, socketSendFlags());
                if (written < 0)
                {
                    if (errno == EINTR)
                    {
                        continue;
                    }
                    return false;
                }
                if (written == 0)
                {
                    return false;
                }
                cursor += written;
                remaining -= static_cast<size_t>(written);
            }
            return true;
        }

        // 发送整段字符串数据。
        bool sendString(int fd, const std::string &data)
        {
            return sendAll(fd, data.data(), data.size());
        }

        // 按当前轻量 HTTP server 的需要拼出一个完整响应报文。
        std::string makeHttpResponse(const std::string &status,
                                     const std::string &content_type,
                                     const std::string &body,
                                     const std::vector<std::string> &extra_headers = {})
        {
            std::ostringstream oss;
            oss << "HTTP/1.1 " << status << "\r\n"
                << "Content-Type: " << content_type << "\r\n"
                << "Content-Length: " << body.size() << "\r\n"
                << "Connection: close\r\n";
            for (const auto &header : extra_headers)
            {
                oss << header << "\r\n";
            }
            oss << "\r\n" << body;
            return oss.str();
        }

        // 以二进制方式读入一个完整文件，主要供图片预览接口使用。
        std::string readFileBinary(const std::filesystem::path &path)
        {
            std::ifstream ifs(path, std::ios::binary);
            if (!ifs)
            {
                throw std::runtime_error("Failed to open file: " + path.string());
            }
            std::ostringstream oss;
            oss << ifs.rdbuf();
            return oss.str();
        }

        // 根据文件后缀推断预览接口返回的 Content-Type。
        std::string contentTypeForPath(const std::filesystem::path &path)
        {
            const std::string ext = workflow::shared::ToLower(path.extension().string());
            if (ext == ".png")
            {
                return "image/png";
            }
            if (ext == ".jpg" || ext == ".jpeg")
            {
                return "image/jpeg";
            }
            if (ext == ".bmp")
            {
                return "image/bmp";
            }
            return "application/octet-stream";
        }

        // 一次路由分发后的结果：普通响应、SSE 升级，或附带 server stop 请求。
        struct RouteOutcome
        {
            std::string response;
            bool keep_open = false;
            bool request_stop = false;
        };

        // 把 HTTP 请求分发到对应控制器动作，并生成统一的 RouteOutcome。
        RouteOutcome dispatchRoute(const detail::HttpRequest &request,
                                   const std::unordered_map<std::string, std::string> &query,
                                   WebConsoleController &controller,
                                   const std::function<bool()> &open_sse)
        {
            RouteOutcome outcome;

            switch (detail::MatchWebConsoleRoute(request))
            {
            case detail::WebConsoleRoute::Index:
                outcome.response = makeHttpResponse("200 OK", "text/html; charset=utf-8", assets::IndexHtml());
                return outcome;
            case detail::WebConsoleRoute::AppJs:
                outcome.response = makeHttpResponse("200 OK", "application/javascript; charset=utf-8", assets::AppJs());
                return outcome;
            case detail::WebConsoleRoute::AppCss:
                outcome.response = makeHttpResponse("200 OK", "text/css; charset=utf-8", assets::AppCss());
                return outcome;
            case detail::WebConsoleRoute::ApiState:
                outcome.response = makeHttpResponse("200 OK",
                                                    "application/json; charset=utf-8",
                                                    MakeStateResponse(controller.snapshot(), controller.manualTelemetry()));
                return outcome;
            case detail::WebConsoleRoute::ApiSettingsGet:
                outcome.response = makeHttpResponse("200 OK",
                                                    "application/json; charset=utf-8",
                                                    MakeSettingsResponse(controller.inferConfig(),
                                                                         controller.rdConfig(),
                                                                         controller.flightSettings()));
                return outcome;
            case detail::WebConsoleRoute::ApiSources:
            {
                shared::SelectedWorkflow workflow = shared::SelectedWorkflow::InferOnly;
                if (const auto it = query.find("workflow"); it != query.end() &&
                                                          !ParseSelectedWorkflow(it->second, workflow))
                {
                    outcome.response = makeHttpResponse("400 Bad Request",
                                                        "application/json; charset=utf-8",
                                                        MakeErrorResponse("invalid_query", "workflow must be rd or infer."));
                }
                else
                {
                    outcome.response = makeHttpResponse("200 OK",
                                                        "application/json; charset=utf-8",
                                                        MakeSourcesResponse(controller.listSources(workflow)));
                }
                return outcome;
            }
            case detail::WebConsoleRoute::ApiSourcePreview:
            {
                const auto it = query.find("id");
                if (it == query.end())
                {
                    outcome.response = makeHttpResponse("400 Bad Request",
                                                        "application/json; charset=utf-8",
                                                        MakeErrorResponse("invalid_query", "id is required."));
                    return outcome;
                }

                const auto preview_path = controller.resolveInferPreviewPath(it->second);
                if (!preview_path.has_value())
                {
                    outcome.response = makeHttpResponse("404 Not Found",
                                                        "application/json; charset=utf-8",
                                                        MakeErrorResponse("not_previewable", "Preview is only available for inference images."));
                    return outcome;
                }

                outcome.response = makeHttpResponse("200 OK",
                                                    contentTypeForPath(*preview_path),
                                                    readFileBinary(*preview_path));
                return outcome;
            }
            case detail::WebConsoleRoute::Events:
                outcome.keep_open = open_sse();
                return outcome;
            case detail::WebConsoleRoute::ApiSelection:
                outcome.response = makeHttpResponse("200 OK",
                                                    "application/json; charset=utf-8",
                                                    controller.applySelection(ParseFlatJsonObject(request.body)));
                return outcome;
            case detail::WebConsoleRoute::ApiSettingsPost:
                outcome.response = makeHttpResponse("200 OK",
                                                    "application/json; charset=utf-8",
                                                    controller.applySettings(ParseFlatJsonObject(request.body)));
                return outcome;
            case detail::WebConsoleRoute::ApiCommandStart:
                outcome.response = makeHttpResponse("200 OK",
                                                    "application/json; charset=utf-8",
                                                    controller.commandStart());
                return outcome;
            case detail::WebConsoleRoute::ApiCommandPause:
                outcome.response = makeHttpResponse("200 OK",
                                                    "application/json; charset=utf-8",
                                                    controller.commandPause());
                return outcome;
            case detail::WebConsoleRoute::ApiCommandStop:
                outcome.response = makeHttpResponse("200 OK",
                                                    "application/json; charset=utf-8",
                                                    controller.commandStop());
                return outcome;
            case detail::WebConsoleRoute::ApiCommandReset:
                outcome.response = makeHttpResponse("200 OK",
                                                    "application/json; charset=utf-8",
                                                    controller.commandReset());
                return outcome;
            case detail::WebConsoleRoute::ApiCommandShutdownWeb:
            {
                const std::string response = controller.commandShutdownWeb();
                outcome.response = makeHttpResponse("200 OK",
                                                    "application/json; charset=utf-8",
                                                    response);
                outcome.request_stop = response.find("\"ok\":false") == std::string::npos;
                return outcome;
            }
            case detail::WebConsoleRoute::ApiManualKey:
                outcome.response = makeHttpResponse("200 OK",
                                                    "application/json; charset=utf-8",
                                                    controller.commandManualKey(ParseFlatJsonObject(request.body)));
                return outcome;
            case detail::WebConsoleRoute::Unknown:
                outcome.response = makeHttpResponse("404 Not Found",
                                                    "application/json; charset=utf-8",
                                                    MakeErrorResponse("not_found", "Unknown route."));
                return outcome;
            }

            outcome.response = makeHttpResponse("404 Not Found",
                                                "application/json; charset=utf-8",
                                                MakeErrorResponse("not_found", "Unknown route."));
            return outcome;
        }
    }

    // SSE 广播中心。
    // 它负责保存长连接客户端、缓存待发送事件，并在 Web 主循环里按批次刷出。
    class SseHub
    {
    public:
        // 接受一个新的 SSE 客户端，并先推送一帧初始 state。
        bool acceptClient(int client_fd, const std::string &initial_state_payload)
        {
            auto client = std::make_shared<Client>(client_fd);

            const std::string headers =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/event-stream\r\n"
                "Cache-Control: no-cache\r\n"
                "Connection: keep-alive\r\n"
                "Access-Control-Allow-Origin: *\r\n\r\n";
            if (!sendString(client_fd, headers))
            {
                return false;
            }

            const std::string initial_frame =
                std::string("event: state\ndata: ") + initial_state_payload + "\n\n";
            if (!sendFrame(client, initial_frame))
            {
                return false;
            }

            std::lock_guard<std::mutex> lock(clients_mutex_);
            clients_.push_back(std::move(client));
            return true;
        }

        // 关闭并清空全部 SSE 客户端，同时丢弃待发送事件。
        void stopAll()
        {
            std::lock_guard<std::mutex> clients_lock(clients_mutex_);
            for (const auto &client : clients_)
            {
                if (!client)
                {
                    continue;
                }
                client->active = false;
                if (client->fd >= 0)
                {
                    ::shutdown(client->fd, SHUT_RDWR);
                    ::close(client->fd);
                    client->fd = -1;
                }
            }
            clients_.clear();

            std::lock_guard<std::mutex> events_lock(events_mutex_);
            pending_events_.clear();
        }

        // 把一条事件暂存到待发送队列。
        void queueEvent(const std::string &event_name, const std::string &payload)
        {
            std::lock_guard<std::mutex> lock(events_mutex_);
            pending_events_.push_back(QueuedEvent{event_name, payload});
        }

        // 批量把积压事件广播给所有仍然存活的 SSE 客户端。
        void flushQueuedEvents()
        {
            std::vector<QueuedEvent> events;
            {
                std::lock_guard<std::mutex> lock(events_mutex_);
                if (pending_events_.empty())
                {
                    return;
                }
                events.swap(pending_events_);
            }

            pruneDeadClients();
            const auto clients = snapshotClients();
            for (const auto &event : events)
            {
                const std::string frame =
                    std::string("event: ") + event.name + "\n" +
                    "data: " + event.payload + "\n\n";
                for (const auto &client : clients)
                {
                    sendFrame(client, frame);
                }
            }
        }

        // 给所有 SSE 客户端发送心跳帧，避免空闲长连接被中间层回收。
        void sendHeartbeatFrames()
        {
            pruneDeadClients();
            const auto clients = snapshotClients();
            for (const auto &client : clients)
            {
                sendFrame(client, ":\n\n");
            }
        }

    private:
        struct Client
        {
            // 记录一个 SSE 客户端连接及其写锁。
            explicit Client(int client_fd)
                : fd(client_fd)
            {
            }

            int fd = -1;
            std::mutex write_mutex;
            std::atomic<bool> active{true};
        };

        // 一条尚未广播的 SSE 事件。
        struct QueuedEvent
        {
            std::string name;
            std::string payload;
        };

        // 返回当前客户端列表快照，避免长时间占用 clients 锁。
        std::vector<std::shared_ptr<Client>> snapshotClients()
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            return clients_;
        }

        // 在单个客户端的写锁保护下发送一帧 SSE 数据。
        bool sendFrame(const std::shared_ptr<Client> &client, const std::string &payload)
        {
            if (!client || !client->active)
            {
                return false;
            }
            std::lock_guard<std::mutex> lock(client->write_mutex);
            if (!sendString(client->fd, payload))
            {
                client->active = false;
                return false;
            }
            return true;
        }

        // 清理已经失活的 SSE 客户端连接。
        void pruneDeadClients()
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            for (auto it = clients_.begin(); it != clients_.end();)
            {
                if (!*it || !(*it)->active)
                {
                    if (*it && (*it)->fd >= 0)
                    {
                        ::shutdown((*it)->fd, SHUT_RDWR);
                        ::close((*it)->fd);
                        (*it)->fd = -1;
                    }
                    it = clients_.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        std::mutex clients_mutex_;
        std::vector<std::shared_ptr<Client>> clients_;
        std::mutex events_mutex_;
        std::vector<QueuedEvent> pending_events_;
    };

    // 保存配置和控制器引用，并把控制器事件接到 SSE 队列。
    WebConsoleServer::WebConsoleServer(const WebConsoleConfig &config, WebConsoleController &controller)
        : config_(config), controller_(controller), sse_hub_(std::make_unique<SseHub>())
    {
        controller_.setEventCallback([this](const std::string &event_name, const std::string &payload) {
            queueEvent(event_name, payload);
        });
    }

    // 析构时断开事件回调并停止所有网络资源。
    WebConsoleServer::~WebConsoleServer()
    {
        controller_.setEventCallback(WebConsoleController::EventCallback{});
        Stop();
    }

    // 启动监听 socket，并进入 accept loop。
    void WebConsoleServer::Run()
    {
        stop_requested_ = false;
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0)
        {
            throw std::runtime_error("Failed to create HTTP listen socket.");
        }

        int reuse = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(config_.port));
        if (::inet_pton(AF_INET, config_.bind_address.c_str(), &addr.sin_addr) != 1)
        {
            ::close(listen_fd_);
            listen_fd_ = -1;
            throw std::runtime_error("Invalid server.bind address: " + config_.bind_address);
        }

        if (::bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
        {
            const std::string message = std::string("Failed to bind HTTP server: ") + std::strerror(errno);
            ::close(listen_fd_);
            listen_fd_ = -1;
            throw std::runtime_error(message);
        }

        if (::listen(listen_fd_, 16) != 0)
        {
            const std::string message = std::string("Failed to listen HTTP server: ") + std::strerror(errno);
            ::close(listen_fd_);
            listen_fd_ = -1;
            throw std::runtime_error(message);
        }

        acceptLoop(listen_fd_);
    }

    // 停止 server：关闭监听 socket，并清空所有 SSE 客户端。
    void WebConsoleServer::Stop()
    {
        stop_requested_ = true;
        if (listen_fd_ >= 0)
        {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }

        if (sse_hub_)
        {
            sse_hub_->stopAll();
        }
    }

    // Web 主循环。
    // 它在单线程里交替处理：事件刷新、SSE 心跳、select 等待、accept 客户端。
    void WebConsoleServer::acceptLoop(int listen_fd)
    {
        auto next_heartbeat = std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.sse_heartbeat_ms);
        while (!stop_requested_)
        {
            flushQueuedEvents();

            const auto now = std::chrono::steady_clock::now();
            if (now >= next_heartbeat)
            {
                sendHeartbeatFrames();
                next_heartbeat = now + std::chrono::milliseconds(config_.sse_heartbeat_ms);
            }

            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(listen_fd, &readfds);
            timeval timeout{};
            const auto wait_until = std::min(next_heartbeat, std::chrono::steady_clock::now() + std::chrono::milliseconds(250));
            const auto wait_duration = std::max(std::chrono::microseconds(0),
                                                std::chrono::duration_cast<std::chrono::microseconds>(
                                                    wait_until - std::chrono::steady_clock::now()));
            timeout.tv_sec = static_cast<long>(wait_duration.count() / 1000000);
            timeout.tv_usec = static_cast<long>(wait_duration.count() % 1000000);
            const int ready = ::select(listen_fd + 1, &readfds, nullptr, nullptr, &timeout);
            if (ready < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                break;
            }
            if (ready == 0)
            {
                continue;
            }

            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            const int client_fd = ::accept(listen_fd, reinterpret_cast<sockaddr *>(&client_addr), &client_len);
            if (client_fd < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                if (stop_requested_)
                {
                    break;
                }
                continue;
            }

            handleClient(client_fd);
        }

        flushQueuedEvents();
    }

    // 处理单个 HTTP 客户端连接；普通请求直接返回响应，SSE 请求则转成长连接。
    void WebConsoleServer::handleClient(int client_fd)
    {
        bool keep_open = false;
        try
        {
            const detail::HttpRequest request = detail::ReadHttpRequestFromSocket(client_fd);
            const auto query = ParseQueryString(request.query);
            const RouteOutcome outcome = dispatchRoute(request,
                                                       query,
                                                       controller_,
                                                       [this, client_fd]() { return handleSseClient(client_fd); });
            keep_open = outcome.keep_open;
            if (!keep_open)
            {
                sendString(client_fd, outcome.response);
            }
            if (outcome.request_stop)
            {
                stop_requested_ = true;
            }
        }
        catch (const detail::HttpRequestError &e)
        {
            sendString(client_fd, makeHttpResponse(detail::MapHttpRequestStatus(e.kind()),
                                                   "application/json; charset=utf-8",
                                                   MakeErrorResponse(detail::MapHttpRequestErrorCode(e.kind()), e.what())));
        }
        catch (const std::exception &e)
        {
            sendString(client_fd, makeHttpResponse("500 Internal Server Error",
                                                   "application/json; charset=utf-8",
                                                   MakeErrorResponse("internal_error", e.what())));
        }

        if (!keep_open)
        {
            ::shutdown(client_fd, SHUT_RDWR);
            ::close(client_fd);
        }
    }

    // 把当前连接升级成 SSE 客户端，并立刻推送一帧初始状态。
    bool WebConsoleServer::handleSseClient(int client_fd)
    {
        if (!sse_hub_)
        {
            return false;
        }
        return sse_hub_->acceptClient(client_fd,
                                      MakeStateResponse(controller_.snapshot(), controller_.manualTelemetry()));
    }

    // 向 SSE 中心暂存一条事件。
    void WebConsoleServer::queueEvent(const std::string &event_name, const std::string &payload)
    {
        if (sse_hub_)
        {
            sse_hub_->queueEvent(event_name, payload);
        }
    }

    // 触发 SSE 中心批量广播积压事件。
    void WebConsoleServer::flushQueuedEvents()
    {
        if (sse_hub_)
        {
            sse_hub_->flushQueuedEvents();
        }
    }

    // 触发 SSE 中心发送心跳帧。
    void WebConsoleServer::sendHeartbeatFrames()
    {
        if (sse_hub_)
        {
            sse_hub_->sendHeartbeatFrames();
        }
    }
}
