#include "workflow/infer/infer_workflow.hpp"
#include "workflow/rd/rd_workflow.hpp"
#include "workflow/shared/app_mode.hpp"
#include "workflow/shared/config_utils.hpp"
#include "workflow/web/web_console.hpp"

#include <iostream>
#include <string>

namespace
{
    // 三种运行模式默认使用的配置文件路径。
    constexpr const char *kRdConfigPath = "configs/rd_imaging.yaml";
    constexpr const char *kInferConfigPath = "configs/infer_workflow.yaml";
    constexpr const char *kWebConfigPath = "configs/web_console.yaml";

    // 在终端里循环显示菜单，直到用户输入一个合法模式编号。
    workflow::AppMode PromptForMode()
    {
        while (true)
        {
            std::cout << "\n=== PSIN SAR Workflow ===\n";
            std::cout << "1. RD only        (" << kRdConfigPath << ")\n";
            std::cout << "2. Inference only (" << kInferConfigPath << ")\n";
            std::cout << "3. Web Console    (" << kWebConfigPath << ")\n";
            std::cout << "0. Exit\n";
            std::cout << "Select mode: ";

            std::string input;
            if (!std::getline(std::cin, input))
            {
                return workflow::AppMode::Exit;
            }

            input = workflow::shared::Trim(input);
            if (input == "1")
            {
                return workflow::AppMode::RdOnly;
            }
            if (input == "2")
            {
                return workflow::AppMode::InferOnly;
            }
            if (input == "0")
            {
                return workflow::AppMode::Exit;
            }
            if (input == "3")
            {
                return workflow::AppMode::WebConsole;
            }

            std::cout << "Invalid input. Please select 1, 2, 3, or 0.\n";
        }
    }
}

// 主程序入口。
// 当前语义是：
// 1. RD / Infer 选择后执行完即退出进程。
// 2. Web Console 正常返回后会重新回到菜单。
int main()
{
    while (true)
    {
        switch (PromptForMode())
        {
        case workflow::AppMode::RdOnly:
            return workflow::rd::Run(kRdConfigPath);
        case workflow::AppMode::InferOnly:
            return workflow::infer::Run(kInferConfigPath);
        case workflow::AppMode::WebConsole:
        {
            const int result = workflow::web::Run(kWebConfigPath);
            if (result != 0)
            {
                return result;
            }
            break;
        }
        case workflow::AppMode::Exit:
        default:
            std::cout << "Exit without running any workflow.\n";
            return 0;
        }
    }
}
