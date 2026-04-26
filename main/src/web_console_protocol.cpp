#include "workflow/web/web_console_protocol.hpp"

#include "workflow/shared/config_utils.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

namespace workflow::web
{
    namespace
    {
        class FlatJsonParser
        {
        public:
            explicit FlatJsonParser(const std::string &input)
                : input_(input)
            {
            }

            std::unordered_map<std::string, std::string> parseObject()
            {
                skipWhitespace();
                expect('{');
                skipWhitespace();

                std::unordered_map<std::string, std::string> values;
                if (peek() == '}')
                {
                    ++pos_;
                    return values;
                }

                while (pos_ < input_.size())
                {
                    const std::string key = parseString();
                    skipWhitespace();
                    expect(':');
                    skipWhitespace();
                    values[key] = parseValue();
                    skipWhitespace();

                    if (peek() == ',')
                    {
                        ++pos_;
                        skipWhitespace();
                        continue;
                    }
                    expect('}');
                    break;
                }

                return values;
            }

        private:
            char peek() const
            {
                return pos_ < input_.size() ? input_[pos_] : '\0';
            }

            void skipWhitespace()
            {
                while (pos_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[pos_])))
                {
                    ++pos_;
                }
            }

            void expect(char ch)
            {
                if (peek() != ch)
                {
                    throw std::runtime_error(std::string("Invalid JSON, expected '") + ch + "'.");
                }
                ++pos_;
            }

            std::string parseString()
            {
                expect('"');
                std::string value;
                while (pos_ < input_.size())
                {
                    const char ch = input_[pos_++];
                    if (ch == '"')
                    {
                        return value;
                    }
                    if (ch == '\\')
                    {
                        if (pos_ >= input_.size())
                        {
                            break;
                        }
                        const char escaped = input_[pos_++];
                        switch (escaped)
                        {
                        case '"':
                        case '\\':
                        case '/':
                            value.push_back(escaped);
                            break;
                        case 'b':
                            value.push_back('\b');
                            break;
                        case 'f':
                            value.push_back('\f');
                            break;
                        case 'n':
                            value.push_back('\n');
                            break;
                        case 'r':
                            value.push_back('\r');
                            break;
                        case 't':
                            value.push_back('\t');
                            break;
                        default:
                            value.push_back(escaped);
                            break;
                        }
                        continue;
                    }
                    value.push_back(ch);
                }
                throw std::runtime_error("Invalid JSON string.");
            }

            std::string parseValue()
            {
                if (peek() == '"')
                {
                    return parseString();
                }

                const size_t start = pos_;
                while (pos_ < input_.size())
                {
                    const char ch = input_[pos_];
                    if (ch == ',' || ch == '}' || std::isspace(static_cast<unsigned char>(ch)))
                    {
                        break;
                    }
                    ++pos_;
                }
                return shared::Trim(input_.substr(start, pos_ - start));
            }

            const std::string &input_;
            size_t pos_ = 0;
        };

        void appendJsonField(std::ostringstream &oss,
                             bool &first,
                             const std::string &key,
                             const std::string &value,
                             bool quote_value = true)
        {
            if (!first)
            {
                oss << ",";
            }
            first = false;
            oss << "\"" << JsonEscape(key) << "\":";
            if (quote_value)
            {
                oss << "\"" << JsonEscape(value) << "\"";
            }
            else
            {
                oss << value;
            }
        }

        bool waitForReadable(int fd, int timeout_ms)
        {
            while (true)
            {
                fd_set readfds;
                FD_ZERO(&readfds);
                FD_SET(fd, &readfds);

                timeval timeout{};
                timeout.tv_sec = timeout_ms / 1000;
                timeout.tv_usec = (timeout_ms % 1000) * 1000;

                const int ready = ::select(fd + 1, &readfds, nullptr, nullptr, &timeout);
                if (ready < 0)
                {
                    if (errno == EINTR)
                    {
                        continue;
                    }
                    throw std::runtime_error("Failed to wait for HTTP request data.");
                }
                return ready > 0;
            }
        }

        std::string TrimHttpToken(std::string value)
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

        std::string ToLowerAscii(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            return value;
        }
    }

    namespace detail
    {
        const HttpRequestLimits &DefaultHttpRequestLimits()
        {
            static const HttpRequestLimits limits{8 * 1024, 1024 * 1024, 5000};
            return limits;
        }

        std::string MapHttpRequestStatus(HttpRequestErrorKind kind)
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

        std::string MapHttpRequestErrorCode(HttpRequestErrorKind kind)
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

        size_t ParseContentLengthValue(const std::string &raw_value, const HttpRequestLimits &limits)
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

        HttpRequest ParseHttpRequestHeaderBlock(const std::string &raw_header, const HttpRequestLimits &limits)
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

        HttpRequest ReadHttpRequestFromSocket(int fd)
        {
            const HttpRequestLimits &limits = DefaultHttpRequestLimits();
            std::string raw;
            char buffer[4096];
            size_t header_end = std::string::npos;
            bool header_parsed = false;
            HttpRequest request;

            while (true)
            {
                if (header_parsed)
                {
                    request.body = raw.substr(header_end + 4);
                    if (request.body.size() >= request.content_length)
                    {
                        if (request.body.size() > request.content_length)
                        {
                            request.body.resize(request.content_length);
                        }
                        return request;
                    }
                }

                if (!waitForReadable(fd, limits.read_timeout_ms))
                {
                    throw HttpRequestError(HttpRequestErrorKind::Timeout,
                                           "Timed out while reading the HTTP request.");
                }

                const ssize_t read_bytes = ::recv(fd, buffer, sizeof(buffer), 0);
                if (read_bytes < 0)
                {
                    if (errno == EINTR)
                    {
                        continue;
                    }
                    throw std::runtime_error("Failed to read HTTP request.");
                }
                if (read_bytes == 0)
                {
                    throw HttpRequestError(HttpRequestErrorKind::BadRequest,
                                           "Client closed the connection before the HTTP request completed.");
                }

                raw.append(buffer, static_cast<size_t>(read_bytes));

                if (!header_parsed)
                {
                    header_end = raw.find("\r\n\r\n");
                    if (header_end == std::string::npos)
                    {
                        if (raw.size() > limits.max_header_bytes)
                        {
                            throw HttpRequestError(HttpRequestErrorKind::PayloadTooLarge,
                                                   "Request headers exceed the configured limit.");
                        }
                        continue;
                    }

                    request = ParseHttpRequestHeaderBlock(raw.substr(0, header_end), limits);
                    header_parsed = true;
                }
            }
        }

        WebConsoleRoute MatchWebConsoleRoute(const HttpRequest &request)
        {
            const bool is_get = request.method == "GET";
            const bool is_post = request.method == "POST";

            if (is_get && request.path == "/")
                return WebConsoleRoute::Index;
            if (is_get && request.path == "/app.js")
                return WebConsoleRoute::AppJs;
            if (is_get && request.path == "/app.css")
                return WebConsoleRoute::AppCss;
            if (is_get && request.path == "/api/state")
                return WebConsoleRoute::ApiState;
            if (is_get && request.path == "/api/settings")
                return WebConsoleRoute::ApiSettingsGet;
            if (is_get && request.path == "/api/sources")
                return WebConsoleRoute::ApiSources;
            if (is_get && request.path == "/api/source/preview")
                return WebConsoleRoute::ApiSourcePreview;
            if (is_get && request.path == "/events")
                return WebConsoleRoute::Events;
            if (is_post && request.path == "/api/selection")
                return WebConsoleRoute::ApiSelection;
            if (is_post && request.path == "/api/settings")
                return WebConsoleRoute::ApiSettingsPost;
            if (is_post && request.path == "/api/command/start")
                return WebConsoleRoute::ApiCommandStart;
            if (is_post && request.path == "/api/command/pause")
                return WebConsoleRoute::ApiCommandPause;
            if (is_post && request.path == "/api/command/stop")
                return WebConsoleRoute::ApiCommandStop;
            if (is_post && request.path == "/api/command/reset")
                return WebConsoleRoute::ApiCommandReset;
            if (is_post && request.path == "/api/command/shutdown_web")
                return WebConsoleRoute::ApiCommandShutdownWeb;
            if (is_post && request.path == "/api/manual/key")
                return WebConsoleRoute::ApiManualKey;
            return WebConsoleRoute::Unknown;
        }
    }

    std::string ToString(shared::ControlState state)
    {
        switch (state)
        {
        case shared::ControlState::Idle:
            return "idle";
        case shared::ControlState::Starting:
            return "starting";
        case shared::ControlState::Running:
            return "running";
        case shared::ControlState::Paused:
            return "paused";
        case shared::ControlState::Stopping:
            return "stopping";
        case shared::ControlState::Finished:
            return "finished";
        case shared::ControlState::Error:
            return "error";
        }
        return "unknown";
    }

    std::string ToString(shared::SelectedWorkflow workflow)
    {
        switch (workflow)
        {
        case shared::SelectedWorkflow::RdOnly:
            return "rd";
        case shared::SelectedWorkflow::InferOnly:
            return "infer";
        }
        return "infer";
    }

    std::string ToString(shared::SelectedPatchMode patch_mode)
    {
        switch (patch_mode)
        {
        case shared::SelectedPatchMode::AutoSnake:
            return "auto_snake";
        case shared::SelectedPatchMode::ManualFlight:
            return "manual_flight";
        }
        return "auto_snake";
    }

    bool ParseSelectedWorkflow(const std::string &value, shared::SelectedWorkflow &workflow)
    {
        const std::string lowered = shared::ToLower(shared::Trim(value));
        if (lowered == "rd" || lowered == "rd_only")
        {
            workflow = shared::SelectedWorkflow::RdOnly;
            return true;
        }
        if (lowered == "infer" || lowered == "inference" || lowered == "infer_only")
        {
            workflow = shared::SelectedWorkflow::InferOnly;
            return true;
        }
        return false;
    }

    bool ParseSelectedPatchMode(const std::string &value, shared::SelectedPatchMode &patch_mode)
    {
        const std::string lowered = shared::ToLower(shared::Trim(value));
        if (lowered == "auto_snake")
        {
            patch_mode = shared::SelectedPatchMode::AutoSnake;
            return true;
        }
        if (lowered == "manual_flight")
        {
            patch_mode = shared::SelectedPatchMode::ManualFlight;
            return true;
        }
        return false;
    }

    std::string JsonEscape(const std::string &value)
    {
        std::ostringstream oss;
        for (const char ch : value)
        {
            switch (ch)
            {
            case '\\':
                oss << "\\\\";
                break;
            case '"':
                oss << "\\\"";
                break;
            case '\n':
                oss << "\\n";
                break;
            case '\r':
                oss << "\\r";
                break;
            case '\t':
                oss << "\\t";
                break;
            default:
                oss << ch;
                break;
            }
        }
        return oss.str();
    }

    std::string MakeOkResponse(const std::string &message)
    {
        std::ostringstream oss;
        oss << "{\"ok\":true,\"code\":\"ok\",\"message\":\"" << JsonEscape(message) << "\"}";
        return oss.str();
    }

    std::string MakeErrorResponse(const std::string &code, const std::string &message)
    {
        std::ostringstream oss;
        oss << "{\"ok\":false,\"code\":\"" << JsonEscape(code) << "\",\"message\":\"" << JsonEscape(message) << "\"}";
        return oss.str();
    }

    std::string MakeStateResponse(const shared::WorkflowRuntimeSnapshot &snapshot,
                                  const ManualFlightTelemetry &manual_telemetry)
    {
        std::ostringstream oss;
        bool first = true;
        oss << "{";
        appendJsonField(oss, first, "control_state", ToString(snapshot.state));
        appendJsonField(oss, first, "workflow_mode", ToString(snapshot.selection.workflow));
        appendJsonField(oss, first, "patch_mode", ToString(snapshot.selection.patch_mode));
        appendJsonField(oss, first, "output_mode", snapshot.selection.output_mode);
        appendJsonField(oss, first, "selected_source", snapshot.selection.selected_source);
        appendJsonField(oss, first, "current_stage", snapshot.current_stage);
        appendJsonField(oss, first, "current_item", snapshot.current_item);
        appendJsonField(oss, first, "last_error", snapshot.last_error);
        appendJsonField(oss, first, "current_index", std::to_string(snapshot.current_index), false);
        appendJsonField(oss, first, "total_count", std::to_string(snapshot.total_count), false);
        appendJsonField(oss, first, "infer_ms", std::to_string(snapshot.infer_ms), false);
        appendJsonField(oss, first, "total_ms", std::to_string(snapshot.total_ms), false);
        appendJsonField(oss, first, "fps", std::to_string(snapshot.fps), false);
        appendJsonField(oss, first, "manual.configured", manual_telemetry.configured ? "true" : "false", false);
        appendJsonField(oss, first, "manual.active", manual_telemetry.active ? "true" : "false", false);
        appendJsonField(oss, first, "manual.paused", manual_telemetry.paused ? "true" : "false", false);
        appendJsonField(oss, first, "manual.edge_blocked", manual_telemetry.edge_blocked ? "true" : "false", false);
        appendJsonField(oss, first, "manual.position_x", std::to_string(manual_telemetry.position_x), false);
        appendJsonField(oss, first, "manual.position_y", std::to_string(manual_telemetry.position_y), false);
        appendJsonField(oss, first, "manual.last_inferred_center_x", std::to_string(manual_telemetry.last_inferred_center_x), false);
        appendJsonField(oss, first, "manual.last_inferred_center_y", std::to_string(manual_telemetry.last_inferred_center_y), false);
        appendJsonField(oss, first, "manual.path_points", std::to_string(manual_telemetry.path_points), false);
        appendJsonField(oss, first, "manual.patch_count", std::to_string(manual_telemetry.patch_count), false);
        appendJsonField(oss, first, "manual.current_direction", manual_telemetry.current_direction);
        appendJsonField(oss, first, "manual.pending_direction", manual_telemetry.pending_direction);
        oss << "}";
        return oss.str();
    }

    std::string MakeLogEvent(const std::string &message)
    {
        std::ostringstream oss;
        oss << "{\"message\":\"" << JsonEscape(message) << "\"}";
        return oss.str();
    }

    std::string MakeErrorEvent(const std::string &message)
    {
        std::ostringstream oss;
        oss << "{\"message\":\"" << JsonEscape(message) << "\"}";
        return oss.str();
    }

    std::string MakeSettingsResponse(const infer::AppConfig &infer_cfg,
                                     const rd::AppConfig &rd_cfg,
                                     const FlightSettings &flight_settings)
    {
        std::ostringstream oss;
        bool first = true;
        oss << "{";

        appendJsonField(oss, first, "infer.sys.device", infer_cfg.device_url);
        appendJsonField(oss, first, "infer.sys.run_backend", infer_cfg.run_backend);
        appendJsonField(oss, first, "infer.sys.mmuMode", infer_cfg.mmu_mode ? "true" : "false", false);
        appendJsonField(oss, first, "infer.sys.speedMode", infer_cfg.speed_mode ? "true" : "false", false);
        appendJsonField(oss, first, "infer.sys.compressFtmp", infer_cfg.compress_ftmp ? "true" : "false", false);
        appendJsonField(oss, first, "infer.sys.ocm_option", std::to_string(infer_cfg.ocm_option), false);
        appendJsonField(oss, first, "infer.sys.profile", infer_cfg.enable_profile ? "true" : "false", false);
        appendJsonField(oss, first, "infer.input.sar_img_dir", infer_cfg.sar_img_dir.string());
        appendJsonField(oss, first, "infer.input.sar_img_ext", infer_cfg.sar_img_ext);
        appendJsonField(oss, first, "infer.input.recursive", infer_cfg.recursive ? "true" : "false", false);
        appendJsonField(oss, first, "infer.pipeline.patch.mode", infer_cfg.patch_mode);
        appendJsonField(oss, first, "infer.pipeline.patch.patch_size", std::to_string(infer_cfg.patch_size), false);
        appendJsonField(oss, first, "infer.pipeline.patch.stride", std::to_string(infer_cfg.stride), false);
        appendJsonField(oss, first, "infer.pipeline.output_wait_ms", std::to_string(infer_cfg.output_wait_ms), false);
        appendJsonField(oss, first, "infer.display.width", std::to_string(infer_cfg.display_width), false);
        appendJsonField(oss, first, "infer.display.height", std::to_string(infer_cfg.display_height), false);
        appendJsonField(oss, first, "infer.display.fps", std::to_string(infer_cfg.display_fps), false);
        appendJsonField(oss, first, "infer.output.mode", infer_cfg.output_mode);
        appendJsonField(oss, first, "infer.output.dir", infer_cfg.output_dir.string());
        appendJsonField(oss, first, "infer.output.overwrite", infer_cfg.overwrite ? "true" : "false", false);

        appendJsonField(oss, first, "rd.execution_mode", rd_cfg.execution_mode);
        appendJsonField(oss, first, "rd.echo_dir", rd_cfg.echo_dir.string());
        appendJsonField(oss, first, "rd.echo_ext", rd_cfg.echo_ext);
        appendJsonField(oss, first, "rd.output_dir", rd_cfg.output_dir.string());
        appendJsonField(oss, first, "rd.scratch_dir", rd_cfg.scratch_dir.string());
        appendJsonField(oss, first, "rd.column_tile", std::to_string(rd_cfg.column_tile), false);
        appendJsonField(oss, first, "rd.row_tile", std::to_string(rd_cfg.row_tile), false);
        appendJsonField(oss, first, "rd.memory_limit_mb", std::to_string(rd_cfg.memory_limit_mb), false);
        appendJsonField(oss, first, "rd.prefer_memory_pipeline", rd_cfg.prefer_memory_pipeline ? "true" : "false", false);
        appendJsonField(oss, first, "rd.keep_scratch", rd_cfg.keep_scratch ? "true" : "false", false);
        appendJsonField(oss, first, "rd.overwrite", rd_cfg.overwrite ? "true" : "false", false);

        appendJsonField(oss, first, "flight.manual_step_px", std::to_string(flight_settings.manual_step_px), false);
        appendJsonField(oss, first, "flight.boost_step_px", std::to_string(flight_settings.boost_step_px), false);
        appendJsonField(oss, first, "flight.trigger_distance_px", std::to_string(flight_settings.trigger_distance_px), false);
        appendJsonField(oss, first, "flight.cache_grid_px", std::to_string(flight_settings.cache_grid_px), false);
        appendJsonField(oss, first, "flight.path_overlay", flight_settings.path_overlay ? "true" : "false", false);
        appendJsonField(oss, first, "flight.control_bindings", flight_settings.control_bindings);

        oss << "}";
        return oss.str();
    }

    std::string MakeSourcesResponse(const std::vector<SourceInfo> &sources)
    {
        std::ostringstream oss;
        oss << "{\"items\":[";
        for (size_t i = 0; i < sources.size(); ++i)
        {
            if (i > 0)
            {
                oss << ",";
            }
            oss << "{"
                << "\"id\":\"" << JsonEscape(sources[i].id) << "\","
                << "\"name\":\"" << JsonEscape(sources[i].name) << "\","
                << "\"type\":\"" << JsonEscape(sources[i].type) << "\","
                << "\"detail\":\"" << JsonEscape(sources[i].detail) << "\","
                << "\"status\":\"" << JsonEscape(sources[i].status) << "\","
                << "\"previewable\":" << (sources[i].previewable ? "true" : "false")
                << "}";
        }
        oss << "]}";
        return oss.str();
    }

    std::unordered_map<std::string, std::string> ParseFlatJsonObject(const std::string &json)
    {
        return FlatJsonParser(json).parseObject();
    }

    std::string UrlDecode(const std::string &value)
    {
        std::string out;
        out.reserve(value.size());
        for (size_t i = 0; i < value.size(); ++i)
        {
            if (value[i] == '%' && i + 2 < value.size())
            {
                const char hex[] = {value[i + 1], value[i + 2], '\0'};
                out.push_back(static_cast<char>(std::strtol(hex, nullptr, 16)));
                i += 2;
            }
            else if (value[i] == '+')
            {
                out.push_back(' ');
            }
            else
            {
                out.push_back(value[i]);
            }
        }
        return out;
    }

    std::unordered_map<std::string, std::string> ParseQueryString(const std::string &query)
    {
        std::unordered_map<std::string, std::string> values;
        size_t start = 0;
        while (start < query.size())
        {
            const size_t end = query.find('&', start);
            const std::string pair = query.substr(start, end == std::string::npos ? std::string::npos : end - start);
            const size_t eq = pair.find('=');
            const std::string key = UrlDecode(pair.substr(0, eq));
            const std::string value = eq == std::string::npos ? std::string() : UrlDecode(pair.substr(eq + 1));
            if (!key.empty())
            {
                values[key] = value;
            }
            if (end == std::string::npos)
            {
                break;
            }
            start = end + 1;
        }
        return values;
    }
}
