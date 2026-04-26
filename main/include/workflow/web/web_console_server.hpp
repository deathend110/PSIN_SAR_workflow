#pragma once

#include "workflow/web/web_console_config.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace workflow::web
{
    namespace detail
    {
        struct HttpRequest
        {
            std::string method;
            std::string target;
            std::string path;
            std::string query;
            std::unordered_map<std::string, std::string> headers;
            std::string body;
            size_t content_length = 0;
        };

        struct HttpRequestLimits
        {
            size_t max_header_bytes;
            size_t max_body_bytes;
            int read_timeout_ms;
        };

        enum class HttpRequestErrorKind
        {
            BadRequest,
            Timeout,
            PayloadTooLarge
        };

        class HttpRequestError : public std::runtime_error
        {
        public:
            HttpRequestError(HttpRequestErrorKind kind, const std::string &message)
                : std::runtime_error(message), kind_(kind)
            {
            }

            HttpRequestErrorKind kind() const noexcept
            {
                return kind_;
            }

        private:
            HttpRequestErrorKind kind_;
        };

        inline const HttpRequestLimits &DefaultHttpRequestLimits()
        {
            static const HttpRequestLimits limits{8 * 1024, 1024 * 1024, 5000};
            return limits;
        }

        inline std::string MapHttpRequestStatus(HttpRequestErrorKind kind)
        {
            switch (kind)
            {
            case HttpRequestErrorKind::BadRequest:
                return "400 Bad Request";
            case HttpRequestErrorKind::Timeout:
                return "408 Request Timeout";
            case HttpRequestErrorKind::PayloadTooLarge:
                return "413 Payload Too Large";
            }
            return "400 Bad Request";
        }

        inline std::string MapHttpRequestErrorCode(HttpRequestErrorKind kind)
        {
            switch (kind)
            {
            case HttpRequestErrorKind::BadRequest:
                return "invalid_request";
            case HttpRequestErrorKind::Timeout:
                return "request_timeout";
            case HttpRequestErrorKind::PayloadTooLarge:
                return "request_too_large";
            }
            return "invalid_request";
        }

        inline std::string TrimHttpToken(std::string value)
        {
            while (!value.empty() &&
                   (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' || value.back() == '\t'))
            {
                value.pop_back();
            }
            size_t offset = 0;
            while (offset < value.size() && (value[offset] == ' ' || value[offset] == '\t'))
            {
                ++offset;
            }
            return value.substr(offset);
        }

        inline std::string ToLowerAscii(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            return value;
        }

        inline size_t ParseContentLengthValue(const std::string &raw_value, const HttpRequestLimits &limits)
        {
            const std::string value = TrimHttpToken(raw_value);
            if (value.empty())
            {
                throw HttpRequestError(HttpRequestErrorKind::BadRequest, "Content-Length is required for request bodies.");
            }
            if (!std::all_of(value.begin(), value.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; }))
            {
                throw HttpRequestError(HttpRequestErrorKind::BadRequest, "Content-Length must be an unsigned integer.");
            }

            size_t content_length = 0;
            try
            {
                content_length = static_cast<size_t>(std::stoull(value));
            }
            catch (const std::exception &)
            {
                throw HttpRequestError(HttpRequestErrorKind::BadRequest, "Content-Length must be an unsigned integer.");
            }

            if (content_length > limits.max_body_bytes)
            {
                throw HttpRequestError(HttpRequestErrorKind::PayloadTooLarge, "Request body exceeds the configured limit.");
            }
            return content_length;
        }

        inline HttpRequest ParseHttpRequestHeaderBlock(const std::string &raw_header, const HttpRequestLimits &limits)
        {
            if (raw_header.size() > limits.max_header_bytes)
            {
                throw HttpRequestError(HttpRequestErrorKind::PayloadTooLarge, "Request headers exceed the configured limit.");
            }

            HttpRequest request;
            std::istringstream stream(raw_header);
            std::string request_line;
            if (!std::getline(stream, request_line))
            {
                throw HttpRequestError(HttpRequestErrorKind::BadRequest, "Invalid HTTP request line.");
            }
            request_line = TrimHttpToken(request_line);

            std::istringstream request_line_stream(request_line);
            request_line_stream >> request.method >> request.target;
            if (request.method.empty() || request.target.empty())
            {
                throw HttpRequestError(HttpRequestErrorKind::BadRequest, "Invalid HTTP request line.");
            }

            const size_t query_pos = request.target.find('?');
            request.path = query_pos == std::string::npos ? request.target : request.target.substr(0, query_pos);
            request.query = query_pos == std::string::npos ? std::string() : request.target.substr(query_pos + 1);

            std::string line;
            while (std::getline(stream, line))
            {
                line = TrimHttpToken(line);
                if (line.empty())
                {
                    continue;
                }
                const size_t colon = line.find(':');
                if (colon == std::string::npos)
                {
                    continue;
                }
                const std::string key = ToLowerAscii(line.substr(0, colon));
                request.headers[key] = TrimHttpToken(line.substr(colon + 1));
            }

            if (const auto it = request.headers.find("content-length"); it != request.headers.end())
            {
                request.content_length = ParseContentLengthValue(it->second, limits);
            }

            return request;
        }
    }

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
