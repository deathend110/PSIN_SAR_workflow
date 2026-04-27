#pragma once

#include <filesystem>

namespace demo
{
    struct DemoConfig
    {
        std::filesystem::path input_dir;
        std::filesystem::path output_dir;
        std::filesystem::path json_path;
        std::filesystem::path raw_path;
    };

    DemoConfig LoadDemoConfig(const std::filesystem::path &config_path);
}
