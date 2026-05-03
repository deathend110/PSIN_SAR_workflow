#pragma once

#include "workflow/web/web_console_protocol.hpp"

#include <filesystem>
#include <string>

namespace workflow::web
{
    // Web Console 的静态配置；主要描述监听地址、UI 标题和关联配置路径。
    struct WebConsoleConfig
    {
        // HTTP 监听地址。
        std::string bind_address = "0.0.0.0";
        // 提示前端访问的板端 IP。
        std::string board_ip = "0.0.0.0";
        // HTTP 监听端口。
        int port = 8080;
        // SSE 心跳周期，单位毫秒。
        int sse_heartbeat_ms = 1000;
        // 前端页面标题。
        std::string ui_title = "PSIN SAR Web Console";
        // Infer runtime 配置路径。
        std::filesystem::path infer_config_path = "configs/infer_workflow.yaml";
        // RD runtime 配置路径。
        std::filesystem::path rd_config_path = "configs/rd_imaging.yaml";
        // manual_flight 的默认飞行/游标参数。
        FlightSettings flight_settings;
    };

    // 从 YAML 读取 Web Console 配置。
    WebConsoleConfig LoadConfig(const std::filesystem::path &config_path);
    // 把 Web Console 配置写回 runtime YAML。
    void SaveConfig(const std::filesystem::path &config_path, const WebConsoleConfig &cfg);
}
