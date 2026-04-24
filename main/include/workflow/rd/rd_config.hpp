#pragma once

#include <filesystem>
#include <string>

namespace workflow::rd
{
    struct AppConfig
    {
        std::filesystem::path echo_dir = "./io/echo";
        std::string echo_ext = ".bin";
        std::filesystem::path output_dir = "./io/sar_img";
        std::filesystem::path scratch_dir = "./io/rd_scratch";
        std::string execution_mode = "auto";
        int column_tile = 64;
        int row_tile = 128;
        int memory_limit_mb = 500;
        bool prefer_memory_pipeline = true;
        bool keep_scratch = false;
        bool overwrite = true;
    };

    AppConfig LoadConfig(const std::filesystem::path &config_path);
}
