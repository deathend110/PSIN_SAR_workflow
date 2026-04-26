#pragma once

#include "workflow/infer/infer_config.hpp"
#include "workflow/rd/rd_config.hpp"
#include "workflow/shared/run_control.hpp"

#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace workflow::web::detail
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

    enum class WebConsoleRoute
    {
        Index,
        AppJs,
        AppCss,
        ApiState,
        ApiSettingsGet,
        ApiSources,
        ApiSourcePreview,
        Events,
        ApiSelection,
        ApiSettingsPost,
        ApiCommandStart,
        ApiCommandPause,
        ApiCommandStop,
        ApiCommandReset,
        ApiCommandShutdownWeb,
        ApiManualKey,
        Unknown
    };

    const HttpRequestLimits &DefaultHttpRequestLimits();
    std::string MapHttpRequestStatus(HttpRequestErrorKind kind);
    std::string MapHttpRequestErrorCode(HttpRequestErrorKind kind);
    size_t ParseContentLengthValue(const std::string &raw_value, const HttpRequestLimits &limits);
    HttpRequest ParseHttpRequestHeaderBlock(const std::string &raw_header, const HttpRequestLimits &limits);
    HttpRequest ReadHttpRequestFromSocket(int fd);
    WebConsoleRoute MatchWebConsoleRoute(const HttpRequest &request);
}

namespace workflow::web
{
    struct FlightSettings
    {
        int manual_step_px = 128;
        int boost_step_px = 256;
        int trigger_distance_px = 128;
        int cache_grid_px = 64;
        bool path_overlay = true;
        std::string control_bindings = "W/A/S/D";
    };

    struct SourceInfo
    {
        std::string id;
        std::string name;
        std::string type;
        std::string detail;
        std::string status;
        bool previewable = false;
    };

    struct ManualFlightTelemetry
    {
        bool configured = false;
        bool active = false;
        bool paused = false;
        bool edge_blocked = false;
        int position_x = 0;
        int position_y = 0;
        int last_inferred_center_x = 0;
        int last_inferred_center_y = 0;
        int path_points = 0;
        int patch_count = 0;
        std::string current_direction;
        std::string pending_direction;
    };

    std::string ToString(shared::ControlState state);
    std::string ToString(shared::SelectedWorkflow workflow);
    std::string ToString(shared::SelectedPatchMode patch_mode);
    bool ParseSelectedWorkflow(const std::string &value, shared::SelectedWorkflow &workflow);
    bool ParseSelectedPatchMode(const std::string &value, shared::SelectedPatchMode &patch_mode);

    std::string JsonEscape(const std::string &value);
    std::string MakeOkResponse(const std::string &message = {});
    std::string MakeErrorResponse(const std::string &code, const std::string &message);
    std::string MakeStateResponse(const shared::WorkflowRuntimeSnapshot &snapshot,
                                  const ManualFlightTelemetry &manual_telemetry = {});
    std::string MakeLogEvent(const std::string &message);
    std::string MakeErrorEvent(const std::string &message);
    std::string MakeSettingsResponse(const infer::AppConfig &infer_cfg,
                                     const rd::AppConfig &rd_cfg,
                                     const FlightSettings &flight_settings);
    std::string MakeSourcesResponse(const std::vector<SourceInfo> &sources);

    std::unordered_map<std::string, std::string> ParseFlatJsonObject(const std::string &json);
    std::string UrlDecode(const std::string &value);
    std::unordered_map<std::string, std::string> ParseQueryString(const std::string &query);
}
