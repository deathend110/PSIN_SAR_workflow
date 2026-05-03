#pragma once

#include <filesystem>

namespace workflow::web
{
    // 启动嵌入式 Web Console，并在退出时完成安全收尾和配置持久化。
    int Run(const std::filesystem::path &config_path);
}
