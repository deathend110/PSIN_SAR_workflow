#pragma once

#include <filesystem>

namespace workflow::rd
{
    int Run(const std::filesystem::path &config_path);
}
