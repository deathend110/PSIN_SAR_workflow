#pragma once

#include "workflow/rd/rd_config.hpp"
#include "workflow/shared/run_control.hpp"

#include <filesystem>

namespace workflow::rd
{
    // 读取配置文件并执行一轮 RD 成像流程。
    int Run(const std::filesystem::path &config_path);
    // 直接使用内存中的配置执行 RD；control 为空时表示独立运行。
    int Run(const AppConfig &config, shared::WorkflowRunControl *control = nullptr);
}
