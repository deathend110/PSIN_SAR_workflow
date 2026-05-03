#include "workflow/web/web_console_config.hpp"

#include "workflow/shared/config_utils.hpp"

#include <sstream>
#include <stdexcept>

namespace workflow::web
{
    namespace
    {
        // 把布尔值写回 YAML 时统一转成 true/false 文本。
        const char *BoolText(bool value)
        {
            return value ? "true" : "false";
        }
    }

    // 读取并校验 Web Console 配置。
    WebConsoleConfig LoadConfig(const std::filesystem::path &config_path)
    {
        const auto runtime_config_path = shared::EnsureRuntimeConfigFile(config_path);
        const auto values = shared::LoadSimpleYaml(runtime_config_path);

        WebConsoleConfig cfg;
        cfg.bind_address = shared::ValueOr(values, "server.bind", cfg.bind_address);
        cfg.board_ip = shared::ValueOr(values, "server.board_ip", cfg.board_ip);
        cfg.port = shared::IntValueOr(values, "server.port", cfg.port);
        cfg.sse_heartbeat_ms = shared::IntValueOr(values, "server.sse_heartbeat_ms", cfg.sse_heartbeat_ms);
        cfg.ui_title = shared::ValueOr(values, "ui.title", cfg.ui_title);
        cfg.infer_config_path = shared::RuntimeConfigPath(shared::ValueOr(values, "config.infer_path", cfg.infer_config_path.string()));
        cfg.rd_config_path = shared::RuntimeConfigPath(shared::ValueOr(values, "config.rd_path", cfg.rd_config_path.string()));
        cfg.flight_settings.manual_step_px = shared::IntValueOr(values, "flight.manual_step_px", cfg.flight_settings.manual_step_px);
        cfg.flight_settings.boost_step_px = shared::IntValueOr(values, "flight.boost_step_px", cfg.flight_settings.boost_step_px);
        cfg.flight_settings.trigger_distance_px = shared::IntValueOr(values, "flight.trigger_distance_px", cfg.flight_settings.trigger_distance_px);
        cfg.flight_settings.cache_grid_px = shared::IntValueOr(values, "flight.cache_grid_px", cfg.flight_settings.cache_grid_px);
        cfg.flight_settings.path_overlay = shared::BoolValueOr(values, "flight.path_overlay", cfg.flight_settings.path_overlay, "Failed to parse flight.path_overlay");
        cfg.flight_settings.control_bindings = shared::ValueOr(values, "flight.control_bindings", cfg.flight_settings.control_bindings);

        cfg.bind_address = shared::Trim(cfg.bind_address);
        cfg.board_ip = shared::Trim(cfg.board_ip);
        if (cfg.bind_address.empty())
        {
            throw std::runtime_error("server.bind must not be empty.");
        }
        if (cfg.board_ip.empty())
        {
            cfg.board_ip = cfg.bind_address;
        }

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

    // 把 Web Console 配置写回 runtime YAML。
    void SaveConfig(const std::filesystem::path &config_path, const WebConsoleConfig &cfg)
    {
        const auto runtime_config_path = shared::RuntimeConfigPath(config_path);
        std::ostringstream oss;
        oss << "server:\n";
        oss << "  bind: " << cfg.bind_address << "\n";
        oss << "  board_ip: " << cfg.board_ip << "\n";
        oss << "  port: " << cfg.port << "\n";
        oss << "  sse_heartbeat_ms: " << cfg.sse_heartbeat_ms << "\n\n";

        oss << "ui:\n";
        oss << "  title: " << cfg.ui_title << "\n\n";

        oss << "config:\n";
        oss << "  infer_path: " << cfg.infer_config_path.string() << "\n";
        oss << "  rd_path: " << cfg.rd_config_path.string() << "\n\n";

        oss << "flight:\n";
        oss << "  manual_step_px: " << cfg.flight_settings.manual_step_px << "\n";
        oss << "  boost_step_px: " << cfg.flight_settings.boost_step_px << "\n";
        oss << "  trigger_distance_px: " << cfg.flight_settings.trigger_distance_px << "\n";
        oss << "  cache_grid_px: " << cfg.flight_settings.cache_grid_px << "\n";
        oss << "  path_overlay: " << BoolText(cfg.flight_settings.path_overlay) << "\n";
        oss << "  control_bindings: " << cfg.flight_settings.control_bindings << "\n";

        shared::WriteTextFileAtomically(runtime_config_path, oss.str());
    }
}
