#include "workflow/web/web_console_config.hpp"

#include "workflow/shared/config_utils.hpp"

#include <stdexcept>

namespace workflow::web
{
    WebConsoleConfig LoadConfig(const std::filesystem::path &config_path)
    {
        const auto values = shared::LoadSimpleYaml(config_path);

        WebConsoleConfig cfg;
        cfg.bind_address = shared::ValueOr(values, "server.bind", cfg.bind_address);
        cfg.port = shared::IntValueOr(values, "server.port", cfg.port);
        cfg.sse_heartbeat_ms = shared::IntValueOr(values, "server.sse_heartbeat_ms", cfg.sse_heartbeat_ms);
        cfg.ui_title = shared::ValueOr(values, "ui.title", cfg.ui_title);
        cfg.infer_config_path = shared::ValueOr(values, "config.infer_path", cfg.infer_config_path.string());
        cfg.rd_config_path = shared::ValueOr(values, "config.rd_path", cfg.rd_config_path.string());

        if (cfg.port <= 0 || cfg.port > 65535)
        {
            throw std::runtime_error("server.port must be between 1 and 65535.");
        }
        if (cfg.sse_heartbeat_ms <= 0)
        {
            throw std::runtime_error("server.sse_heartbeat_ms must be positive.");
        }

        return cfg;
    }
}
