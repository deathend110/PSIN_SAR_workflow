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
    // 解析完成的一份 HTTP 请求；字段尽量保持接近原始报文，方便上层路由。
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

    // 当前手写 HTTP 解析器的安全上限，防止异常请求拖垮进程。
    struct HttpRequestLimits
    {
        size_t max_header_bytes;
        size_t max_body_bytes;
        int read_timeout_ms;
    };

    // HTTP 解析阶段可能出现的错误类别。
    enum class HttpRequestErrorKind
    {
        BadRequest,
        Timeout,
        PayloadTooLarge
    };

    // 带错误类别的请求解析异常，供上层映射到 HTTP 状态码。
    class HttpRequestError : public std::runtime_error
    {
    public:
        // 保存解析错误类别和说明文本。
        HttpRequestError(HttpRequestErrorKind kind, const std::string &message)
            : std::runtime_error(message), kind_(kind)
        {
        }

        // 返回当前异常的分类，供上层生成响应码。
        HttpRequestErrorKind kind() const noexcept
        {
            return kind_;
        }

    private:
        HttpRequestErrorKind kind_;
    };

    // Web Console 支持的路由枚举，供 server 的请求分发使用。
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

    // 返回 HTTP 解析器默认限制配置。
    const HttpRequestLimits &DefaultHttpRequestLimits();
    // 把解析错误映射到 HTTP 状态行。
    std::string MapHttpRequestStatus(HttpRequestErrorKind kind);
    // 把解析错误映射到 JSON 错误码。
    std::string MapHttpRequestErrorCode(HttpRequestErrorKind kind);
    // 解析 Content-Length，并检查是否超出限制。
    size_t ParseContentLengthValue(const std::string &raw_value, const HttpRequestLimits &limits);
    // 解析头部文本块，提取 method/path/header 元信息。
    HttpRequest ParseHttpRequestHeaderBlock(const std::string &raw_header, const HttpRequestLimits &limits);
    // 从 socket 完整读取一份 HTTP 请求。
    HttpRequest ReadHttpRequestFromSocket(int fd);
    // 根据 method + path 把请求映射到内部路由枚举。
    WebConsoleRoute MatchWebConsoleRoute(const HttpRequest &request);
}

namespace workflow::web
{
    // Web 侧管理的 manual_flight 参数集合。
    struct FlightSettings
    {
        int manual_step_px = 128;
        int boost_step_px = 256;
        int trigger_distance_px = 128;
        int cache_grid_px = 64;
        bool path_overlay = true;
        std::string control_bindings = "W/A/S/D";
    };

    // 前端源列表中的一项，既可代表输入文件，也可代表目录中扫描出的候选文件。
    struct SourceInfo
    {
        std::string id;
        std::string name;
        std::string type;
        std::string detail;
        std::string status;
        bool previewable = false;
    };

    // Web 层暴露给前端的 manual_flight 遥测。
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

    // 把控制状态转成前端使用的字符串。
    std::string ToString(shared::ControlState state);
    // 把工作流选择转成前端使用的字符串。
    std::string ToString(shared::SelectedWorkflow workflow);
    // 把 patch 模式转成前端使用的字符串。
    std::string ToString(shared::SelectedPatchMode patch_mode);
    // 从字符串解析工作流选择。
    bool ParseSelectedWorkflow(const std::string &value, shared::SelectedWorkflow &workflow);
    // 从字符串解析 patch 模式。
    bool ParseSelectedPatchMode(const std::string &value, shared::SelectedPatchMode &patch_mode);

    // 转义 JSON 字符串内容，避免手写序列化时破坏格式。
    std::string JsonEscape(const std::string &value);
    // 生成通用成功响应。
    std::string MakeOkResponse(const std::string &message = {});
    // 生成通用失败响应。
    std::string MakeErrorResponse(const std::string &code, const std::string &message);
    // 把运行态快照和 manual 遥测序列化为 `/api/state` 响应。
    std::string MakeStateResponse(const shared::WorkflowRuntimeSnapshot &snapshot,
                                  const ManualFlightTelemetry &manual_telemetry = {});
    // 生成 SSE log 事件载荷。
    std::string MakeLogEvent(const std::string &message);
    // 生成 SSE error 事件载荷。
    std::string MakeErrorEvent(const std::string &message);
    // 把三份设置序列化为 `/api/settings` 响应。
    std::string MakeSettingsResponse(const infer::AppConfig &infer_cfg,
                                     const rd::AppConfig &rd_cfg,
                                     const FlightSettings &flight_settings);
    // 把源列表序列化为 `/api/sources` 响应。
    std::string MakeSourcesResponse(const std::vector<SourceInfo> &sources);

    // 解析只包含一层键值对的 JSON 对象。
    std::unordered_map<std::string, std::string> ParseFlatJsonObject(const std::string &json);
    // 对 URL 编码文本做解码。
    std::string UrlDecode(const std::string &value);
    // 把查询字符串解析为键值表。
    std::unordered_map<std::string, std::string> ParseQueryString(const std::string &query);
}
