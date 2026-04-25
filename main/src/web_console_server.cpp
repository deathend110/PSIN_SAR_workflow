#include "workflow/web/web_console_server.hpp"

#include "workflow/web/web_console_controller.hpp"
#include "workflow/web/web_console_protocol.hpp"
#include "workflow/shared/config_utils.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include <thread>

namespace workflow::web
{
    namespace
    {
        struct HttpRequest
        {
            std::string method;
            std::string target;
            std::string path;
            std::string query;
            std::unordered_map<std::string, std::string> headers;
            std::string body;
        };

        int socketSendFlags()
        {
            int flags = 0;
#ifdef MSG_NOSIGNAL
            flags |= MSG_NOSIGNAL;
#endif
            return flags;
        }

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

        bool sendString(int fd, const std::string &data)
        {
            return sendAll(fd, data.data(), data.size());
        }

        std::string trimHeaderValue(std::string value)
        {
            while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' '))
            {
                value.pop_back();
            }
            while (!value.empty() && value.front() == ' ')
            {
                value.erase(value.begin());
            }
            return value;
        }

        HttpRequest parseRequest(int fd)
        {
            std::string raw;
            char buffer[4096];
            size_t header_end = std::string::npos;

            while ((header_end = raw.find("\r\n\r\n")) == std::string::npos)
            {
                const ssize_t read_bytes = ::recv(fd, buffer, sizeof(buffer), 0);
                if (read_bytes <= 0)
                {
                    throw std::runtime_error("Failed to read HTTP request.");
                }
                raw.append(buffer, static_cast<size_t>(read_bytes));
            }

            HttpRequest request;
            std::istringstream stream(raw.substr(0, header_end));
            std::string request_line;
            if (!std::getline(stream, request_line))
            {
                throw std::runtime_error("Invalid HTTP request line.");
            }
            request_line = trimHeaderValue(request_line);

            std::istringstream request_line_stream(request_line);
            request_line_stream >> request.method >> request.target;
            if (request.method.empty() || request.target.empty())
            {
                throw std::runtime_error("Invalid HTTP request line.");
            }

            const size_t query_pos = request.target.find('?');
            request.path = query_pos == std::string::npos ? request.target : request.target.substr(0, query_pos);
            request.query = query_pos == std::string::npos ? std::string() : request.target.substr(query_pos + 1);

            std::string line;
            while (std::getline(stream, line))
            {
                line = trimHeaderValue(line);
                if (line.empty())
                {
                    continue;
                }
                const size_t colon = line.find(':');
                if (colon == std::string::npos)
                {
                    continue;
                }
                const std::string key = line.substr(0, colon);
                request.headers[workflow::shared::ToLower(key)] = trimHeaderValue(line.substr(colon + 1));
            }

            size_t content_length = 0;
            if (const auto it = request.headers.find("content-length"); it != request.headers.end())
            {
                content_length = static_cast<size_t>(std::stoul(it->second));
            }

            request.body = raw.substr(header_end + 4);
            while (request.body.size() < content_length)
            {
                const ssize_t read_bytes = ::recv(fd, buffer, sizeof(buffer), 0);
                if (read_bytes <= 0)
                {
                    throw std::runtime_error("Failed to read HTTP request body.");
                }
                request.body.append(buffer, static_cast<size_t>(read_bytes));
            }
            if (request.body.size() > content_length)
            {
                request.body.resize(content_length);
            }

            return request;
        }

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
    }

    struct WebConsoleServer::SseClient
    {
        explicit SseClient(int client_fd)
            : fd(client_fd)
        {
        }

        int fd = -1;
        std::mutex write_mutex;
        std::atomic<bool> active{true};
    };

    WebConsoleServer::WebConsoleServer(const WebConsoleConfig &config, WebConsoleController &controller)
        : config_(config), controller_(controller)
    {
        controller_.setEventCallback([this](const std::string &event_name, const std::string &payload) {
            queueEvent(event_name, payload);
        });
    }

    WebConsoleServer::~WebConsoleServer()
    {
        controller_.setEventCallback(WebConsoleController::EventCallback{});
        Stop();
    }

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

    void WebConsoleServer::Stop()
    {
        stop_requested_ = true;
        if (listen_fd_ >= 0)
        {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }

        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (const auto &client : sse_clients_)
        {
            client->active = false;
            ::shutdown(client->fd, SHUT_RDWR);
            ::close(client->fd);
        }
        sse_clients_.clear();

        std::lock_guard<std::mutex> events_lock(events_mutex_);
        pending_events_.clear();
    }

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
                                                std::chrono::duration_cast<std::chrono::microseconds>(wait_until - std::chrono::steady_clock::now()));
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

    void WebConsoleServer::handleClient(int client_fd)
    {
        bool keep_open = false;
        try
        {
            const HttpRequest request = parseRequest(client_fd);
            const auto query = ParseQueryString(request.query);

            if (request.method == "GET" && request.path == "/")
            {
                sendString(client_fd, makeHttpResponse("200 OK", "text/html; charset=utf-8", assets::IndexHtml()));
            }
            else if (request.method == "GET" && request.path == "/app.js")
            {
                sendString(client_fd, makeHttpResponse("200 OK", "application/javascript; charset=utf-8", assets::AppJs()));
            }
            else if (request.method == "GET" && request.path == "/app.css")
            {
                sendString(client_fd, makeHttpResponse("200 OK", "text/css; charset=utf-8", assets::AppCss()));
            }
            else if (request.method == "GET" && request.path == "/api/state")
            {
                sendString(client_fd, makeHttpResponse("200 OK",
                                                       "application/json; charset=utf-8",
                                                       MakeStateResponse(controller_.snapshot(), controller_.manualTelemetry())));
            }
            else if (request.method == "GET" && request.path == "/api/settings")
            {
                sendString(client_fd, makeHttpResponse("200 OK",
                                                       "application/json; charset=utf-8",
                                                       MakeSettingsResponse(controller_.inferConfig(),
                                                                            controller_.rdConfig(),
                                                                            controller_.flightSettings())));
            }
            else if (request.method == "GET" && request.path == "/api/sources")
            {
                shared::SelectedWorkflow workflow = shared::SelectedWorkflow::InferOnly;
                if (const auto it = query.find("workflow"); it != query.end() && !ParseSelectedWorkflow(it->second, workflow))
                {
                    sendString(client_fd, makeHttpResponse("400 Bad Request",
                                                           "application/json; charset=utf-8",
                                                           MakeErrorResponse("invalid_query", "workflow must be rd or infer.")));
                }
                else
                {
                    sendString(client_fd, makeHttpResponse("200 OK",
                                                           "application/json; charset=utf-8",
                                                           MakeSourcesResponse(controller_.listSources(workflow))));
                }
            }
            else if (request.method == "GET" && request.path == "/api/source/preview")
            {
                const auto it = query.find("id");
                if (it == query.end())
                {
                    sendString(client_fd, makeHttpResponse("400 Bad Request",
                                                           "application/json; charset=utf-8",
                                                           MakeErrorResponse("invalid_query", "id is required.")));
                }
                else
                {
                    const auto preview_path = controller_.resolveInferPreviewPath(it->second);
                    if (!preview_path.has_value())
                    {
                        sendString(client_fd, makeHttpResponse("404 Not Found",
                                                               "application/json; charset=utf-8",
                                                               MakeErrorResponse("not_previewable", "Preview is only available for inference images.")));
                    }
                    else
                    {
                        const std::string body = readFileBinary(*preview_path);
                        sendString(client_fd,
                                   makeHttpResponse("200 OK",
                                                    contentTypeForPath(*preview_path),
                                                    body));
                    }
                }
            }
            else if (request.method == "GET" && request.path == "/events")
            {
                keep_open = handleSseClient(client_fd);
            }
            else if (request.method == "POST" && request.path == "/api/selection")
            {
                sendString(client_fd, makeHttpResponse("200 OK",
                                                       "application/json; charset=utf-8",
                                                       controller_.applySelection(ParseFlatJsonObject(request.body))));
            }
            else if (request.method == "POST" && request.path == "/api/settings")
            {
                sendString(client_fd, makeHttpResponse("200 OK",
                                                       "application/json; charset=utf-8",
                                                       controller_.applySettings(ParseFlatJsonObject(request.body))));
            }
            else if (request.method == "POST" && request.path == "/api/command/start")
            {
                sendString(client_fd, makeHttpResponse("200 OK", "application/json; charset=utf-8", controller_.commandStart()));
            }
            else if (request.method == "POST" && request.path == "/api/command/pause")
            {
                sendString(client_fd, makeHttpResponse("200 OK", "application/json; charset=utf-8", controller_.commandPause()));
            }
            else if (request.method == "POST" && request.path == "/api/command/stop")
            {
                sendString(client_fd, makeHttpResponse("200 OK", "application/json; charset=utf-8", controller_.commandStop()));
            }
            else if (request.method == "POST" && request.path == "/api/command/reset")
            {
                sendString(client_fd, makeHttpResponse("200 OK", "application/json; charset=utf-8", controller_.commandReset()));
            }
            else if (request.method == "POST" && request.path == "/api/command/shutdown_web")
            {
                const std::string response = controller_.commandShutdownWeb();
                sendString(client_fd, makeHttpResponse("200 OK", "application/json; charset=utf-8", response));
                if (response.find("\"ok\":false") == std::string::npos)
                {
                    stop_requested_ = true;
                }
            }
            else if (request.method == "POST" && request.path == "/api/manual/key")
            {
                sendString(client_fd, makeHttpResponse("200 OK",
                                                       "application/json; charset=utf-8",
                                                       controller_.commandManualKey(ParseFlatJsonObject(request.body))));
            }
            else
            {
                sendString(client_fd, makeHttpResponse("404 Not Found",
                                                       "application/json; charset=utf-8",
                                                       MakeErrorResponse("not_found", "Unknown route.")));
            }
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

    bool WebConsoleServer::handleSseClient(int client_fd)
    {
        auto client = std::make_shared<SseClient>(client_fd);

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

        if (!sendSseFrame(client, std::string("event: state\ndata: ") + MakeStateResponse(controller_.snapshot(), controller_.manualTelemetry()) + "\n\n"))
        {
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            sse_clients_.push_back(client);
        }
        return true;
    }

    void WebConsoleServer::queueEvent(const std::string &event_name, const std::string &payload)
    {
        std::lock_guard<std::mutex> lock(events_mutex_);
        pending_events_.push_back(QueuedEvent{event_name, payload});
    }

    void WebConsoleServer::flushQueuedEvents()
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
        std::vector<std::shared_ptr<SseClient>> clients;
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            clients = sse_clients_;
        }
        for (const auto &event : events)
        {
            const std::string frame = std::string("event: ") + event.name + "\n" +
                                      "data: " + event.payload + "\n\n";
            for (const auto &client : clients)
            {
                sendSseFrame(client, frame);
            }
        }
    }

    void WebConsoleServer::sendHeartbeatFrames()
    {
        pruneDeadClients();

        std::vector<std::shared_ptr<SseClient>> clients;
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            clients = sse_clients_;
        }

        for (const auto &client : clients)
        {
            sendSseFrame(client, ":\n\n");
        }
    }

    bool WebConsoleServer::sendSseFrame(const std::shared_ptr<SseClient> &client, const std::string &payload)
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

    void WebConsoleServer::pruneDeadClients()
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (auto it = sse_clients_.begin(); it != sse_clients_.end();)
        {
            if (!*it || !(*it)->active)
            {
                if (*it && (*it)->fd >= 0)
                {
                    ::shutdown((*it)->fd, SHUT_RDWR);
                    ::close((*it)->fd);
                    (*it)->fd = -1;
                }
                it = sse_clients_.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
}
