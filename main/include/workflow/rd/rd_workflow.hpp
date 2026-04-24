#pragma once

#include "workflow/rd/rd_config.hpp"
#include "workflow/shared/run_control.hpp"

#include <filesystem>

namespace workflow::rd
{
    int Run(const std::filesystem::path &config_path);
    int Run(const AppConfig &config, shared::WorkflowRunControl *control = nullptr);
}
