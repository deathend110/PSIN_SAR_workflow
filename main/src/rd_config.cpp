#include "workflow/rd/rd_config.hpp"

#include "workflow/shared/config_utils.hpp"

#include <sstream>
#include <stdexcept>

namespace workflow::rd
{
    namespace
    {
        const char *BoolText(bool value)
        {
            return value ? "true" : "false";
        }
    }

    AppConfig LoadConfig(const std::filesystem::path &config_path)
    {
        const auto values = shared::LoadSimpleYaml(config_path);

        AppConfig cfg;
        cfg.echo_dir = shared::ValueOr(values, "rd.echo_dir", cfg.echo_dir.string());
        cfg.echo_ext = shared::ValueOr(values, "rd.echo_ext", cfg.echo_ext);
        cfg.output_dir = shared::ValueOr(values, "rd.output_dir", cfg.output_dir.string());
        cfg.scratch_dir = shared::ValueOr(values, "rd.scratch_dir", cfg.scratch_dir.string());
        cfg.execution_mode = shared::ToLower(shared::ValueOr(values, "rd.execution_mode", cfg.execution_mode));
        cfg.column_tile = shared::IntValueOr(values, "rd.column_tile", cfg.column_tile);
        cfg.row_tile = shared::IntValueOr(values, "rd.row_tile", cfg.row_tile);
        cfg.memory_limit_mb = shared::IntValueOr(values, "rd.memory_limit_mb", cfg.memory_limit_mb);
        cfg.prefer_memory_pipeline = shared::BoolValueOr(values, "rd.prefer_memory_pipeline", cfg.prefer_memory_pipeline, "Failed to parse rd.prefer_memory_pipeline");
        cfg.keep_scratch = shared::BoolValueOr(values, "rd.keep_scratch", cfg.keep_scratch, "Failed to parse rd.keep_scratch");
        cfg.overwrite = shared::BoolValueOr(values, "rd.overwrite", cfg.overwrite, "Failed to parse rd.overwrite");

        if (!cfg.echo_ext.empty() && cfg.echo_ext.front() != '.')
        {
            cfg.echo_ext = "." + cfg.echo_ext;
        }
        cfg.echo_ext = shared::ToLower(cfg.echo_ext);

        if (cfg.column_tile <= 0 || cfg.row_tile <= 0)
        {
            throw std::runtime_error("rd.column_tile and rd.row_tile must be positive.");
        }
        if (cfg.memory_limit_mb <= 0)
        {
            throw std::runtime_error("rd.memory_limit_mb must be positive.");
        }
        if (cfg.execution_mode != "auto" &&
            cfg.execution_mode != "memory_float32" &&
            cfg.execution_mode != "scratch_double")
        {
            throw std::runtime_error("rd.execution_mode must be auto, memory_float32, or scratch_double.");
        }

        return cfg;
    }

    void SaveConfig(const std::filesystem::path &config_path, const AppConfig &cfg)
    {
        std::ostringstream oss;
        oss << "rd:\n";
        oss << "  execution_mode: " << cfg.execution_mode << "\n";
        oss << "  echo_dir: " << cfg.echo_dir.string() << "\n";
        oss << "  echo_ext: " << cfg.echo_ext << "\n";
        oss << "  output_dir: " << cfg.output_dir.string() << "\n";
        oss << "  scratch_dir: " << cfg.scratch_dir.string() << "\n";
        oss << "  column_tile: " << cfg.column_tile << "\n";
        oss << "  row_tile: " << cfg.row_tile << "\n";
        oss << "  prefer_memory_pipeline: " << BoolText(cfg.prefer_memory_pipeline) << "\n";
        oss << "  memory_limit_mb: " << cfg.memory_limit_mb << "\n";
        oss << "  keep_scratch: " << BoolText(cfg.keep_scratch) << "\n";
        oss << "  overwrite: " << BoolText(cfg.overwrite) << "\n";

        shared::WriteTextFileAtomically(config_path, oss.str());
    }
}
