#pragma once

#include <filesystem>
#include <string>

namespace workflow::infer
{
    inline constexpr int kExpectedN = 1;
    inline constexpr int kExpectedH = 512;
    inline constexpr int kExpectedW = 512;
    inline constexpr int kExpectedC = 1;
    inline constexpr int kSegClasses = 6;

    struct AppConfig
    {
        std::string device_url;
        std::string run_backend = "zg330";
        bool mmu_mode = true;
        bool speed_mode = false;
        bool compress_ftmp = false;
        int ocm_option = 1;
        bool enable_profile = false;

        std::filesystem::path sar_img_dir = "./io/sar_img";
        std::string sar_img_ext = ".png";
        bool recursive = false;

        std::string patch_mode = "auto_snake";
        int patch_size = kExpectedH;
        int stride = 256;

        std::string json_path;
        std::string raw_path;
        int output_wait_ms = 20000;

        int display_width = 1280;
        int display_height = 720;
        int display_fps = 0;

        std::string output_mode = "hdmi";
        std::filesystem::path output_dir = "./io/output";
        bool overwrite = true;

        bool dump_backend_log = true;
    };

    AppConfig LoadConfig(const std::filesystem::path &config_path);
    void SaveConfig(const std::filesystem::path &config_path, const AppConfig &cfg);
}
