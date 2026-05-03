#include "workflow/infer/infer_config.hpp"

#include "workflow/shared/config_utils.hpp"

#include <sstream>
#include <stdexcept>

namespace workflow::infer
{
    namespace
    {
        // 把布尔值写回 YAML 时统一转成 true/false 文本。
        const char *BoolText(bool value)
        {
            return value ? "true" : "false";
        }
    }

    // 读取并校验 Infer 配置。
    // 这里除了做 YAML 字段解析，还会提前做与当前实现强相关的约束检查。
    AppConfig LoadConfig(const std::filesystem::path &config_path)
    {
        const auto runtime_config_path = shared::EnsureRuntimeConfigFile(config_path);
        const auto values = shared::LoadSimpleYaml(runtime_config_path);

        AppConfig cfg;
        cfg.device_url = shared::ValueOr(values, "sys.device", "axi://zg330aiu?npu=0x40000000&dma=0x80000000");
        cfg.run_backend = shared::ValueOr(values, "sys.run_backend", cfg.run_backend);
        cfg.mmu_mode = shared::BoolValueOr(values, "sys.mmuMode", cfg.mmu_mode, "Failed to parse sys.mmuMode");
        cfg.speed_mode = shared::BoolValueOr(values, "sys.speedMode", cfg.speed_mode, "Failed to parse sys.speedMode");
        cfg.compress_ftmp = shared::BoolValueOr(values, "sys.compressFtmp", cfg.compress_ftmp, "Failed to parse sys.compressFtmp");
        cfg.ocm_option = shared::IntValueOr(values, "sys.ocm_option", cfg.ocm_option);
        cfg.enable_profile = shared::BoolValueOr(values, "sys.profile", cfg.enable_profile, "Failed to parse sys.profile");

        cfg.sar_img_dir = shared::ValueOr(values, "input.sar_img_dir", cfg.sar_img_dir.string());
        cfg.sar_img_ext = shared::ValueOr(values, "input.sar_img_ext", cfg.sar_img_ext);
        cfg.recursive = shared::BoolValueOr(values, "input.recursive", cfg.recursive, "Failed to parse input.recursive");

        cfg.patch_mode = shared::ToLower(shared::ValueOr(values, "pipeline.patch.mode", cfg.patch_mode));
        cfg.patch_size = shared::IntValueOr(values, "pipeline.patch.patch_size", cfg.patch_size);
        cfg.stride = shared::IntValueOr(values, "pipeline.patch.stride", cfg.stride);
        cfg.debug_stride_x_px = shared::IntValueOr(values, "pipeline.debug.stride_x_px", cfg.debug_stride_x_px);
        cfg.debug_stride_y_px = shared::IntValueOr(values, "pipeline.debug.stride_y_px", cfg.debug_stride_y_px);
        cfg.json_path = shared::ValueOr(values, "pipeline.icore.json", "");
        cfg.raw_path = shared::ValueOr(values, "pipeline.icore.raw", "");
        cfg.output_wait_ms = shared::IntValueOr(values, "pipeline.output_wait_ms", cfg.output_wait_ms);

        cfg.display_width = shared::IntValueOr(values, "display.width", cfg.display_width);
        cfg.display_height = shared::IntValueOr(values, "display.height", cfg.display_height);
        cfg.display_fps = shared::IntValueOr(values, "display.fps", cfg.display_fps);

        cfg.output_mode = shared::ToLower(shared::ValueOr(values, "output.mode", cfg.output_mode));
        cfg.output_dir = shared::ValueOr(values, "output.dir", cfg.output_dir.string());
        cfg.overwrite = shared::BoolValueOr(values, "output.overwrite", cfg.overwrite, "Failed to parse output.overwrite");
        cfg.dump_backend_log = shared::BoolValueOr(values, "debug.dump_backend_log", cfg.dump_backend_log, "Failed to parse debug.dump_backend_log");

        if (!cfg.sar_img_ext.empty() && cfg.sar_img_ext.front() != '.')
        {
            cfg.sar_img_ext = "." + cfg.sar_img_ext;
        }
        if (cfg.patch_size != kExpectedH || cfg.patch_size != kExpectedW)
        {
            throw std::runtime_error("Only 512x512 patch_size is supported in stage 0.");
        }
        if (cfg.stride <= 0)
        {
            throw std::runtime_error("pipeline.patch.stride must be positive.");
        }
        if (cfg.debug_stride_x_px <= 0 || cfg.debug_stride_y_px <= 0)
        {
            throw std::runtime_error("pipeline.debug.stride_x_px/stride_y_px must be positive.");
        }
        if (cfg.json_path.empty() || cfg.raw_path.empty())
        {
            throw std::runtime_error("pipeline.icore.json/raw must be configured.");
        }
        if (cfg.output_mode != "hdmi" && cfg.output_mode != "png")
        {
            throw std::runtime_error("output.mode must be either hdmi or png.");
        }
        if (cfg.patch_mode == "debug_raster" && cfg.output_mode != "png")
        {
            throw std::runtime_error("debug_raster requires output_mode=png.");
        }

        return cfg;
    }

    // 把当前 Infer 配置写回 runtime YAML，供 Web Console 退出时持久化。
    void SaveConfig(const std::filesystem::path &config_path, const AppConfig &cfg)
    {
        const auto runtime_config_path = shared::RuntimeConfigPath(config_path);
        std::ostringstream oss;
        oss << "sys:\n";
        oss << "  device: " << cfg.device_url << "\n";
        oss << "  run_backend: " << cfg.run_backend << "\n";
        oss << "  mmuMode: " << BoolText(cfg.mmu_mode) << "\n";
        oss << "  speedMode: " << BoolText(cfg.speed_mode) << "\n";
        oss << "  compressFtmp: " << BoolText(cfg.compress_ftmp) << "\n";
        oss << "  ocm_option: " << cfg.ocm_option << "\n";
        oss << "  profile: " << BoolText(cfg.enable_profile) << "\n\n";

        oss << "input:\n";
        oss << "  sar_img_dir: " << cfg.sar_img_dir.string() << "\n";
        oss << "  sar_img_ext: " << cfg.sar_img_ext << "\n";
        oss << "  recursive: " << BoolText(cfg.recursive) << "\n\n";

        oss << "pipeline:\n";
        oss << "  patch:\n";
        oss << "    mode: " << cfg.patch_mode << "\n";
        oss << "    patch_size: " << cfg.patch_size << "\n";
        oss << "    stride: " << cfg.stride << "\n";
        oss << "  debug:\n";
        oss << "    stride_x_px: " << cfg.debug_stride_x_px << "\n";
        oss << "    stride_y_px: " << cfg.debug_stride_y_px << "\n";
        oss << "  icore:\n";
        oss << "    json: " << cfg.json_path << "\n";
        oss << "    raw: " << cfg.raw_path << "\n";
        oss << "  output_wait_ms: " << cfg.output_wait_ms << "\n\n";

        oss << "display:\n";
        oss << "  width: " << cfg.display_width << "\n";
        oss << "  height: " << cfg.display_height << "\n";
        oss << "  fps: " << cfg.display_fps << "\n\n";

        oss << "output:\n";
        oss << "  mode: " << cfg.output_mode << "\n";
        oss << "  dir: " << cfg.output_dir.string() << "\n";
        oss << "  overwrite: " << BoolText(cfg.overwrite) << "\n\n";

        oss << "debug:\n";
        oss << "  dump_backend_log: " << BoolText(cfg.dump_backend_log) << "\n";

        shared::WriteTextFileAtomically(runtime_config_path, oss.str());
    }
}
