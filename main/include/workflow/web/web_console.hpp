#pragma once

#include <filesystem>

namespace workflow::web
{
    int Run(const std::filesystem::path &config_path);
}
