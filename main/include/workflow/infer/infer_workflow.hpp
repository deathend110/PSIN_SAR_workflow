#pragma once

#include "workflow/infer/infer_config.hpp"
#include "workflow/shared/run_control.hpp"

#include <filesystem>

namespace workflow::infer
{
    int Run(const std::filesystem::path &config_path);
    int Run(const AppConfig &config, shared::WorkflowRunControl *control = nullptr);
}
