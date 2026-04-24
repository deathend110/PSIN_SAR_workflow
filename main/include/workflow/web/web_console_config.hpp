#pragma once

#include <filesystem>
#include <string>

namespace workflow::web
{
    struct WebConsoleConfig
    {
        std::string bind_address = "0.0.0.0";
        int port = 8080;
        int sse_heartbeat_ms = 1000;
        std::string ui_title = "PSIN SAR Web Console";
        std::filesystem::path infer_config_path = "configs/infer_workflow.yaml";
        std::filesystem::path rd_config_path = "configs/rd_imaging.yaml";
    };

    WebConsoleConfig LoadConfig(const std::filesystem::path &config_path);
}
