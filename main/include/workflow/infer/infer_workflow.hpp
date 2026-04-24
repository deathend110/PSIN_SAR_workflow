#pragma once

#include <filesystem>

namespace workflow::infer
{
    int Run(const std::filesystem::path &config_path);
}
