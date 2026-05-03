#pragma once

namespace workflow
{
    // 主程序菜单支持的运行模式。
    enum class AppMode
    {
        // 仅执行 RD 成像流程。
        RdOnly = 1,
        // 仅执行推理流程。
        InferOnly = 2,
        // 启动嵌入式 Web Console 控制台。
        WebConsole = 3,
        // 直接退出主程序。
        Exit = 0,
    };
}
